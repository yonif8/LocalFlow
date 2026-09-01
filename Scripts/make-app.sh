#!/bin/bash
# make-app.sh — build LocalFlow in release mode and assemble dist/LocalFlow.app.
#
# Works with Command Line Tools only (no Xcode required).
# Usage: Scripts/make-app.sh [--version X.Y.Z] [--scratch-path <dir>]
#
#   --version X.Y.Z   Release build: stamps CFBundleShortVersionString/
#                     CFBundleVersion and embeds the Sparkle feed URL +
#                     EdDSA public key (Resources/sparkle-public-ed-key.txt)
#                     so auto-updates are live.
#   (no --version)    Dev build: version 1.0.0-dev, no Sparkle feed keys —
#                     the app detects this and never starts the updater.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

# Prefer the Command Line Tools toolchain when present: this repo only needs
# CLT, and a machine whose xcode-select points at a Xcode.app with an
# unaccepted license fails `swift build` with an exit-69 license error.
if [[ -d /Library/Developer/CommandLineTools && -z "${DEVELOPER_DIR:-}" ]]; then
    export DEVELOPER_DIR=/Library/Developer/CommandLineTools
fi

VERSION="1.0.0-dev"
RELEASE=0
SCRATCH_DIR="$REPO_ROOT/.build"
SCRATCH_ARGS=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --version)
            VERSION="${2:?--version requires a value}"; RELEASE=1; shift 2 ;;
        --scratch-path)
            SCRATCH_DIR="${2:?--scratch-path requires a value}"
            SCRATCH_ARGS=(--scratch-path "$2"); shift 2 ;;
        *)
            echo "error: unknown argument: $1" >&2; exit 2 ;;
    esac
done

# CFBundleVersion must be numeric and monotonically increasing across
# releases for Sparkle's version comparison. Derive it from the numeric part
# of the version: major*10000 + minor*100 + patch (so 1.0.0 -> 10000,
# 1.2.3 -> 10203). Two digits per component — keep minor/patch < 100.
NUMERIC="${VERSION%%-*}"
IFS='.' read -r V_MAJ V_MIN V_PAT <<< "$NUMERIC"
V_MAJ="${V_MAJ:-0}"; V_MIN="${V_MIN:-0}"; V_PAT="${V_PAT:-0}"
if ! [[ "$V_MAJ$V_MIN$V_PAT" =~ ^[0-9]+$ ]]; then
    echo "error: --version must look like X.Y.Z (got: $VERSION)" >&2; exit 2
fi
if (( V_MIN > 99 || V_PAT > 99 )); then
    echo "error: minor/patch must be < 100 for the CFBundleVersion scheme" >&2; exit 2
