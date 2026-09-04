#include "InternalFactories.hpp"
#include "ShortcutState.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#if defined(LOCALFLOW_LINUX_WITH_X11)
#include <X11/Xlib.h>
#include <X11/XKBlib.h>
#include <X11/keysym.h>
#undef Status
#include "X11ShortcutPolicy.hpp"
#endif

namespace localflow::platform::linux::detail {

#if defined(LOCALFLOW_LINUX_WITH_X11)
bool isLegacyAutoRepeatPair(
    const bool triggerHeld,
    const KeyCode configuredKeycode,
    const XEvent& release,
    const XEvent& next) noexcept {
    return triggerHeld && release.type == KeyRelease &&
        release.xkey.keycode == configuredKeycode && next.type == KeyPress &&
        next.xkey.keycode == release.xkey.keycode &&
        next.xkey.time == release.xkey.time &&
        next.xkey.window == release.xkey.window;
}
#endif

namespace {

#if defined(LOCALFLOW_LINUX_WITH_X11)

unsigned int modifierMask(const std::vector<Modifier>& modifiers) {
    unsigned int mask = 0;
    for (const auto modifier : modifiers) {
        switch (modifier) {
            case Modifier::shift: mask |= ShiftMask; break;
            case Modifier::control: mask |= ControlMask; break;
            case Modifier::alt: mask |= Mod1Mask; break;
            case Modifier::super: mask |= Mod4Mask; break;
        }
    }
    return mask;
}

std::uint64_t nowMs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Xlib reports passive-grab conflicts asynchronously through a process-global
// error handler. Without a scoped trap, an ordinary BadAccess (another app
// already owns the shortcut) reaches Xlib's default handler and terminates the
// process. Serialize our short request/sync windows and restore the host's
// handler immediately afterwards.
std::mutex xErrorTrapMutex;
thread_local Display* trappedDisplay = nullptr;
thread_local int trappedError = Success;
std::atomic<XErrorHandler> fallbackXErrorHandler{nullptr};

int captureXError(Display* display, XErrorEvent* event) noexcept {
    if (display == trappedDisplay) {
        if (trappedError == Success) {
            trappedError = event->error_code;
        }
        return 0;
    }
    const auto fallback = fallbackXErrorHandler.load();
    return fallback != nullptr ? fallback(display, event) : 0;
}

template <typename Operation>
int withXErrorTrap(Display* display, Operation&& operation) noexcept {
    std::lock_guard<std::mutex> lock(xErrorTrapMutex);
    trappedDisplay = display;
    trappedError = Success;
    const auto previous = XSetErrorHandler(&captureXError);
    fallbackXErrorHandler.store(previous);
    operation();
    XSync(display, False);
    XSetErrorHandler(previous);
    fallbackXErrorHandler.store(nullptr);
    trappedDisplay = nullptr;
    return std::exchange(trappedError, Success);
}

// PlatformBridge intentionally uses one native backend for the keyboard and a
// second for an optional mouse button. Only one of those X clients may own the
// temporary AnyModifier Escape grab. Coordinating it in-process avoids a
// guaranteed BadAccess when both physical triggers overlap.
std::mutex escapeOwnerMutex;
const void* escapeOwner = nullptr;

class X11ShortcutBackend final : public GlobalShortcutBackend {
public:
    ~X11ShortcutBackend() override { stop(); }

    std::string name() const override { return "XGrabKey/XGrabButton"; }

