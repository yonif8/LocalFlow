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
- An event-loop-neutral `GlobalShortcutsPortal` boundary mapping Wayland
  `Activated` and `Deactivated` signals to push-to-talk edges.
- X11 active-app metadata through EWMH and an active-window BGRA capture for
  the shared OCR pipeline.
- A persistent-session `ScreenCastPortal` boundary for Wayland capture. The UI
  transport owns consent and restore tokens; capture is never silently faked.
- AT-SPI2 focused-caret insertion, with bounded accessibility traversal.
- Clipboard/paste fallback with restoration on both success and failure.
  X11 uses XTest (or `xdotool` when XTest was not built); Wayland uses an
  explicitly approved `RemoteDesktopPortal` transport. No root helper,
  `/dev/uinput`, or `ydotool` path exists.
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
| Screen context | Active-window capture | User-selected ScreenCast stream |
| Direct insertion | AT-SPI2 | AT-SPI2 |
| Paste fallback | Clipboard + XTest | Clipboard + approved RemoteDesktop portal |
| Audio | PipeWire, then PulseAudio | PipeWire, then PulseAudio |

Wayland intentionally prevents an application from discovering and capturing
arbitrary foreground windows or injecting global input without compositor
consent. LocalFlow works with those constraints instead of bypassing them.

## Portal UI integration

The three portal interfaces are intentionally event-loop neutral:

- `GlobalShortcutsPortal`: create a session, bind once, and forward both
  `Activated` and `Deactivated`.
- `ScreenCastPortal`: run CreateSession/SelectSources/Start, consume its
  PipeWire node, and persist the restore token where supported.
- `RemoteDesktopPortal`: request keyboard access and implement `sendKeysym`
  using `NotifyKeyboardKeysym` or libei.

The Qt application should implement these with QDBus so consent dialogs have a
real parent window and signals are delivered on the UI event loop. Tests use
small fake transports and therefore do not require a running desktop.

## Build and test

```sh
cmake -S CrossPlatform/platform/linux -B build/linux-platform
cmake --build build/linux-platform
ctest --test-dir build/linux-platform --output-on-failure
```

Optional native adapters are enabled when their development packages are
present (`libx11-dev`, `libxtst-dev`, `libatspi2.0-dev`). The dependency-free
build remains useful for CI and produces explicit missing-dependency errors.

Runtime packages normally include `xdg-desktop-portal`, the matching GNOME or
KDE portal backend, PipeWire, `wl-clipboard` on Wayland, and `xclip` or `xsel`
on X11.

## Known integration work

- The Qt portal transports and full-fidelity Qt clipboard adapter belong in
  the Linux application shell, not this event-loop-neutral layer.
- The command clipboard adapter preserves UTF-8 plain text. Release builds
  should inject the Qt clipboard adapter to preserve every MIME flavor (for
  example images and rich text) exactly like the macOS clipboard transaction.
- Device selection currently exposes the stable system-default device. Native
  PipeWire device enumeration can replace it without changing the core API.
- Certify current GNOME and KDE releases separately; other compositors may
  omit one or more portal interfaces and will be reported as unsupported.

