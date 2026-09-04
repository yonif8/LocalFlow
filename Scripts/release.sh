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
if (( $# != 1 )); then
    echo "error: usage: Scripts/release.sh <version>" >&2
    exit 2
fi
if [[ ! "$VERSION" =~ ^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$ ]]; then
    echo "error: stable versions must have the exact form X.Y.Z" >&2
    exit 2
fi
IFS=. read -r VERSION_MAJOR VERSION_MINOR VERSION_PATCH <<<"$VERSION"
if (( VERSION_MINOR > 99 || VERSION_PATCH > 99 )); then
    echo "error: minor and patch must be below 100 for the macOS build-number scheme" >&2
    exit 2
fi
DIST="$REPO_ROOT/dist"
APP="$DIST/LocalFlow.app"
DMG="$DIST/LocalFlow-$VERSION.dmg"
RELEASES_DIR="$DIST/releases"
DOWNLOAD_URL_PREFIX="https://github.com/yonif8/LocalFlow/releases/download/v$VERSION/"
PUBLIC_KEY="$REPO_ROOT/Resources/sparkle-public-ed-key.txt"
SCRATCH_DIR="$REPO_ROOT/.build-release"

command -v curl >/dev/null || { echo "error: curl is required" >&2; exit 1; }
command -v file >/dev/null || { echo "error: file is required" >&2; exit 1; }
command -v hdiutil >/dev/null || { echo "error: hdiutil is required" >&2; exit 1; }
command -v python3 >/dev/null || { echo "error: python3 is required" >&2; exit 1; }
command -v shasum >/dev/null || { echo "error: shasum is required" >&2; exit 1; }
command -v swift >/dev/null || { echo "error: swift is required" >&2; exit 1; }
[[ -s "$PUBLIC_KEY" ]] || { echo "error: pinned Sparkle public key is missing" >&2; exit 1; }

# A distributable binary must come from committed source. appcast.xml is the
# one generated file that release.sh intentionally replaces after the build.
UNRELATED_CHANGES="$(git status --porcelain=v1 --untracked-files=all -- . ':(exclude)appcast.xml')"
if [[ -n "$UNRELATED_CHANGES" ]]; then
    echo "error: commit or remove unrelated worktree changes before building a release" >&2
    printf '%s\n' "$UNRELATED_CHANGES" >&2
    exit 1
fi

# ---- Sparkle tools --------------------------------------------------------
# generate_appcast ships in Sparkle's release tarball, not the SPM artifact.
# Cache the checksum-pinned upstream archive under .build/sparkle-tools.
SPARKLE_VERSION="2.9.6"
SPARKLE_ARCHIVE_SHA256="52bf9e88cdd972fc0c81501377a880e90d47031bd8ca5462488f843e2609e192"
TOOLS_ROOT="$REPO_ROOT/.build/sparkle-tools"
TOOLS_DIR="$TOOLS_ROOT/bin"
ARCHIVE="$TOOLS_ROOT/Sparkle-$SPARKLE_VERSION.tar.xz"
archive_is_valid() {
    [[ -f "$ARCHIVE" ]] \
        && [[ "$(shasum -a 256 "$ARCHIVE" | awk '{print $1}')" == "$SPARKLE_ARCHIVE_SHA256" ]]
}
if ! archive_is_valid; then
    echo "==> Fetching Sparkle $SPARKLE_VERSION tools (generate_appcast, sign_update)…"
    mkdir -p "$TOOLS_ROOT"
    ARCHIVE_TEMP="$ARCHIVE.download"
    rm -f -- "$ARCHIVE_TEMP"
    curl -fsSL -o "$ARCHIVE_TEMP" \
        "https://github.com/sparkle-project/Sparkle/releases/download/$SPARKLE_VERSION/Sparkle-$SPARKLE_VERSION.tar.xz"
    ACTUAL_SHA256="$(shasum -a 256 "$ARCHIVE_TEMP" | awk '{print $1}')"
    if [[ "$ACTUAL_SHA256" != "$SPARKLE_ARCHIVE_SHA256" ]]; then
        rm -f -- "$ARCHIVE_TEMP"
        echo "error: Sparkle tools archive checksum mismatch" >&2
        exit 1
    fi
    mv -f -- "$ARCHIVE_TEMP" "$ARCHIVE"
fi
# Re-extract the two executables on every release so a modified cache cannot
# bypass the archive digest check.
rm -f -- "$TOOLS_DIR/generate_appcast" "$TOOLS_DIR/sign_update"
tar -xf "$ARCHIVE" -C "$TOOLS_ROOT" ./bin/generate_appcast ./bin/sign_update
[[ -x "$TOOLS_DIR/generate_appcast" && -x "$TOOLS_DIR/sign_update" ]] || {
    echo "error: the verified Sparkle archive did not contain the required tools" >&2
    exit 1
}

# ---- Behavior tests ------------------------------------------------------
echo "==> Resolving pinned Swift dependencies and running release tests…"
swift package --scratch-path "$SCRATCH_DIR" resolve
git diff --exit-code -- Package.resolved
swift test --configuration release --parallel --scratch-path "$SCRATCH_DIR" \
    --disable-automatic-resolution

# ---- Build ---------------------------------------------------------------
echo "==> Building LocalFlow ${VERSION}…"
# Always build releases in an isolated scratch dir: the default .build is
# shared with dev/IDE/other-session builds and its llbuild state has served
# STALE BINARIES that shipped without the code they claimed to contain.
"$REPO_ROOT/Scripts/make-app.sh" --version "$VERSION" \
    --scratch-path "$SCRATCH_DIR"

# ---- DMG -----------------------------------------------------------------
echo "==> Creating ${DMG}…"
STAGING="$(mktemp -d)"
trap 'rm -rf "$STAGING"' EXIT
cp -R "$APP" "$STAGING/LocalFlow.app"
ln -s /Applications "$STAGING/Applications"
rm -f "$DMG"
hdiutil create -volname LocalFlow -srcfolder "$STAGING" -ov -format UDZO -quiet "$DMG"
hdiutil verify "$DMG" >/dev/null
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
ED_SIG="$(python3 - "$REPO_ROOT/appcast.xml" "$DMG" "$VERSION" <<'PY'
import os
import sys
import xml.etree.ElementTree as ET

appcast_path, dmg_path, version = sys.argv[1:]
sparkle = "http://www.andymatuschak.org/xml-namespaces/sparkle"
expected_build = str(int(version.split(".")[0]) * 10000 + int(version.split(".")[1]) * 100 + int(version.split(".")[2]))
expected_url = f"https://github.com/yonif8/LocalFlow/releases/download/v{version}/LocalFlow-{version}.dmg"
items = ET.parse(appcast_path).getroot().findall("./channel/item")
if not items or items[0].findtext(f"{{{sparkle}}}shortVersionString") != version:
    raise SystemExit(f"error: {version} must be the newest appcast item")
matches = [item for item in items if item.findtext(f"{{{sparkle}}}shortVersionString") == version]
if len(matches) != 1:
    raise SystemExit(f"error: appcast.xml must contain exactly one item for {version}")
item = matches[0]
enclosure = item.find("enclosure")
if enclosure is None:
    raise SystemExit("error: current appcast item has no enclosure")
checks = {
    "build number": (item.findtext(f"{{{sparkle}}}version"), expected_build),
    "minimum macOS version": (item.findtext(f"{{{sparkle}}}minimumSystemVersion"), "15.0"),
    "hardware requirement": (item.findtext(f"{{{sparkle}}}hardwareRequirements"), "arm64"),
    "download URL": (enclosure.get("url"), expected_url),
    "archive length": (enclosure.get("length"), str(os.path.getsize(dmg_path))),
}
for label, (actual, expected) in checks.items():
    if actual != expected:
        raise SystemExit(f"error: appcast {label} mismatch: expected {expected!r}, got {actual!r}")
signature = enclosure.get(f"{{{sparkle}}}edSignature")
if not signature:
    raise SystemExit("error: current appcast item has no Sparkle Ed25519 signature")
print(signature)
PY
)"
swift "$REPO_ROOT/Scripts/verify-sparkle-signature.swift" \
    "$RELEASES_DIR/LocalFlow-$VERSION.dmg" "$ED_SIG" "$PUBLIC_KEY"
"$TOOLS_DIR/sign_update" --verify "$RELEASES_DIR/LocalFlow-$VERSION.dmg" "$ED_SIG"
echo "==> Signature OK."

echo ""
echo "==> Release $VERSION ready:"
echo "    $DMG"
echo "    $REPO_ROOT/appcast.xml"
echo "    Next: commit appcast.xml, then run Scripts/publish.sh $VERSION"
