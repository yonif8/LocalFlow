# LocalFlow Linux platform layer

This directory contains the Linux-specific boundary used by the shared
LocalFlow application. It is deliberately separate from transcription,
polishing, terminology, and product UI so those features remain common across
macOS, Windows, and Linux.

## What is implemented

- A runtime capability report for X11, Wayland, XDG Desktop Portals, AT-SPI2,
  PipeWire/PulseAudio, clipboard tools, paste injection, and audio ducking.
- Native X11 press/release shortcuts using `XGrabKey`/`XGrabButton`, including
  Caps Lock and Num Lock variants and a synthetic release during shutdown.
- A production Qt/QDBus GlobalShortcuts transport mapping Wayland `Activated`
  and `Deactivated` signals to push-to-talk edges. Portal requests are bounded,
  cancellable, and close their session deterministically.
- X11 active-app metadata through EWMH and an active-window BGRA capture for
  the shared OCR pipeline.
- A production Qt/QDBus Screenshot transport for Wayland OCR context. It reads
  the compositor-provided image URI into a bounded RGBA frame and leaves no
  persistent screen-sharing session behind.
- AT-SPI2 focused-caret insertion, with bounded accessibility traversal.
- Clipboard/paste fallback with restoration on both success and failure.
  X11 uses XTest (or `xdotool` when XTest was not built); Wayland uses an
  explicitly approved Qt/QDBus RemoteDesktop keyboard session. It sends a
  balanced Ctrl+V sequence and observes compositor session closure. No root
  helper, `/dev/uinput`, or `ydotool` path exists.
- PipeWire-first, PulseAudio-second microphone adapters (`pw-record`/`parec`)
  producing normalized float PCM, plus best-effort `wpctl`/`pactl` ducking.

Every unavailable path returns a structured status containing an error code,
human-readable explanation, and remediation text suitable for Settings or a
diagnostics report.

## Desktop support contract

| Capability | X11 | Wayland |
| --- | --- | --- |
| Hold keyboard shortcut | Native press/release | GlobalShortcuts portal |
| Side mouse button | Native | Not in the standard portal; clearly rejected |
| Active window metadata | EWMH | Best effort through AT-SPI |
| Screen context | Active-window capture | Compositor-mediated Screenshot |
| Direct insertion | AT-SPI2 | AT-SPI2 |
| Paste fallback | Clipboard + XTest | Clipboard + approved RemoteDesktop portal |
| Audio | PipeWire, then PulseAudio | PipeWire, then PulseAudio |

Wayland intentionally prevents an application from discovering and capturing
arbitrary foreground windows or injecting global input without compositor
consent. LocalFlow works with those constraints instead of bypassing them.

## Wayland portal behavior

The public portal boundaries remain injectable, so unit tests use deterministic
fakes without a running desktop. When Qt 6 DBus and Gui are available, the
normal factories automatically construct the production QDBus transports:

- `GlobalShortcutsPortal` performs CreateSession and BindShortcuts, validates
  that the requested binding was accepted, forwards both edges, synthesizes a
  release if the session closes while held, and closes the portal session.
- `ScreenshotPortal` requests a non-interactive full-screen screenshot. Portal
  version 3 gets an explicit screen target; older versions use their compatible
  default behavior. A first capture can display a consent prompt.
- `RemoteDesktopPortal` performs CreateSession, SelectDevices and Start,
  verifies that keyboard control was granted, and uses
  NotifyKeyboardKeysym. The session is reused until shutdown or compositor
  closure, then recreated only after a subsequent paste request.

Interactive requests time out after 60 seconds; direct D-Bus methods have a
five-second bound. Stopping LocalFlow closes any outstanding Request object,
which also dismisses its consent dialog. Cancellation, denial, missing service,
timeout, and malformed-response failures have distinct status codes and
actionable remediation text.

Portal dialogs currently use an empty parent-window identifier. That is valid
for portal calls, but some desktops show the prompt as a separate unparented
window. Supplying an exported Wayland/X11 parent identifier can be added by the
application shell without changing the transport state machines.

The GlobalShortcuts specification has real press and release signals, so
hold-to-talk works on a compliant backend. It only standardizes keyboard
shortcuts. Side mouse buttons are unavailable, and some compositors will not
accept a modifier-only binding such as Right Ctrl; those desktops require a
regular key or chord such as F8 or Ctrl+F8. LocalFlow rejects unsupported mouse
bindings instead of silently changing their behavior.

RemoteDesktop consent is deliberately visible and may include a persistent
desktop control indicator. LocalFlow uses it only when AT-SPI direct insertion
fails. A compositor that does not expose keyboard control cannot support the
Ctrl+V fallback without an unsafe input-injection helper, so LocalFlow reports
that limitation rather than requesting root access.

## Build and test

```sh
cmake -S CrossPlatform/platform/linux -B build/linux-platform
cmake --build build/linux-platform
ctest --test-dir build/linux-platform --output-on-failure
```

When `dbus-run-session` is available, the test suite also starts an isolated
mock portal service and exercises the real QDBus wire signatures end to end:
session creation/binding and both shortcut edges, screenshot URI decoding,
RemoteDesktop device approval, key press/release ordering, and session close.

Optional native adapters are enabled when their development packages are
present (`libx11-dev`, `libxtst-dev`, `libatspi2.0-dev`). Wayland portal
transports additionally use Qt 6 Core, DBus, and Gui. A build without QtDBus
remains useful for minimal CI and produces explicit not-configured errors.

Runtime packages normally include `xdg-desktop-portal`, the matching GNOME or
KDE portal backend, PipeWire, `wl-clipboard` on Wayland, and `xclip` or `xsel`
on X11.

## Remaining platform limitations

- The command clipboard adapter preserves UTF-8 plain text. Release builds
  should inject the Qt clipboard adapter to preserve every MIME flavor (for
  example images and rich text) exactly like the macOS clipboard transaction.
- Device selection currently exposes the stable system-default device. Native
  PipeWire device enumeration can replace it without changing the core API.
- Wayland deliberately has no universal foreground-window identity API. AT-SPI
  metadata is best effort, so focus-change verification is weaker than X11 in
  applications that do not expose accessibility metadata.
- Certify current GNOME and KDE releases separately; other compositors may
  omit one or more portal interfaces and will be reported as unsupported.
