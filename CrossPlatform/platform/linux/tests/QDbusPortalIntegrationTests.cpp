#include "localflow/linux/LinuxPlatform.hpp"

#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusVariant>
#include <QElapsedTimer>
#include <QImage>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>
#include <QtDBus/QDBusVirtualObject>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

using namespace localflow::platform::linux;

namespace {

int failures = 0;

#define EXPECT_TRUE(value) do { \
    if (!(value)) { \
        std::cerr << __FILE__ << ':' << __LINE__ << " expected true: " #value "\n"; \
        ++failures; \
    } \
} while (false)

#define EXPECT_EQ(left, right) do { \
    const auto actualLeft = (left); \
    const auto actualRight = (right); \
    if (!(actualLeft == actualRight)) { \
        std::cerr << __FILE__ << ':' << __LINE__ << " expected equality: " #left " == " #right "\n"; \
        ++failures; \
    } \
} while (false)

constexpr auto kPortalService = "org.freedesktop.portal.Desktop";
constexpr auto kPortalRoot = "/org/freedesktop/portal/desktop";

class MockPortal final : public QDBusVirtualObject {
public:
    explicit MockPortal(QDBusConnection bus)
        : bus_(std::move(bus)) {
        screenshotPath_ = temporaryDirectory_.filePath(QStringLiteral("screen.png"));
        QImage screenshot(2, 1, QImage::Format_RGBA8888);
        screenshot.fill(qRgba(12, 34, 56, 255));
        imageReady_ = screenshot.save(screenshotPath_, "PNG");
    }

    QString introspect(const QString&) const override {
        return QStringLiteral("<node/>");
    }

    bool handleMessage(
        const QDBusMessage& message,
        const QDBusConnection& connection) override {
        const auto interfaceName = message.interface();
        const auto member = message.member();
        if (interfaceName == QStringLiteral("org.freedesktop.DBus.Properties") &&
            member == QStringLiteral("Get")) {
            return handleProperty(message, connection);
        }
        if (interfaceName == QStringLiteral("org.freedesktop.portal.Session") &&
            member == QStringLiteral("Close")) {
            ++closedSessions;
            return sendReply(message, connection, {});
        }
        if (interfaceName == QStringLiteral("org.freedesktop.portal.Request") &&
            member == QStringLiteral("Close")) {
            ++closedRequests;
            return sendReply(message, connection, {});
        }
        if (member == QStringLiteral("CreateSession")) {
            const bool global = interfaceName ==
                QStringLiteral("org.freedesktop.portal.GlobalShortcuts");
            const auto session = global
                ? QStringLiteral("/org/freedesktop/portal/desktop/session/mock/global")
                : QStringLiteral("/org/freedesktop/portal/desktop/session/mock/remote");
            if (global) globalSession = session;
            else remoteSession = session;
            QVariantMap results;
            results.insert(QStringLiteral("session_handle"), session);
            return sendRequest(message, connection, std::move(results));
        }
        if (interfaceName == QStringLiteral("org.freedesktop.portal.GlobalShortcuts") &&
            member == QStringLiteral("BindShortcuts")) {
            if (holdNextGlobalBind) {
                holdNextGlobalBind = false;
                return sendHeldRequest(message, connection);
            }
            QVariantMap results;
            results.insert(QStringLiteral("shortcuts"), message.arguments().at(1));
            return sendRequest(message, connection, std::move(results));
        }
        if (interfaceName == QStringLiteral("org.freedesktop.portal.Screenshot") &&
            member == QStringLiteral("Screenshot")) {
            QVariantMap results;
            results.insert(
                QStringLiteral("uri"),
                QUrl::fromLocalFile(screenshotPath_).toString());
            const auto response = nextScreenshotResponse;
            nextScreenshotResponse = 0;
            return sendRequest(
                message, connection, std::move(results), response);
        }
        if (interfaceName == QStringLiteral("org.freedesktop.portal.RemoteDesktop") &&
            member == QStringLiteral("SelectDevices")) {
            return sendRequest(message, connection, {});
        }
        if (interfaceName == QStringLiteral("org.freedesktop.portal.RemoteDesktop") &&
            member == QStringLiteral("Start")) {
            QVariantMap results;
            results.insert(QStringLiteral("devices"), std::uint32_t{1});
            return sendRequest(message, connection, std::move(results));
        }
        if (interfaceName == QStringLiteral("org.freedesktop.portal.RemoteDesktop") &&
            member == QStringLiteral("NotifyKeyboardKeysym")) {
            const auto arguments = message.arguments();
            keyEvents.emplace_back(
                arguments.at(2).toInt(),
                arguments.at(3).toUInt() == 1U);
            return sendReply(message, connection, {});
        }
        return false;
    }

