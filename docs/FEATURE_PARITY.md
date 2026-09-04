# LocalFlow feature parity contract

This is the release contract for LocalFlow on macOS, Windows, and Linux. A row
may be marked complete only after its behavior and failure modes are exercised
on the platform's certification matrix.

Current availability: v1.3.0 is public for macOS, Windows, and Linux.
Windows/Linux automated production builds, inference, signature verification,
and installer smoke tests passed. Hands-on certification remains pending.
As of 2026-09-05 it is deferred at the owner's request until test machines are
available. See [release evidence](releases/v1.3.0.md). Availability and
certification are tracked separately below.

Status meanings:

- **Existing macOS release** — established macOS behavior, with its existing
  regression coverage and user testing; not a claim that every app is certified.
- **Available; certification pending** — included in public Windows/Linux
  packages with automated coverage, but the full hands-on matrix is incomplete.
- **In progress** — some implementation or automated coverage exists, but one
  or more release requirements remain. A passing build alone is not completion.
- **Not started** — no usable implementation exists on that platform.
- **Platform exception** — the OS cannot provide the identical mechanism; a
  documented, user-visible substitute and its failure path have been tested.

| Feature | Behavioral contract | macOS | Windows | Linux |
|---|---|---:|---:|---:|
| Hold-to-talk | Press begins capture, release ends it, Escape cancels, accidental taps are ignored | Existing macOS release | Available; certification pending | Available; certification pending |
| Configurable triggers | Keyboard plus middle/side mouse trigger where the OS exposes it | Existing macOS release | Available; certification pending | Available; certification pending |
| Microphone capture | Device selection, default fallback, warm mode, 16 kHz mono, live level | Existing macOS release | Available; certification pending | Available; certification pending |
| Audio ducking | Lower output during capture and safely restore it on every exit path | Existing macOS release | Available; certification pending | Available; certification pending |
| Local ASR | Parakeet TDT v3, automatic punctuation, model progress, no uploaded audio | Existing macOS release | Available; certification pending | Available; certification pending |
| Local polish | S1-mini, deterministic prompt, app-aware tone, timeout, guardrails, fail-open | Existing macOS release | Available; certification pending | Available; certification pending |
| Personal dictionary | Ordered replacements, casing, spoken punctuation and JSON persistence | Existing macOS release | Available; certification pending | Available; certification pending |
| Screen terminology | Concurrent local OCR plus bounded accessibility metadata | Existing macOS release | Available; certification pending | Available; certification pending |
| Learned terminology | Conservative correction, review/removal, 500-term and 10-alias bounds | Existing macOS release | Available; certification pending | Available; certification pending |
| Text insertion | Direct insertion where reliable, safe paste fallback, Unicode typing fallback | Existing macOS release | Available; certification pending | Available; certification pending |
| Clipboard safety | Preserve all formats and restore only if no third party changed the clipboard | Existing macOS release | Available; certification pending | Available; certification pending |
| Recording HUD | Non-activating recording, processing and error status with live level | Existing macOS release | Available; certification pending | Available; certification pending |
| History | Session-only, configurable 0–50 items, copy and clear | Existing macOS release | Available; certification pending | Available; certification pending |
| Settings | General, Dictation, Polish, Dictionary, Insertion and About | Existing macOS release | Available; certification pending | Available; certification pending |
| Onboarding | Capability permissions and resumable model-download progress | Existing macOS release | Available; certification pending | Available; certification pending |
| Diagnostics | Capability report and user-approved, prefilled bug report | Existing macOS release | Available; certification pending | Available; certification pending |
| Launch at login | User-controlled native startup registration | Existing macOS release | Available; certification pending | Available; certification pending |
| Updates | User-visible manual check plus signed automatic update verification | Existing macOS release | Available; certification pending | Available; certification pending |

## Known implementation and OS limitations

- **Windows capture:** GDI is implemented; Windows Graphics Capture is not.
  Some GPU-rendered surfaces may provide incomplete or unusable OCR pixels.
- **Linux Wayland controls:** arbitrary global mouse triggers and conditional
  global Escape interception are unavailable through the current adapter.
- **Linux DEB updates:** open Releases for manual package installation rather
  than replacing package-managed files. AppImage supports in-app updates.

## Deliberate OS substitutions

- **Fn/Globe:** most Windows and Linux keyboards do not expose this firmware key.
  Those editions use a configurable ordinary key by default.
- **Linux Wayland:** shortcuts, screenshots and synthetic input use consented XDG
  desktop portals. The portal UI and privacy indicator belong to the desktop.
- **Secure surfaces:** password fields, protected media, lock screens, secure
  desktops and higher-privilege applications are never bypassed. LocalFlow falls
  back to copying the transcript and explains what happened.
- **HUD placement and tray:** Wayland compositors and stock GNOME may control
  placement or tray visibility. The same state remains available through the
  closest compositor-approved surface.

These substitutions are not permission to omit a workflow. Each must have a
tested fallback that gets the user's transcript back safely.

## Certification and future release gates

- The exact tagged commit passes the macOS, Windows, and Linux native CI lanes
  with production model runtimes; stubs and unsigned smoke artifacts are barred.
- Shared or equivalent transcript, dictionary, terminology, polish, and
  insertion regressions pass in every implementation.
- Real supported systems complete press/release/cancel, microphone selection,
  local OCR, ASR, polish, safe insertion, clipboard restoration, audio
  restoration, history, settings, launch-at-login, and diagnostics checks.
- No first- or final-syllable clipping, duplicate insertion, cross-field paste,
  clipboard loss, or stale audio restoration occurs across the certified app
  matrix. Release never waits for OCR.
- With models installed and networking blocked, dictation and polish still work
  and no captured audio, screen pixels, recognized text, or learned terms leave
  the device.
- A clean supported machine can install, onboard, and uninstall while
  preserving user data. An authenticated predecessor-to-candidate update is
  proven; a platform's first public release uses an equivalent authenticated
  update rehearsal.
  Secure/elevated targets and unavailable desktop capabilities fail safely with
  an in-app explanation.
- The public release contains the same stable version for all three operating
  systems. macOS update metadata, the Windows LocalFlow Ed25519 update
  signature, and Linux artifacts/checksums are signed and verified before
  publication. Authenticode is optional for the free Windows distribution.

Do not mark a Windows/Linux feature certified merely because its source, CI
build, or installer exists. Record actual certification evidence in the release
pull request before changing its status. The public v1.3.0 release does not
satisfy all of the hands-on gates above; keep that gap visible.
