#pragma once

#ifdef _WIN32

#include "localflow/windows/PttStateMachine.hpp"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <system_error>
#include <thread>
#include <vector>

namespace localflow::windows {

enum class MouseButton : std::uint32_t {
    left = 1,
    right = 2,
    middle = 3,
    x1 = 4,
    x2 = 5,
};

[[nodiscard]] constexpr Trigger keyboard_trigger(const std::uint32_t virtual_key) noexcept {
    return {TriggerDevice::keyboard, virtual_key};
}

[[nodiscard]] constexpr Trigger mouse_trigger(const MouseButton button) noexcept {
    return {TriggerDevice::mouse, static_cast<std::uint32_t>(button)};
}

enum class PttEventKind : std::uint8_t {
    pressed,
    released,
    cancelled,
};

enum class CancelReason : std::uint8_t {
    none,
    escape_key,
    monitor_stopped,
};

struct PttEvent {
    PttEventKind kind{PttEventKind::cancelled};
    Trigger trigger{};
    CancelReason cancel_reason{CancelReason::none};
    std::chrono::steady_clock::time_point timestamp{};
};

struct InputMonitorOptions {
    /// Windows does not expose most laptop Fn keys. F8 is a conservative
    /// keyboard default; production UI should make this user-configurable.
    std::vector<Trigger> triggers{keyboard_trigger(VK_F8)};
    std::uint32_t cancel_virtual_key{VK_ESCAPE};
    bool ignore_injected_events{true};
    /// Prevent a configured physical mouse trigger from also reaching the
    /// foreground application (for example, browser Back for XBUTTON1).
    /// Keyboard shortcuts and unrelated mouse buttons are always observed
    /// without being consumed.
    bool consume_mouse_trigger_events{true};
};

namespace detail {

struct MouseInputDecision {
    Trigger trigger{};
    bool configured{false};
    bool pressed{false};
    bool consume{false};
};

struct KeyboardCancelDecision {
    bool matched{false};
    bool request_cancel{false};
    bool consume{false};
};

/// Tracks one physical cancellation gesture so Escape is intercepted only
/// while it is cancelling an active PTT hold. The corresponding repeat/down
/// and release events are consumed with that gesture; idle Escape remains a
/// normal foreground-app key.
class CancelKeyInputState final {
public:
    [[nodiscard]] KeyboardCancelDecision classify(
        const InputMonitorOptions& options,
        WPARAM message,
        const KBDLLHOOKSTRUCT& event,
        bool ptt_active) noexcept;

    void reset() noexcept { awaiting_release_ = false; }

private:
    bool awaiting_release_{false};
};

/// Pure hook policy kept visible for native adapter tests. Injected events are
/// ignored when requested, and only configured down/up messages may be
/// consumed.
[[nodiscard]] MouseInputDecision classify_mouse_input(
    const InputMonitorOptions& options,
    WPARAM message,
    const MSLLHOOKSTRUCT& event) noexcept;

}  // namespace detail

/// WH_KEYBOARD_LL/WH_MOUSE_LL based global hold-to-talk monitor.
///
/// Hooks live on a dedicated message-loop thread. User callbacks are delivered
/// on a second thread, ensuring model/microphone startup can never stall the
/// hook long enough for Windows to silently remove it. Configured keyboard PTT
/// input is only observed; an Escape gesture that cancels an active hold is
/// consumed. A configured physical mouse trigger is consumed by default so it
/// cannot also activate a foreground-app action.
class GlobalInputMonitor final {
public:
    using Callback = std::function<void(const PttEvent&)>;

    explicit GlobalInputMonitor(InputMonitorOptions options, Callback callback);
    ~GlobalInputMonitor();

    GlobalInputMonitor(const GlobalInputMonitor&) = delete;
    GlobalInputMonitor& operator=(const GlobalInputMonitor&) = delete;

    [[nodiscard]] std::error_code start();
    void stop() noexcept;

    [[nodiscard]] bool is_running() const noexcept { return running_.load(); }

private:
    struct DispatchState;

    static LRESULT CALLBACK keyboard_hook(int code, WPARAM message, LPARAM data) noexcept;
    static LRESULT CALLBACK mouse_hook(int code, WPARAM message, LPARAM data) noexcept;

    void hook_main(std::promise<std::error_code> ready);
    [[nodiscard]] bool process_keyboard(
        WPARAM message, const KBDLLHOOKSTRUCT& event) noexcept;
    [[nodiscard]] bool process_mouse(
        WPARAM message, const MSLLHOOKSTRUCT& event) noexcept;
    void process_down(Trigger trigger) noexcept;
    void process_up(Trigger trigger) noexcept;
    void process_cancel(CancelReason reason) noexcept;
    void enqueue(PttEvent event) noexcept;
    [[nodiscard]] bool configured(Trigger trigger) const noexcept;

    InputMonitorOptions options_;
    std::shared_ptr<DispatchState> dispatch_;
    std::thread hook_thread_;
    std::thread dispatch_thread_;
    std::atomic<bool> running_{false};
    std::atomic<DWORD> hook_thread_id_{0};
    std::mutex lifecycle_mutex_;
    PttStateMachine state_;
    detail::CancelKeyInputState cancel_key_input_;
    HHOOK keyboard_hook_{nullptr};
    HHOOK mouse_hook_{nullptr};
};

}  // namespace localflow::windows

#endif
