# Contributing to LocalFlow

LocalFlow is one product in one repository. The macOS implementation lives at
the repository root; Windows and Linux share the portable application under
`CrossPlatform/`. A feature is complete only when users receive the same
behavior on all three supported operating systems, apart from an explicit OS
substitution in `docs/FEATURE_PARITY.md`.

## Feature changes

For every user-visible behavior change:

1. Define or update its behavioral contract in `docs/FEATURE_PARITY.md`.
2. Implement the macOS, Windows, Linux X11, and Linux Wayland behavior in the
   same pull request. Keep product policy in shared/core code where practical;
   native adapters should contain only the OS mechanism.
3. Add a shared fixture when both implementations can consume the same data.
   Otherwise add equivalent native tests for the same success, cancellation,
   timeout, privacy, and recovery cases.
4. Update onboarding, settings, diagnostics, packaging, and update behavior on
   every platform the change affects.
5. Complete the pull-request parity checklist and explain any “unaffected” or
   platform-exception claim with concrete test evidence.

An OS limitation is not a silent opt-out. Document the reason, expose the
limitation in the app, preserve the user's transcript/data safely, and test the
substitute behavior. Split follow-up issues do not make a partially ported
feature complete.

## Validation and release policy

The public v1.3.0 release has automated validation evidence but incomplete
hands-on Windows/Linux certification. This is recorded in
[`docs/releases/v1.3.0.md`](docs/releases/v1.3.0.md); do not infer full parity
from the public release label or mark untested rows certified. As of 2026-09-05,
hands-on testing is deferred at the owner's request until machines are available.

The native macOS, Windows, and Linux workflows are the source of truth for
build dependencies and automated checks. Relevant lanes must pass before merge;
all three must pass at the exact stable release commit. Unsigned CI artifacts
are for engineering smoke tests only and must never be shared as downloads.

A stable version uses one `vX.Y.Z` tag and one GitHub release for all three
operating systems. `Scripts/publish.sh` and `.github/workflows/release.yml`
publish transactionally: the release stays a draft unless every expected
signed artifact verifies. A build or installer existing is not enough to mark
a feature **certified**; complete the real-system gates in
`docs/FEATURE_PARITY.md` first.

Windows and Linux CI signing credentials belong only in GitHub Actions secrets;
macOS local identities follow `Scripts/setup-signing.sh`. Runtime URLs, sizes,
hashes, versions, and licenses belong in the reviewed
`CrossPlatform/dependencies/runtime-lock.json`, never mutable CI variables.

## Fast development and release checks

- During iteration, run the affected Swift suites (`swift test --filter …`) or
  C++ test targets first. Do not build/sign installers for each code edit.
- Use `bash Scripts/test-macos.sh` for the complete Mac check. It builds the
  app and executes all tests in **one** release-mode `swift test` plan.
  Do not precede/follow it with a product-only build:
  switching plans recompiles dependencies. Windows/Linux lanes remain parallel.
- `Scripts/macos-toolchain.env` is the single CI Xcode pin. Both Mac workflows
  use `.github/actions/setup-macos`; local packaging uses the same preflight
  helper to check the Swift version required by `Package.swift` and the Metal
  tools before expensive work. A newer local full Xcode is allowed and logged.
- CI caches dependency downloads and compiled Swift dependencies, keyed by
  platform, toolchain, dependency locks, and build/cache helper versions.
  `Scripts/prune-macos-build-cache.py` derives all root targets from SwiftPM
  and removes their products/modules before saving and after restoring a cache.
  Our app and tests are rebuilt even on a hit. Runtime archive size/hash checks
  still run on every hit. Never cache signing material, user data, app binaries,
  or test results. Keep the pruning step in **both** Mac workflows.
  SwiftPM replans each run so removed root output-file maps are regenerated;
  do not re-enable build-manifest caching on this path.
  Exact-tag builds, model tests, signatures, and installer checks remain mandatory.
- `Scripts/release.sh X.Y.Z` invokes the tested packaging path directly; it
  does not compile again after tests. Keep its isolated `.build-release` scratch
  tree separate from development/IDE work and never run two packagers together.
- Build helpers print stage durations; CI also puts them in the run summary.
  Compare these measurements, not estimates, before claiming a speed improvement.
- Run `python3 -m unittest discover -s Tests/ReleaseWorkflowTests -v` when
  changing this orchestration. Build-only changes need no new app version or
  public release. Never rerun publication just to benchmark CI.