    Status start(const ShortcutSpec& shortcut, ShortcutCallback callback) override {
        if (!callback || shortcut.id.empty()) {
            return Status::failure(
                ErrorCode::invalid_argument,
                "A shortcut id and callback are required.");
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (running_) {
            return Status::failure(ErrorCode::busy, "The X11 shortcut listener is already running.");
        }

        (void)XInitThreads();
        display_ = XOpenDisplay(nullptr);
        if (display_ == nullptr) {
            return Status::failure(
                ErrorCode::service_unavailable,
                "LocalFlow could not connect to the X11 display.",
                "Check DISPLAY and ensure LocalFlow is running in the graphical user session.");
        }

        root_ = DefaultRootWindow(display_);
        modifierMask_ = ::localflow::platform::linux::detail::modifierMask(shortcut.modifiers);
        shortcut_ = shortcut;
        callback_ = std::move(callback);
        escapeKeycode_ = XKeysymToKeycode(display_, XK_Escape);

        if (shortcut.kind == ShortcutKind::key) {
            const auto keysym = XStringToKeysym(shortcut.trigger.c_str());
            if (keysym == NoSymbol) {
                cleanupDisplay();
                return Status::failure(
                    ErrorCode::invalid_argument,
                    "X11 does not recognize the key name '" + shortcut.trigger + "'.",
                    "Use an X11 keysym such as F8, space, or Control_R.");
            }
            keycode_ = XKeysymToKeycode(display_, keysym);
            if (keycode_ == 0) {
                cleanupDisplay();
                return Status::failure(
                    ErrorCode::invalid_argument,
                    "The selected key is not present in the current X11 keyboard map.");
            }
        } else if (shortcut.mouseButton == 0) {
            cleanupDisplay();
            return Status::failure(
                ErrorCode::invalid_argument,
                "An X11 mouse button number is required.");
        }

        // Grabbing lock-modifier variants avoids a shortcut that mysteriously
        // stops working while Caps Lock or Num Lock is active.
        constexpr unsigned int lockVariants[] = {
            0,
            LockMask,
            Mod2Mask,
            LockMask | Mod2Mask,
        };
        const int grabError = withXErrorTrap(display_, [&] {
            for (const auto locks : lockVariants) {
                if (shortcut.kind == ShortcutKind::key) {
                    XGrabKey(
                        display_, keycode_, modifierMask_ | locks, root_, False,
                        GrabModeAsync, GrabModeAsync);
                } else {
                    XGrabButton(
                        display_, shortcut.mouseButton, modifierMask_ | locks, root_, False,
                        ButtonPressMask | ButtonReleaseMask,
                        GrabModeAsync, GrabModeAsync, None, None);
                }
            }
        });
        if (grabError != Success) {
            // Some lock variants may have succeeded before X11 reported the
            // conflicting one, so roll the entire set back on this connection.
            (void)withXErrorTrap(display_, [&] {
                for (const auto locks : lockVariants) {
                    if (shortcut.kind == ShortcutKind::key) {
                        XUngrabKey(display_, keycode_, modifierMask_ | locks, root_);
                    } else {
                        XUngrabButton(
                            display_, shortcut.mouseButton,
                            modifierMask_ | locks, root_);
                    }
                }
            });
            char description[256]{};
            XGetErrorText(display_, grabError, description, int(sizeof(description)));
            const std::string detail = description[0] == '\0'
                ? std::string("X11 error ") + std::to_string(grabError)
                : std::string(description);
            cleanupDisplay();
            return Status::failure(
                grabError == BadAccess ? ErrorCode::busy : ErrorCode::io_error,
                "LocalFlow could not reserve the selected X11 shortcut (" +
                    detail + ").",
                grabError == BadAccess
                    ? "Choose a different shortcut or close the application already using it."
                    : "Try restarting LocalFlow in the current graphical session.");
        }

        Bool detectable = False;
        (void)XkbSetDetectableAutoRepeat(display_, True, &detectable);
        running_ = true;
        eventThread_ = std::thread([this] { eventLoop(); });
        return Status::success();
    }

    void stop() noexcept override {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!display_) {
            return;
        }
        running_ = false;
        lock.unlock();

        if (eventThread_.joinable()) {
            eventThread_.join();
        }

        lock.lock();
        setEscapeGrabbed(false);
        constexpr unsigned int lockVariants[] = {
            0,
            LockMask,
            Mod2Mask,
            LockMask | Mod2Mask,
        };
        (void)withXErrorTrap(display_, [&] {
            for (const auto locks : lockVariants) {
                if (shortcut_.kind == ShortcutKind::key) {
                    XUngrabKey(display_, keycode_, modifierMask_ | locks, root_);
                } else {
                    XUngrabButton(
                        display_, shortcut_.mouseButton,
                        modifierMask_ | locks, root_);
                }
            }
        });
        cleanupDisplay();
    }

private:
    bool discardAutoRepeatPair(
        const XEvent& release, const PushToTalkShortcutState& state) noexcept {
        if (!state.held() || shortcut_.kind != ShortcutKind::key ||
            release.type != KeyRelease || release.xkey.keycode != keycode_ ||
            XEventsQueued(display_, QueuedAfterReading) <= 0) {
            return false;
        }

        // Legacy X11 autorepeat is encoded as a synthetic KeyRelease followed
        // immediately by a KeyPress with the same keycode and timestamp. Peek
        // and consume that exact pair before it reaches the hold state. The
        // narrow identity check is also a safety net for servers that
        // misreport XKB support: a deliberate release/repress cannot normally
        // share a server timestamp and event window.
        XEvent next{};
        XPeekEvent(display_, &next);
        if (!isLegacyAutoRepeatPair(state.held(), keycode_, release, next)) {
            return false;
        }
        XNextEvent(display_, &next);
        return true;
    }

