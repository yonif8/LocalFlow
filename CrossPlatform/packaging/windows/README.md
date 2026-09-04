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
with release mode enabled. Production builds fail closed if the ref or version
does not match, any signing input is absent, the PFX has the wrong fingerprint
or EKU, the certificate is expired, timestamping fails, or Authenticode
verification does not return `Valid`.

All third-party and GitHub actions in the workflow are pinned to immutable
commit SHAs. Qt itself is pinned to 6.8.3. `windows-2022` supplies Visual Studio,
the Windows SDK/signing tools, and Inno Setup; the workflow discovers their
installed paths and fails rather than downloading an unverified compiler.

## Required GitHub Actions secrets

Configure these four GitHub Actions repository secrets:

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
remain stale after the signed installer updates the registered application,
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
executable and Inno Setup uses the same file for the signed installer, so the
app, shortcuts, Add/Remove Programs entry, and setup UI keep one brand asset.

The top-level release workflow combines them with the signed macOS and Linux
artifacts, rejects missing, extra, empty, renamed, or mismatched assets, and
publishes one release only after the complete set verifies. It never replaces
a different asset already attached to the draft. The manifest contains the
tag's immutable installer URL, size, SHA-256, Authenticode status, and signer
fingerprint. Once that three-platform release is public, the stable client feed
is:

```text
https://github.com/yonif8/LocalFlow/releases/latest/download/windows-update.json
```

The Windows updater must compare semantic versions, download over HTTPS, verify
the exact size and SHA-256, then use `WinVerifyTrust` and require a signer
thumbprint compiled into the app before launching the installer. The thumbprint
inside the downloaded manifest is diagnostic metadata—not a trust root. During
certificate rotation, ship an app version trusting both old and new certificates
before signing later installers only with the new certificate.

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

`Sign-Artifacts.ps1` reads signing material only from the four environment
variables named above. Pass `-RequireSigning` for any artifact intended for
distribution.