    void emitShortcut(const QString& member, const QString& id, qulonglong timestamp) {
        auto signal = QDBusMessage::createSignal(
            QString::fromLatin1(kPortalRoot),
            QStringLiteral("org.freedesktop.portal.GlobalShortcuts"),
            member);
        signal << QVariant::fromValue(QDBusObjectPath(globalSession))
               << id << timestamp << QVariantMap{};
        EXPECT_TRUE(bus_.send(signal));
    }

    bool imageReady() const noexcept { return imageReady_; }

    QString globalSession;
    QString remoteSession;
    std::vector<std::pair<int, bool>> keyEvents;
    int closedSessions{0};
    int closedRequests{0};
    bool holdNextGlobalBind{false};
    std::uint32_t nextScreenshotResponse{0};

private:
    bool handleProperty(
        const QDBusMessage& message,
        const QDBusConnection& connection) {
        const auto arguments = message.arguments();
        if (arguments.size() != 2) return false;
        const auto interfaceName = arguments.at(0).toString();
        const auto propertyName = arguments.at(1).toString();
        std::uint32_t value = 0;
        if (propertyName == QStringLiteral("version")) {
            value = interfaceName ==
                            QStringLiteral("org.freedesktop.portal.Screenshot")
                        ? 3U
                        : 1U;
        } else if (propertyName == QStringLiteral("AvailableTargets") ||
                   propertyName == QStringLiteral("AvailableDeviceTypes")) {
            value = 1U;
        } else {
            return false;
        }
        return sendReply(
            message,
            connection,
            {QVariant::fromValue(QDBusVariant(value))});
    }

    bool sendRequest(
        const QDBusMessage& message,
        const QDBusConnection& connection,
        QVariantMap results,
        std::uint32_t responseCode = 0) {
        const auto serial = ++nextRequest_;
        const auto path = QStringLiteral(
                              "/org/freedesktop/portal/desktop/request/mock/request") +
                          QString::number(serial);
        if (!sendReply(
                message,
                connection,
                {QVariant::fromValue(QDBusObjectPath(path))})) {
            return false;
        }
        QTimer::singleShot(10, this, [
            bus = bus_,
            path,
            results = std::move(results),
            responseCode
        ] {
            auto response = QDBusMessage::createSignal(
                path,
                QStringLiteral("org.freedesktop.portal.Request"),
                QStringLiteral("Response"));
            response << responseCode << results;
            (void)bus.send(response);
        });
        return true;
    }

    bool sendHeldRequest(
        const QDBusMessage& message,
        const QDBusConnection& connection) {
        const auto path = QStringLiteral(
                              "/org/freedesktop/portal/desktop/request/mock/held") +
                          QString::number(++nextRequest_);
        return sendReply(
            message,
            connection,
            {QVariant::fromValue(QDBusObjectPath(path))});
    }

    static bool sendReply(
        const QDBusMessage& message,
        const QDBusConnection& connection,
        const QVariantList& arguments) {
        return connection.send(message.createReply(arguments));
    }

    QDBusConnection bus_;
    QTemporaryDir temporaryDirectory_;
    QString screenshotPath_;
    std::uint64_t nextRequest_{0};
    bool imageReady_{false};
};

CapabilityReport waylandReport() {
    CapabilityReport report;
    report.session.type = SessionType::wayland;
    report.capabilities.push_back({
        Feature::global_shortcut,
        Availability::permission_required,
        "GlobalShortcuts",
        {},
        {},
    });
    report.capabilities.push_back({
        Feature::clipboard_paste,
        Availability::permission_required,
        "RemoteDesktop",
        {},
        {},
    });
    return report;
}

template <typename Predicate>
bool waitFor(Predicate predicate) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 1000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }
    return predicate();
}

