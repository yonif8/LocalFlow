#include "ModelManager.hpp"

#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QtConcurrentRun>

namespace {
constexpr qint64 kReadChunk = 4 * 1024 * 1024;
}

ModelManager::ModelManager(QObject* parent) : QObject(parent) {
    QDir().mkpath(modelsDirectory());
    statusText_ = ready() ? QStringLiteral("Models are ready")
                          : QStringLiteral("Two local models are required");
    detailText_ = ready()
        ? QStringLiteral("Speech and polish work entirely on this computer.")
        : QStringLiteral("Download Parakeet and S1-mini once (about 1.2 GB total). Audio and text are never uploaded.");
}

const ModelManager::Spec& ModelManager::asrSpec() {
    static const Spec value{
        QStringLiteral("Parakeet TDT v3"),
        QStringLiteral("parakeet-tdt-0.6b-v3.q8_0.gguf"),
        QUrl(QStringLiteral("https://huggingface.co/nvidia/parakeet-tdt-0.6b-v3/resolve/541d1f99c6b0c3cd0b11a95167540bb8edefd82b/parakeet-tdt-0.6b-v3.q8_0.gguf")),
        QByteArrayLiteral("e3880d0aaaaf2c308ea2c35016b2b895c423eb3fda924c1b463d1c19b7f4d32e"),
        713975456,
    };
    return value;
}

const ModelManager::Spec& ModelManager::polishSpec() {
    static const Spec value{
        QStringLiteral("S1-mini"),
        QStringLiteral("s1-mini-q4_k_m.gguf"),
        QUrl(QStringLiteral("https://huggingface.co/superwhisper/s1-mini-GGUF/resolve/34add00a48a2e5d24e5a4ee5405a99620a3a240c/s1-mini-q4_k_m.gguf")),
        QByteArrayLiteral("3b41ebe2502cbd03e811d5d16b022f5ab551eda58d62597d152f89535003c634"),
        484219808,
    };
    return value;
}

QString ModelManager::modelsDirectory() {
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
        + QStringLiteral("/models");
}

QString ModelManager::finalPath(const Spec& spec) const {
    return modelsDirectory() + QLatin1Char('/') + spec.filename;
}

QString ModelManager::asrModelPath() const { return finalPath(asrSpec()); }
QString ModelManager::polishModelPath() const { return finalPath(polishSpec()); }

bool ModelManager::validOnDisk(const Spec& spec) const {
    const QFileInfo file(finalPath(spec));
    // Integrity is verified before the atomic rename. Checking the pinned size
    // here makes startup instant without trusting truncated files.
    return file.isFile() && file.size() == spec.bytes;
}

bool ModelManager::ready() const {
    return validOnDisk(asrSpec()) && validOnDisk(polishSpec());
}

double ModelManager::progress() const {
    const qint64 complete = (validOnDisk(asrSpec()) ? asrSpec().bytes : 0)
        + (validOnDisk(polishSpec()) ? polishSpec().bytes : 0);
    const qint64 inFlight = active_ ? qMin(active_->bytes, resumedBytes_ + receivedBytes_) : 0;
    return qBound(0.0, double(complete + inFlight) / double(asrSpec().bytes + polishSpec().bytes), 1.0);
}

QString ModelManager::humanBytes(qint64 bytes) {
    const double value = double(bytes);
    if (bytes >= 1024LL * 1024 * 1024) return QString::number(value / (1024.0 * 1024 * 1024), 'f', 1) + QStringLiteral(" GB");
    if (bytes >= 1024LL * 1024) return QString::number(value / (1024.0 * 1024), 'f', 0) + QStringLiteral(" MB");
    return QString::number(value / 1024.0, 'f', 0) + QStringLiteral(" KB");
}

void ModelManager::downloadMissing() {
    if (busy()) return;
    cancelled_ = false;
    detailText_.clear();
    startNext();
}

void ModelManager::retry() { downloadMissing(); }

void ModelManager::cancel() {
    cancelled_ = true;
    if (reply_) reply_->abort();
    if (verificationCancellation_) verificationCancellation_->store(true);
    statusText_ = QStringLiteral("Download paused");
    detailText_ = QStringLiteral("Your progress was saved. Resume whenever you’re ready.");
    emit changed();
}

void ModelManager::revealModels() {
    QDesktopServices::openUrl(QUrl::fromLocalFile(modelsDirectory()));
}

void ModelManager::startNext() {
    if (!validOnDisk(asrSpec())) return start(asrSpec());
    if (!validOnDisk(polishSpec())) return start(polishSpec());
    active_ = nullptr;
    statusText_ = QStringLiteral("Models are ready");
    detailText_ = QStringLiteral("Speech and polish work entirely on this computer.");
    emit changed();
    emit modelsReady();
}