    bool setEscapeGrabbed(bool grab) noexcept {
        if (escapeKeycode_ == 0 ||
            (shortcut_.kind == ShortcutKind::key && escapeKeycode_ == keycode_) ||
            escapeGrabbed_ == grab) {
            return escapeGrabbed_ == grab;
        }
        std::lock_guard<std::mutex> ownerLock(escapeOwnerMutex);
        if (grab) {
            if (escapeOwner != nullptr && escapeOwner != this) {
                return false;
            }
            // Escape is intercepted only for the duration of an active hold.
            // AnyModifier also catches it while the PTT chord's modifiers are
            // still physically down.
            const int error = withXErrorTrap(display_, [&] {
                XGrabKey(
                    display_, escapeKeycode_, AnyModifier, root_, False,
                    GrabModeAsync, GrabModeAsync);
            });
            if (error != Success) {
                return false;
            }
            escapeOwner = this;
            escapeGrabbed_ = true;
        } else {
            if (escapeOwner == this) {
                (void)withXErrorTrap(display_, [&] {
                    XUngrabKey(display_, escapeKeycode_, AnyModifier, root_);
                });
                escapeOwner = nullptr;
            }
            escapeGrabbed_ = false;
        }
        return true;
    }

    void eventLoop() noexcept {
        PushToTalkShortcutState state;
        while (running_) {
            while (running_ && XPending(display_) > 0) {
                XEvent event{};
                XNextEvent(display_, &event);
                if (discardAutoRepeatPair(event, state)) continue;

                std::optional<ShortcutEdge> edge;
                if (shortcut_.kind == ShortcutKind::key &&
                    (event.type == KeyPress || event.type == KeyRelease) &&
                    event.xkey.keycode == keycode_) {
                    edge = state.handle(
                        event.type == KeyPress
                            ? ShortcutInput::trigger_pressed
                            : ShortcutInput::trigger_released);
                } else if (shortcut_.kind == ShortcutKind::mouse_button &&
                           (event.type == ButtonPress || event.type == ButtonRelease) &&
                           event.xbutton.button == shortcut_.mouseButton) {
                    edge = state.handle(
                        event.type == ButtonPress
                            ? ShortcutInput::trigger_pressed
                            : ShortcutInput::trigger_released);
                } else if (event.type == KeyPress &&
                           event.xkey.keycode == escapeKeycode_) {
                    edge = state.handle(ShortcutInput::escape_pressed);
                }

                if (!edge) continue;

                setEscapeGrabbed(*edge == ShortcutEdge::pressed);
                try {
                    callback_({shortcut_.id, *edge, nowMs()});
                } catch (...) {
                    // A UI callback must never tear down the native event loop.
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }

        // A listener shutdown while held must not leave dictation recording.
        setEscapeGrabbed(false);
        if (state.held()) {
            try {
                callback_({shortcut_.id, ShortcutEdge::released, nowMs()});
            } catch (...) {
            }
        }
    }

    void cleanupDisplay() noexcept {
        if (display_ != nullptr) {
            XCloseDisplay(display_);
            display_ = nullptr;
        }
        callback_ = {};
        keycode_ = 0;
        escapeKeycode_ = 0;
        modifierMask_ = 0;
        root_ = 0;
        escapeGrabbed_ = false;
    }

    std::mutex mutex_;
    std::atomic<bool> running_{false};
    Display* display_{nullptr};
    Window root_{0};
    KeyCode keycode_{0};
    KeyCode escapeKeycode_{0};
    unsigned int modifierMask_{0};
    bool escapeGrabbed_{false};
    ShortcutSpec shortcut_;
    ShortcutCallback callback_;
    std::thread eventThread_;
};

#else

class X11ShortcutBackend final : public GlobalShortcutBackend {
public:
    std::string name() const override { return "X11 (not built)"; }
    Status start(const ShortcutSpec&, ShortcutCallback) override {
        return Status::failure(
            ErrorCode::missing_dependency,
            "This LocalFlow build does not contain the X11 shortcut adapter.",
            "Rebuild with the X11 development package installed.");
    }
    void stop() noexcept override {}
};

#endif

}  // namespace

std::unique_ptr<GlobalShortcutBackend> makeX11ShortcutBackend() {
    return std::make_unique<X11ShortcutBackend>();
}

}  // namespace localflow::platform::linux::detail
