#ifdef linux
#undef linux
#endif

#include "InternalFactories.hpp"

#include "PortalSupport.hpp"

#ifndef LOCALFLOW_LINUX_WITH_QT_PORTALS
#define LOCALFLOW_LINUX_WITH_QT_PORTALS 0
#endif

#if LOCALFLOW_LINUX_WITH_QT_PORTALS

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusError>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusVariant>
#include <QEventLoop>
#include <QImage>
#include <QImageReader>
#include <QMetaType>
#include <QObject>
#include <QRandomGenerator>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace localflow::platform::linux::detail {
struct PortalShortcutDescription {
    QString id;
    QVariantMap options;
};

using PortalShortcutDescriptions = QList<PortalShortcutDescription>;

QDBusArgument& operator<<(
    QDBusArgument& argument,
    const PortalShortcutDescription& shortcut) {
    argument.beginStructure();
    argument << shortcut.id << shortcut.options;
    argument.endStructure();
    return argument;
}

const QDBusArgument& operator>>(
    const QDBusArgument& argument,
    PortalShortcutDescription& shortcut) {
    argument.beginStructure();
    argument >> shortcut.id >> shortcut.options;
    argument.endStructure();
    return argument;
}

}  // namespace localflow::platform::linux::detail

Q_DECLARE_METATYPE(
    localflow::platform::linux::detail::PortalShortcutDescription)
Q_DECLARE_METATYPE(
    localflow::platform::linux::detail::PortalShortcutDescriptions)

namespace localflow::platform::linux::detail {
namespace {

constexpr auto kPortalService = "org.freedesktop.portal.Desktop";
constexpr auto kPortalPath = "/org/freedesktop/portal/desktop";
constexpr auto kRequestInterface = "org.freedesktop.portal.Request";
constexpr auto kSessionInterface = "org.freedesktop.portal.Session";
constexpr auto kPropertiesInterface = "org.freedesktop.DBus.Properties";
constexpr auto kGlobalShortcutsInterface =
    "org.freedesktop.portal.GlobalShortcuts";
constexpr auto kScreenshotInterface = "org.freedesktop.portal.Screenshot";
constexpr auto kRemoteDesktopInterface =
    "org.freedesktop.portal.RemoteDesktop";
constexpr std::uint32_t kKeyboardDevice = 1U;
constexpr std::uint32_t kScreenshotTargetScreen = 1U;
constexpr auto kInteractionTimeout = std::chrono::seconds(60);
constexpr auto kMethodTimeout = std::chrono::seconds(5);

class PortalResponseReceiver final : public QObject {
    Q_OBJECT

public Q_SLOTS:
    void response(uint response, const QVariantMap& results) {
        Q_EMIT received(response, results);
    }

Q_SIGNALS:
    void received(uint response, const QVariantMap& results);
};

QString requestToken(const QString& prefix) {
    static std::atomic<std::uint64_t> counter{0};
    const auto serial = counter.fetch_add(1, std::memory_order_relaxed);
    const auto random = QRandomGenerator::global()->generate64();
    return prefix + QString::number(random, 16) + QString::number(serial, 16);
}

QString requestPath(const QDBusConnection& bus, const QString& token) {
    QString sender = bus.baseService();
    if (sender.startsWith(':')) sender.remove(0, 1);
    sender.replace('.', '_');
    return QStringLiteral("/org/freedesktop/portal/desktop/request/") +
           sender + QLatin1Char('/') + token;
}

QVariant unwrapped(QVariant value) {
    while (value.metaType() == QMetaType::fromType<QDBusVariant>()) {
        value = value.value<QDBusVariant>().variant();
    }
    return value;
}

std::string text(const QString& value) {
    return value.toUtf8().toStdString();
}

Status dbusFailure(const QDBusError& error, const std::string& operation) {
    const auto name = error.name();
    ErrorCode code = ErrorCode::protocol_error;
    std::string remediation;
    if (name.contains(QStringLiteral("AccessDenied"), Qt::CaseInsensitive) ||
        name.contains(QStringLiteral("NotAllowed"), Qt::CaseInsensitive) ||
        name.contains(QStringLiteral("Permission"), Qt::CaseInsensitive)) {
        code = ErrorCode::permission_denied;
        remediation =
            "Allow LocalFlow in the desktop's privacy or portal permission settings.";
    } else if (name.contains(QStringLiteral("NoReply"), Qt::CaseInsensitive) ||
               name.contains(QStringLiteral("Timeout"), Qt::CaseInsensitive)) {
        code = ErrorCode::timed_out;
        remediation = "Restart xdg-desktop-portal and try again.";
    } else if (name.contains(QStringLiteral("ServiceUnknown"), Qt::CaseInsensitive) ||
               name.contains(QStringLiteral("NameHasNoOwner"), Qt::CaseInsensitive) ||
               name.contains(QStringLiteral("UnknownInterface"), Qt::CaseInsensitive) ||
               name.contains(QStringLiteral("UnknownObject"), Qt::CaseInsensitive) ||
               name.contains(QStringLiteral("Disconnected"), Qt::CaseInsensitive)) {
        code = ErrorCode::service_unavailable;
        remediation =
            "Install or restart xdg-desktop-portal and the backend for your desktop.";
    }

    auto detail = "The desktop portal could not " + operation;
    if (!error.message().isEmpty()) detail += ": " + text(error.message());
    return Status::failure(code, std::move(detail), std::move(remediation));
}

Result<QVariant> portalProperty(
    const QDBusConnection& bus,
    const QString& interfaceName,
    const QString& propertyName) {
    auto message = QDBusMessage::createMethodCall(
        QString::fromLatin1(kPortalService),
        QString::fromLatin1(kPortalPath),
        QString::fromLatin1(kPropertiesInterface),
        QStringLiteral("Get"));
    message << interfaceName << propertyName;
    const auto reply = bus.call(
        message,
        QDBus::Block,
        static_cast<int>(kMethodTimeout.count() * 1000));
    if (reply.type() == QDBusMessage::ErrorMessage) {
        return Result<QVariant>::failure(dbusFailure(
            QDBusError(reply),
            "read " + text(interfaceName) + "." + text(propertyName)));
    }
    if (reply.arguments().size() != 1) {
        return Result<QVariant>::failure(Status::failure(
            ErrorCode::protocol_error,
            "The desktop portal returned an invalid property response."));
    }
    return Result<QVariant>::success(unwrapped(reply.arguments().front()));
}

Status requirePortalVersion(
    const QDBusConnection& bus,
    const QString& interfaceName,
    std::uint32_t minimum,
    std::uint32_t* version = nullptr) {
    if (!bus.isConnected()) {
        return Status::failure(
            ErrorCode::service_unavailable,
            "The D-Bus session bus is unavailable.",
            "Start LocalFlow inside a graphical desktop session.");
    }
    const auto property = portalProperty(bus, interfaceName, QStringLiteral("version"));
    if (!property) return property.status();
    bool converted = false;
    const auto detected = property.value().toUInt(&converted);
    if (!converted || detected < minimum) {
        return Status::failure(
            ErrorCode::missing_dependency,
            text(interfaceName) + " version " + std::to_string(minimum) +
                " or newer is required.",
            "Update xdg-desktop-portal and the portal backend for your desktop.");
    }
    if (version != nullptr) *version = detected;
    return Status::success();
}

void closeRequest(const QDBusConnection& bus, const QString& path) noexcept {
    if (path.isEmpty() || !bus.isConnected()) return;
    auto message = QDBusMessage::createMethodCall(
        QString::fromLatin1(kPortalService),
        path,
        QString::fromLatin1(kRequestInterface),
        QStringLiteral("Close"));
    (void)bus.asyncCall(message, static_cast<int>(kMethodTimeout.count() * 1000));
}

void closeSession(const QDBusConnection& bus, const QString& path) noexcept {
    if (path.isEmpty() || !bus.isConnected()) return;
    auto message = QDBusMessage::createMethodCall(
        QString::fromLatin1(kPortalService),
        path,
        QString::fromLatin1(kSessionInterface),
        QStringLiteral("Close"));
    (void)bus.call(
        message,
        QDBus::Block,
        static_cast<int>(kMethodTimeout.count() * 1000));
}

class PortalRequestRunner final {
public:
    explicit PortalRequestRunner(QDBusConnection bus)
        : bus_(std::move(bus)) {}