fi
BUILD_NUM=$((10#$V_MAJ * 10000 + 10#$V_MIN * 100 + 10#$V_PAT))

APP_NAME="LocalFlow"
BUNDLE_ID="com.localflow.app"
DIST="$REPO_ROOT/dist"
APP="$DIST/$APP_NAME.app"

# Sparkle update-feed configuration (release builds only).
SU_FEED_URL="https://raw.githubusercontent.com/yonif8/LocalFlow/main/appcast.xml"
SU_PUBLIC_KEY=""
if [[ $RELEASE -eq 1 ]]; then
    KEY_FILE="$REPO_ROOT/Resources/sparkle-public-ed-key.txt"
    [[ -f "$KEY_FILE" ]] || {
        echo "error: $KEY_FILE missing — run Sparkle's generate_keys and save the public key there" >&2
        exit 1; }
    SU_PUBLIC_KEY="$(tr -d ' \n' < "$KEY_FILE")"
    [[ -n "$SU_PUBLIC_KEY" ]] || { echo "error: $KEY_FILE is empty" >&2; exit 1; }
fi

echo "==> Building $APP_NAME $VERSION (release, build $BUILD_NUM)…"
# ${arr[@]+...} guard: bash 3.2's set -u treats an empty array as unbound.
swift build -c release --product LocalFlowApp ${SCRATCH_ARGS[@]+"${SCRATCH_ARGS[@]}"}
# (not `swift build --show-bin-path` — that trips an Xcode license check on
# machines with a dormant Xcode.app; the layout below is stable for SwiftPM)
BIN_PATH="$SCRATCH_DIR/release/LocalFlowApp"
[[ -x "$BIN_PATH" ]] || { echo "error: built binary not found at $BIN_PATH" >&2; exit 1; }

echo "==> Assembling $APP…"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources" "$APP/Contents/Frameworks"

cp "$BIN_PATH" "$APP/Contents/MacOS/$APP_NAME"

# MLX (the Granite ASR backend) loads its GPU kernels from an mlx.metallib
# colocated with the executable. Copy it into Contents/MacOS when the build
# produced one; warn otherwise (the mock-transcriber app runs fine without it,
# the real engine will not).
METALLIB=""
for candidate in \
    "$SCRATCH_DIR/release/mlx.metallib" \
    "$REPO_ROOT/.build/release/mlx.metallib" \
    "$REPO_ROOT/.build/debug/mlx.metallib"; do
    if [[ -f "$candidate" ]]; then METALLIB="$candidate"; break; fi
done
if [[ -n "$METALLIB" ]]; then
    cp "$METALLIB" "$APP/Contents/MacOS/mlx.metallib"
    echo "==> Copied mlx.metallib ($(basename "$(dirname "$METALLIB")") build)"
else
    echo "==> WARNING: mlx.metallib not found next to the built products;"
    echo "    the real Granite/MLX transcriber will not run from this bundle."
fi

# ---- Sparkle.framework ----------------------------------------------------
# SwiftPM stages the (binary-artifact) framework next to the built products.
SPARKLE_SRC=""
for candidate in \
    "$SCRATCH_DIR/release/Sparkle.framework" \
    "$REPO_ROOT/.build/release/Sparkle.framework" \
    "$REPO_ROOT"/.build/artifacts/sparkle/Sparkle/Sparkle.xcframework/macos-*/Sparkle.framework; do
    if [[ -d "$candidate" ]]; then SPARKLE_SRC="$candidate"; break; fi
done
[[ -n "$SPARKLE_SRC" ]] || { echo "error: Sparkle.framework not found in build products" >&2; exit 1; }
# cp -R (no -L) preserves the framework's Versions/Current symlink layout.
cp -R "$SPARKLE_SRC" "$APP/Contents/Frameworks/Sparkle.framework"
# Non-sandboxed app: the XPC services are unused (Sparkle docs say they may
# be removed). Removing them also keeps every nested binary signed by OUR
# identity instead of Sparkle's Developer ID.
rm -rf "$APP/Contents/Frameworks/Sparkle.framework/Versions/B/XPCServices" \
       "$APP/Contents/Frameworks/Sparkle.framework/XPCServices"
# The binary references @rpath/Sparkle.framework/…; SwiftPM only emits an
# @loader_path rpath (framework beside the binary), so add the bundle-layout
# rpath. || true: re-running against an already-patched binary is fine.
install_name_tool -add_rpath "@executable_path/../Frameworks" \
    "$APP/Contents/MacOS/$APP_NAME" 2>/dev/null || true
echo "==> Embedded Sparkle.framework ($(basename "$(dirname "$SPARKLE_SRC")"))"

# ---- Info.plist -----------------------------------------------------------
SPARKLE_PLIST_KEYS=""
if [[ $RELEASE -eq 1 ]]; then
    SPARKLE_PLIST_KEYS=$(cat <<KEYS
	<key>SUFeedURL</key>
	<string>$SU_FEED_URL</string>
	<key>SUPublicEDKey</key>
	<string>$SU_PUBLIC_KEY</string>
	<key>SUEnableAutomaticChecks</key>
	<true/>
KEYS
)
fi

cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleDevelopmentRegion</key>
	<string>en</string>
	<key>CFBundleExecutable</key>
	<string>$APP_NAME</string>
	<key>CFBundleIdentifier</key>
	<string>$BUNDLE_ID</string>
	<key>CFBundleInfoDictionaryVersion</key>
	<string>6.0</string>
	<key>CFBundleName</key>
	<string>$APP_NAME</string>
	<key>CFBundleDisplayName</key>
	<string>$APP_NAME</string>
	<key>CFBundlePackageType</key>
	<string>APPL</string>
	<key>CFBundleShortVersionString</key>
	<string>$VERSION</string>
	<key>CFBundleVersion</key>
	<string>$BUILD_NUM</string>
	<key>CFBundleIconFile</key>
	<string>AppIcon</string>
	<key>LSMinimumSystemVersion</key>
	<string>15.0</string>
	<key>LSUIElement</key>
	<true/>
	<key>NSMicrophoneUsageDescription</key>
	<string>LocalFlow records your voice while the hold-to-talk key is held so it can transcribe what you say — entirely on this Mac.</string>
	<key>NSHighResolutionCapable</key>
	<true/>
	<key>NSPrincipalClass</key>
	<string>NSApplication</string>
$SPARKLE_PLIST_KEYS
</dict>
</plist>
PLIST

# ---- App icon -------------------------------------------------------------
# Resources/AppIcon.icns is checked in (regenerate with Scripts/make-icon.sh).
ICON_SRC="$REPO_ROOT/Resources/AppIcon.icns"
if [[ ! -f "$ICON_SRC" ]]; then
    echo "==> Resources/AppIcon.icns missing; regenerating via Scripts/make-icon.sh…"
    "$REPO_ROOT/Scripts/make-icon.sh" || echo "==> Icon generation failed (non-fatal)."
fi
if [[ -f "$ICON_SRC" ]]; then
    cp "$ICON_SRC" "$APP/Contents/Resources/AppIcon.icns"
    echo "==> Copied Resources/AppIcon.icns"
fi

# ---- Codesign -------------------------------------------------------------
# Prefer the stable "LocalFlow Signing" identity (created by
# Scripts/setup-signing.sh): its designated requirement is stable across
# rebuilds, so TCC grants (Microphone / Input Monitoring / Accessibility)
# persist. Fall back to ad-hoc, whose grants die on every re-sign.
#
# Sign INSIDE-OUT, never --deep: --deep breaks Sparkle's nested structure and
# is deprecated. Order: Sparkle's inner executables, then the framework, then
# the outer app. No hardened runtime: it gates mic access behind entitlements
# and is only needed for notarization; this is a locally-built app.
IDENTITY="LocalFlow Signing"
# The identity lives in a dedicated keychain with a throwaway password (see
# setup-signing.sh). Unlock it so signing works in non-interactive shells
# (a locked keychain fails codesign with errSecInternalComponent).
security unlock-keychain -p localflow-signing localflow-signing.keychain 2>/dev/null || true
if ! security find-identity -v -p codesigning 2>/dev/null | grep -q "$IDENTITY"; then
    echo "==> Codesigning (ad-hoc — permissions will NOT survive rebuilds;"
    echo "    run Scripts/setup-signing.sh once to fix)…"
    IDENTITY="-"
else
    echo "==> Codesigning (stable identity: $IDENTITY)…"
fi

SPARKLE_FW="$APP/Contents/Frameworks/Sparkle.framework"
codesign --force -s "$IDENTITY" "$SPARKLE_FW/Versions/B/Autoupdate"
codesign --force -s "$IDENTITY" "$SPARKLE_FW/Versions/B/Updater.app"
codesign --force -s "$IDENTITY" "$SPARKLE_FW/Versions/B"
# mlx.metallib sits in Contents/MacOS, so codesign treats it as a nested
# subcomponent that must be signed (previously covered by --deep).
if [[ -f "$APP/Contents/MacOS/mlx.metallib" ]]; then
    codesign --force -s "$IDENTITY" "$APP/Contents/MacOS/mlx.metallib"
fi
codesign --force -s "$IDENTITY" --identifier "$BUNDLE_ID" "$APP"
codesign --verify --verbose=1 "$APP"

echo "==> Done: $APP ($VERSION, build $BUILD_NUM)"
