# LocalFlow

Fully local dictation for **macOS, Windows, and Linux**. Hold a shortcut, speak,
and release to insert polished text into the focused app. Speech recognition,
polishing, and screen terminology run on your computer. No account or cloud
processing is required. Internet access is used for model/update downloads;
bug reports are sent only when you choose to submit them.

- Hold-to-talk with configurable keyboard and supported mouse triggers.
- Parakeet TDT v3 speech recognition and optional S1-mini polishing.
- Optional screen terminology and a bounded, editable local terminology bank.
- Tray/menu bar controls, recording HUD, settings, and session history.
- Signed update verification, with installation appropriate to each platform.

## Downloads and validation status

[Download LocalFlow](https://github.com/yonif8/LocalFlow/releases/latest).
Version 1.3.0 has public packages for all three platforms. Windows and Linux
passed automated builds, real-model inference, signing checks, and installer
smoke tests. **Hands-on Windows/Linux testing is still pending.** Availability
does not mean every desktop/app combination has been certified.
See the [validation record and limitations](docs/FEATURE_PARITY.md).

| Platform | Download | Target |
| --- | --- | --- |
| macOS | `LocalFlow-<version>.dmg` | Apple Silicon, macOS 15 or later |
| Windows | `LocalFlow-<version>-windows-x64-setup.exe` | Windows 11, x86-64 |
| Linux | `LocalFlow-x86_64.AppImage` (recommended), or `localflow_<version>_amd64.deb` | x86-64; packaged on Ubuntu 22.04; desktop capabilities vary |

Windows ARM64 and Linux ARM64 packages are not included in this release.

## Install

### macOS

1. Download the DMG, open it, and drag **LocalFlow** into **Applications**.
2. Open LocalFlow. The free distribution uses a self-signed certificate and is
   not Apple-notarized, so macOS may block the first launch.
3. If blocked, dismiss the warning, then open **System Settings → Privacy &
   Security → Open Anyway** and confirm. If the option is absent, try opening
   the app again before returning to Settings.
4. Grant Microphone, Input Monitoring, and Accessibility permissions when asked.
5. Complete the model download, then hold **Fn**, speak, and release. Change the
   shortcut in Settings if needed.

Screen terminology is opt-in under **Settings → Dictionary** and additionally
requires Screen Recording permission. macOS uses Apple Vision OCR supplemented
by accessibility metadata.

### Windows

1. Download and run the Windows setup EXE from Releases.
2. The free installer has no paid Authenticode certificate. Windows may show
   **Unknown publisher** or SmartScreen. If **More info → Run anyway** is offered,
   use it for the installer downloaded from this repository. Managed security
   policies may prevent installation.
3. Follow setup, then open LocalFlow from the Start Menu. It installs for your
   user without administrator access.
4. Allow microphone access, complete the model download, and choose your
   push-to-talk shortcut in Settings. Fn/Globe is not generally available.

Screen terminology uses GDI window capture and Windows OCR. Some GPU-rendered
surfaces may not capture correctly. Elevated and protected apps can restrict
insertion; LocalFlow does not bypass those protections.

### Linux

Download the AppImage, make it executable, and open it:

```sh
chmod +x LocalFlow-x86_64.AppImage
./LocalFlow-x86_64.AppImage
```

If FUSE is unavailable, try `APPIMAGE_EXTRACT_AND_RUN=1 ./LocalFlow-x86_64.AppImage`.
The matching `LocalFlow-<version>-Linux-Integration.tar.gz` contains optional
per-user application-menu installation/uninstallation scripts. See the
[Linux installation guide](CrossPlatform/packaging/linux/README.md#desktop-integration-and-uninstall).

On Debian/Ubuntu, install the DEB with
`sudo apt install ./localflow_<version>_amd64.deb`, replacing `<version>` with
the downloaded version. Complete onboarding and model downloads after launch.

X11 uses native shortcuts and capture. Wayland depends on desktop portals and
accessibility support; approve desktop prompts when requested. Global mouse
triggers, Escape cancellation, tray visibility, and insertion into inaccessible
apps have limitations. GNOME/KDE hands-on certification is pending. See
[Linux platform limitations](CrossPlatform/platform/linux/README.md#remaining-platform-limitations).

## Models, terminology, and updates

First-run setup downloads local models; Windows/Linux require about 1.2 GB
for Parakeet and S1-mini. macOS uses CoreML/MLX model files and manages their
downloads separately. Models are not embedded in the installers.

Screen terminology is optional. OCR runs while you speak, and available results
help restore visible names and spellings. High-confidence corrections may be
remembered locally. Review or delete learned terms in Settings; the bank is
limited to 500 terms and 10 aliases per term. Screen images are not uploaded.

- **macOS:** Sparkle checks automatically; use **Check for Updates…** in the menu
  bar menu for a manual check.
- **Windows:** use **Check for Updates…**, download, then confirm installation.
  LocalFlow verifies the installer with its pinned Ed25519 key even though the
  free installer has no Authenticode certificate.
- **Linux AppImage:** the in-app updater checks and verifies signed updates;
  restart after installation. **DEB:** the app opens Releases for a manual
  package update and does not overwrite package-managed files.

## Development

- `Sources/`, `Tests/`, `Scripts/`: native macOS Swift application.
- `CrossPlatform/`: shared Windows/Linux C++/Qt app and native adapters.
- [Cross-platform build guide](CrossPlatform/README.md).
- [Contributing and parity rules](CONTRIBUTING.md).
- [Feature parity and release evidence](docs/FEATURE_PARITY.md).
- [v1.3.0 release notes](docs/releases/v1.3.0.md).

Feature changes must address every affected platform. Sharing a repository
does not automatically port Swift changes to C++; equivalent behavior and
validation are required.

For a macOS app/release build, use full Xcode with its Metal Toolchain component
installed and select it explicitly:

```sh
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
Scripts/setup-signing.sh
Scripts/make-app.sh
```

Maintainers use `Scripts/release.sh X.Y.Z` followed by `Scripts/publish.sh X.Y.Z`
for a new version. Preserve existing tags and signed assets. See the release
record for the v1.3.0 publication exception and pending manual validation.

## Credits and licensing status

Speech recognition: NVIDIA Parakeet TDT v3 through FluidAudio on macOS and
NeMo-Speech.cpp on Windows/Linux. Polish: S1-mini by Superwhisper through MLX
on macOS and llama.cpp on Windows/Linux. Desktop UI: AppKit/SwiftUI and Qt/QML.
OCR: Apple Vision, Windows OCR, and Tesseract. Updates: Sparkle, LocalFlow's
Ed25519 verifier, and AppImageUpdate.

The application is available as a free download. This repository currently has
no project-wide LICENSE file; public source availability is not a declaration
of an open-source license. The owner has not selected a project-wide license.
Bundled third-party components retain their own licenses and notices.
