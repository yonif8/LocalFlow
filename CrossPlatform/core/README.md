# LocalFlow portable core

This directory contains product behavior that must remain identical on Windows
and Linux. It deliberately has no Qt, operating-system, inference-runtime, or
third-party dependencies. Platform adapters provide capture, OCR, inference,
and insertion; this library owns the deterministic text behavior around them.

Public headers live in `include/localflow/core`. Link the CMake target
`LocalFlow::Core`.

The first port covers:

- shared pipeline contracts and value types;
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

Build and run the standalone tests with:

```sh
cmake -S CrossPlatform/tests/core -B build/core-tests
cmake --build build/core-tests
ctest --test-dir build/core-tests --output-on-failure
```

