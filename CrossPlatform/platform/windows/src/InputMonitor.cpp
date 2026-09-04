#include "localflow/windows/InputMonitor.hpp"

#ifdef _WIN32

#include "localflow/windows/WinError.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <future>
#include <utility>

namespace localflow::windows {
namespace {

thread_local GlobalInputMonitor* g_monitor = nullptr;

std::optional<MouseButton> button_for_message(const WPARAM message, const MSLLHOOKSTRUCT& event) {
    switch (message) {
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
            return MouseButton::left;
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
            return MouseButton::right;
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
            return MouseButton::middle;
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
            return HIWORD(event.mouseData) == XBUTTON1 ? MouseButton::x1 : MouseButton::x2;
        default:
            return std::nullopt;
    }
}

bool is_mouse_down(const WPARAM message) noexcept {
    return message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN
        || message == WM_MBUTTONDOWN || message == WM_XBUTTONDOWN;
}

}  // namespace

detail::MouseInputDecision detail::classify_mouse_input(
    const InputMonitorOptions& options,
    const WPARAM message,
    const MSLLHOOKSTRUCT& event) noexcept {
    MouseInputDecision decision;
    constexpr ULONG_PTR injectedFlags =
        LLMHF_INJECTED | LLMHF_LOWER_IL_INJECTED;
    if (options.ignore_injected_events && (event.flags & injectedFlags) != 0) {
        return decision;
    }
    const auto button = button_for_message(message, event);
    if (!button.has_value()) {
        return decision;
    }
    const bool down = is_mouse_down(message);
    const bool up = message == WM_LBUTTONUP || message == WM_RBUTTONUP
        || message == WM_MBUTTONUP || message == WM_XBUTTONUP;
    if (!down && !up) {
        return decision;
    }

    decision.trigger = mouse_trigger(*button);
    decision.configured = std::find(
        options.triggers.begin(), options.triggers.end(), decision.trigger)
        != options.triggers.end();
    decision.pressed = down;
    decision.consume = decision.configured && options.consume_mouse_trigger_events;
    return decision;
}

detail::KeyboardCancelDecision detail::CancelKeyInputState::classify(
    const InputMonitorOptions& options,
    const WPARAM message,
    const KBDLLHOOKSTRUCT& event,
    const bool ptt_active) noexcept {
    KeyboardCancelDecision decision;
    constexpr ULONG_PTR injectedFlags =
        LLKHF_INJECTED | LLKHF_LOWER_IL_INJECTED;
    if (options.ignore_injected_events && (event.flags & injectedFlags) != 0) {
        return decision;
    }

    const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    const bool up = message == WM_KEYUP || message == WM_SYSKEYUP;
    if ((!down && !up) || event.vkCode != options.cancel_virtual_key) {
        return decision;
    }

    decision.matched = true;
    if (down && (ptt_active || awaiting_release_)) {
        decision.request_cancel = ptt_active;
        decision.consume = true;
        awaiting_release_ = true;
    } else if (up && awaiting_release_) {
        decision.consume = true;
        awaiting_release_ = false;
    }
    return decision;
}

struct GlobalInputMonitor::DispatchState {
    explicit DispatchState(Callback value) : callback(std::move(value)) {}

