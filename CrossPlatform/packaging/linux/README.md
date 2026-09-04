# Linux packaging and updates

The Linux release lane builds on Ubuntu 22.04 x86-64 and produces one shared
runtime payload in two formats:

- `LocalFlow-x86_64.AppImage` and `LocalFlow-x86_64.AppImage.zsync`
- `localflow_VERSION_amd64.deb`
- a small integration bundle for installing or uninstalling the AppImage from
  a user's applications menu
- `SHA256SUMS` and, for releases, `SHA256SUMS.asc`

The AppImage and Debian package are both made from the same Qt-deployed AppDir.
Models are not baked into either package; the application downloads pinned,
checksum-verified models on first run.

## Local packaging

Configure and build the top-level cross-platform project first, then run:

```sh
CrossPlatform/packaging/linux/package.sh \
  --build-dir build/linux \
  --dist-dir dist/linux \
  --version 1.3.0

CrossPlatform/packaging/linux/smoke-test.sh dist/linux 1.3.0
```

Unsigned mode is intentionally marked `UNSIGNED-NOT-FOR-DISTRIBUTION.txt`.
Those packages are CI smoke artifacts, not downloads for friends or family.

The packaging tool downloads fixed releases of linuxdeploy, its Qt plugin,
appimagetool, and the AppImage runtime and verifies hard-coded SHA-256
checksums before executing them. appimagetool is never allowed to substitute
its mutable `continuous` runtime.

## Production signing policy

A `vX.Y.Z` Git tag switches `.github/workflows/linux.yml` to release mode. It
fails before packaging unless all of the following are true:

1. Real NeMo-Speech.cpp and llama.cpp runtime archives were downloaded,
   checksum verified, and linked. A build containing inference stubs cannot be
   released.
2. A secret OpenPGP signing key was imported and its complete fingerprint
   exactly matches the configured fingerprint.
3. appimagetool created non-empty embedded signature and public-key sections.
4. The checksum manifest was signed and verifies with the imported key.
5. The `.zsync` metadata and all package smoke tests passed.

Required GitHub Actions secrets:

| Secret | Purpose |
| --- | --- |
| `LINUX_GPG_PRIVATE_KEY` | ASCII-armored release-only OpenPGP private key |
| `LINUX_GPG_PASSPHRASE` | Passphrase for that key |
| `LINUX_GPG_FINGERPRINT` | Full 40-hex-character fingerprint, never a short key ID |

Required repository variables:

| Variable | Purpose |
| --- | --- |
| `LINUX_NEMO_RUNTIME_URL` | HTTPS tar archive containing `include/` and `lib/` |
| `LINUX_NEMO_RUNTIME_SHA256` | Exact archive SHA-256 |
| `LINUX_LLAMA_RUNTIME_URL` | HTTPS tar archive containing `include/` and `lib/` |
| `LINUX_LLAMA_RUNTIME_SHA256` | Exact archive SHA-256 |

Each archive must contain one top-level directory; CI strips that directory.
The runtime archive versions and checksums should be changed in a reviewed pull
request, never silently replaced at the existing URL.

Pull requests, normal branch pushes, and manual workflow runs never receive or
use the release key. They intentionally create unsigned smoke artifacts.

## AppImage update handoff

The AppImage embeds this update-information contract:

```text
gh-releases-zsync|yonif8|LocalFlow|latest|LocalFlow-*-x86_64.AppImage.zsync
```

The publishing step must upload these exact assets to the same public GitHub
release:

- `LocalFlow-x86_64.AppImage`
- `LocalFlow-x86_64.AppImage.zsync`
- `localflow_VERSION_amd64.deb`
- `LocalFlow-VERSION-Linux-Integration.tar.gz`
- `SHA256SUMS`
- `SHA256SUMS.asc`

Before making the release public, the publisher must verify `SHA256SUMS.asc`,
compare the workflow artifact digest, and ensure the signing fingerprint is the
fingerprint pinned in the application updater. The release must remain draft if
any Linux artifact is missing. Never substitute an unsigned smoke build.

The desktop app's **Check for Updates** integration should use the embedded
zsync information, verify the AppImage signature against the pinned public-key
fingerprint, download beside the running image, and atomically replace it only
after verification. A `.deb` install should hand off to the configured package
manager or download the signed `.deb`; it must not overwrite files managed by
`dpkg`. Packaging provides the signed artifacts and update hooks, while that UI
wiring remains application-layer work.

## Desktop integration and uninstall

Debian installs a desktop entry, scalable icon, AppStream metadata, and
`localflow-autostart`. `dpkg --remove localflow` removes program files but never
walks home directories or deletes user data.

For AppImage users, unpack the integration archive next to the downloaded image
and run:

```sh
./install-appimage.sh ../LocalFlow-x86_64.AppImage
# Add --autostart only when the user explicitly wants launch at login.
```

It installs only into the current user's XDG data directories. Run the copied
`~/.local/share/localflow/uninstall.sh` to remove the app and desktop
integration. Uninstall preserves settings, dictation history, learned
terminology, and models by design.

The application settings layer should call `localflow-autostart enable` or
`localflow-autostart disable` when the Launch at Login toggle changes. Merely
changing `QSettings` is not sufficient.
