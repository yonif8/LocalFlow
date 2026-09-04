#include "InternalFactories.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

#if defined(LOCALFLOW_LINUX_WITH_X11)
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#undef Status
#endif

namespace localflow::platform::linux::detail {
namespace {

#if defined(LOCALFLOW_LINUX_WITH_X11)

class DisplayHandle {
public:
    DisplayHandle() : display_(XOpenDisplay(nullptr)) {}
    ~DisplayHandle() {
        if (display_) {
            XCloseDisplay(display_);
        }
    }
    Display* get() const { return display_; }

private:
    Display* display_;
};

Result<Window> activeWindow(Display* display) {
    const auto atom = XInternAtom(display, "_NET_ACTIVE_WINDOW", True);
    if (atom == None) {
        return Result<Window>::failure(Status::failure(
            ErrorCode::protocol_error,
            "The X11 window manager does not publish _NET_ACTIVE_WINDOW.",
            "Use an EWMH-compatible window manager."));
    }

    Atom actualType = None;
    int actualFormat = 0;
    unsigned long itemCount = 0;
    unsigned long bytesAfter = 0;
    unsigned char* raw = nullptr;
    const auto result = XGetWindowProperty(
        display,
        DefaultRootWindow(display),
        atom,
        0,
        1,
        False,
        XA_WINDOW,
        &actualType,
        &actualFormat,
        &itemCount,
        &bytesAfter,
        &raw);
    if (result != Success || raw == nullptr || itemCount != 1 || actualFormat != 32) {
        if (raw) {
            XFree(raw);
        }
        return Result<Window>::failure(Status::failure(
            ErrorCode::service_unavailable,
            "No active X11 window is currently available."));
    }

    const Window window = *reinterpret_cast<unsigned long*>(raw);
    XFree(raw);
    if (window == None) {
        return Result<Window>::failure(Status::failure(
            ErrorCode::service_unavailable,
            "The X11 window manager reported no active window."));
    }
    return Result<Window>::success(window);
}

std::string stringProperty(Display* display, Window window, const char* propertyName) {
    const auto property = XInternAtom(display, propertyName, True);
    if (property == None) {
        return {};
    }

    Atom actualType = None;
    int actualFormat = 0;
    unsigned long itemCount = 0;
    unsigned long bytesAfter = 0;
    unsigned char* raw = nullptr;
    if (XGetWindowProperty(
            display, window, property, 0, 4096, False, AnyPropertyType,
            &actualType, &actualFormat, &itemCount, &bytesAfter, &raw) != Success ||
        raw == nullptr || actualFormat != 8) {
        if (raw) {
            XFree(raw);
        }
        return {};
    }
    std::string value(reinterpret_cast<char*>(raw), itemCount);
    XFree(raw);
    return value;
}

std::int64_t integerProperty(Display* display, Window window, const char* propertyName) {
    const auto property = XInternAtom(display, propertyName, True);
    if (property == None) {
        return -1;
    }
    Atom actualType = None;
    int actualFormat = 0;
    unsigned long itemCount = 0;
    unsigned long bytesAfter = 0;
    unsigned char* raw = nullptr;
    if (XGetWindowProperty(
            display, window, property, 0, 1, False, XA_CARDINAL,
            &actualType, &actualFormat, &itemCount, &bytesAfter, &raw) != Success ||
        raw == nullptr || itemCount != 1 || actualFormat != 32) {
        if (raw) {
            XFree(raw);
        }
        return -1;
    }
    const auto value = static_cast<std::int64_t>(*reinterpret_cast<unsigned long*>(raw));
    XFree(raw);
    return value;
}

std::uint8_t channel(unsigned long pixel, unsigned long mask) {
    if (mask == 0) {
        return 0;
    }
    unsigned int shift = 0;
    while (((mask >> shift) & 1UL) == 0) {
        ++shift;
    }
    const auto maximum = mask >> shift;
    const auto value = (pixel & mask) >> shift;
    return static_cast<std::uint8_t>((value * 255UL + maximum / 2UL) / maximum);
}

ApplicationInfo applicationInfo(Display* display, Window window) {
    ApplicationInfo info;
    info.nativeWindowId = static_cast<std::uint64_t>(window);
    info.windowTitle = stringProperty(display, window, "_NET_WM_NAME");
    if (info.windowTitle.empty()) {
        char* title = nullptr;
        if (XFetchName(display, window, &title) > 0 && title) {
            info.windowTitle = title;
            XFree(title);
        }
    }

    XClassHint classHint{};
    if (XGetClassHint(display, window, &classHint) != 0) {
        if (classHint.res_class) {
            info.applicationId = classHint.res_class;
            XFree(classHint.res_class);
        }
        if (classHint.res_name) {
            info.name = classHint.res_name;
            XFree(classHint.res_name);
        }
    }
    if (info.name.empty()) info.name = info.applicationId;
    info.processId = integerProperty(display, window, "_NET_WM_PID");
    return info;
}

Result<ScreenFrame> captureWindow(Display* display, Window window) {
    XWindowAttributes attributes{};
    if (XGetWindowAttributes(display, window, &attributes) == 0 ||
        attributes.width <= 0 || attributes.height <= 0 ||
        attributes.map_state != IsViewable) {
        return Result<ScreenFrame>::failure(Status::failure(
            ErrorCode::service_unavailable,
            "The active X11 window is not currently visible."));
    }

    XImage* image = XGetImage(
        display, window, 0, 0,
        static_cast<unsigned int>(attributes.width),
        static_cast<unsigned int>(attributes.height), AllPlanes, ZPixmap);
    if (!image) {
        return Result<ScreenFrame>::failure(Status::failure(
            ErrorCode::io_error,
            "X11 could not capture the active window.",
            "Check that the window is visible and the X server permits capture."));
    }

    ScreenFrame frame;
    frame.width = attributes.width;
    frame.height = attributes.height;
    frame.bytesPerRow = frame.width * 4;
    frame.pixelFormat = PixelFormat::bgra8;
    frame.pixels.resize(static_cast<std::size_t>(frame.bytesPerRow) * frame.height);

    for (int y = 0; y < frame.height; ++y) {
        auto* destination =
            frame.pixels.data() + static_cast<std::size_t>(y) * frame.bytesPerRow;
        for (int x = 0; x < frame.width; ++x) {
            const auto pixel = XGetPixel(image, x, y);
            destination[x * 4 + 0] = channel(pixel, image->blue_mask);
            destination[x * 4 + 1] = channel(pixel, image->green_mask);
            destination[x * 4 + 2] = channel(pixel, image->red_mask);
            destination[x * 4 + 3] = 255;
        }
    }
    XDestroyImage(image);
    return Result<ScreenFrame>::success(std::move(frame));
}

class X11ScreenContext final : public ScreenContextBackend {
public:
    std::string name() const override { return "X11 EWMH"; }

