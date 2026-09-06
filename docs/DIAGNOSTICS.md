# Local dictation diagnostics

Keep the working installation in `/Applications/LocalFlow.app`. Build test apps
under `dist/diagnostics.noindex/` so Spotlight does not show duplicate apps.
Never run both copies together: they share the hotkey and model resources.

```sh
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
Scripts/make-app.sh --version 1.4.4 --scratch-path "$PWD/.build-release" \
  --output-dir "$PWD/dist/diagnostics.noindex"
```

`--diagnostic-mode` uses process-only preferences: screen terminology on,
mic warming and audio ducking off. Preferences are not overwritten and learned
terminology writes are suppressed. Push-to-talk still uses the actual event tap,
microphone, transcription, terminology, polish and insertion adapters.

## Privacy-safe evidence

The macOS unified log has system-managed retention. Dictation logs report stage
timings, sample/character counts and error codes. Background chunks report audio
duration, sample offsets, processing time and the adaptive pause-search threshold.
Terminology logs report expansion sizes and skipped aliases, not raw terms.
Recognized speech and model output are not logged by the current pipeline.

```sh
/usr/bin/log show --last 15m --info --style compact \
  --predicate 'subsystem BEGINSWITH "com.localflow"'
```

Old versions' historical logs may contain transcript text. Do not upload logs
automatically. OS crash reports are in `~/Library/Logs/DiagnosticReports/`.

## Regressions covered

- Long screen-derived paths previously produced an invalid closed integer range
  in terminology matching. Oversized aliases are now skipped safely while valid
  shorter aliases remain usable. Boundary tests reproduce the original crash.
- The 700-character polish cutoff skipped otherwise processable long dictations.
  Current defaults allow 4000 characters with a 3-second base deadline (at most
  6 seconds for longer text). User-configured limits remain respected.
- See [incremental dictation](INCREMENTAL_DICTATION.md) for measured chunk timing,
  release latency, cancellation and real microphone test evidence.

Known model limitation: unquoted mentions of the word "um" may be mistaken for
fillers. Do not claim all semantic filler distinctions are solved or add a blanket
word-deletion rule.
