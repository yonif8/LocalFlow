# LocalFlow portable core

This directory contains product behavior that must remain identical on Windows
and Linux. It deliberately has no Qt, operating-system, inference-runtime, or
third-party dependencies. Platform adapters provide capture, OCR, inference,
and insertion; this library owns the deterministic text behavior around them.

Public headers live in `include/localflow/core`. Link the CMake target
`LocalFlow::Core`.

The first port covers:

- shared pipeline contracts and value types;
- streaming or batch mono PCM conversion from 8–96 kHz to the 16 kHz model
  rate, with a band-limited filter and deterministic end-of-stream flushing;
- the end-to-end dictation pipeline and privacy-safe stage diagnostics;
- personal-dictionary replacement and optional spoken punctuation;
- Unicode-safe insertion chunking;
- screen-term extraction, correction, and path reconstruction;
- bounded learned terminology (500 terms, 10 aliases per term); and
- the regression guardrails from the shipping macOS implementation.

The terminology code intentionally uses generic word-shape rules. Product or
company names are not hard-coded. Learned-term persistence remains an app-layer
responsibility so each platform can use atomic writes and its native data path;
`LearnedTerminologyBank::sanitized` is the required validation boundary after
decoding and before encoding.

`MonoResampler16k` is stateful. Pass each captured mono float buffer to
`process`, then call `finish` exactly when the utterance ends. `finish` extends
the filter at the final sample and emits the complete duration; dropping that
call would drop the filter-delayed tail. Invalid (`NaN`/infinite) buffers throw
before mutating stream state. For already-complete recordings,
`resample_mono_to_16khz` runs the same streaming implementation in one call.

`DictationPipeline` owns the shared stage order: transcription, personal
replacements, pre-polish terminology correction, fail-open polish, reassertion
of only terms that matched in the pre-pass, insertion, and finally terminology
learning. Screen terms and the target application are supplied in the ended
request from the press-time snapshot; the pipeline never looks up a newer
foreground application. Learning is committed only after successful insertion
and only for corrections still represented in the final text. Diagnostics
contain timings, enum outcomes, and counts—not transcript or terminology text.

Build and run the standalone tests with:

```sh
cmake -S CrossPlatform/tests/core -B build/core-tests
cmake --build build/core-tests
ctest --test-dir build/core-tests --output-on-failure
```
