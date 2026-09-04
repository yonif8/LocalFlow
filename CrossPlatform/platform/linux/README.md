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
- A bounded AT-SPI2 focused-target snapshot containing application PID/id,
  unique bus name, accessible object path/id, role, editable state, and an
  explicit secure/non-secure/unknown classification. The exact object is
  revalidated immediately before insertion.
- AT-SPI2 focused-caret insertion through the same object that was just
  validated. Password/protected fields and unknown security state are always
  rejected; clipboard paste cannot bypass that decision.
- A full-fidelity Qt clipboard transaction that preserves every advertised
  MIME payload, including text, HTML, URI lists, images, custom binary data,
  zero-length formats, and an originally empty clipboard. A private random
  marker makes restoration conditional on LocalFlow's transient clipboard
  still being current, so a copy made by the user during the paste delay wins.
  Clipboard calls are safely dispatched to the Qt GUI thread.
- Clipboard/paste fallback with restoration on both success and failure. X11
  uses XTest (or `xdotool` when XTest was not built); Wayland uses an explicitly
  approved Qt/QDBus RemoteDesktop keyboard session. It sends a balanced Ctrl+V
  sequence and observes compositor session closure. No root helper,
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
| Active window metadata | EWMH | Exact focused AT-SPI target, or safe failure |
| Screen context | Active-window capture | Compositor-mediated Screenshot |
| Direct insertion | AT-SPI2 | AT-SPI2 |
| Paste fallback | Clipboard + XTest | Clipboard + approved RemoteDesktop portal |
| Audio | PipeWire, then PulseAudio | PipeWire, then PulseAudio |

Wayland intentionally prevents an application from discovering and capturing
arbitrary foreground windows or injecting global input without compositor
consent. LocalFlow works with those constraints instead of bypassing them.

## Wayland target safety

At push-to-talk press, `FocusedTargetProvider::snapshotFocusedTarget()` walks a
maximum of 4,096 accessibility nodes and 40 levels. Individual AT-SPI calls
have a 400 ms timeout and the tree walk has a 500 ms wall-clock budget. A valid
snapshot requires an application PID/id and the focused object's unique AT-SPI
bus name and object path. Missing or ambiguous identity is a safe failure.

Before insertion, `validateFocusedTarget()` requires the same application PID,
application id, bus name, object path, and toolkit accessible id, as well as a
currently focused, editable, explicitly non-secure object. Direct insertion
retains that validated accessible and writes through it. Clipboard fallback
rechecks immediately before Ctrl+V. A verified move to another normal text
field refuses the paste and can leave the transcript on the clipboard for
manual recovery. Unknown, inaccessible, non-editable, or protected targets
restore the prior clipboard and fail without injecting input.

The application shell should create one provider and share it with both
boundaries, then pass the press-time `ApplicationInfo` back at insertion:

```cpp
auto targets = makeAtSpiFocusedTargetProvider(capabilities);
screen = makeScreenContextBackend(SessionType::wayland, {}, targets);
inserter = TextInsertionCoordinator(
    std::move(accessibility), std::move(clipboard), std::move(paste), options, targets);

auto expected = screen->activeApplication(); // at push-to-talk press
inserter.insert(transcript, expected.value()); // after transcription
```

The shell must retain the complete `ApplicationInfo`; comparing only an app id
or PID is not sufficient when focus moves between fields in the same app.

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
When Qt Gui is available, an offscreen integration test also exercises the real
`QClipboard` backend from a worker thread: rich MIME and image round trips,
empty clipboard restoration, and a user-copy race.

Optional native adapters are enabled when their development packages are
present (`libx11-dev`, `libxtst-dev`, `libatspi2.0-dev`). Wayland portal
transports additionally use Qt 6 Core, DBus, and Gui. Qt Gui supplies the
production clipboard backend even when QtDBus is absent. A build without Qt
remains useful for minimal CI and uses a clearly degraded command clipboard.

Runtime packages normally include `xdg-desktop-portal`, the matching GNOME or
KDE portal backend, `at-spi2-core`, and PipeWire. Clipboard paste is disabled
when AT-SPI cannot verify the exact destination field. `wl-clipboard` on
Wayland or `xclip`/`xsel` on X11 is needed only by a build without the Qt
clipboard backend.

## Remaining platform limitations

- The production Qt adapter targets the standard clipboard used by Ctrl+V, not
  X11's separate PRIMARY selection. Clipboard managers or applications may
  normalize a MIME representation after taking ownership; LocalFlow restores
  the exact bytes Qt offered when the snapshot was taken.
- A build without Qt Gui falls back to clipboard commands and preserves only
  UTF-8 plain text. It skips restoration when the visible text changed, but it
  cannot distinguish a newer rich-text copy with the same plain text from its
  own transient value. This degraded adapter is not used in normal packages.
- Device selection currently exposes the stable system-default device. Native
  PipeWire device enumeration can replace it without changing the core API.
- Wayland deliberately has no universal foreground-window identity API. AT-SPI
  identity is exact when an application participates in accessibility, but
  applications that do not expose it are intentionally unavailable rather
  than receiving a blind paste.
- Certify current GNOME and KDE releases separately; other compositors may
  omit one or more portal interfaces and will be reported as unsupported.
