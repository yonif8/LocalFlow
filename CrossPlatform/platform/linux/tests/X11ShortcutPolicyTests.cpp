#include "X11ShortcutPolicy.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

int failures = 0;

void expect(const bool condition, const std::string_view description) {
    if (condition) return;
    std::cerr << "FAIL: " << description << '\n';
    ++failures;
}

XEvent keyEvent(
    const int type,
    const KeyCode keycode,
    const Time time = 123,
    const Window window = 456) {
    XEvent event{};
    event.type = type;
    event.xkey.keycode = keycode;
    event.xkey.time = time;
    event.xkey.window = window;
    return event;
}

void matchesOnlyAnExactHeldReleasePressPair() {
    constexpr KeyCode configured = 42;
    const auto release = keyEvent(KeyRelease, configured);
    const auto press = keyEvent(KeyPress, configured);
    using localflow::platform::linux::detail::isLegacyAutoRepeatPair;

    expect(isLegacyAutoRepeatPair(true, configured, release, press),
           "an exact held release/press pair is legacy autorepeat");
    expect(!isLegacyAutoRepeatPair(false, configured, release, press),
           "an idle pair is not filtered");
    expect(!isLegacyAutoRepeatPair(
               true, configured + 1, release, press),
           "a pair for another configured key is not filtered");
    expect(!isLegacyAutoRepeatPair(
               true, configured, release, keyEvent(KeyPress, configured, 124)),
           "different server timestamps are not filtered");
    expect(!isLegacyAutoRepeatPair(
               true, configured, release,
               keyEvent(KeyPress, configured, 123, 457)),
           "different event windows are not filtered");
    expect(!isLegacyAutoRepeatPair(
               true, configured, release, keyEvent(KeyRelease, configured)),
           "a second release is not filtered");
    expect(!isLegacyAutoRepeatPair(
               true, configured, keyEvent(KeyPress, configured), press),
           "a leading press is not filtered");
    expect(!isLegacyAutoRepeatPair(
               true, configured, release, keyEvent(KeyPress, configured + 1)),
           "a different following key is not filtered");
}

}  // namespace

int main() {
    matchesOnlyAnExactHeldReleasePressPair();
    if (failures != 0) {
        std::cerr << failures << " X11 shortcut policy assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "X11ShortcutPolicyTests: all assertions passed\n";
    return EXIT_SUCCESS;
}
