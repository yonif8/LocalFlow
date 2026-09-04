# LocalFlow Windows platform adapter

This directory is the native Windows edge of LocalFlow's cross-platform app.
It is C++17, has no UI or model dependency, and is designed to sit behind the
shared pipeline rather than duplicate dictation behavior.

## Implemented

- Stable foreground-window identity using HWND, PID, package family, and the
  classic executable path.
- Non-consuming global keyboard and mouse PTT hooks with press, release, and
  Escape/stop cancellation semantics.
- A dedicated callback dispatcher so slow microphone/model work cannot cause
  Windows to remove the low-level hook.
- WASAPI shared-mode capture, device enumeration, native PCM-to-float
  conversion, mono downmix, and a bounded post-release tail drain.
- Default-output audio ducking that conditionally restores LocalFlow's exact
  endpoint/volume without overwriting a user's in-flight adjustment or mute.
- Exact editable-field identity through UI Automation (with a guarded native
  Edit/RichEdit fallback), protected-field refusal, and fail-closed focus
  revalidation immediately before clipboard or `KEYEVENTF_UNICODE` insertion.
- Full-format clipboard preservation through OLE, sequence-number guarded
  restoration, Ctrl+V insertion, and `KEYEVENTF_UNICODE` fallback.
- A capture backend interface plus a dependency-free foreground-window GDI
  implementation that produces local BGRA frames for OCR.
- Fully local `Windows.Media.Ocr` recognition with an asynchronous future API,
  language selection, bilinear high-DPI downscaling, concurrency limits, hard
  deadlines/cancellation, and bounded terminology-preserving text cleanup.
- Platform-neutral PTT transition tests that run on macOS/Linux CI as well as
  Windows.

## Build

On Windows with Visual Studio 2022:

```powershell
cmake -S CrossPlatform/platform/windows -B build/windows -A x64
cmake --build build/windows --config Release
ctest --test-dir build/windows -C Release --output-on-failure
```

The repository-level `CrossPlatform/CMakeLists.txt` already composes this
adapter with the shared Qt application and inference layers. Use the full-app
commands in the [cross-platform build guide](../../README.md) when validating a
distributable build.

On non-Windows hosts, this subproject intentionally builds only
`LocalFlow::WindowsState` and its tests.

## Product wiring

The app coordinator should keep one `FocusedTextTargetCapture` from the PTT
press and follow this sequence:

1. On `pressed`, call `capture_focused_text_target()`. Start a dictation only
   when `safe_for_insertion()` is true. A `protected_content` result is a hard
   refusal; do not record or retain speech for a password field. Duck output
   audio and start WASAPI capture. In parallel, submit
   `IScreenCapture::capture()` to a background worker and pass its BGRA frame
   to `WindowsMediaOcr::recognize_async()`.
2. On `released`, stop capture, restore audio immediately, run shared ASR,
   terminology, and polish, then call
   `ForegroundTextInserter::insert_utf8_into_focused_target()` with the
   captured identity.
3. On `cancelled`, stop capture and restore audio without transcribing or
   inserting.
4. Inspect `TextInsertionOutcome::target_status`. If focus moved to another
   field—even inside the same top-level window—or the field became protected
   or read-only, insertion fails before synthetic input. Preserve the final
   transcript for an explicit copy/retry action. Never retry dictation through
   the legacy window-only `insert_utf8()` overload.
5. Queue `AudioChunk` data quickly in its callback. Resample the mono native-rate
   stream to 16 kHz in the shared core so all platforms share one algorithm.

Keep the OCR future from step 1 with the active utterance. At PTT release, use
its result only if `wait_for(0ms)` says it is ready; screen context is helpful
but must never delay transcription. The implementation invokes only the local
Windows Runtime OCR engine. It contains no networking code and neither pixels
nor recognized text are uploaded.

`ForegroundTextInserter` is synchronous because the transient clipboard must
remain alive long enough for asynchronous Electron/Chromium paste handlers.
Call it on the pipeline worker, not the UI thread.

## Windows-specific UX and safety constraints

- Most laptop **Fn** keys are handled in keyboard firmware and never reach
  Win32. The shipped default should therefore be user-selectable (F8 and side
  mouse button are sensible choices; F8 is the adapter default), with the
  onboarding screen listening for the user's preferred key.
- Windows prevents a normal-integrity process from injecting into an elevated
  process (UIPI), and secure-desktop/password fields intentionally reject
  capture or insertion. The UI should report this clearly; LocalFlow should not
  request administrator privileges to bypass it.
- Clipboard restoration never overwrites a copy the user performs during the
  paste delay. The retained OLE `IDataObject` preserves images, files, HTML,
  rich text, and delayed-rendered formats—not only plain text.
- The GDI capture backend is a reliable baseline for ordinary foreground apps.
  A production Windows 11 build should add a Windows Graphics Capture
  `IScreenCapture` implementation for GPU-rendered surfaces. The shared OCR and
  terminology layers do not need to change when that backend is swapped in.
- `PrintWindow` can be slow for a hung target. Capture must stay on the existing
  PTT background path and must never be awaited at key release, matching the
  macOS fail-open behavior.
- WASAPI reports device invalidation through the error callback. The coordinator
  should end the active hold safely, restore ducking, and offer the newly
  selected default microphone on the next hold.

## Remaining Windows release work

The shared Qt shell, Parakeet/S1 runtimes, persistence, Inno Setup packaging,
and authenticated update client now live in the app and release layers. Their
presence does not make this adapter a public download. Windows remains an
engineering beta until the exact-field APIs are exercised through the complete
app, Windows Graphics Capture covers GPU-rendered surfaces, the real application
matrix passes on clean Windows 11 systems, and the installer/update path passes
the pinned LocalFlow Ed25519 release-signature gates. Authenticode is an optional
additional identity layer for a future paid-certificate distribution. Track
those gates in the [feature parity contract](../../../docs/FEATURE_PARITY.md);
do not distribute CI smoke artifacts.