    Result<QVariantMap> request(
        const QString& interfaceName,
        const QString& method,
        QVariantList arguments,
        int optionsIndex,
        const std::string& operation,
        const std::string& permissionRemediation) {
        std::lock_guard<std::mutex> operationLock(operationMutex_);
        if (cancelled_.load(std::memory_order_acquire)) {
            return Result<QVariantMap>::failure(Status::failure(
                ErrorCode::cancelled,
                "The " + operation + " request was cancelled."));
        }
        if (optionsIndex < 0 || optionsIndex >= arguments.size() ||
            !arguments.at(optionsIndex).canConvert<QVariantMap>()) {
            return Result<QVariantMap>::failure(Status::failure(
                ErrorCode::internal_error,
                "LocalFlow constructed an invalid " + operation + " portal request."));
        }

        const auto token = requestToken(QStringLiteral("localflow"));
        auto options = arguments.at(optionsIndex).toMap();
        options.insert(QStringLiteral("handle_token"), token);
        arguments[optionsIndex] = options;

        QString responsePath = requestPath(bus_, token);
        PortalResponseReceiver receiver;
        QEventLoop loop;
        bool responseReceived = false;
        bool methodFinished = false;
        bool timedOut = false;
        std::uint32_t responseCode = std::numeric_limits<std::uint32_t>::max();
        QVariantMap responseResults;
        std::optional<Status> methodFailure;

        const auto connectResponse = [&](const QString& path) {
            return bus_.connect(
                QString::fromLatin1(kPortalService),
                path,
                QString::fromLatin1(kRequestInterface),
                QStringLiteral("Response"),
                &receiver,
                SLOT(response(uint,QVariantMap)));
        };
        const auto disconnectResponse = [&](const QString& path) {
            return bus_.disconnect(
                QString::fromLatin1(kPortalService),
                path,
                QString::fromLatin1(kRequestInterface),
                QStringLiteral("Response"),
                &receiver,
                SLOT(response(uint,QVariantMap)));
        };

        if (!connectResponse(responsePath)) {
            return Result<QVariantMap>::failure(Status::failure(
                ErrorCode::service_unavailable,
                "LocalFlow could not listen for the " + operation + " portal response.",
                "Restart xdg-desktop-portal and try again."));
        }
        setActivePath(responsePath);

        QObject::connect(
            &receiver,
            &PortalResponseReceiver::received,
            &loop,
            [&](uint code, const QVariantMap& results) {
                responseCode = code;
                responseResults = results;
                responseReceived = true;
                if (methodFinished) loop.quit();
            });

        QDBusInterface portal(
            QString::fromLatin1(kPortalService),
            QString::fromLatin1(kPortalPath),
            interfaceName,
            bus_);
        portal.setTimeout(static_cast<int>(kMethodTimeout.count() * 1000));
        if (!portal.isValid()) {
            disconnectResponse(responsePath);
            clearActivePath(responsePath);
            return Result<QVariantMap>::failure(Status::failure(
                ErrorCode::service_unavailable,
                text(interfaceName) + " is not available on the session bus.",
                "Install xdg-desktop-portal and the portal backend for your desktop."));
        }

        const auto pending = portal.asyncCallWithArgumentList(method, arguments);
        QDBusPendingCallWatcher watcher(pending);
        QObject::connect(
            &watcher,
            &QDBusPendingCallWatcher::finished,
            &loop,
            [&](QDBusPendingCallWatcher* completed) {
                QDBusPendingReply<QDBusObjectPath> reply = *completed;
                if (reply.isError()) {
                    methodFailure = dbusFailure(reply.error(), operation);
                    loop.quit();
                    return;
                }
                methodFinished = true;
                const auto returnedPath = reply.value().path();
                if (returnedPath.isEmpty()) {
                    methodFailure = Status::failure(
                        ErrorCode::protocol_error,
                        "The desktop portal returned an empty request handle for " +
                            operation + ".");
                    loop.quit();
                    return;
                }
                if (returnedPath != responsePath) {
                    disconnectResponse(responsePath);
                    clearActivePath(responsePath);
                    responsePath = returnedPath;
                    if (!connectResponse(responsePath)) {
                        methodFailure = Status::failure(
                            ErrorCode::service_unavailable,
                            "LocalFlow could not follow the desktop's " + operation +
                                " request handle.");
                        loop.quit();
                        return;
                    }
                    setActivePath(responsePath);
                }
                if (responseReceived) loop.quit();
            });

        QTimer deadline;
        deadline.setSingleShot(true);
        QObject::connect(&deadline, &QTimer::timeout, &loop, [&] {
            timedOut = true;
            loop.quit();
        });
        deadline.start(static_cast<int>(kInteractionTimeout.count() * 1000));

        QTimer cancellationPoll;
        cancellationPoll.setInterval(20);
        QObject::connect(&cancellationPoll, &QTimer::timeout, &loop, [&] {
            if (cancelled_.load(std::memory_order_acquire)) loop.quit();
        });
        cancellationPoll.start();
        loop.exec();

        deadline.stop();
        cancellationPoll.stop();
        disconnectResponse(responsePath);
        clearActivePath(responsePath);

        if (cancelled_.load(std::memory_order_acquire)) {
            closeRequest(bus_, responsePath);
            return Result<QVariantMap>::failure(Status::failure(
                ErrorCode::cancelled,
                "The " + operation + " request was cancelled."));
        }
        if (timedOut) {
            closeRequest(bus_, responsePath);
            return Result<QVariantMap>::failure(Status::failure(
                ErrorCode::timed_out,
                "The desktop did not answer the " + operation +
                    " request within 60 seconds.",
                "Dismiss any stale portal dialog, restart xdg-desktop-portal, and try again."));
        }
        if (methodFailure.has_value()) {
            closeRequest(bus_, responsePath);
            return Result<QVariantMap>::failure(std::move(*methodFailure));
        }
        if (!responseReceived) {
            closeRequest(bus_, responsePath);
            return Result<QVariantMap>::failure(Status::failure(
                ErrorCode::protocol_error,
                "The desktop portal ended the " + operation +
                    " request without a response."));
        }
        const auto status = portalResponseStatus(
            responseCode,
            operation,
            permissionRemediation);
        if (!status.ok()) return Result<QVariantMap>::failure(status);
        return Result<QVariantMap>::success(std::move(responseResults));
    }

