## What changed

<!-- Describe user-visible behavior, not only implementation details. -->

## Parity evidence

<!-- Link the parity row and tests, or explain concretely why a platform is unaffected. -->

- macOS:
- Windows:
- Linux X11:
- Linux Wayland:

## Platform parity

- [ ] macOS behavior implemented and tested, or “unaffected” is justified above
- [ ] Windows behavior implemented and tested, or “unaffected” is justified above
- [ ] Linux X11 behavior implemented and tested, or “unaffected” is justified above
- [ ] Linux Wayland behavior implemented and tested, or a tested exception is documented
- [ ] `docs/FEATURE_PARITY.md` updated when the feature contract/status changed
- [ ] Shared fixtures were updated, or equivalent native regressions cover the same contract

## Quality gates

- [ ] Clipboard, audio and focus restoration paths considered
- [ ] Offline/privacy behavior unchanged or documented
- [ ] Unsupported capability has a visible fallback
- [ ] Install/update impact considered
- [ ] Relevant native CI lanes pass; all three are required for a stable release
- [ ] No unsigned CI artifact is presented as an end-user download
