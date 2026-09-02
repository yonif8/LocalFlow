# LocalFlow — agent notes

Fully-local macOS dictation (Wispr Flow replacement). Hold Fn (or a mouse
button) → speak → release → polished text lands at the caret. Menu bar agent
app, SwiftPM, AppKit lifecycle (no Xcode project). Public repo + releases:
github.com/yonif8/LocalFlow.

## Pipeline

mic (LFCapture: CGEventTap hold-to-talk + AVAudioEngine 16kHz)
→ ASR (LFEngine: **Parakeet TDT v3** via FluidAudio CoreML — verbatim,
  natively punctuated; the only engine since 1.1.0, Granite was removed)
→ polish (LFPolish: dictionary replacements always; **S1-mini** LLM via
  mlx-swift-lm — fillers/self-corrections/number formatting, FAIL-OPEN with
  hard timeout + plausibility guardrail; output never blocks on the LLM)
→ insert (LFInsert: AX-at-caret, checks settability FIRST — Catalyst apps
  lie — then pasteboard+Cmd-V with clipboard restore)
Contracts in `Sources/LFContracts` (Utterance/Transcriber/TextPolisher/…).

## Commands

- Build/test: `swift build` / `swift test` (CLT is enough; no Xcode needed).
- Debug CLIs: `engine-cli <wav>`, `polish-cli`, `capture-cli`, `insert-cli
  --doctor`. Fixtures in `Fixtures/`.
- Release: `Scripts/release.sh X.Y.Z` → commit `appcast.xml` →
  `Scripts/publish.sh X.Y.Z` (gh CLI at ~/.local/bin/gh, authed as yonif8).
  Sparkle auto-updates every install; version every user-visible change.
- Headless app driving: distributed notifications `com.localflow.app.simulate`
  / `.showSettings` / `.checkForUpdates`; env `LOCALFLOW_SIM_WAV=<wav>` feeds
  real audio to Simulate Dictation. Logs: `log show --info --predicate
  'subsystem BEGINSWITH "com.localflow"'` (pipeline stage timings, raw
  transcript at debug, per-second level peaks, insertion strategy per app).

## Hard-won gotchas (violate at your peril)

1. **Stale builds**: another agent session may share `.build/`; its llbuild
   state has served hour-old binaries that "Build complete!" in 0.3s and
   SHIPPED missing features. Releases always build in `.build-release/`
   (release.sh enforces it). If a change doesn't show up, suspect this first.
2. **Do not verify Swift features with `strings | grep`** — short literals
   are inlined by the compiler. Grep a long literal, or test behavior.
3. **Signing**: self-signed cert "LocalFlow Signing" (keychain
   `localflow-signing.keychain`, password `localflow-signing`, created by
   Scripts/setup-signing.sh). Sign inside-out, no `--deep`, NO hardened
   runtime (breaks mic). The cert-anchored designated requirement is why TCC
   permissions survive updates — never switch to ad-hoc.
4. **Sparkle EdDSA private key** lives in the login keychain and is
   IRREPLACEABLE (no Apple signing backstop). Never print/commit it; never
   remove SUPublicEDKey from Info.plist in an update.
5. **Polish must fail open, fast**: Swift task groups await all children —
   a timeout that uses one gets held hostage by an LLM that ignores
   cancellation (this shipped as 8-10s hangs once). `withTimeout` in
   Polisher.swift races via continuation; keep it that way.
6. **The guardrail must allow number formatting** ("three thirty pm" →
   "3:30pm" shares no letters) — numberWords exclusion in looksLikeCleanup.
7. **mlx.metallib** must sit next to the app binary (make-app.sh copies it)
   or S1-mini dies at runtime with a cryptic Metal error.
8. Models: Parakeet in FluidAudio's own App Support cache; S1-mini in
   `~/Library/Application Support/LocalFlow/s1-mini`. First-run download UI
   is driven by ModelSetupState via progress callbacks.

## Backlog (user-endorsed, not started)

- Screen-aware dictionary: harvest on-screen terms via AX at hotkey-press,
  feed to S1/polish as preferred vocabulary (user liked this idea a lot).
- Command mode (speak an edit over selected text), text snippets.
- Fresh-Mac install test on the user's second Mac (README-only walkthrough;
  Gatekeeper "Open Anyway" path never live-tested).
- Hebrew shelved (would need a Whisper-class engine; see ivrit.ai models).

User memory (auto-memory dir) has deeper history. Sibling project RicoLive
(~/Projects/RicoLive) is separate — don't mix.