    void cancel() noexcept {
        cancelled_.store(true, std::memory_order_release);
        QString path;
        {
            std::lock_guard<std::mutex> lock(activeMutex_);
            path = activePath_;
        }
        closeRequest(bus_, path);
    }

    void reset() noexcept {
        std::lock_guard<std::mutex> lock(activeMutex_);
        if (activePath_.isEmpty()) {
            cancelled_.store(false, std::memory_order_release);
        }
    }

private:
    void setActivePath(const QString& path) {
        std::lock_guard<std::mutex> lock(activeMutex_);
        activePath_ = path;
    }

    void clearActivePath(const QString& path) {
        std::lock_guard<std::mutex> lock(activeMutex_);
        if (activePath_ == path) activePath_.clear();
    }

    QDBusConnection bus_;
    std::mutex operationMutex_;
    std::mutex activeMutex_;
    QString activePath_;
    std::atomic<bool> cancelled_{false};
};

Result<QVariantMap> callRequest(
    PortalRequestRunner& requests,
    const char* interfaceName,
    const char* method,
    QVariantList arguments,
    int optionsIndex,
    const std::string& operation,
    const std::string& remediation) {
    return requests.request(
        QString::fromLatin1(interfaceName),
        QString::fromLatin1(method),
        std::move(arguments),
        optionsIndex,
        operation,
        remediation);
}

std::optional<QString> resultString(
    const QVariantMap& results,
    const QString& key) {
    const auto found = results.constFind(key);
    if (found == results.cend()) return std::nullopt;
    const auto value = unwrapped(*found);
    if (value.metaType() == QMetaType::fromType<QDBusObjectPath>()) {
        return value.value<QDBusObjectPath>().path();
    }
    if (!value.canConvert<QString>()) return std::nullopt;
    const auto converted = value.toString();
    return converted.isEmpty() ? std::nullopt
                               : std::optional<QString>(converted);
}

std::optional<std::uint32_t> resultUnsigned(
    const QVariantMap& results,
    const QString& key) {
    const auto found = results.constFind(key);
    if (found == results.cend()) return std::nullopt;
    bool converted = false;
    const auto value = unwrapped(*found).toUInt(&converted);
    return converted ? std::optional<std::uint32_t>(value) : std::nullopt;
}

std::uint64_t monotonicMilliseconds() {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch());
    return static_cast<std::uint64_t>(elapsed.count());
}

class QDbusGlobalShortcutsPortal final : public QObject,
                                         public GlobalShortcutsPortal {
    Q_OBJECT

public:
    QDbusGlobalShortcutsPortal()
        : bus_(QDBusConnection::sessionBus()), requests_(bus_) {
        qDBusRegisterMetaType<PortalShortcutDescription>();
        qDBusRegisterMetaType<PortalShortcutDescriptions>();
    }

    ~QDbusGlobalShortcutsPortal() override { close(); }

    Status bind(
        const ShortcutSpec& shortcut,
        ShortcutCallback callback) override {
        {
            std::lock_guard<std::mutex> lock(lifecycleMutex_);
            if (lifecycle_ != Lifecycle::idle) {
                return Status::failure(
                    ErrorCode::busy,
                    "A Wayland global shortcut session is already active or changing state.");
            }
            lifecycle_ = Lifecycle::binding;
        }
        requests_.reset();
        const auto available = requirePortalVersion(
            bus_, QString::fromLatin1(kGlobalShortcutsInterface), 1);
        if (!available.ok()) return failBinding({}, available);

        QVariantMap createOptions;
        createOptions.insert(
            QStringLiteral("session_handle_token"),
            requestToken(QStringLiteral("localflowsession")));
        auto created = callRequest(
            requests_,
            kGlobalShortcutsInterface,
            "CreateSession",
            {createOptions},
            0,
            "global-shortcut session",
            "Allow LocalFlow to register a global shortcut.");
        if (!created) return failBinding({}, created.status());
        const auto session = resultString(
            created.value(), QStringLiteral("session_handle"));
        if (!session.has_value()) {
            return failBinding({}, Status::failure(
                ErrorCode::protocol_error,
                "The GlobalShortcuts portal did not return a session handle."));
        }
        const auto sessionHandle = *session;
        bool acceptSession = false;
        {
            std::lock_guard<std::mutex> lock(lifecycleMutex_);
            if (lifecycle_ == Lifecycle::binding) {
                sessionHandle_ = sessionHandle;
                acceptSession = true;
            } else {
                lifecycle_ = Lifecycle::idle;
            }
        }
        if (!acceptSession) {
            closeSession(bus_, sessionHandle);
            return cancelledDuringBinding();
        }

        if (!connectSignals(sessionHandle)) {
            return failBinding(sessionHandle, Status::failure(
                ErrorCode::service_unavailable,
                "LocalFlow could not subscribe to global shortcut press and release events.",
                "Restart xdg-desktop-portal and try again."));
        }

        // Install the callback before BindShortcuts completes. A very fast
        // user press can otherwise race the method response and be lost.
        {
            std::lock_guard<std::mutex> callbackLock(callbackMutex_);
            eventSessionHandle_ = sessionHandle;
            shortcutId_ = shortcut.id;
            callback_ = std::move(callback);
            pressed_ = false;
        }
        if (!isBinding()) {
            return failBinding(sessionHandle, cancelledDuringBinding());
        }

        PortalShortcutDescription description;
        description.id = QString::fromStdString(shortcut.id);
        description.options.insert(
            QStringLiteral("description"),
            QStringLiteral("Hold to talk in LocalFlow"));
        description.options.insert(
            QStringLiteral("preferred_trigger"),
            QString::fromStdString(portalShortcutTrigger(shortcut)));
        const PortalShortcutDescriptions shortcuts{description};
        QVariantMap bindOptions;
        auto bound = callRequest(
            requests_,
            kGlobalShortcutsInterface,
            "BindShortcuts",
            {
                QVariant::fromValue(QDBusObjectPath(sessionHandle)),
                QVariant::fromValue(shortcuts),
                QString{},
                bindOptions,
            },
            3,
            "global-shortcut binding",
            "Approve LocalFlow's hold-to-talk shortcut when the desktop asks.");
        if (!bound) {
            return failBinding(sessionHandle, bound.status());
        }

        const auto found = bound.value().constFind(QStringLiteral("shortcuts"));
        if (found == bound.value().cend()) {
            return failBinding(sessionHandle, Status::failure(
                ErrorCode::protocol_error,
                "The GlobalShortcuts portal did not report the shortcuts it bound."));
        }
        const auto accepted = qdbus_cast<PortalShortcutDescriptions>(*found);
        const auto id = QString::fromStdString(shortcut.id);
        const auto wasAccepted = std::any_of(
            accepted.cbegin(),
            accepted.cend(),
            [&id](const PortalShortcutDescription& item) { return item.id == id; });
        if (!wasAccepted) {
            return failBinding(sessionHandle, Status::failure(
                ErrorCode::permission_denied,
                "The desktop did not bind LocalFlow's hold-to-talk shortcut.",
                "Choose a keyboard shortcut accepted by your desktop; some compositors reject modifier-only shortcuts."));
        }

        bool completedBinding = false;
        {
            std::lock_guard<std::mutex> lock(lifecycleMutex_);
            if (lifecycle_ == Lifecycle::binding) {
                lifecycle_ = Lifecycle::bound;
                completedBinding = true;
            }
        }
        if (!completedBinding) {
            return failBinding(sessionHandle, cancelledDuringBinding());
        }
        return Status::success();
    }

    void close() noexcept override {
        // A Request runs a nested event loop. Cancel it before taking any
        // lifecycle mutex so close() is safe even when re-entered on the same
        // UI thread from a Quit action.
        requests_.cancel();
        QString session;
        Lifecycle previous = Lifecycle::idle;
        {
            std::lock_guard<std::mutex> lock(lifecycleMutex_);
            previous = lifecycle_;
            if (previous == Lifecycle::idle) return;
            lifecycle_ = Lifecycle::closing;
            session = sessionHandle_;
            sessionHandle_.clear();
        }
        disconnectSignals(session);

        ShortcutCallback callback;
        std::string shortcutId;
        bool synthesizeRelease = false;
        {
            std::lock_guard<std::mutex> callbackLock(callbackMutex_);
            callback = callback_;
            shortcutId = shortcutId_;
            synthesizeRelease = pressed_;
            callback_ = {};
            eventSessionHandle_.clear();
            shortcutId_.clear();
            pressed_ = false;
        }
        if (synthesizeRelease && callback) {
            callback({
                std::move(shortcutId),
                ShortcutEdge::released,
                monotonicMilliseconds(),
            });
        }

        closeSession(bus_, session);
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        if (previous == Lifecycle::bound && lifecycle_ == Lifecycle::closing) {
            lifecycle_ = Lifecycle::idle;
        }
    }

private Q_SLOTS:
    void activated(
        const QDBusObjectPath& session,
        const QString& id,
        qulonglong timestamp,
        const QVariantMap&) {
        dispatch(session, id, timestamp, ShortcutEdge::pressed);
    }

    void deactivated(
        const QDBusObjectPath& session,
        const QString& id,
        qulonglong timestamp,
        const QVariantMap&) {
        dispatch(session, id, timestamp, ShortcutEdge::released);
    }

    void sessionClosed() {
        requests_.cancel();
        ShortcutCallback callback;
        std::string shortcutId;
        QString session;
        {
            std::lock_guard<std::mutex> lock(callbackMutex_);
            const bool wasPressed = pressed_;
            pressed_ = false;
            callback = callback_;
            shortcutId = shortcutId_;
            callback_ = {};
            shortcutId_.clear();
            session = eventSessionHandle_;
            eventSessionHandle_.clear();
            if (!wasPressed) callback = {};
        }
        {
            std::lock_guard<std::mutex> lock(lifecycleMutex_);
            if (sessionHandle_ == session) sessionHandle_.clear();
            lifecycle_ = Lifecycle::idle;
        }
        disconnectSignals(session);
        if (callback) {
            callback({
                std::move(shortcutId),
                ShortcutEdge::released,
                monotonicMilliseconds(),
            });
        }
    }

private:
    enum class Lifecycle {
        idle,
        binding,
        bound,
        closing,
    };

    bool connectSignals(const QString& session) {
        const auto root = QString::fromLatin1(kPortalPath);
        const auto interfaceName = QString::fromLatin1(kGlobalShortcutsInterface);
        const bool activatedConnected = bus_.connect(
            QString::fromLatin1(kPortalService),
            root,
            interfaceName,
            QStringLiteral("Activated"),
            this,
            SLOT(activated(QDBusObjectPath,QString,qulonglong,QVariantMap)));
        const bool deactivatedConnected = bus_.connect(
            QString::fromLatin1(kPortalService),
            root,
            interfaceName,
            QStringLiteral("Deactivated"),
            this,
            SLOT(deactivated(QDBusObjectPath,QString,qulonglong,QVariantMap)));
        const bool closedConnected = bus_.connect(
            QString::fromLatin1(kPortalService),
            session,
            QString::fromLatin1(kSessionInterface),
            QStringLiteral("Closed"),
            this,
            SLOT(sessionClosed()));
        const bool connected =
            activatedConnected && deactivatedConnected && closedConnected;
        if (!connected) disconnectSignals(session);
        return connected;
    }

    void disconnectSignals(const QString& session) noexcept {
        const auto root = QString::fromLatin1(kPortalPath);
        const auto interfaceName = QString::fromLatin1(kGlobalShortcutsInterface);
        (void)bus_.disconnect(
            QString::fromLatin1(kPortalService), root, interfaceName,
            QStringLiteral("Activated"), this,
            SLOT(activated(QDBusObjectPath,QString,qulonglong,QVariantMap)));
        (void)bus_.disconnect(
            QString::fromLatin1(kPortalService), root, interfaceName,
            QStringLiteral("Deactivated"), this,
            SLOT(deactivated(QDBusObjectPath,QString,qulonglong,QVariantMap)));
        if (!session.isEmpty()) {
            (void)bus_.disconnect(
                QString::fromLatin1(kPortalService), session,
                QString::fromLatin1(kSessionInterface), QStringLiteral("Closed"),
                this, SLOT(sessionClosed()));
        }
    }

    void dispatch(
        const QDBusObjectPath& session,
        const QString& id,
        qulonglong timestamp,
        ShortcutEdge edge) {
        ShortcutCallback callback;
        std::string shortcutId;
        {
            std::lock_guard<std::mutex> lock(callbackMutex_);
            if (session.path() != eventSessionHandle_ ||
                id.toStdString() != shortcutId_) {
                return;
            }
            if ((edge == ShortcutEdge::pressed && pressed_) ||
                (edge == ShortcutEdge::released && !pressed_)) {
                return;
            }
            pressed_ = edge == ShortcutEdge::pressed;
            callback = callback_;
            shortcutId = shortcutId_;
        }
        if (callback) {
            callback({std::move(shortcutId), edge, timestamp});
        }
    }

    void clearCallbackState() noexcept {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        callback_ = {};
        eventSessionHandle_.clear();
        shortcutId_.clear();
        pressed_ = false;
    }

    bool isBinding() noexcept {
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        return lifecycle_ == Lifecycle::binding;
    }

    Status failBinding(QString session, Status status) {
        disconnectSignals(session);
        clearCallbackState();
        closeSession(bus_, session);
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        if (sessionHandle_ == session) sessionHandle_.clear();
        lifecycle_ = Lifecycle::idle;
        return status;
    }

    static Status cancelledDuringBinding() {
        return Status::failure(
            ErrorCode::cancelled,
            "Global shortcut setup was cancelled while waiting for desktop consent.");
    }

    QDBusConnection bus_;
    PortalRequestRunner requests_;
    std::mutex lifecycleMutex_;
    std::mutex callbackMutex_;
    QString sessionHandle_;
    QString eventSessionHandle_;
    std::string shortcutId_;
    ShortcutCallback callback_;
    bool pressed_{false};
    Lifecycle lifecycle_{Lifecycle::idle};
};

class QDbusScreenshotPortal final : public ScreenshotPortal {
public:
    QDbusScreenshotPortal()
        : bus_(QDBusConnection::sessionBus()), requests_(bus_) {}

