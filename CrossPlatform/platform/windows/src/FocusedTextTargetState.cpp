#include "localflow/windows/FocusedTextTargetState.hpp"

namespace localflow::windows {

bool is_valid_focused_text_target(const FocusedTextTargetFingerprint& target) noexcept {
    if (target.foreground_window == 0 || target.process_id == 0 || !target.editable
        || target.protected_content) {
        return false;
    }
    switch (target.backend) {
        case FocusedTextTargetBackend::ui_automation_runtime_id:
            return !target.automation_runtime_id.empty()
                && target.automation_runtime_id.size() <= 128U;
        case FocusedTextTargetBackend::native_edit_control:
            return target.native_focus_window != 0 && target.automation_runtime_id.empty();
    }
    return false;
}

FocusedTextTargetMatch compare_focused_text_targets(
    const FocusedTextTargetFingerprint& expected,
    const FocusedTextTargetFingerprint& current) noexcept {
    if (!is_valid_focused_text_target(expected)) {
        return FocusedTextTargetMatch::invalid_expected_identity;
    }
    if (current.protected_content) {
        return FocusedTextTargetMatch::became_protected;
    }
    if (expected.foreground_window != current.foreground_window
        || expected.process_id != current.process_id) {
        return FocusedTextTargetMatch::foreground_changed;
    }
    if (!current.editable) {
        return FocusedTextTargetMatch::became_non_editable;
    }
    if (expected.backend != current.backend) {
        return FocusedTextTargetMatch::focused_field_changed;
    }
    switch (expected.backend) {
        case FocusedTextTargetBackend::ui_automation_runtime_id:
            if (expected.automation_runtime_id != current.automation_runtime_id
                || expected.native_focus_window != current.native_focus_window) {
                return FocusedTextTargetMatch::focused_field_changed;
            }
            break;
        case FocusedTextTargetBackend::native_edit_control:
            if (expected.native_focus_window != current.native_focus_window) {
                return FocusedTextTargetMatch::focused_field_changed;
            }
            break;
    }
    return FocusedTextTargetMatch::matched;
}

}  // namespace localflow::windows