    Result<ApplicationInfo> activeApplication() override {
        DisplayHandle handle;
        if (!handle.get()) {
            return Result<ApplicationInfo>::failure(noDisplay());
        }
        const auto active = activeWindow(handle.get());
        if (!active) {
            return Result<ApplicationInfo>::failure(active.status());
        }

        return Result<ApplicationInfo>::success(
            applicationInfo(handle.get(), active.value()));
    }

    Result<ScreenFrame> captureContextFrame() override {
        DisplayHandle handle;
        if (!handle.get()) {
            return Result<ScreenFrame>::failure(noDisplay());
        }
        const auto active = activeWindow(handle.get());
        if (!active) {
            return Result<ScreenFrame>::failure(active.status());
        }

        return captureWindow(handle.get(), active.value());
    }

    Result<ScreenFrame> captureContextFrame(
        const ApplicationInfo& expectedTarget) override {
        DisplayHandle handle;
        if (!handle.get()) {
            return Result<ScreenFrame>::failure(noDisplay());
        }
        const auto active = activeWindow(handle.get());
        if (!active) return Result<ScreenFrame>::failure(active.status());

        const auto current = applicationInfo(handle.get(), active.value());
        const auto validation =
            validateScreenCaptureTarget(expectedTarget, current);
        if (!validation.ok()) {
            return Result<ScreenFrame>::failure(validation);
        }
        return captureWindow(handle.get(), active.value());
    }

private:
    static Status noDisplay() {
        return Status::failure(
            ErrorCode::service_unavailable,
            "LocalFlow could not connect to the X11 display.",
            "Check DISPLAY and run LocalFlow inside the graphical user session.");
    }
};

#else

class X11ScreenContext final : public ScreenContextBackend {
public:
    std::string name() const override { return "X11 (not built)"; }
    Result<ApplicationInfo> activeApplication() override {
        return Result<ApplicationInfo>::failure(unavailable());
    }
    Result<ScreenFrame> captureContextFrame() override {
        return Result<ScreenFrame>::failure(unavailable());
    }

private:
    static Status unavailable() {
        return Status::failure(
            ErrorCode::missing_dependency,
            "This LocalFlow build does not contain the X11 screen-context adapter.",
            "Rebuild with the X11 development package installed.");
    }
};

#endif

}  // namespace

std::unique_ptr<ScreenContextBackend> makeX11ScreenContextBackend() {
    return std::make_unique<X11ScreenContext>();
}

}  // namespace localflow::platform::linux::detail