    ~QDbusScreenshotPortal() override { close(); }

    Result<ScreenFrame> captureFrame() override {
        if (closed_.load(std::memory_order_acquire)) {
            return Result<ScreenFrame>::failure(Status::failure(
                ErrorCode::cancelled,
                "Screenshot capture was stopped."));
        }
        std::uint32_t version = 0;
        {
            std::lock_guard<std::mutex> lock(metadataMutex_);
            if (!checked_) {
                const auto available = requirePortalVersion(
                    bus_, QString::fromLatin1(kScreenshotInterface), 1, &version_);
                if (!available.ok()) {
                    return Result<ScreenFrame>::failure(available);
                }
                checked_ = true;
            }
            version = version_;
        }

        QVariantMap options;
        options.insert(QStringLiteral("interactive"), false);
        options.insert(QStringLiteral("modal"), false);
        if (version >= 3) {
            const auto availableTargets = portalProperty(
                bus_,
                QString::fromLatin1(kScreenshotInterface),
                QStringLiteral("AvailableTargets"));
            if (availableTargets) {
                bool converted = false;
                const auto targets = availableTargets.value().toUInt(&converted);
                if (converted && (targets & kScreenshotTargetScreen) != 0) {
                    options.insert(
                        QStringLiteral("target"), kScreenshotTargetScreen);
                }
            }
        }
        auto captured = callRequest(
            requests_,
            kScreenshotInterface,
            "Screenshot",
            {QString{}, options},
            1,
            "screenshot",
            "Allow LocalFlow to capture the screen for terminology context.");
        if (!captured) return Result<ScreenFrame>::failure(captured.status());
        const auto uri = resultString(captured.value(), QStringLiteral("uri"));
        if (!uri.has_value()) {
            return Result<ScreenFrame>::failure(Status::failure(
                ErrorCode::protocol_error,
                "The Screenshot portal did not return an image URI."));
        }
        const QUrl location(*uri);
        if (!location.isValid() || !location.isLocalFile()) {
            return Result<ScreenFrame>::failure(Status::failure(
                ErrorCode::protocol_error,
                "The Screenshot portal returned an unsupported image URI."));
        }

        QImageReader reader(location.toLocalFile());
        reader.setAutoTransform(true);
        const auto advertisedSize = reader.size();
        constexpr qint64 kMaximumPixels = 128LL * 1024LL * 1024LL;
        if (advertisedSize.isValid() &&
            (advertisedSize.width() > 32768 || advertisedSize.height() > 32768 ||
             static_cast<qint64>(advertisedSize.width()) *
                     advertisedSize.height() >
                 kMaximumPixels)) {
            return Result<ScreenFrame>::failure(Status::failure(
                ErrorCode::protocol_error,
                "The portal screenshot is too large to process safely."));
        }
        auto image = reader.read();
        if (image.isNull()) {
            return Result<ScreenFrame>::failure(Status::failure(
                ErrorCode::io_error,
                "LocalFlow could not decode the portal screenshot" +
                    (reader.errorString().isEmpty()
                         ? std::string{"."}
                         : ": " + text(reader.errorString()))));
        }
        image = image.convertToFormat(QImage::Format_RGBA8888);
        if (image.width() <= 0 || image.height() <= 0 ||
            image.bytesPerLine() <= 0 || image.sizeInBytes() <= 0 ||
            image.sizeInBytes() > 512LL * 1024LL * 1024LL) {
            return Result<ScreenFrame>::failure(Status::failure(
                ErrorCode::protocol_error,
                "The portal screenshot has invalid dimensions."));
        }

        ScreenFrame frame;
        frame.width = image.width();
        frame.height = image.height();
        frame.bytesPerRow = image.bytesPerLine();
        frame.pixelFormat = PixelFormat::rgba8;
        const auto byteCount = static_cast<std::size_t>(image.sizeInBytes());
        frame.pixels.assign(image.constBits(), image.constBits() + byteCount);
        return Result<ScreenFrame>::success(std::move(frame));
    }

