#include "PolishWorkerClient.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

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
    recentStderr_.clear();
#ifdef Q_OS_WIN
    process_->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* arguments) {
        arguments->flags |= CREATE_NO_WINDOW;
    });
#endif
    const QString program = workerPath();
    process_->setProgram(program);
#ifndef Q_OS_WIN
    // AppImage launchers prepend their shared Qt directory to LD_LIBRARY_PATH.
    // Put the private worker directory first so its llama/ggml ABI can never
    // resolve to the incompatible NeMo ggml used by the desktop process.
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    const QString workerDirectory = QFileInfo(program).absolutePath();
    const QString inherited = environment.value(QStringLiteral("LD_LIBRARY_PATH"));
    environment.insert(QStringLiteral("LD_LIBRARY_PATH"), inherited.isEmpty()
        ? workerDirectory
        : workerDirectory + QLatin1Char(':') + inherited);
    process_->setProcessEnvironment(environment);
#endif
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

void PolishWorkerClient::drainStderr() {
    if (!process_) return;
    recentStderr_ += process_->readAllStandardError();
    constexpr qsizetype kDiagnosticLimit = 16 * 1024;
    if (recentStderr_.size() > kDiagnosticLimit) {
        recentStderr_.remove(0, recentStderr_.size() - kDiagnosticLimit);
    }
}

bool PolishWorkerClient::readLine(QByteArray* line, int timeoutMs, QString* error) {
    QElapsedTimer timer;
    timer.start();
    for (;;) {
        drainStderr();
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
            drainStderr();
            const qsizetype finalNewline = pendingOutput_.indexOf('\n');
            if (finalNewline >= 0) continue;
            if (error) {
                const QString stderrText = QString::fromUtf8(recentStderr_).trimmed();
                *error = process_->state() == QProcess::NotRunning
                    ? QStringLiteral("Polish worker stopped unexpectedly%1")
                        .arg(stderrText.isEmpty() ? QString() : QStringLiteral(": ") + stderrText.left(500))
                    : QStringLiteral("Polish worker timed out");
            }
            return false;
        }
        pendingOutput_ += process_->readAllStandardOutput();
        drainStderr();
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

bool PolishWorkerClient::prewarm(QString* error) {
    return ensureStarted(error);
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
    recentStderr_.clear();
    process_.reset();
}
