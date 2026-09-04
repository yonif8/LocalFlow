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

The supported production path is `.github/workflows/release.yml` at an exact
stable `vX.Y.Z` tag. It invokes the Linux lane against that same tagged commit
with release mode enabled. The build fails before packaging unless all of the
following are true:

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

Inference runtime locations are not repository variables. The bootstrap reads
the immutable URL, byte size, SHA-256, architecture, and expected archive layout
from the checked-in
[`runtime-lock.json`](../../dependencies/runtime-lock.json). A runtime change
therefore requires a reviewed lockfile/bootstrap change; never replace an
archive at an existing URL or override its digest in CI.

Pull requests and normal branch pushes never receive or use the release key.
Non-release workflow runs intentionally create unsigned smoke artifacts.

## AppImage update handoff

The AppImage embeds this update-information contract:

```text
gh-releases-zsync|yonif8|LocalFlow|latest|LocalFlow-x86_64.AppImage.zsync
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

The desktop app's **Check for Updates** action runs a checksum-pinned
`appimageupdatetool` locally against the embedded zsync information. It reports
whether an update exists, verifies the signed update, and replaces the installed
AppImage in place only after a successful delta download. A `.deb` install opens
the Releases page instead; it never overwrites files managed by `dpkg`.

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