    void close() noexcept override {
        closed_.store(true, std::memory_order_release);
        requests_.cancel();
    }

private:
    QDBusConnection bus_;
    PortalRequestRunner requests_;
    std::mutex metadataMutex_;
    std::uint32_t version_{0};
    bool checked_{false};
    std::atomic<bool> closed_{false};
};

class QDbusRemoteDesktopPortal final : public QObject,
                                       public RemoteDesktopPortal {
    Q_OBJECT

public:
    QDbusRemoteDesktopPortal()
        : bus_(QDBusConnection::sessionBus()), requests_(bus_) {}

    ~QDbusRemoteDesktopPortal() override { close(); }

    Status ensureKeyboardSession() override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (state_ == State::active && !sessionHandle_.isEmpty()) {
                return Status::success();
            }
            if (state_ == State::closed) {
                return Status::failure(
                    ErrorCode::cancelled,
                    "The Wayland keyboard-control session was stopped.");
            }
            if (state_ == State::starting) {
                return Status::failure(
                    ErrorCode::busy,
                    "A Wayland keyboard-control request is already in progress.");
            }
            state_ = State::starting;
        }
        requests_.reset();
        const auto available = requirePortalVersion(
            bus_, QString::fromLatin1(kRemoteDesktopInterface), 1);
        if (!available.ok()) return failSetup({}, available);

