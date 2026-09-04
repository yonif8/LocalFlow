#include "localflow/windows/InputMonitor.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

using localflow::windows::InputMonitorOptions;
using localflow::windows::MouseButton;
using localflow::windows::mouse_trigger;

int failures = 0;

void expect(const bool condition, const std::string_view description) {
    if (!condition) {
        std::cerr << "FAIL: " << description << '\n';
        ++failures;
    }
}

MSLLHOOKSTRUCT mouseEvent(const DWORD flags = 0, const DWORD data = 0) {
    MSLLHOOKSTRUCT event{};
    event.flags = flags;
    event.mouseData = data;
    return event;
}

void consumesOnlyConfiguredPhysicalButtonEdges() {
    InputMonitorOptions options;
    options.triggers = {mouse_trigger(MouseButton::x1)};

    const auto x1Data = DWORD(XBUTTON1) << 16U;
    const auto down = localflow::windows::detail::classify_mouse_input(
        options, WM_XBUTTONDOWN, mouseEvent(0, x1Data));
    expect(down.configured, "configured XBUTTON1 down is recognized");
    expect(down.pressed, "configured down is classified as pressed");
    expect(down.consume, "configured physical mouse down is consumed");

    const auto up = localflow::windows::detail::classify_mouse_input(
        options, WM_XBUTTONUP, mouseEvent(0, x1Data));
    expect(up.configured, "configured XBUTTON1 up is recognized");
    expect(!up.pressed, "configured up is classified as released");
    expect(up.consume, "configured physical mouse up is consumed");

    const auto unrelated = localflow::windows::detail::classify_mouse_input(
        options, WM_MBUTTONDOWN, mouseEvent());
    expect(!unrelated.configured, "unrelated middle click is ignored");
    expect(!unrelated.consume, "unrelated middle click is never consumed");

    const auto injected = localflow::windows::detail::classify_mouse_input(
        options, WM_XBUTTONDOWN, mouseEvent(LLMHF_INJECTED, x1Data));
    expect(!injected.configured, "injected configured button is ignored");
    expect(!injected.consume, "injected configured button is never consumed");

    const auto lowerIntegrityInjected =
        localflow::windows::detail::classify_mouse_input(
            options, WM_XBUTTONDOWN,
            mouseEvent(LLMHF_LOWER_IL_INJECTED, x1Data));
    expect(!lowerIntegrityInjected.configured,
           "lower-integrity injected button is ignored");
    expect(!lowerIntegrityInjected.consume,
           "lower-integrity injected button is never consumed");
}

void supportsExplicitObservationOnlyMode() {
    InputMonitorOptions options;
    options.triggers = {mouse_trigger(MouseButton::middle)};
    options.consume_mouse_trigger_events = false;

    const auto decision = localflow::windows::detail::classify_mouse_input(
        options, WM_MBUTTONDOWN, mouseEvent());
    expect(decision.configured, "observation-only trigger still drives PTT");
    expect(!decision.consume, "observation-only trigger reaches foreground app");
}

}  // namespace

int main() {
    consumesOnlyConfiguredPhysicalButtonEdges();
    supportsExplicitObservationOnlyMode();
    if (failures != 0) {
        std::cerr << failures << " assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "InputMonitorPolicyTests: all assertions passed\n";
    return EXIT_SUCCESS;
}