    Callback callback;
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<PttEvent> pending;
    bool stopping{false};
};

GlobalInputMonitor::GlobalInputMonitor(InputMonitorOptions options, Callback callback)
    : options_(std::move(options)), dispatch_(std::make_shared<DispatchState>(std::move(callback))) {}

GlobalInputMonitor::~GlobalInputMonitor() {
    stop();
}

std::error_code GlobalInputMonitor::start() {
    std::lock_guard lock(lifecycle_mutex_);
    if (running_.load()) {
        return {};
    }
    if (options_.triggers.empty()) {
        return win32_error(ERROR_INVALID_PARAMETER);
    }

    state_.reset();
    cancel_key_input_.reset();
    {
        std::lock_guard dispatch_lock(dispatch_->mutex);
        dispatch_->stopping = false;
        dispatch_->pending.clear();
    }

    const auto dispatch = dispatch_;
    dispatch_thread_ = std::thread([dispatch] {
        for (;;) {
            PttEvent event;
            {
                std::unique_lock lock(dispatch->mutex);
                dispatch->changed.wait(lock, [&] {
                    return dispatch->stopping || !dispatch->pending.empty();
                });
                if (dispatch->pending.empty() && dispatch->stopping) {
                    return;
                }
                event = dispatch->pending.front();
                dispatch->pending.pop_front();
            }
            try {
                if (dispatch->callback) {
                    dispatch->callback(event);
                }
            } catch (...) {
                // A client exception must not permanently kill PTT delivery.
            }
        }
    });

    std::promise<std::error_code> promise;
    auto future = promise.get_future();
    hook_thread_ = std::thread(&GlobalInputMonitor::hook_main, this, std::move(promise));
    const std::error_code error = future.get();
    if (error) {
        if (hook_thread_.joinable()) {
            hook_thread_.join();
        }
        {
            std::lock_guard dispatch_lock(dispatch_->mutex);
            dispatch_->stopping = true;
        }
        dispatch_->changed.notify_all();
        if (dispatch_thread_.joinable()) {
            dispatch_thread_.join();
        }
        return error;
    }
    running_.store(true);
    return {};
}

void GlobalInputMonitor::stop() noexcept {
    std::unique_lock lifecycle_lock(lifecycle_mutex_);
    const DWORD thread_id = hook_thread_id_.load();
    if (thread_id != 0) {
        PostThreadMessageW(thread_id, WM_QUIT, 0, 0);
    }
    lifecycle_lock.unlock();

    if (hook_thread_.joinable() && hook_thread_.get_id() != std::this_thread::get_id()) {
        hook_thread_.join();
    }

    {
        std::lock_guard lock(dispatch_->mutex);
        dispatch_->stopping = true;
    }
    dispatch_->changed.notify_all();
    if (dispatch_thread_.joinable()) {
        if (dispatch_thread_.get_id() == std::this_thread::get_id()) {
            // The thread owns only DispatchState, so detaching here is safe and
            // permits a callback to stop/destroy its monitor without deadlock.
            dispatch_thread_.detach();
        } else {
            dispatch_thread_.join();
        }
    }

    lifecycle_lock.lock();
    running_.store(false);
    hook_thread_id_.store(0);
}

void GlobalInputMonitor::hook_main(std::promise<std::error_code> ready) {
    g_monitor = this;
    hook_thread_id_.store(GetCurrentThreadId());

    // Force creation of this thread's queue before start() can call stop().
    MSG message{};
    PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

    keyboard_hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, &GlobalInputMonitor::keyboard_hook, nullptr, 0);
    if (keyboard_hook_ == nullptr) {
        const auto error = last_win32_error();
        hook_thread_id_.store(0);
        g_monitor = nullptr;
        ready.set_value(error);
        return;
    }

    const bool needs_mouse = std::any_of(
        options_.triggers.begin(), options_.triggers.end(),
        [](const Trigger& trigger) { return trigger.device == TriggerDevice::mouse; });
    if (needs_mouse) {
        mouse_hook_ = SetWindowsHookExW(WH_MOUSE_LL, &GlobalInputMonitor::mouse_hook, nullptr, 0);
        if (mouse_hook_ == nullptr) {
            const auto error = last_win32_error();
            UnhookWindowsHookEx(keyboard_hook_);
            keyboard_hook_ = nullptr;
            hook_thread_id_.store(0);
            g_monitor = nullptr;
            ready.set_value(error);
            return;
        }
    }

    ready.set_value({});
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    process_cancel(CancelReason::monitor_stopped);
    if (mouse_hook_ != nullptr) {
        UnhookWindowsHookEx(mouse_hook_);
        mouse_hook_ = nullptr;
    }
    if (keyboard_hook_ != nullptr) {
        UnhookWindowsHookEx(keyboard_hook_);
        keyboard_hook_ = nullptr;
    }
    hook_thread_id_.store(0);
    g_monitor = nullptr;
}