        const auto deviceTypes = portalProperty(
            bus_,
            QString::fromLatin1(kRemoteDesktopInterface),
            QStringLiteral("AvailableDeviceTypes"));
        if (!deviceTypes) return failSetup({}, deviceTypes.status());
        bool converted = false;
        const auto types = deviceTypes.value().toUInt(&converted);
        if (!converted || (types & kKeyboardDevice) == 0) {
            return failSetup({}, Status::failure(
                ErrorCode::unsupported_session,
                "This desktop's RemoteDesktop portal does not offer keyboard control.",
                "Use AT-SPI insertion, or switch to a desktop portal backend that supports keyboard control."));
        }

        QVariantMap createOptions;
        createOptions.insert(
            QStringLiteral("session_handle_token"),
            requestToken(QStringLiteral("localflowremote")));
        auto created = callRequest(
            requests_,
            kRemoteDesktopInterface,
            "CreateSession",
            {createOptions},
            0,
            "keyboard-control session",
            "Allow LocalFlow to control the keyboard for paste fallback.");
        if (!created) return failSetup({}, created.status());
        const auto session = resultString(
            created.value(), QStringLiteral("session_handle"));
        if (!session.has_value()) {
            return failSetup({}, Status::failure(
                ErrorCode::protocol_error,
                "The RemoteDesktop portal did not return a session handle."));
        }
        const auto sessionHandle = *session;

