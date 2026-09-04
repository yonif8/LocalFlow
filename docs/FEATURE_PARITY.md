# LocalFlow feature parity contract

This is the release contract for LocalFlow on macOS, Windows, and Linux. A row
may be marked complete only after its behavior and failure modes are exercised
on the platform's certification matrix.

Status: **Released**, **In progress**, **Not started**, or **Platform exception**.

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

- Shared transcript, dictionary and terminology regression suites pass.
- No duplicate insertion or clipboard loss across the certified application matrix.
- Push-to-talk does not clip the first syllable and release never waits for OCR.
- Cancellation, errors and forced shutdown restore audio state.
- The app works with networking blocked after models are installed.
- Installers and update metadata are signed; a clean machine can install,
  onboard, update and uninstall successfully.
- Unsupported capabilities are diagnosed in-app rather than silently ignored.
