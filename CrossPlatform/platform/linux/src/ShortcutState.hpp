#pragma once

#include "localflow/linux/GlobalShortcut.hpp"

#include <optional>

namespace localflow::platform::linux::detail {

enum class ShortcutInput {
    trigger_pressed,
    trigger_released,
    escape_pressed,
};

// Shared state rules make auto-repeat, a late release after cancellation, and
// shutdown deterministic without depending on a live X server in unit tests.
class PushToTalkShortcutState {
public:
    [[nodiscard]] std::optional<ShortcutEdge> handle(ShortcutInput input) noexcept {
        switch (input) {
            case ShortcutInput::trigger_pressed:
                if (held_) return std::nullopt;
                held_ = true;
                return ShortcutEdge::pressed;
            case ShortcutInput::trigger_released:
                if (!held_) return std::nullopt;
                held_ = false;
                return ShortcutEdge::released;
            case ShortcutInput::escape_pressed:
                if (!held_) return std::nullopt;
                held_ = false;
                return ShortcutEdge::cancelled;
        }
        return std::nullopt;
    }

    [[nodiscard]] bool held() const noexcept { return held_; }

private:
    bool held_{false};
};

}  // namespace localflow::platform::linux::detail
