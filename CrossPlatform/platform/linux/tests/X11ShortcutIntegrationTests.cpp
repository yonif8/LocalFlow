#include "InternalFactories.hpp"

#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>
#undef Status

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

using namespace localflow::platform::linux;

namespace {

bool waitForEdges(
    std::condition_variable& changed,
    std::mutex& mutex,
    const std::vector<ShortcutEdge>& edges,
    std::size_t count) {
    std::unique_lock<std::mutex> lock(mutex);
    return changed.wait_for(lock, std::chrono::seconds(2), [&] {
        return edges.size() >= count;
    });
}

}  // namespace

int main() {
    auto backend = detail::makeX11ShortcutBackend();
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<ShortcutEdge> edges;
    ShortcutSpec shortcut;
    shortcut.trigger = "F8";
    shortcut.modifiers.push_back(Modifier::control);
    const auto started = backend->start(shortcut, [&](const ShortcutEvent& event) {
        std::lock_guard<std::mutex> lock(mutex);
        edges.push_back(event.edge);
        changed.notify_all();
    });
    if (!started.ok()) {
        std::cerr << "Could not start X11 shortcut backend: " << started.message << '\n';
        return EXIT_FAILURE;
    }

    // A passive grab collision is an expected desktop condition, not a fatal
    // Xlib error. A second client requesting the same chord must fail cleanly
    // while the first listener remains usable.
    auto conflictingBackend = detail::makeX11ShortcutBackend();
    const auto conflicting = conflictingBackend->start(
        shortcut, [](const ShortcutEvent&) {});
    if (conflicting.code != ErrorCode::busy) {
        std::cerr << "A conflicting X11 shortcut did not report busy.\n";
        return EXIT_FAILURE;
    }

    Display* display = XOpenDisplay(nullptr);
    if (display == nullptr) {
        std::cerr << "Could not open the Xvfb display.\n";
        return EXIT_FAILURE;
    }
    const auto control = XKeysymToKeycode(display, XK_Control_L);
    const auto f8 = XKeysymToKeycode(display, XK_F8);
    const auto escape = XKeysymToKeycode(display, XK_Escape);
    if (control == 0 || f8 == 0 || escape == 0) {
        std::cerr << "Xvfb keyboard map is missing Control, F8, or Escape.\n";
        XCloseDisplay(display);
        return EXIT_FAILURE;
    }

    XTestFakeKeyEvent(display, control, True, CurrentTime);
    XTestFakeKeyEvent(display, f8, True, CurrentTime);
    XFlush(display);
    if (!waitForEdges(changed, mutex, edges, 1)) {
        std::cerr << "Timed out waiting for PTT press.\n";
        XCloseDisplay(display);
        return EXIT_FAILURE;
    }
    XTestFakeKeyEvent(display, escape, True, CurrentTime);
    XTestFakeKeyEvent(display, escape, False, CurrentTime);
    XFlush(display);
    if (!waitForEdges(changed, mutex, edges, 2)) {
        std::cerr << "Timed out waiting for Escape cancellation.\n";
        XCloseDisplay(display);
        return EXIT_FAILURE;
    }
    XTestFakeKeyEvent(display, f8, False, CurrentTime);
    XTestFakeKeyEvent(display, control, False, CurrentTime);
    XFlush(display);

    // A late physical release after cancellation is intentionally ignored.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    backend->stop();
    XCloseDisplay(display);

    {
        std::lock_guard<std::mutex> lock(mutex);
        if (edges.size() != 2 || edges[0] != ShortcutEdge::pressed ||
            edges[1] != ShortcutEdge::cancelled) {
            std::cerr << "Unexpected X11 shortcut edge sequence.\n";
            return EXIT_FAILURE;
        }
    }

    // LocalFlow may run separate X11 listeners for its keyboard and optional
    // mouse triggers. Their overlapping holds must not race two AnyModifier
    // Escape grabs into BadAccess (or terminate the process).
    auto keyboardBackend = detail::makeX11ShortcutBackend();
    auto mouseBackend = detail::makeX11ShortcutBackend();
    std::vector<ShortcutEdge> keyboardEdges;
    std::vector<ShortcutEdge> mouseEdges;
    ShortcutSpec keyboardShortcut;
    keyboardShortcut.id = "keyboard";
    keyboardShortcut.trigger = "F9";
    ShortcutSpec mouseShortcut;
    mouseShortcut.id = "mouse";
    mouseShortcut.kind = ShortcutKind::mouse_button;
    mouseShortcut.mouseButton = 8;
    const auto keyboardStarted = keyboardBackend->start(
        keyboardShortcut, [&](const ShortcutEvent& event) {
            std::lock_guard<std::mutex> callbackLock(mutex);
            keyboardEdges.push_back(event.edge);
            changed.notify_all();
        });
    const auto mouseStarted = mouseBackend->start(
        mouseShortcut, [&](const ShortcutEvent& event) {
            std::lock_guard<std::mutex> callbackLock(mutex);
            mouseEdges.push_back(event.edge);
            changed.notify_all();
        });
    if (!keyboardStarted.ok() || !mouseStarted.ok()) {
        std::cerr << "Could not start simultaneous X11 keyboard/mouse listeners.\n";
        return EXIT_FAILURE;
    }

    display = XOpenDisplay(nullptr);
    if (display == nullptr) {
        std::cerr << "Could not reopen the Xvfb display.\n";
        return EXIT_FAILURE;
    }
    const auto f9 = XKeysymToKeycode(display, XK_F9);
    if (f9 == 0) {
        std::cerr << "Xvfb keyboard map is missing F9.\n";
        XCloseDisplay(display);
        return EXIT_FAILURE;
    }
    XTestFakeKeyEvent(display, f9, True, CurrentTime);
    XFlush(display);
    if (!waitForEdges(changed, mutex, keyboardEdges, 1)) {
        std::cerr << "Timed out waiting for the overlapping keyboard press.\n";
        XCloseDisplay(display);
        return EXIT_FAILURE;
    }
    XTestFakeButtonEvent(display, 8, True, CurrentTime);
    XFlush(display);
    if (!waitForEdges(changed, mutex, mouseEdges, 1)) {
        std::cerr << "Timed out waiting for the overlapping mouse press.\n";
        XCloseDisplay(display);
        return EXIT_FAILURE;
    }
    XTestFakeKeyEvent(display, escape, True, CurrentTime);
    XTestFakeKeyEvent(display, escape, False, CurrentTime);
    XFlush(display);
    if (!waitForEdges(changed, mutex, keyboardEdges, 2)) {
        std::cerr << "Timed out waiting for overlapping Escape cancellation.\n";
        XCloseDisplay(display);
        return EXIT_FAILURE;
    }
    XTestFakeButtonEvent(display, 8, False, CurrentTime);
    XTestFakeKeyEvent(display, f9, False, CurrentTime);
    XFlush(display);
    if (!waitForEdges(changed, mutex, mouseEdges, 2)) {
        std::cerr << "Timed out waiting for the overlapping mouse release.\n";
        XCloseDisplay(display);
        return EXIT_FAILURE;
    }
    keyboardBackend->stop();
    mouseBackend->stop();
    XCloseDisplay(display);

    {
        std::lock_guard<std::mutex> lock(mutex);
        if (keyboardEdges.size() != 2 ||
            keyboardEdges[0] != ShortcutEdge::pressed ||
            keyboardEdges[1] != ShortcutEdge::cancelled ||
            mouseEdges.size() != 2 ||
            mouseEdges[0] != ShortcutEdge::pressed ||
            mouseEdges[1] != ShortcutEdge::released) {
            std::cerr << "Unexpected overlapping X11 shortcut edge sequence.\n";
            return EXIT_FAILURE;
        }
    }
    return EXIT_SUCCESS;
}
