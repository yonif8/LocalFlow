#include "localflow/windows/AudioSafetyState.hpp"
#include "localflow/windows/FocusedTextTargetState.hpp"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {

int failures = 0;

void expect(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

localflow::windows::FocusedTextTargetFingerprint uia_target() {
    localflow::windows::FocusedTextTargetFingerprint target;
    target.backend =
        localflow::windows::FocusedTextTargetBackend::ui_automation_runtime_id;
    target.foreground_window = 0x100U;
    target.process_id = 42U;
    target.native_focus_window = 0x101U;
    target.automation_runtime_id = {-3, 42, 7};
    target.editable = true;
    return target;
}

void test_focused_target_policy() {
    using localflow::windows::FocusedTextTargetBackend;
    using localflow::windows::FocusedTextTargetMatch;
    using localflow::windows::compare_focused_text_targets;
    using localflow::windows::is_valid_focused_text_target;

    const auto expected = uia_target();
    expect(is_valid_focused_text_target(expected),
           "accepts a complete UI Automation field identity");
    expect(compare_focused_text_targets(expected, expected)
               == FocusedTextTargetMatch::matched,
           "accepts the exact same focused field");

    auto changed = expected;
    changed.automation_runtime_id.back() = 8;
    expect(compare_focused_text_targets(expected, changed)
               == FocusedTextTargetMatch::focused_field_changed,
           "rejects a different field in the same HWND");

    changed = expected;
    changed.native_focus_window = 0x102U;
    expect(compare_focused_text_targets(expected, changed)
               == FocusedTextTargetMatch::focused_field_changed,
           "uses a UIA element's native handle as additional identity");

    changed = expected;
    changed.foreground_window = 0x200U;
    expect(compare_focused_text_targets(expected, changed)
               == FocusedTextTargetMatch::foreground_changed,
           "rejects a different foreground window");

    changed = expected;
    changed.protected_content = true;
    changed.editable = false;
    expect(compare_focused_text_targets(expected, changed)
               == FocusedTextTargetMatch::became_protected,
           "protected fields fail before any insertion comparison");

    changed = expected;
    changed.editable = false;
    expect(compare_focused_text_targets(expected, changed)
               == FocusedTextTargetMatch::became_non_editable,
           "rejects a field that became read-only");

    changed = expected;
    changed.backend = FocusedTextTargetBackend::native_edit_control;
    changed.automation_runtime_id.clear();
    expect(compare_focused_text_targets(expected, changed)
               == FocusedTextTargetMatch::focused_field_changed,
           "never downgrades a captured UIA identity to HWND-only validation");

    auto native = expected;
    native.backend = FocusedTextTargetBackend::native_edit_control;
    native.automation_runtime_id.clear();
    expect(is_valid_focused_text_target(native),
           "accepts a recognized native edit-control identity");
    changed = native;
    changed.native_focus_window = 0x103U;
    expect(compare_focused_text_targets(native, changed)
               == FocusedTextTargetMatch::focused_field_changed,
           "native fallback requires the exact same focused child HWND");

    auto invalid = expected;
    invalid.automation_runtime_id.clear();
    expect(!is_valid_focused_text_target(invalid),
           "rejects a UIA identity with no opaque runtime ID");
    invalid = expected;
    invalid.protected_content = true;
    expect(!is_valid_focused_text_target(invalid),
           "a protected field can never become an insertion target");
}

void test_bounded_audio_drain() {
    using namespace std::chrono_literals;
    using localflow::windows::audio_tail_wait_milliseconds;
    using localflow::windows::clamp_audio_tail_drain;

    expect(clamp_audio_tail_drain(-10ms) == 0ms,
           "negative audio drains clamp to zero");
    expect(clamp_audio_tail_drain(100ms) == 100ms,
           "normal audio drains are retained");
    expect(clamp_audio_tail_drain(5s) == 250ms,
           "audio drains are capped at the fixed responsiveness bound");

    const auto start = std::chrono::steady_clock::time_point{};
    expect(audio_tail_wait_milliseconds(start, start) == 0U,
           "an expired drain never waits");
    expect(audio_tail_wait_milliseconds(start, start + 1ns) == 1U,
           "a fractional millisecond is ceil-rounded so tail audio is not lost");
    expect(audio_tail_wait_milliseconds(start, start + 80ms) == 80U,
           "an ordinary drain exposes its exact remaining wait");
    expect(audio_tail_wait_milliseconds(start, start + 5s) == 250U,
           "wait conversion independently preserves the hard upper bound");
}

void test_conditional_volume_restore() {
    using localflow::windows::VolumeRestoreDecision;
    using localflow::windows::decide_volume_restore;

    expect(decide_volume_restore(true, 0.2F, 0.2F)
               == VolumeRestoreDecision::restore_original,
           "restores only while the endpoint remains at LocalFlow's level");
    expect(decide_volume_restore(true, 0.2000005F, 0.2F)
               == VolumeRestoreDecision::restore_original,
           "allows harmless endpoint scalar round-trip noise");
    expect(decide_volume_restore(true, 0.25F, 0.2F)
               == VolumeRestoreDecision::skip_user_adjusted_volume,
           "a user volume adjustment always wins");
    expect(decide_volume_restore(
               true, std::numeric_limits<float>::quiet_NaN(), 0.2F)
               == VolumeRestoreDecision::skip_user_adjusted_volume,
           "invalid endpoint values fail without writing volume");
    expect(decide_volume_restore(false, 0.2F, 0.2F)
               == VolumeRestoreDecision::skip_not_active,
           "an inactive duck never changes volume");
}

}  // namespace

int main() {
    test_focused_target_policy();
    test_bounded_audio_drain();
    test_conditional_volume_restore();
    if (failures == 0) {
        std::cout << "Windows safety state tests passed\n";
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
}
