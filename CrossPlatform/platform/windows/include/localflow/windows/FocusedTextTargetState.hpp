#pragma once

#include <cstdint>
#include <vector>

namespace localflow::windows {

enum class FocusedTextTargetBackend : std::uint8_t {
    ui_automation_runtime_id,
    native_edit_control,
};

/// Opaque, text-free identity for the field that owned keyboard focus when a
/// dictation began. No accessible name, value, or surrounding text is retained.
struct FocusedTextTargetFingerprint {
    FocusedTextTargetBackend backend{FocusedTextTargetBackend::ui_automation_runtime_id};
    std::uintptr_t foreground_window{0};
    std::uint32_t process_id{0};
    std::uintptr_t native_focus_window{0};
    std::vector<std::int32_t> automation_runtime_id;
    bool editable{false};
    bool protected_content{false};
};

enum class FocusedTextTargetMatch : std::uint8_t {
    matched,
    invalid_expected_identity,
    foreground_changed,
    focused_field_changed,
    became_non_editable,
    became_protected,
};

[[nodiscard]] bool is_valid_focused_text_target(
    const FocusedTextTargetFingerprint& target) noexcept;

/// Pure comparison used immediately before synthetic input. Keeping this
/// policy independent from UI Automation makes all fail-closed cases
/// deterministic and testable on every build host.
[[nodiscard]] FocusedTextTargetMatch compare_focused_text_targets(
    const FocusedTextTargetFingerprint& expected,
    const FocusedTextTargetFingerprint& current) noexcept;

}  // namespace localflow::windows
