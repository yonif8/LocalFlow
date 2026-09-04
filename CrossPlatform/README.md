# LocalFlow for Windows and Linux

This directory contains the Windows and Linux editions of LocalFlow. They share
one portable product core and use small native adapters where the operating
systems differ.

The released macOS implementation remains in the repository root. It is the
current behavioral reference while the portable core is established; shared
golden fixtures prevent the implementations from drifting.

## Architecture

```text
CrossPlatform/
  core/                 Dictionary, terminology, pipeline contracts, persistence
  inference/            Parakeet ASR and S1-mini polish model adapters
  platform/windows/     Win32, WASAPI, UI Automation and Graphics Capture
  platform/linux/       X11, Wayland portals, PipeWire and AT-SPI
  app/                   Shared Qt/QML tray app, settings, onboarding and HUD
  tests/                 Portable behavior and platform contract tests
```

The dependency direction is deliberate: `core` knows nothing about Qt or an
operating system; `inference` and each `platform` implement core interfaces; the
`app` composes them. No platform adapter may contain terminology, dictionary, or
polishing policy.

## Initial supported targets

- Windows 11 x86-64
- Ubuntu/Fedora x86-64 on recent GNOME or KDE Wayland
- Mainstream x86-64 X11 desktops

Other architectures and Linux compositors are added only after their complete
capability and regression matrices pass.

## Product rules

1. Nothing leaves the device except explicit model/update downloads and a bug
   report the user chooses to submit.
2. OCR starts on push-to-talk press and never delays processing after release.
3. Every model stage fails open; captured speech is never discarded because a
   polish or screen-context stage failed.
4. Clipboard and audio state are restored conditionally and safely.
5. Unsupported OS capabilities are shown to the user instead of failing
   silently.
6. A feature is not release-ready until `docs/FEATURE_PARITY.md` and its shared
   regression fixtures cover macOS, Windows, and Linux.

## Model stack

- ASR: NVIDIA Parakeet TDT 0.6B v3 through NeMo-Speech.cpp
- Polish: S1-mini by Superwhisper through llama.cpp
- OCR: selected by a cross-platform technical-token benchmark; it must remain
  fully local

Models are downloaded on first run with progress, resumption, size limits, and
cryptographic checksum verification.

## Build status

The cross-platform application is under active development and is not yet a
public download. Build and test commands will be added here as each target
becomes runnable. Do not distribute development artifacts to end users.