LRESULT CALLBACK GlobalInputMonitor::keyboard_hook(
    const int code, const WPARAM message, const LPARAM data) noexcept {
    if (code >= HC_ACTION && g_monitor != nullptr) {
        if (g_monitor->process_keyboard(
                message, *reinterpret_cast<const KBDLLHOOKSTRUCT*>(data))) {
            return 1;
        }
    }
    return CallNextHookEx(nullptr, code, message, data);
}

LRESULT CALLBACK GlobalInputMonitor::mouse_hook(
    const int code, const WPARAM message, const LPARAM data) noexcept {
    if (code >= HC_ACTION && g_monitor != nullptr) {
        if (g_monitor->process_mouse(
                message, *reinterpret_cast<const MSLLHOOKSTRUCT*>(data))) {
            return 1;
        }
    }
    return CallNextHookEx(nullptr, code, message, data);
}

bool GlobalInputMonitor::process_keyboard(
    const WPARAM message, const KBDLLHOOKSTRUCT& event) noexcept {
    constexpr ULONG_PTR injectedFlags =
        LLKHF_INJECTED | LLKHF_LOWER_IL_INJECTED;
    if (options_.ignore_injected_events && (event.flags & injectedFlags) != 0) {
        return false;
    }

    const auto cancellation = cancel_key_input_.classify(
        options_, message, event, state_.is_active());
    if (cancellation.matched) {
        if (cancellation.request_cancel) {
            process_cancel(CancelReason::escape_key);
        }
        return cancellation.consume;
    }

    const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    const bool up = message == WM_KEYUP || message == WM_SYSKEYUP;
    if (!down && !up) {
        return false;
    }
    const Trigger trigger = keyboard_trigger(event.vkCode);
    if (!configured(trigger)) {
        return false;
    }
    if (down) {
        process_down(trigger);
    } else {
        process_up(trigger);
    }
    return false;
}

bool GlobalInputMonitor::process_mouse(
    const WPARAM message, const MSLLHOOKSTRUCT& event) noexcept {
    const auto decision = detail::classify_mouse_input(options_, message, event);
    if (!decision.configured) {
        return false;
    }
    if (decision.pressed) {
        process_down(decision.trigger);
    } else {
        process_up(decision.trigger);
    }
    return decision.consume;
}

void GlobalInputMonitor::process_down(const Trigger trigger) noexcept {
    if (state_.press(trigger) == PttTransition::pressed) {
        enqueue({PttEventKind::pressed, trigger, CancelReason::none, std::chrono::steady_clock::now()});
    }
}

void GlobalInputMonitor::process_up(const Trigger trigger) noexcept {
    if (state_.release(trigger) == PttTransition::released) {
        enqueue({PttEventKind::released, trigger, CancelReason::none, std::chrono::steady_clock::now()});
    }
}

void GlobalInputMonitor::process_cancel(const CancelReason reason) noexcept {
    const auto trigger = state_.active_trigger();
    if (state_.cancel() == PttTransition::cancelled && trigger.has_value()) {
        enqueue({PttEventKind::cancelled, *trigger, reason, std::chrono::steady_clock::now()});
    }
}

void GlobalInputMonitor::enqueue(PttEvent event) noexcept {
    try {
        {
            std::lock_guard lock(dispatch_->mutex);
            if (dispatch_->stopping) {
                return;
            }
            dispatch_->pending.push_back(std::move(event));
        }
        dispatch_->changed.notify_one();
    } catch (...) {
        // Hooks must never unwind through user32. Losing one notification is
        // safer than Windows removing the hook or terminating the process.
    }
}

bool GlobalInputMonitor::configured(const Trigger trigger) const noexcept {
    return std::find(options_.triggers.begin(), options_.triggers.end(), trigger)
        != options_.triggers.end();
}

}  // namespace localflow::windows

#endif
