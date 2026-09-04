#include "SingleInstance.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QLocalServer>
#include <QLocalSocket>
#include <QThread>
#include <QUuid>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>

namespace {
using namespace std::chrono_literals;

int failures = 0;

void expect(const bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

QString uniqueServerName() {
    return QStringLiteral("LocalFlow-test-") +
           QUuid::createUuid().toString(QUuid::WithoutBraces);
}

template <typename Predicate>
bool pumpUntil(Predicate&& predicate, const int timeoutMs = 2000) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!predicate() && elapsed.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return predicate();
}

void testAcknowledgedActivationAndStartupQueue() {
    localflow::app::SingleInstanceServer server;
    QString error;
    const QString name = uniqueServerName();
    expect(server.listen(name, &error), "the private activation server listens");

    auto notification = std::async(std::launch::async, [name] {
        return localflow::app::notifyExistingInstance(
            name, localflow::app::InstanceCommand::activate, 1000);
    });
    // Simulate slow synchronous capability probes on the UI thread. The tiny
    // activation server has its own event loop and must still ACK immediately.
    expect(notification.wait_for(750ms) == std::future_status::ready,
           "the activation server responds while the UI thread is blocked");
    expect(notification.get(), "the sender succeeds only after receiving an ACK");

    int activations = 0;
    server.setCommandHandler([&](const auto command) {
        if (command == localflow::app::InstanceCommand::activate) ++activations;
    });
    expect(pumpUntil([&] { return activations == 1; }),
           "an activation received during startup is delivered when the UI handler is ready");
}

void testFragmentedFrameWaitsForNewline() {
    localflow::app::SingleInstanceServer server;
    const QString name = uniqueServerName();
    expect(server.listen(name), "the fragmented-frame server listens");
    int activations = 0;
    server.setCommandHandler([&](const auto command) {
        if (command == localflow::app::InstanceCommand::activate) ++activations;
    });

    QLocalSocket socket;
    socket.connectToServer(name, QIODevice::ReadWrite);
    expect(socket.waitForConnected(500), "the fragmented client connects");
    expect(socket.write(QByteArrayLiteral("acti")) == 4,
           "the first activation fragment is queued");
    socket.flush();
    pumpUntil([] { return false; }, 30);
    expect(activations == 0, "a partial command is never dispatched");

    expect(socket.write(QByteArrayLiteral("vate\n")) == 5,
           "the final activation fragment is queued");
    socket.flush();
    QByteArray response;
    expect(pumpUntil([&] {
        response += socket.readAll();
        return response.contains('\n');
    }), "a complete fragmented command receives a response");
    expect(response.startsWith(QByteArrayLiteral("ok\n")),
           "the complete command receives the protocol ACK");
    expect(activations == 1, "the complete command is dispatched exactly once");
}

void testMissingAcknowledgementFailsClosed() {
    QLocalServer silentServer;
    const QString name = uniqueServerName();
    QLocalServer::removeServer(name);
    silentServer.setSocketOptions(QLocalServer::UserAccessOption);
    expect(silentServer.listen(name), "the no-ACK fixture server listens");
    QObject::connect(&silentServer, &QLocalServer::newConnection, &silentServer, [&] {
        while (silentServer.hasPendingConnections()) {
            QLocalSocket* socket = silentServer.nextPendingConnection();
            if (socket == nullptr) continue;
            socket->abort();
            socket->deleteLater();
        }
    });

    auto notification = std::async(std::launch::async, [name] {
        return localflow::app::notifyExistingInstance(
            name, localflow::app::InstanceCommand::activate, 400);
    });
    expect(pumpUntil([&] { return notification.wait_for(0ms) == std::future_status::ready; }),
           "the missing-ACK attempt completes within its deadline");
    expect(!notification.get(), "a connection without an ACK is not reported as delivered");
}

void testLockToListenStartupWindowIsRetried() {
    const QString name = uniqueServerName();
    auto notification = std::async(std::launch::async, [name] {
        return localflow::app::notifyStartingInstance(
            name, localflow::app::InstanceCommand::activate, 1200, 100);
    });
    pumpUntil([] { return false; }, 80);

    localflow::app::SingleInstanceServer server;
    expect(server.listen(name), "the delayed startup server listens");
    int activations = 0;
    server.setCommandHandler([&](const auto command) {
        if (command == localflow::app::InstanceCommand::activate) ++activations;
    });
    expect(pumpUntil([&] { return notification.wait_for(0ms) == std::future_status::ready; }),
           "a sender retries across the lock-to-listen startup window");
    expect(notification.get(), "the delayed server eventually acknowledges activation");
    expect(activations == 1, "the delayed activation is dispatched exactly once");
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    testAcknowledgedActivationAndStartupQueue();
    testFragmentedFrameWaitsForNewline();
    testMissingAcknowledgementFailsClosed();
    testLockToListenStartupWindowIsRetried();
    if (failures == 0) {
        std::cout << "Single-instance protocol tests passed\n";
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
}
