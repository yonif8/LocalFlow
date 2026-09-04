#include "PolishWorkerClient.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

PolishWorkerClient::PolishWorkerClient(QString modelPath)
    : modelPath_(std::move(modelPath)) {}

PolishWorkerClient::~PolishWorkerClient() { stop(); }

QString PolishWorkerClient::workerPath() {
    const QString overridden = qEnvironmentVariable("LOCALFLOW_POLISH_WORKER");
    if (!overridden.isEmpty()) return overridden;
    const QDir appDirectory(QCoreApplication::applicationDirPath());
#ifdef Q_OS_WIN
    const QString installed = appDirectory.filePath(QStringLiteral("polish/localflow-polish-worker.exe"));
    if (QFileInfo::exists(installed)) return installed;
    return appDirectory.filePath(QStringLiteral("localflow-polish-worker.exe"));
#else
    const QString installed = appDirectory.filePath(QStringLiteral("../libexec/localflow/localflow-polish-worker"));
    if (QFileInfo::exists(installed)) return QDir::cleanPath(installed);
    return appDirectory.filePath(QStringLiteral("localflow-polish-worker"));
#endif
}

bool PolishWorkerClient::ensureStarted(QString* error) {
    if (process_ && process_->state() == QProcess::Running) return true;
    process_ = std::make_unique<QProcess>();
    process_->setProcessChannelMode(QProcess::SeparateChannels);
    pendingOutput_.clear();
    process_->setProgram(workerPath());
    process_->setArguments({QStringLiteral("--model"), modelPath_});
    process_->start();
    if (!process_->waitForStarted(5000)) {
        if (error) *error = QStringLiteral("Could not start local polish worker: %1").arg(process_->errorString());
        process_.reset();
        return false;
    }
    QByteArray line;
    if (!readLine(&line, 30000, error)) {
        stop();
        return false;
    }
    const QJsonDocument message = QJsonDocument::fromJson(line);
    if (!message.isObject() || !message.object().value(QStringLiteral("ready")).toBool()) {
        if (error) {
            *error = message.object().value(QStringLiteral("error")).toString(
                QStringLiteral("Polish worker returned an invalid startup response"));
        }
        stop();
        return false;
    }
    return true;
}

bool PolishWorkerClient::readLine(QByteArray* line, int timeoutMs, QString* error) {
    QElapsedTimer timer;
    timer.start();
    for (;;) {
        const qsizetype newline = pendingOutput_.indexOf('\n');
        if (newline >= 0) {
            *line = pendingOutput_.left(newline);
            pendingOutput_.remove(0, newline + 1);
            return line->size() <= 1024 * 1024;
        }
        const int remaining = qMax(0, timeoutMs - int(timer.elapsed()));
        if (!process_) {
            if (error) *error = QStringLiteral("Polish worker is not running");
            return false;
        }
        if (remaining == 0 || !process_->waitForReadyRead(remaining)) {
            pendingOutput_ += process_->readAllStandardOutput();
            const qsizetype finalNewline = pendingOutput_.indexOf('\n');
            if (finalNewline >= 0) continue;
            if (error) {
                const QString stderrText = QString::fromUtf8(process_->readAllStandardError()).trimmed();
                *error = process_->state() == QProcess::NotRunning
                    ? QStringLiteral("Polish worker stopped unexpectedly%1")
                        .arg(stderrText.isEmpty() ? QString() : QStringLiteral(": ") + stderrText.left(500))
                    : QStringLiteral("Polish worker timed out");
            }
            return false;
        }
        pendingOutput_ += process_->readAllStandardOutput();
        if (pendingOutput_.size() > 1024 * 1024) {
            if (error) *error = QStringLiteral("Polish worker response exceeded the safety limit");
            return false;
        }
    }
}

PolishWorkerResult PolishWorkerClient::polish(
    const QString& text, const QString& tone, int timeoutMs, int maxOutputTokens) {
    QString error;
    if (!ensureStarted(&error)) return {false, {}, std::move(error), 0};

    const quint64 id = nextId_++;
    const QJsonObject request{
        {QStringLiteral("id"), QString::number(id)},
        {QStringLiteral("text"), text},
        {QStringLiteral("tone"), tone},
        {QStringLiteral("timeoutMs"), qBound(250, timeoutMs, 10000)},
        {QStringLiteral("maxOutputTokens"), qBound(16, maxOutputTokens, 2048)},
    };
    QByteArray payload = QJsonDocument(request).toJson(QJsonDocument::Compact);
    payload += '\n';
    if (!process_ || process_->write(payload) != payload.size() || !process_->waitForBytesWritten(1000)) {
        stop();
        return {false, {}, QStringLiteral("Could not send text to local polish worker"), 0};
    }

    QByteArray line;
    if (!readLine(&line, qBound(250, timeoutMs, 10000) + 750, &error)) {
        stop();
        return {false, {}, std::move(error), 0};
    }
    const QJsonDocument response = QJsonDocument::fromJson(line);
    if (!response.isObject()) return {false, {}, QStringLiteral("Invalid polish worker response"), 0};
    const QJsonObject object = response.object();
    if (object.value(QStringLiteral("id")).toString().toULongLong() != id) {
        stop();
        return {false, {}, QStringLiteral("Out-of-order polish worker response"), 0};
    }
    return {
        object.value(QStringLiteral("ok")).toBool(),
        object.value(QStringLiteral("text")).toString(),
        object.value(QStringLiteral("error")).toString(),
        qint64(object.value(QStringLiteral("elapsedMs")).toDouble()),
    };
}

void PolishWorkerClient::prewarm() {
    QString ignored;
    ensureStarted(&ignored);
}

void PolishWorkerClient::stop() {
    if (!process_) return;
    if (process_->state() != QProcess::NotRunning) {
        process_->write("{\"command\":\"quit\"}\n");
        process_->closeWriteChannel();
        if (!process_->waitForFinished(750)) {
            process_->kill();
            process_->waitForFinished(1000);
        }
    }
    pendingOutput_.clear();
    process_.reset();
}