void ModelManager::start(const Spec& spec) {
    QDir().mkpath(modelsDirectory());
    active_ = &spec;
    receivedBytes_ = 0;
    transferFailure_.clear();
    const QString partPath = finalPath(spec) + QStringLiteral(".part");
    resumedBytes_ = QFileInfo(partPath).size();
    if (resumedBytes_ < 0 || resumedBytes_ > spec.bytes) {
        QFile stale(partPath);
        if (stale.open(QIODevice::WriteOnly | QIODevice::Truncate)) stale.close();
        resumedBytes_ = 0;
    }

    output_.setFileName(partPath);
    if (!output_.open(QIODevice::WriteOnly | QIODevice::Append)) {
        return fail(QStringLiteral("Couldn’t save the model"), output_.errorString());
    }

    QNetworkRequest request(spec.url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setRawHeader("User-Agent", "LocalFlow/" LOCALFLOW_VERSION);
    if (resumedBytes_ > 0) request.setRawHeader("Range", "bytes=" + QByteArray::number(resumedBytes_) + "-");
    reply_ = network_.get(request);
    reply_->setReadBufferSize(kReadChunk);
    connect(reply_, &QNetworkReply::metaDataChanged, this, &ModelManager::handleMetadata);
    connect(reply_, &QIODevice::readyRead, this, &ModelManager::handleReadyRead);
    connect(reply_, &QNetworkReply::downloadProgress, this, [this](qint64 received, qint64) {
        receivedBytes_ = qMax<qint64>(0, received);
        if (active_) {
            statusText_ = QStringLiteral("Downloading %1 — %2 of %3")
                .arg(active_->displayName, humanBytes(qMin(active_->bytes, resumedBytes_ + receivedBytes_)), humanBytes(active_->bytes));
        }
        emit changed();
    });
    connect(reply_, &QNetworkReply::finished, this, &ModelManager::handleFinished);
    statusText_ = QStringLiteral("Downloading %1…").arg(spec.displayName);
    detailText_ = resumedBytes_ > 0 ? QStringLiteral("Resuming the saved download.") : QString();
    emit changed();
}

void ModelManager::handleMetadata() {
    if (!reply_ || resumedBytes_ == 0) return;
    const int status = reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 200) {
        // The origin ignored Range. Keep this response but restart the file so
        // the completed artifact cannot contain the prefix twice.
        output_.resize(0);
        output_.seek(0);
        resumedBytes_ = 0;
    }
}

void ModelManager::handleReadyRead() {
    if (!reply_ || !output_.isOpen()) return;
    const QByteArray bytes = reply_->readAll();
    if (bytes.isEmpty() || !transferFailure_.isEmpty()) return;
    const qint64 currentSize = output_.size();
    if (!active_ || currentSize < 0 || bytes.size() > active_->bytes - currentSize) {
        transferFailure_ = QStringLiteral(
            "The server sent more data than the pinned model size. No excess data was saved.");
        reply_->abort();
        return;
    }
    if (output_.write(bytes) != bytes.size()) {
        transferFailure_ = output_.errorString();
        reply_->abort();
    }
}

void ModelManager::handleFinished() {
    if (!reply_ || !active_) return;
    QNetworkReply* completed = reply_;
    const Spec spec = *active_;
    handleReadyRead();
    output_.flush();
    output_.close();
    const auto networkError = completed->error();
    const QString networkDetail = completed->errorString();
    completed->deleteLater();
    reply_ = nullptr;

    if (cancelled_) {
        resetTransfer();
        emit changed();
        return;
    }
    if (!transferFailure_.isEmpty()) {
        const QString detail = transferFailure_;
        resetTransfer();
        return fail(QStringLiteral("Model download was rejected"), detail);
    }
    if (networkError != QNetworkReply::NoError) {
        resetTransfer();
        return fail(QStringLiteral("Download interrupted"), networkDetail + QStringLiteral(" Your progress is saved."));
    }
    const QString partPath = finalPath(spec) + QStringLiteral(".part");
    if (QFileInfo(partPath).size() != spec.bytes) {
        resetTransfer();
        return fail(QStringLiteral("Download was incomplete"), QStringLiteral("Expected %1 but received %2. You can safely retry.")
            .arg(humanBytes(spec.bytes), humanBytes(QFileInfo(partPath).size())));
    }
    verifyDownloaded(spec, partPath);
}

void ModelManager::verifyDownloaded(const Spec& spec, const QString& partPath) {
    verifying_ = true;
    const auto cancellation = std::make_shared<std::atomic_bool>(false);
    verificationCancellation_ = cancellation;
    statusText_ = QStringLiteral("Verifying %1…").arg(spec.displayName);
    detailText_ = QStringLiteral("Checking the model before LocalFlow uses it.");
    emit changed();

    auto* watcher = new QFutureWatcher<QByteArray>(this);
    connect(watcher, &QFutureWatcher<QByteArray>::finished, this, [
        this, watcher, spec, partPath, cancellation
    ] {
        const QByteArray digest = watcher->result();
        watcher->deleteLater();
        verifying_ = false;
        if (cancelled_ || cancellation->load()) {
            resetTransfer();
            emit changed();
            return;
        }
        if (digest.toHex() != spec.sha256) {
            QFile::remove(partPath);
            resetTransfer();
            return fail(QStringLiteral("Model verification failed"), QStringLiteral("The downloaded file was damaged and has been removed. Please retry."));
        }
        const QString destination = finalPath(spec);
        QFile::remove(destination);
        if (!QFile::rename(partPath, destination)) {
            resetTransfer();
            return fail(QStringLiteral("Couldn’t install the model"), QStringLiteral("LocalFlow could not move the verified model into place."));
        }
        resetTransfer();
        startNext();
    });
    watcher->setFuture(QtConcurrent::run([partPath, cancellation] {
        QFile file(partPath);
        if (!file.open(QIODevice::ReadOnly)) return QByteArray();
        QCryptographicHash hash(QCryptographicHash::Sha256);
        while (!file.atEnd()) {
            if (cancellation->load()) return QByteArray();
            const QByteArray chunk = file.read(kReadChunk);
            if (chunk.isEmpty() && !file.atEnd()) return QByteArray();
            hash.addData(chunk);
        }
        if (cancellation->load()) return QByteArray();
        return hash.result();
    }));
}

void ModelManager::fail(QString summary, QString detail) {
    statusText_ = std::move(summary);
    detailText_ = std::move(detail);
    emit changed();
}

void ModelManager::resetTransfer() {
    if (output_.isOpen()) output_.close();
    active_ = nullptr;
    resumedBytes_ = 0;
    receivedBytes_ = 0;
    transferFailure_.clear();
    verificationCancellation_.reset();
}