void testGlobalShortcuts(MockPortal& portal) {
    auto backend = makeGlobalShortcutBackend(waylandReport());
    std::vector<ShortcutEvent> events;
    const auto status = backend->start(
        {"push-to-talk", ShortcutKind::key, "F8", {}, 0},
        [&](const ShortcutEvent& event) { events.push_back(event); });
    EXPECT_TRUE(status.ok());
    portal.emitShortcut(QStringLiteral("Activated"), QStringLiteral("push-to-talk"), 101);
    portal.emitShortcut(QStringLiteral("Deactivated"), QStringLiteral("push-to-talk"), 202);
    EXPECT_TRUE(waitFor([&] { return events.size() == 2; }));
    if (events.size() == 2) {
        EXPECT_EQ(events[0].edge, ShortcutEdge::pressed);
        EXPECT_EQ(events[0].monotonicTimestampMs, std::uint64_t{101});
        EXPECT_EQ(events[1].edge, ShortcutEdge::released);
        EXPECT_EQ(events[1].monotonicTimestampMs, std::uint64_t{202});
    }
    backend->stop();
}

void testQDbusCapabilityProbe() {
    SystemHostProbe probe;
    EXPECT_TRUE(probe.sessionBusNameAvailable(kPortalService));
    EXPECT_TRUE(probe.portalInterfaceAvailable(
        "org.freedesktop.portal.GlobalShortcuts"));
    EXPECT_TRUE(probe.portalInterfaceAvailable(
        "org.freedesktop.portal.Screenshot"));
    EXPECT_TRUE(probe.portalInterfaceAvailable(
        "org.freedesktop.portal.RemoteDesktop"));
}

void testScreenshot(MockPortal& portal) {
    auto context = makeScreenContextBackend(SessionType::wayland);
    const auto captured = context->captureContextFrame();
    EXPECT_TRUE(captured.ok());
    if (captured) {
        EXPECT_EQ(captured.value().width, 2);
        EXPECT_EQ(captured.value().height, 1);
        EXPECT_EQ(captured.value().pixelFormat, PixelFormat::rgba8);
        EXPECT_TRUE(captured.value().pixels.size() >= std::size_t{8});
    }

    portal.nextScreenshotResponse = 2;
    auto deniedContext = makeScreenContextBackend(SessionType::wayland);
    const auto denied = deniedContext->captureContextFrame();
    EXPECT_TRUE(!denied.ok());
    EXPECT_EQ(denied.status().code, ErrorCode::permission_denied);
    EXPECT_TRUE(!denied.status().remediation.empty());
}

void testReentrantShortcutCancellation(MockPortal& portal) {
    portal.holdNextGlobalBind = true;
    const auto closedBefore = portal.closedRequests;
    auto backend = makeGlobalShortcutBackend(waylandReport());
    QTimer::singleShot(50, [&] { backend->stop(); });
    const auto status = backend->start(
        {"push-to-talk", ShortcutKind::key, "F8", {}, 0},
        [](const ShortcutEvent&) {});
    EXPECT_EQ(status.code, ErrorCode::cancelled);
    EXPECT_TRUE(waitFor([&] { return portal.closedRequests > closedBefore; }));
}

void testRemoteDesktopPaste(MockPortal& portal) {
    auto paste = makePasteInjector(waylandReport());
    EXPECT_TRUE(paste->paste().ok());
    EXPECT_EQ(portal.keyEvents.size(), std::size_t{4});
    if (portal.keyEvents.size() == 4) {
        EXPECT_EQ(portal.keyEvents[0], std::make_pair(0xffe3, true));
        EXPECT_EQ(portal.keyEvents[1], std::make_pair(0x0076, true));
        EXPECT_EQ(portal.keyEvents[2], std::make_pair(0x0076, false));
        EXPECT_EQ(portal.keyEvents[3], std::make_pair(0xffe3, false));
    }
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    auto bus = QDBusConnection::sessionBus();
    if (!bus.isConnected() || !bus.registerService(QString::fromLatin1(kPortalService))) {
        std::cerr << "Could not register the mock portal service.\n";
        return EXIT_FAILURE;
    }
    MockPortal portal(bus);
    if (!portal.imageReady() ||
        !bus.registerVirtualObject(
            QStringLiteral("/org/freedesktop/portal/desktop"),
            &portal,
            QDBusConnection::SubPath)) {
        std::cerr << "Could not register the mock portal object.\n";
        return EXIT_FAILURE;
    }

    testQDbusCapabilityProbe();
    testGlobalShortcuts(portal);
    testReentrantShortcutCancellation(portal);
    testScreenshot(portal);
    testRemoteDesktopPaste(portal);
    EXPECT_TRUE(portal.closedSessions >= 2);

    if (failures == 0) {
        std::cout << "All QDBus portal integration tests passed.\n";
        return EXIT_SUCCESS;
    }
    std::cerr << failures << " QDBus portal assertion(s) failed.\n";
    return EXIT_FAILURE;
}
