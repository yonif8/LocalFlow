#!/bin/bash
# release.sh — build a versioned, Sparkle-enabled LocalFlow release locally.
#
#   Scripts/release.sh <version>          e.g. Scripts/release.sh 1.0.0
#
# Produces:
#   dist/LocalFlow.app                    versioned, Sparkle feed embedded
#   dist/LocalFlow-<version>.dmg          drag-to-/Applications disk image
#   dist/releases/LocalFlow-<version>.dmg archive dir generate_appcast scans
#   appcast.xml (repo root)               EdDSA-signed update feed
#
# Requires the Sparkle EdDSA private key in the login keychain (created once
# by Sparkle's generate_keys; the public half lives in
# Resources/sparkle-public-ed-key.txt). The private key is never written to
# disk or printed. Idempotent: re-running a version rebuilds and re-signs.
#
# Publishing (repo/release creation, pushes) is Scripts/publish.sh — not here.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

VERSION="${1:?usage: Scripts/release.sh <version>   (e.g. 1.0.0)}"
DIST="$REPO_ROOT/dist"
APP="$DIST/LocalFlow.app"
DMG="$DIST/LocalFlow-$VERSION.dmg"
RELEASES_DIR="$DIST/releases"
DOWNLOAD_URL_PREFIX="https://github.com/yonif8/LocalFlow/releases/download/v$VERSION/"

# ---- Sparkle tools --------------------------------------------------------
# generate_appcast ships in Sparkle's release tarball, not the SPM artifact.
# Cache it under .build/sparkle-tools; override with SPARKLE_TOOLS=<dir/bin>.
SPARKLE_VERSION="2.9.6"
TOOLS_DIR="${SPARKLE_TOOLS:-$REPO_ROOT/.build/sparkle-tools/bin}"
if [[ ! -x "$TOOLS_DIR/generate_appcast" ]]; then
    echo "==> Fetching Sparkle $SPARKLE_VERSION tools (generate_appcast, sign_update)…"
    TOOLS_ROOT="$REPO_ROOT/.build/sparkle-tools"
    mkdir -p "$TOOLS_ROOT"
    curl -fsSL -o "$TOOLS_ROOT/Sparkle.tar.xz" \
        "https://github.com/sparkle-project/Sparkle/releases/download/$SPARKLE_VERSION/Sparkle-$SPARKLE_VERSION.tar.xz"
    tar -xf "$TOOLS_ROOT/Sparkle.tar.xz" -C "$TOOLS_ROOT" bin/generate_appcast bin/sign_update
    TOOLS_DIR="$TOOLS_ROOT/bin"
fi

# ---- Build ---------------------------------------------------------------
echo "==> Building LocalFlow ${VERSION}…"
# Always build releases in an isolated scratch dir: the default .build is
# shared with dev/IDE/other-session builds and its llbuild state has served
# STALE BINARIES that shipped without the code they claimed to contain.
# Extra args pass through to make-app.sh and may override the scratch path.
"$REPO_ROOT/Scripts/make-app.sh" --version "$VERSION" \
    --scratch-path "$REPO_ROOT/.build-release" "${@:2}"

# ---- DMG -----------------------------------------------------------------
echo "==> Creating ${DMG}…"
STAGING="$(mktemp -d)"
trap 'rm -rf "$STAGING"' EXIT
cp -R "$APP" "$STAGING/LocalFlow.app"
ln -s /Applications "$STAGING/Applications"
rm -f "$DMG"
hdiutil create -volname LocalFlow -srcfolder "$STAGING" -ov -format UDZO -quiet "$DMG"
echo "==> DMG: $DMG ($(du -h "$DMG" | cut -f1 | tr -d ' '))"

# ---- Appcast -------------------------------------------------------------
# generate_appcast scans dist/releases/ and signs each archive with the
# EdDSA private key from the keychain. --download-url-prefix applies to the
# archives scanned in THIS run, so the dir holds only the current version;
# previously published items are preserved from the existing appcast.xml.
mkdir -p "$RELEASES_DIR"
find "$RELEASES_DIR" -name 'LocalFlow-*.dmg' ! -name "LocalFlow-$VERSION.dmg" -delete
cp -f "$DMG" "$RELEASES_DIR/"
echo "==> Generating EdDSA-signed appcast.xml…"
"$TOOLS_DIR/generate_appcast" \
    --download-url-prefix "$DOWNLOAD_URL_PREFIX" \
    -o "$REPO_ROOT/appcast.xml" \
    "$RELEASES_DIR"

echo "==> Verifying the appcast's EdDSA signature…"
ED_SIG="$(sed -n 's/.*sparkle:edSignature="\([^"]*\)".*/\1/p' "$REPO_ROOT/appcast.xml" | head -1)"
[[ -n "$ED_SIG" ]] || { echo "error: no sparkle:edSignature in appcast.xml" >&2; exit 1; }
"$TOOLS_DIR/sign_update" --verify "$RELEASES_DIR/LocalFlow-$VERSION.dmg" "$ED_SIG"
echo "==> Signature OK."

echo ""
echo "==> Release $VERSION ready:"
echo "    $DMG"
echo "    $REPO_ROOT/appcast.xml"
echo "    Next: commit appcast.xml, then run Scripts/publish.sh $VERSION"
