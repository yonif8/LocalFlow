#!/bin/bash
# make-app.sh — build LocalFlow in release mode and assemble dist/LocalFlow.app.
#
# Works with Command Line Tools only (no Xcode required).
# Usage: Scripts/make-app.sh [--scratch-path <dir>]

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

# Prefer the Command Line Tools toolchain when present: this repo only needs
# CLT, and a machine whose xcode-select points at a Xcode.app with an
# unaccepted license fails `swift build` with an exit-69 license error.
if [[ -d /Library/Developer/CommandLineTools && -z "${DEVELOPER_DIR:-}" ]]; then
    export DEVELOPER_DIR=/Library/Developer/CommandLineTools
fi

SCRATCH_DIR="$REPO_ROOT/.build"
SCRATCH_ARGS=()
if [[ "${1:-}" == "--scratch-path" && -n "${2:-}" ]]; then
    SCRATCH_DIR="$2"
    SCRATCH_ARGS=(--scratch-path "$2")
fi

APP_NAME="LocalFlow"
BUNDLE_ID="com.localflow.app"
DIST="$REPO_ROOT/dist"
APP="$DIST/$APP_NAME.app"

echo "==> Building $APP_NAME (release)…"
# ${arr[@]+...} guard: bash 3.2's set -u treats an empty array as unbound.
swift build -c release --product LocalFlowApp ${SCRATCH_ARGS[@]+"${SCRATCH_ARGS[@]}"}
# (not `swift build --show-bin-path` — that trips an Xcode license check on
# machines with a dormant Xcode.app; the layout below is stable for SwiftPM)
BIN_PATH="$SCRATCH_DIR/release/LocalFlowApp"
[[ -x "$BIN_PATH" ]] || { echo "error: built binary not found at $BIN_PATH" >&2; exit 1; }

echo "==> Assembling $APP…"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"

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
	<string>0.1.0</string>
	<key>CFBundleVersion</key>
	<string>1</string>
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
</dict>
</plist>
PLIST

# ---- App icon -------------------------------------------------------------
# Copy a checked-in icon if present; otherwise try to generate a simple
# placeholder (rounded blue square with a waveform-ish glyph) via sips/iconutil.
# Failures here are non-fatal: the app just uses the generic icon.
ICON_SRC="$REPO_ROOT/Resources/AppIcon.icns"
if [[ -f "$ICON_SRC" ]]; then
    cp "$ICON_SRC" "$APP/Contents/Resources/AppIcon.icns"
    echo "==> Copied Resources/AppIcon.icns"
else
    echo "==> No Resources/AppIcon.icns; generating placeholder icon…"
    ICONTMP="$(mktemp -d)"
    if /usr/bin/swift - "$ICONTMP/icon-1024.png" <<'SWIFT' 2>/dev/null
import AppKit
let args = CommandLine.arguments
let out = URL(fileURLWithPath: args[1])
let size = NSSize(width: 1024, height: 1024)
let image = NSImage(size: size)
image.lockFocus()
let rect = NSRect(origin: .zero, size: size)
let bg = NSBezierPath(roundedRect: rect.insetBy(dx: 90, dy: 90), xRadius: 200, yRadius: 200)
NSColor(calibratedRed: 0.16, green: 0.36, blue: 0.95, alpha: 1).setFill()
bg.fill()
// Waveform bars
NSColor.white.setFill()
let heights: [CGFloat] = [180, 320, 480, 620, 480, 320, 180]
let barWidth: CGFloat = 56
let gap: CGFloat = 44
let total = CGFloat(heights.count) * barWidth + CGFloat(heights.count - 1) * gap
var x = (size.width - total) / 2
for h in heights {
    let bar = NSRect(x: x, y: (size.height - h) / 2, width: barWidth, height: h)
    NSBezierPath(roundedRect: bar, xRadius: barWidth / 2, yRadius: barWidth / 2).fill()
    x += barWidth + gap
}
image.unlockFocus()
guard let tiff = image.tiffRepresentation,
      let rep = NSBitmapImageRep(data: tiff),
      let png = rep.representation(using: .png, properties: [:]) else { exit(1) }
try png.write(to: out)
SWIFT
    then
        ICONSET="$ICONTMP/AppIcon.iconset"
        mkdir -p "$ICONSET"
        for s in 16 32 128 256 512; do
            sips -z $s $s "$ICONTMP/icon-1024.png" --out "$ICONSET/icon_${s}x${s}.png" >/dev/null
            d=$((s * 2))
            sips -z $d $d "$ICONTMP/icon-1024.png" --out "$ICONSET/icon_${s}x${s}@2x.png" >/dev/null
        done
        if iconutil -c icns "$ICONSET" -o "$APP/Contents/Resources/AppIcon.icns" 2>/dev/null; then
            echo "==> Placeholder icon generated."
        else
            echo "==> iconutil failed; shipping without an icon (non-fatal)."
        fi
    else
        echo "==> Icon rendering failed; shipping without an icon (non-fatal)."
    fi
    rm -rf "$ICONTMP"
fi

# ---- Codesign -------------------------------------------------------------
# Prefer the stable "LocalFlow Signing" identity (created by
# Scripts/setup-signing.sh): its designated requirement is stable across
# rebuilds, so TCC grants (Microphone / Input Monitoring / Accessibility)
# persist. Fall back to ad-hoc, whose grants die on every re-sign.
IDENTITY="LocalFlow Signing"
if security find-identity -v -p codesigning 2>/dev/null | grep -q "$IDENTITY"; then
    echo "==> Codesigning (stable identity: $IDENTITY)…"
    # No hardened runtime: it gates mic access behind entitlements and is
    # only needed for notarization; this is a locally-built app.
    codesign --force --deep -s "$IDENTITY" --identifier "$BUNDLE_ID" "$APP"
else
    echo "==> Codesigning (ad-hoc — permissions will NOT survive rebuilds;"
    echo "    run Scripts/setup-signing.sh once to fix)…"
    codesign --force --deep -s - "$APP"
fi

echo "==> Done: $APP"
