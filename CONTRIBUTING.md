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

The native macOS, Windows, and Linux workflows are the source of truth for
build dependencies and automated checks. Relevant lanes must pass before merge;
all three must pass at the exact stable release commit. Unsigned CI artifacts
are for engineering smoke tests only and must never be shared as downloads.

A stable version uses one `vX.Y.Z` tag and one GitHub release for all three
operating systems. `Scripts/publish.sh` and `.github/workflows/release.yml`
publish transactionally: the release stays a draft unless every expected
signed artifact verifies. A build or installer existing is not enough to mark
a feature **Released**; complete the real-system gates in
`docs/FEATURE_PARITY.md` first.

Windows and Linux CI signing credentials belong only in GitHub Actions secrets;
macOS local identities follow `Scripts/setup-signing.sh`. Runtime URLs, sizes,
hashes, versions, and licenses belong in the reviewed
`CrossPlatform/dependencies/runtime-lock.json`, never mutable CI variables.
