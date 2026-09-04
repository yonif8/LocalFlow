#pragma once

#include <cstdint>
#include <optional>

namespace localflow::windows {

enum class TriggerDevice : std::uint8_t {
    keyboard,
    mouse,
};

/// Platform-neutral identity for one physical hold-to-talk trigger.
///
/// Keyboard codes are Win32 virtual-key values in the Windows adapter. Mouse
/// codes use the MouseButton values declared by InputMonitor.hpp.
struct Trigger {
    TriggerDevice device{TriggerDevice::keyboard};
    std::uint32_t code{0};

    friend constexpr bool operator==(const Trigger& lhs, const Trigger& rhs) noexcept {
        return lhs.device == rhs.device && lhs.code == rhs.code;
    }

    friend constexpr bool operator!=(const Trigger& lhs, const Trigger& rhs) noexcept {
        return !(lhs == rhs);
    }
};

enum class PttTransition : std::uint8_t {
    none,
    pressed,
    released,
    cancelled,
};

/// Enforces the same hold semantics as LocalFlow on macOS: the first matching
/// trigger owns the hold, other configured triggers are ignored until it is
/// released, and a cancellation consumes the later physical release.
class PttStateMachine final {
public:
    [[nodiscard]] PttTransition press(Trigger trigger) noexcept;
    [[nodiscard]] PttTransition release(Trigger trigger) noexcept;
    [[nodiscard]] PttTransition cancel() noexcept;

    void reset() noexcept;

    [[nodiscard]] bool is_active() const noexcept { return active_.has_value(); }
    [[nodiscard]] std::optional<Trigger> active_trigger() const noexcept { return active_; }

private:
    std::optional<Trigger> active_;
};

}  // namespace localflow::windows
