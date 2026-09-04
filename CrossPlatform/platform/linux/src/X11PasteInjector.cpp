#include "InternalFactories.hpp"

#include "Process.hpp"

#include <memory>

#if defined(LOCALFLOW_LINUX_WITH_XTEST)
#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>
#undef Status
#endif

namespace localflow::platform::linux::detail {
namespace {

class X11PasteInjector final : public PasteInjector {
public:
    Status paste() override {
#if defined(LOCALFLOW_LINUX_WITH_XTEST)
        Display* display = XOpenDisplay(nullptr);
        if (!display) {
            return Status::failure(
                ErrorCode::service_unavailable,
                "LocalFlow could not connect to the X11 display for paste injection.");
        }
        const auto control = XKeysymToKeycode(display, XK_Control_L);
        const auto letterV = XKeysymToKeycode(display, XK_v);
        if (control == 0 || letterV == 0) {
            XCloseDisplay(display);
            return Status::failure(
                ErrorCode::protocol_error,
                "The current X11 keymap does not contain Control+V.");
        }
        const bool sent = XTestFakeKeyEvent(display, control, True, CurrentTime) &&
                          XTestFakeKeyEvent(display, letterV, True, CurrentTime) &&
                          XTestFakeKeyEvent(display, letterV, False, CurrentTime) &&
                          XTestFakeKeyEvent(display, control, False, CurrentTime);
        XFlush(display);
        XCloseDisplay(display);
        return sent
            ? Status::success()
            : Status::failure(ErrorCode::io_error, "XTest rejected the Ctrl+V event sequence.");
#else
        if (executableOnPath("xdotool")) {
            const auto response = runCommand({"xdotool", "key", "--clearmodifiers", "ctrl+v"});
            if (response.launched && !response.timedOut && response.exitCode == 0) {
                return Status::success();
            }
            return Status::failure(
                response.timedOut ? ErrorCode::timed_out : ErrorCode::io_error,
                "xdotool could not inject Ctrl+V" +
                    (response.standardError.empty() ? std::string{"."} : ": " + response.standardError));
        }
        return Status::failure(
            ErrorCode::missing_dependency,
            "This LocalFlow build has neither XTest support nor xdotool.",
            "Install the libXtst development package before building, or install xdotool.");
#endif
    }
};

}  // namespace

std::unique_ptr<PasteInjector> makeX11PasteInjector() {
    return std::make_unique<X11PasteInjector>();
}

}  // namespace localflow::platform::linux::detail
