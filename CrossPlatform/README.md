# LocalFlow for Windows and Linux

This directory contains the Windows and Linux editions of LocalFlow. They share
one portable product core and use small native adapters where the operating
systems differ.

The released macOS implementation remains in the repository root. It is the
current behavioral reference while the portable core is established. Product
changes belong in this one repository and must update all affected editions in
the same pull request unless `docs/FEATURE_PARITY.md` records a tested operating
system substitution.

## Architecture

```text
CrossPlatform/
  core/                 Dictionary, terminology, pipeline contracts, persistence
  inference/            Parakeet ASR and S1-mini polish model adapters
  platform/windows/     Win32, WASAPI, UI Automation, GDI capture and Windows OCR
  platform/linux/       X11, Wayland portals, PipeWire and AT-SPI
  app/                   Shared Qt/QML tray app, settings, onboarding and HUD
  tests/                 Portable behavior and platform contract tests
```

The dependency direction is deliberate: `core` knows nothing about Qt or an
operating system; `inference` and each `platform` implement core interfaces; the
`app` composes them. No platform adapter may contain terminology, dictionary, or
polishing policy.

## Package targets

- Windows 11 x86-64
- Linux x86-64, packaged on Ubuntu 22.04, with X11 or desktop-provided Wayland portals

Hands-on Windows 11 and Linux X11/GNOME/KDE certification is pending. Fedora
and individual compositor versions are not certified by the Ubuntu CI run.
See the root README for installation instructions and platform limitations.

## Product rules

1. Nothing leaves the device except explicit model/update downloads and a bug
   report the user chooses to submit.
2. OCR starts on push-to-talk press and never delays processing after release.
3. Every model stage fails open; captured speech is never discarded because a
   polish or screen-context stage failed.
4. Clipboard and audio state are restored conditionally and safely.
5. Unsupported OS capabilities are shown to the user instead of failing
   silently.
6. A feature is not release-ready until `docs/FEATURE_PARITY.md` and shared or
   equivalent native regressions cover macOS, Windows, and Linux.

## Model stack

- ASR: NVIDIA Parakeet TDT 0.6B v3 through NeMo-Speech.cpp
- Polish: S1-mini by Superwhisper through llama.cpp
- OCR: Windows OCR with GDI capture on Windows; Tesseract on Linux. Both run locally.

Models are downloaded on first run with progress, resumption, size limits, and
cryptographic checksum verification.

## Development status

Windows and Linux packages are public in v1.3.0. Automated production builds,
real-model inference, signature checks, and installer smoke tests passed.
Hands-on certification across real applications and Linux desktops remains
pending; it is explicitly deferred until test machines are available.
Use public Releases downloads, not unsigned GitHub Actions smoke artifacts.
See the [release evidence](../docs/releases/v1.3.0.md).

Windows currently uses GDI capture; Windows Graphics Capture is not implemented.
GPU-rendered surfaces may not provide useful OCR pixels. Wayland functionality
depends on available desktop portals and application accessibility support.

The checked-in CMake presets provide the shortest local build path. Bootstrap
the checksum-pinned runtimes first, then build and test from `CrossPlatform/`:

```sh
# Linux
dependencies/bootstrap-linux.sh
source dependencies/.runtime/linux-$(uname -m)/activate.sh
cmake --preset linux-release -DLOCALFLOW_REQUIRE_INFERENCE=ON -DLOCALFLOW_REQUIRE_OCR=ON
cmake --build --preset linux-release
ctest --preset linux-release
```

```powershell
# Windows x64
.\dependencies\bootstrap-windows.ps1
cmake --preset windows-release -DLOCALFLOW_REQUIRE_INFERENCE=ON
cmake --build --preset windows-release
ctest --preset windows-release
```

The native workflows are the canonical dependency and packaging recipes. See
[`CONTRIBUTING.md`](../CONTRIBUTING.md) for feature-parity rules and
[`docs/FEATURE_PARITY.md`](../docs/FEATURE_PARITY.md) for the release gates.

The Windows local preset uses Visual Studio 2022; CI uses the
`windows-2025-vs2026` runner. The source-build default version remains
`1.3.0-beta.1`; production workflows explicitly supply the stable tag version.
This development default does not describe the public v1.3.0 installer.
