# Windows packaging and release handoff

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

Every `vX.Y.Z` tag is a production build. Manual production builds require the
workflow's `release` input plus an explicit version. Production builds fail
closed if any signing input is absent, the PFX has the wrong thumbprint or EKU,
the certificate is expired, timestamping fails, or Authenticode verification
does not return `Valid`.

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

## Release and update-feed handoff

The workflow deliberately has read-only repository permissions. After a tagged
build passes, the release publisher takes these immutable workflow artifacts:

```text
LocalFlow-X.Y.Z-windows-x64-setup.exe
LocalFlow-X.Y.Z-windows-x64-setup.exe.sha256
LocalFlow-X.Y.Z-windows-x64-portable.zip
windows-update.json
```

Upload all four, without renaming them, to the matching GitHub release tag
`vX.Y.Z`. The manifest already contains that tag's immutable installer URL,
size, SHA-256, Authenticode status, and signer thumbprint. Once the GitHub
release is public, the stable client feed is:

```text
https://github.com/yonif8/LocalFlow/releases/latest/download/windows-update.json
```

The Windows updater must compare semantic versions, download over HTTPS, verify
the exact size and SHA-256, then use `WinVerifyTrust` and require a signer
thumbprint compiled into the app before launching the installer. The thumbprint
inside the downloaded manifest is diagnostic metadata—not a trust root. During
certificate rotation, ship an app version trusting both old and new certificates
before signing later installers only with the new certificate.

The release publisher should make the GitHub release public only after a clean
Windows VM install/update/uninstall smoke test. If any artifact changes, rerun
the workflow; never edit the manifest or checksum by hand.

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
