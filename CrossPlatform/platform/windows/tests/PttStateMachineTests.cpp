#include "localflow/windows/PttStateMachine.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

using localflow::windows::PttStateMachine;
using localflow::windows::PttTransition;
using localflow::windows::Trigger;
using localflow::windows::TriggerDevice;

int failures = 0;

void expect(const bool condition, const std::string_view description) {
    if (!condition) {
        std::cerr << "FAIL: " << description << '\n';
        ++failures;
    }
}

constexpr Trigger key(const std::uint32_t code) {
    return {TriggerDevice::keyboard, code};
}

constexpr Trigger mouse(const std::uint32_t code) {
    return {TriggerDevice::mouse, code};
}

void basic_hold() {
    PttStateMachine state;
    expect(!state.is_active(), "new state is idle");
    expect(state.press(key(119)) == PttTransition::pressed, "first down starts PTT");
    expect(state.is_active(), "state is active after down");
    expect(state.active_trigger() == key(119), "active key is retained");
    expect(state.release(key(119)) == PttTransition::released, "matching up ends PTT");
    expect(!state.is_active(), "state is idle after matching up");
}

void ignores_auto_repeat_and_secondary_trigger() {
    PttStateMachine state;
    expect(state.press(key(119)) == PttTransition::pressed, "first press starts");
    expect(state.press(key(119)) == PttTransition::none, "repeat does not press twice");
    expect(state.press(mouse(4)) == PttTransition::none, "second trigger cannot steal hold");
    expect(state.release(mouse(4)) == PttTransition::none, "second trigger cannot end hold");
    expect(state.is_active(), "original hold survives unrelated release");
    expect(state.release(key(119)) == PttTransition::released, "owner release ends hold");
}

void cancellation_consumes_later_release() {
    PttStateMachine state;
    expect(state.press(mouse(4)) == PttTransition::pressed, "mouse starts PTT");
    expect(state.cancel() == PttTransition::cancelled, "escape cancels active PTT");
    expect(!state.is_active(), "cancel clears active trigger");
    expect(state.release(mouse(4)) == PttTransition::none, "release after cancel is ignored");
    expect(state.cancel() == PttTransition::none, "idle cancel is a no-op");
}

void reset_is_silent() {
    PttStateMachine state;
    (void)state.press(key(17));
    state.reset();
    expect(!state.is_active(), "reset clears state");
    expect(state.release(key(17)) == PttTransition::none, "release after reset is ignored");
}

}  // namespace

int main() {
    basic_hold();
    ignores_auto_repeat_and_secondary_trigger();
    cancellation_consumes_later_release();
    reset_is_silent();
    if (failures != 0) {
        std::cerr << failures << " assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "PttStateMachineTests: all assertions passed\n";
    return EXIT_SUCCESS;
}
