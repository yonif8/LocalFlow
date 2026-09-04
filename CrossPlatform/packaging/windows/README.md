<!-- sparkle-sign-warning:
IMPORTANT: This file was signed by Sparkle. Any modifications to this file requires updating signatures in appcasts that reference this file! This will involve re-running generate_appcast or sign_update.
-->
# Windows packaging and updates

The Windows workflow builds `CrossPlatform/CMakeLists.txt` with Visual Studio
2022 for x64, runs CTest, installs `localflow_desktop` as `LocalFlow.exe`, runs
`windeployqt`, and creates a per-user Inno Setup installer. Installation never
asks for elevation and defaults to:

```text
%LOCALAPPDATA%\Programs\LocalFlow
```

It creates a Start Menu shortcut, offers an unchecked **Start LocalFlow when I
sign in** task, and registers a normal uninstaller. User models, settings, and
history are intentionally retained when uninstalling.

## CI versus production

Pull requests and ordinary `main` builds are smoke builds. They may be unsigned
and are uploaded only as short-lived GitHub Actions artifacts.

The supported production path is `.github/workflows/release.yml` at an exact
stable `vX.Y.Z` tag. It invokes the Windows lane against that same tagged commit
with release mode enabled. Every production installer is signed with LocalFlow's
Ed25519 release key. That detached signature protects the in-app update path
without requiring a paid Windows certificate.

The free direct-download installer is intentionally not Authenticode-signed.
Windows therefore identifies it as **Unknown publisher** and may require the
user to choose **More info → Run anyway**. This affects Windows' reputation UI,
not LocalFlow's own tamper protection. Authenticode remains an optional additive
layer if certificate credentials are configured later.

All third-party and GitHub actions in the workflow are pinned to immutable
commit SHAs. Qt itself is pinned to 6.8.3. `windows-2025` supplies Visual Studio,
the Windows SDK/signing tools, and Inno Setup while satisfying the app's Windows
11 build floor. The workflow discovers their installed paths and fails rather
than downloading an unverified compiler.

## Required GitHub Actions secret

- `LOCALFLOW_UPDATE_ED25519_PRIVATE_KEY`: the canonical base64 32-byte seed for
  the public key in `Resources/sparkle-public-ed-key.txt`. This is the same
  stable release identity already used by the macOS updater. It is read only by
  the exact-tag Windows release job and must never be printed or committed.

The following four secrets are optional. Configure all four only if LocalFlow
later adds paid Authenticode signing:

- `WINDOWS_CODESIGN_PFX_BASE64`: base64 of the complete PKCS#12/PFX containing
  the Windows code-signing leaf certificate, private key, and intermediate
  chain. Do not insert PEM headers.
- `WINDOWS_CODESIGN_PFX_PASSWORD`: the PFX export password.
- `WINDOWS_CODESIGN_CERT_SHA256`: the expected SHA-256 fingerprint of the leaf
  certificate, hexadecimal with or without spaces. This prevents silently
  signing with the wrong certificate from a replaced PFX.
- `WINDOWS_CODESIGN_TIMESTAMP_URL`: an RFC 3161 timestamp endpoint, for example
  `http://timestamp.digicert.com` when that service is covered by the signing
  provider's policy.

To encode a PFX without printing it in PowerShell:

```powershell
$bytes = [IO.File]::ReadAllBytes("LocalFlow-code-signing.pfx")
$base64 = [Convert]::ToBase64String($bytes)
$base64 | Set-Clipboard
[Array]::Clear($bytes, 0, $bytes.Length)
```

Protect release tags, require reviewed pull requests for workflow changes, and
limit repository administration to trusted maintainers. Never add signing
values to repository variables, workflow files, build logs, artifacts, or the
update manifest.

## Unified release and update feed

The Windows lane has read-only repository permissions and produces these
immutable workflow artifacts:

```text
LocalFlow-X.Y.Z-windows-x64-setup.exe
LocalFlow-X.Y.Z-windows-x64-setup.exe.sha256
windows-update.json
```

LocalFlow intentionally does not publish a portable ZIP. A portable copy can
remain stale after the verified installer updates the registered application,
which makes it too easy to launch an older binary accidentally. The per-user
installer is the one supported Windows distribution and update identity.

The installer keeps redistribution texts beside the program at
`share\licenses`. It carries the complete, module-separated Qt 6.8.3
`LICENSES` directories pinned in the repository and the complete `nemo-speech` and `llama.cpp`
license/notice directories created by the checksum-pinned runtime bootstrap.
Staging and installed-layout smoke tests fail when any required notice is
missing.

`LocalFlow.ico` is a mechanical 256-pixel Windows conversion of the
canonical `Resources/AppIcon.icns`. `LocalFlow.rc` embeds it in the desktop
executable and Inno Setup uses the same file for the installer, so the
app, shortcuts, Add/Remove Programs entry, and setup UI keep one brand asset.

The top-level release workflow combines them with the signed macOS and Linux
artifacts, rejects missing, extra, empty, renamed, or mismatched assets, and
publishes one release only after the complete set verifies. It never replaces
a different asset already attached to the draft. The manifest contains the
tag's immutable installer URL, size, SHA-256, LocalFlow Ed25519 signature, and
optional Authenticode status. Once that three-platform release is public, the
stable client feed is:

```text
https://github.com/yonif8/LocalFlow/releases/latest/download/windows-update.json
```

The Windows updater compares semantic versions, downloads over HTTPS, verifies
the exact size and SHA-256, and then starts a private copy of the already-running
LocalFlow executable to verify the detached Ed25519 signature and embedded
ProductVersion before launching the installer. The release public key is
compiled into the app and cannot be selected by the downloaded manifest. When
an Authenticode thumbprint is compiled in, the same helper additionally requires
`WinVerifyTrust` and that exact certificate.

CI installs and uninstalls the generated installer on its Windows runner. A
stable release additionally requires the clean-machine, real-app certification
in `docs/FEATURE_PARITY.md`; CI installation alone is not sufficient. If any
artifact changes, rerun the workflow; never edit the manifest or checksum by
hand.

## Local packaging

After staging a Release build and running `windeployqt`:

```powershell
ISCC.exe /DAppVersion=1.2.3 `
  /DSourceDir=C:\build\stage `
  /DOutputDir=C:\build\package `
  /DOutputBaseFilename=LocalFlow-1.2.3-windows-x64-setup `
  CrossPlatform\packaging\windows\LocalFlow.iss
```

`Sign-Artifacts.ps1` reads optional Authenticode material only from the four
`WINDOWS_CODESIGN_*` environment variables named above. The official free
release leaves them unset. `localflow_update_signer` creates the required
detached release signature without changing the installer bytes.
