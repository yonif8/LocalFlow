#pragma once

#include <QProcess>
#include <QString>

#include <memory>

struct PolishWorkerResult {
    bool ok = false;
    QString text;
    QString error;
    qint64 elapsedMs = 0;
};

/// Blocking client for the persistent, process-isolated S1 runtime. Use each
/// instance from one owning worker thread. Isolation prevents NeMo and modern
/// llama.cpp from loading incompatible libggml versions into one process.
class PolishWorkerClient final {
public:
    explicit PolishWorkerClient(QString modelPath);
    ~PolishWorkerClient();

    PolishWorkerResult polish(
        const QString& text,
        const QString& tone,
        int timeoutMs,
        int maxOutputTokens = 1024);
    bool prewarm(QString* error = nullptr);
    void stop();

private:
    bool ensureStarted(QString* error);
    bool readLine(QByteArray* line, int timeoutMs, QString* error);
    void drainStderr();
    static QString workerPath();

    QString modelPath_;
    std::unique_ptr<QProcess> process_;
    QByteArray pendingOutput_;
    QByteArray recentStderr_;
    quint64 nextId_ = 1;
};
