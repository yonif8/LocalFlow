#include "SingleInstance.hpp"

#include <QDeadlineTimer>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMetaObject>
#include <QThread>
#include <QTimer>
#include <QVariant>

#include <algorithm>
#include <utility>

namespace localflow::app {
namespace {

constexpr qsizetype kMaximumFrameBytes = 64;
constexpr int kServerFrameTimeoutMs = 1000;
constexpr auto kBufferProperty = "localflow.activationBuffer";
constexpr auto kHandledProperty = "localflow.activationHandled";

QByteArray frameFor(const InstanceCommand command) {
    return command == InstanceCommand::activate ? QByteArrayLiteral("activate\n")
                                                : QByteArrayLiteral("background\n");
}

int remainingMilliseconds(const QDeadlineTimer& deadline) {
    return std::max(0, int(deadline.remainingTime()));
}

}  // namespace

bool notifyExistingInstance(
    const QString& serverName,
    const InstanceCommand command,
    const int timeoutMs) {
    if (timeoutMs <= 0) return false;

    QDeadlineTimer deadline(timeoutMs);
    QLocalSocket socket;
    socket.connectToServer(serverName, QIODevice::ReadWrite);
    if (!socket.waitForConnected(remainingMilliseconds(deadline))) return false;

    const QByteArray frame = frameFor(command);
    if (socket.write(frame) != frame.size()) {
        socket.abort();
        return false;
    }
    socket.flush();
    while (socket.bytesToWrite() > 0) {
        const int remaining = remainingMilliseconds(deadline);
        if (remaining == 0 ||
            (!socket.waitForBytesWritten(remaining) && socket.bytesToWrite() > 0)) {
            socket.abort();
            return false;
        }
    }

    QByteArray response;
    while (!deadline.hasExpired()) {
        response += socket.readAll();
        if (response.size() > kMaximumFrameBytes) {
            socket.abort();
            return false;
        }
        const qsizetype newline = response.indexOf('\n');
        if (newline >= 0) {
            QByteArray line = response.left(newline);
            if (line.endsWith('\r')) line.chop(1);
            socket.disconnectFromServer();
            return line == QByteArrayLiteral("ok");
        }
        if (socket.state() == QLocalSocket::UnconnectedState) return false;
        const int remaining = remainingMilliseconds(deadline);
        if (remaining == 0 ||
            (!socket.waitForReadyRead(remaining) && socket.bytesAvailable() == 0)) {
            return false;
        }
    }
    return false;
}

bool notifyStartingInstance(
    const QString& serverName,
    const InstanceCommand command,
    const int totalTimeoutMs,
    const int attemptTimeoutMs) {
    if (totalTimeoutMs <= 0 || attemptTimeoutMs <= 0) return false;
    QDeadlineTimer deadline(totalTimeoutMs);
    while (!deadline.hasExpired()) {
        const int remaining = remainingMilliseconds(deadline);
        if (remaining == 0) break;
        if (notifyExistingInstance(
                serverName, command, std::min(attemptTimeoutMs, remaining))) {
            return true;
        }
        const int pause = std::min(50, remainingMilliseconds(deadline));
        if (pause > 0) QThread::msleep(static_cast<unsigned long>(pause));
    }
    return false;
}

class SingleInstanceWorker final : public QObject {
public:
    explicit SingleInstanceWorker(SingleInstanceServer* owner)
        : owner_(owner), server_(this) {
        connect(&server_, &QLocalServer::newConnection,
                this, &SingleInstanceWorker::acceptPendingConnections);
    }

    bool listen(const QString& serverName, QString* error) {
        QLocalServer::removeServer(serverName);
        server_.setSocketOptions(QLocalServer::UserAccessOption);
        if (server_.listen(serverName)) return true;
        if (error != nullptr) *error = server_.errorString();
        return false;
    }

    void close() { server_.close(); }

private:
    void acceptPendingConnections() {
        while (server_.hasPendingConnections()) {
            QLocalSocket* socket = server_.nextPendingConnection();
            if (socket == nullptr) continue;
            socket->setReadBufferSize(kMaximumFrameBytes + 1);
            connect(socket, &QLocalSocket::readyRead,
                    this, [this, socket] { consume(socket); });
            connect(socket, &QLocalSocket::disconnected,
                    socket, &QObject::deleteLater);
            QTimer::singleShot(kServerFrameTimeoutMs, socket, [socket] {
                if (socket->state() != QLocalSocket::UnconnectedState) socket->abort();
            });
            consume(socket);
        }
    }

    void consume(QLocalSocket* socket) {
        if (socket == nullptr || socket->property(kHandledProperty).toBool()) return;
        QByteArray buffer = socket->property(kBufferProperty).toByteArray();
        buffer += socket->readAll();
        if (buffer.size() > kMaximumFrameBytes) {
            socket->abort();
            return;
        }

        const qsizetype newline = buffer.indexOf('\n');
        if (newline < 0) {
            socket->setProperty(kBufferProperty, buffer);
            return;
        }
        QByteArray line = buffer.left(newline);
        if (line.endsWith('\r')) line.chop(1);

        InstanceCommand command;
        if (line == QByteArrayLiteral("activate")) {
            command = InstanceCommand::activate;
        } else if (line == QByteArrayLiteral("background")) {
            command = InstanceCommand::background;
        } else {
            socket->abort();
            return;
        }

        socket->setProperty(kHandledProperty, true);
        QMetaObject::invokeMethod(
            owner_, [owner = owner_, command] { owner->dispatch(command); },
            Qt::QueuedConnection);
        const QByteArray acknowledgement = QByteArrayLiteral("ok\n");
        if (socket->write(acknowledgement) != acknowledgement.size()) {
            socket->abort();
            return;
        }
        socket->flush();
        socket->disconnectFromServer();
    }

    SingleInstanceServer* owner_;
    QLocalServer server_;
};

SingleInstanceServer::SingleInstanceServer(QObject* parent)
    : QObject(parent), worker_(new SingleInstanceWorker(this)) {
    workerThread_.setObjectName(QStringLiteral("LocalFlow activation server"));
    worker_->moveToThread(&workerThread_);
    connect(&workerThread_, &QThread::finished, worker_, &QObject::deleteLater);
    workerThread_.start();
}

SingleInstanceServer::~SingleInstanceServer() {
    if (worker_ != nullptr && workerThread_.isRunning()) {
        QMetaObject::invokeMethod(
            worker_, [worker = worker_] { worker->close(); },
            Qt::BlockingQueuedConnection);
        workerThread_.quit();
        workerThread_.wait();
    }
    worker_ = nullptr;
}

bool SingleInstanceServer::listen(const QString& serverName, QString* error) {
    bool started = false;
    QString detail;
    QMetaObject::invokeMethod(
        worker_,
        [this, &started, &detail, serverName] {
            started = worker_->listen(serverName, &detail);
        },
        Qt::BlockingQueuedConnection);
    if (!started && error != nullptr) *error = std::move(detail);
    return started;
}

void SingleInstanceServer::setCommandHandler(CommandHandler handler) {
    handler_ = std::move(handler);
    if (!handler_) return;
    auto pending = std::move(pendingCommands_);
    pendingCommands_.clear();
    for (const InstanceCommand command : pending) handler_(command);
}

void SingleInstanceServer::dispatch(const InstanceCommand command) {
    if (handler_) {
        handler_(command);
        return;
    }
    if (command == InstanceCommand::activate && pendingCommands_.empty()) {
        pendingCommands_.push_back(command);
    }
}

}  // namespace localflow::app