        QVariantMap selectOptions;
        selectOptions.insert(QStringLiteral("types"), kKeyboardDevice);
        auto selected = callRequest(
            requests_,
            kRemoteDesktopInterface,
            "SelectDevices",
            {
                QVariant::fromValue(QDBusObjectPath(sessionHandle)),
                selectOptions,
            },
            1,
            "keyboard selection",
            "Allow keyboard control for LocalFlow's paste fallback.");
        if (!selected) {
            return failSetup(sessionHandle, selected.status());
        }

        QVariantMap startOptions;
        auto started = callRequest(
            requests_,
            kRemoteDesktopInterface,
            "Start",
            {
                QVariant::fromValue(QDBusObjectPath(sessionHandle)),
                QString{},
                startOptions,
            },
            2,
            "keyboard control",
            "Approve keyboard control for LocalFlow. AT-SPI remains the preferred insertion method.");
        if (!started) {
            return failSetup(sessionHandle, started.status());
        }
        const auto devices = resultUnsigned(
            started.value(), QStringLiteral("devices"));
        if (!devices.has_value() || (*devices & kKeyboardDevice) == 0) {
            return failSetup(sessionHandle, Status::failure(
                ErrorCode::permission_denied,
                "The desktop started remote control without granting keyboard access.",
                "Allow keyboard control, or use an application that supports AT-SPI text insertion."));
        }
        if (!bus_.connect(
                QString::fromLatin1(kPortalService),
                sessionHandle,
                QString::fromLatin1(kSessionInterface),
                QStringLiteral("Closed"),
                this,
                SLOT(sessionClosed()))) {
            return failSetup(sessionHandle, Status::failure(
                ErrorCode::service_unavailable,
                "LocalFlow could not monitor the Wayland keyboard-control session.",
                "Restart xdg-desktop-portal and try again."));
        }
        bool accepted = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (state_ == State::starting) {
                sessionHandle_ = sessionHandle;
                state_ = State::active;
                accepted = true;
            }
        }
        if (!accepted) {
            return failSetup(sessionHandle, Status::failure(
                ErrorCode::cancelled,
                "Keyboard-control setup was cancelled while waiting for desktop consent."));
        }
        return Status::success();
    }

    Status sendKeysym(std::uint32_t keysym, bool pressed) override {
        QString session;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (state_ != State::active || sessionHandle_.isEmpty()) {
                return Status::failure(
                    ErrorCode::not_configured,
                    "No approved Wayland keyboard-control session is active.",
                    "Approve keyboard control before using paste fallback.");
            }
            session = sessionHandle_;
        }
        if (keysym > static_cast<std::uint32_t>(std::numeric_limits<qint32>::max())) {
            return Status::failure(
                ErrorCode::invalid_argument,
                "The requested keyboard symbol is outside the portal's supported range.");
        }

        auto message = QDBusMessage::createMethodCall(
            QString::fromLatin1(kPortalService),
            QString::fromLatin1(kPortalPath),
            QString::fromLatin1(kRemoteDesktopInterface),
            QStringLiteral("NotifyKeyboardKeysym"));
        message << QVariant::fromValue(QDBusObjectPath(session))
                << QVariantMap{}
                << static_cast<qint32>(keysym)
                << static_cast<std::uint32_t>(pressed ? 1U : 0U);
        const auto reply = bus_.call(
            message,
            QDBus::Block,
            static_cast<int>(kMethodTimeout.count() * 1000));
        if (reply.type() == QDBusMessage::ErrorMessage) {
            bool clearSession = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (sessionHandle_ == session) {
                    sessionHandle_.clear();
                    if (state_ != State::closed) state_ = State::idle;
                    clearSession = true;
                }
            }
            if (clearSession) disconnectSessionSignal(session);
            return dbusFailure(
                QDBusError(reply),
                pressed ? "press a paste key" : "release a paste key");
        }
        return Status::success();
    }

    void close() noexcept override {
        requests_.cancel();
        QString session;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (state_ == State::closed && sessionHandle_.isEmpty()) return;
            state_ = State::closed;
            session = sessionHandle_;
            sessionHandle_.clear();
        }
        disconnectSessionSignal(session);
        closeSession(bus_, session);
    }

