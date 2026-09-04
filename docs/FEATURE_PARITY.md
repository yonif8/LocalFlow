# LocalFlow feature parity contract

This is the release contract for LocalFlow on macOS, Windows, and Linux. A row
may be marked complete only after its behavior and failure modes are exercised
on the platform's certification matrix.

Current product status: macOS is released. Windows and Linux are engineering
betas and are not yet public downloads.

Status meanings:

- **Released** — implemented, covered by regression tests, certified on real
  supported systems, and available as a signed public stable download.
- **In progress** — some implementation or automated coverage exists, but one
  or more release requirements remain. A passing build alone is not completion.
- **Not started** — no usable implementation exists on that platform.
- **Platform exception** — the OS cannot provide the identical mechanism; a
  documented, user-visible substitute and its failure path have been tested.

| Feature | Behavioral contract | macOS | Windows | Linux |
|---|---|---:|---:|---:|
| Hold-to-talk | Press begins capture, release ends it, Escape cancels, accidental taps are ignored | Released | In progress | In progress |
| Configurable triggers | Keyboard plus middle/side mouse trigger where the OS exposes it | Released | In progress | In progress |
| Microphone capture | Device selection, default fallback, warm mode, 16 kHz mono, live level | Released | In progress | In progress |
| Audio ducking | Lower output during capture and safely restore it on every exit path | Released | In progress | In progress |
| Local ASR | Parakeet TDT v3, automatic punctuation, model progress, no uploaded audio | Released | In progress | In progress |
| Local polish | S1-mini, deterministic prompt, app-aware tone, timeout, guardrails, fail-open | Released | In progress | In progress |
| Personal dictionary | Ordered replacements, casing, spoken punctuation and JSON persistence | Released | In progress | In progress |
| Screen terminology | Concurrent local OCR plus bounded accessibility metadata | Released | In progress | In progress |
| Learned terminology | Conservative correction, review/removal, 500-term and 10-alias bounds | Released | In progress | In progress |
| Text insertion | Direct insertion where reliable, safe paste fallback, Unicode typing fallback | Released | In progress | In progress |
| Clipboard safety | Preserve all formats and restore only if no third party changed the clipboard | Released | In progress | In progress |
| Recording HUD | Non-activating recording, processing and error status with live level | Released | In progress | In progress |
| History | Session-only, configurable 0–50 items, copy and clear | Released | In progress | In progress |
| Settings | General, Dictation, Polish, Dictionary, Insertion and About | Released | In progress | In progress |
| Onboarding | Capability permissions and resumable model-download progress | Released | In progress | In progress |
| Diagnostics | Capability report and user-approved, prefilled bug report | Released | In progress | In progress |
| Launch at login | User-controlled native startup registration | Released | In progress | In progress |
| Updates | User-visible manual check plus signed automatic update verification | Released | In progress | In progress |

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

## Release gates

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

Do not change a Windows or Linux cell to **Released** merely because its source,
CI build, or installer exists. Record the certification evidence in the release
pull request, then update the row as part of the same tagged release change.
