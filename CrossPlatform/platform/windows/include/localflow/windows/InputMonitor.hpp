#pragma once

#ifdef _WIN32

#include "localflow/windows/PttStateMachine.hpp"

#include <Windows.h>

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
};

/// WH_KEYBOARD_LL/WH_MOUSE_LL based global hold-to-talk monitor.
///
/// Hooks live on a dedicated message-loop thread. User callbacks are delivered
/// on a second thread, ensuring model/microphone startup can never stall the
/// hook long enough for Windows to silently remove it. This class observes but
/// never consumes input.
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
    void process_keyboard(WPARAM message, const KBDLLHOOKSTRUCT& event) noexcept;
    void process_mouse(WPARAM message, const MSLLHOOKSTRUCT& event) noexcept;
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
    HHOOK keyboard_hook_{nullptr};
    HHOOK mouse_hook_{nullptr};
};

}  // namespace localflow::windows

#endif