private Q_SLOTS:
    void sessionClosed() {
        QString session;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            session = sessionHandle_;
            sessionHandle_.clear();
            if (state_ != State::closed) state_ = State::idle;
        }
        disconnectSessionSignal(session);
    }

private:
    enum class State {
        idle,
        starting,
        active,
        closed,
    };

    Status failSetup(QString session, Status status) {
        disconnectSessionSignal(session);
        closeSession(bus_, session);
        std::lock_guard<std::mutex> lock(mutex_);
        if (sessionHandle_ == session) sessionHandle_.clear();
        if (state_ != State::closed) state_ = State::idle;
        return status;
    }

    void disconnectSessionSignal(const QString& session) noexcept {
        if (session.isEmpty()) return;
        (void)bus_.disconnect(
            QString::fromLatin1(kPortalService),
            session,
            QString::fromLatin1(kSessionInterface),
            QStringLiteral("Closed"),
            this,
            SLOT(sessionClosed()));
    }

    QDBusConnection bus_;
    PortalRequestRunner requests_;
    std::mutex mutex_;
    QString sessionHandle_;
    State state_{State::idle};
};

}  // namespace

std::shared_ptr<GlobalShortcutsPortal> makeQDbusGlobalShortcutsPortal() {
    return std::make_shared<QDbusGlobalShortcutsPortal>();
}

std::shared_ptr<ScreenshotPortal> makeQDbusScreenshotPortal() {
    return std::make_shared<QDbusScreenshotPortal>();
}

std::shared_ptr<RemoteDesktopPortal> makeQDbusRemoteDesktopPortal() {
    return std::make_shared<QDbusRemoteDesktopPortal>();
}

}  // namespace localflow::platform::linux::detail

#include "QDbusPortals.moc"

#else

namespace localflow::platform::linux::detail {

std::shared_ptr<GlobalShortcutsPortal> makeQDbusGlobalShortcutsPortal() {
    return {};
}

std::shared_ptr<ScreenshotPortal> makeQDbusScreenshotPortal() {
    return {};
}

std::shared_ptr<RemoteDesktopPortal> makeQDbusRemoteDesktopPortal() {
    return {};
}

}  // namespace localflow::platform::linux::detail

#endif
