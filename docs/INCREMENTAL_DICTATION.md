# Incremental push-to-talk

Target: roughly two seconds from hotkey release to completed insertion. This is
a performance aim, not a deadline that discards speech or bypasses cleanup.

## Measured starting policy

`dictation-bench` runs real Parakeet and S1-mini sequentially on a supplied audio
file, with three warm measurements per length. It logs timings/counts, not text.
Initial calibration on this M1 Max used generated English speech at 185 wpm:

| Audio | Slowest ASR + polish |
| --- | --- |
| 12 seconds | 0.503 seconds |
| 24 seconds | 0.823 seconds |
| 36 seconds | 1.116 seconds |
| 48 seconds | 1.368 seconds |
| 60 seconds | 1.765 seconds |

Insertion previously measured about 0.34 seconds through paste. The initial
pause-search threshold is `(2 - 0.35 - 0.5) / (0.503 / 12)`, about 27.4 seconds.
The 0.5-second headroom allows for carry-over text and load variation. This is
conservative calibration, not a universal audio-to-processing conversion.
Slower observed processing tightens the threshold for the current app process;
one fast/silent chunk cannot loosen it. Calibration is not persisted yet.
Windows/Linux share the C++/Qt implementation and start with the same seed;
their ONNX/llama runtimes and hardware can be slower, so this is not a claim
that they reproduce the Mac timings. Runtime feedback tightens their threshold.

## Operation and safety

- Capture keeps its original PCM. At a quiet boundary after the threshold, take
  a non-destructive snapshot of only the unprocessed samples.
- Require 650 ms of quiet meter readings and recheck the actual snapshot's last
  300 ms. Delayed UI events must not split newly resumed speech.
- Serial background tasks transcribe and polish while the capture event pump
  continues. Each task depends on the preceding result; failure is not skipped.
- Hold the last recognized sentence for the following segment. It is polished
  with the next words, including the final words after release.
- Nothing is inserted before release. At release, finish the exact remaining
  sample range, flush held words, assemble in order, and insert once.
- Cancel invalidates the session and suppresses its pending result. On segment
  failure, retry using the retained original recording rather than inserting a
  partial transcript. The original audio is retained in memory, not saved.
- Short dictation follows the existing single-pass path.

## Limits and testing

There is no forced mid-word cut: continuous speech or persistent background
noise can delay a boundary and exceed the latency aim. A pause is not proof of
a sentence ending; last-sentence carry reduces but cannot eliminate model
boundary errors. Existing polish limits and fallback behavior still apply to
unusually long unbroken text. Recovery can also exceed two seconds.

The synthetic benchmark is not certification for all voices, speaking rates,
Bluetooth devices, or CPU/GPU load. Test with the user's real speech before
publication. Audio retention grows with recording duration, as it did before.

Unit tests cover ordering, serial ASR, sentence carry, quiet tails, short
dictation, cancellation/new-session isolation, failure propagation, calibration,
and rejecting stale quiet-meter boundaries. Preview is built under
`dist/diagnostics.noindex/`; never overwrite the stable installed app for testing.
Windows/Linux use the shared `IncrementalDictation` core and Qt application
executor, with non-destructive native sample snapshots. Sentence carry uses
Qt Unicode boundaries. Their private terminology bank is committed only after
the final real insertion succeeds. Native hardware/user testing remains pending.

## Local verification, 2026-09-06

- 87 Swift tests pass (after removing the abandoned experimental feature).
- Actual hotkey + microphone + OCR run: 43.8 seconds of audio, one background
  chunk completed before release; release processing + insertion took 0.324 s.
- A second run with distinct speech throughout captured 47.6 seconds. A 42.7 s
  chunk processed in 1.135 s while recording continued. TextEdit remained empty
  before release. Final processing + insertion took 0.318 s, and the expected
  final request appeared after the complete preceding passage.
- These runs used speaker playback into the real microphone, not injected
  capture events or a mocked speech engine. TextEdit used direct accessibility
  insertion; Electron apps may add the measured ~0.34 s paste overhead.
- Final build cancellation check: a 40.6 s background chunk completed in
  1.089 s; Escape then cancelled the recording. TextEdit remained empty and
  there was no release/insertion event for that recording.
- The immediately following 4.6 s short dictation inserted the expected fixture
  text in 0.262 s after release, with no cancelled-session text carried over.
- C++ portable validation: 67 existing core tests and six incremental scenarios
  pass. Qt sentence-carry tests cover Unicode, file terms and unfinished text.
  The shared Qt controller and Linux bridge also pass local syntax checks with
  the pinned Qt 6.8.3 headers. Native builds are verified by the release gate;
  these local checks are not a substitute for hands-on Windows/Linux testing.
