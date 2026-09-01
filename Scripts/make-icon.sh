#!/bin/bash
# make-icon.sh — render LocalFlow's app icon programmatically and emit
# Resources/AppIcon.icns (checked into the repo; make-app.sh copies it).
#
# Design: macOS-style rounded square (824pt body on the 1024 canvas, Big Sur
# grid), indigo→blue vertical gradient, white rounded waveform bars with a
# soft shadow. Pure AppKit/CoreGraphics + sips + iconutil — no assets, no
# third-party tools.
#
# Usage: Scripts/make-icon.sh   (re-run any time to regenerate)

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_ICNS="$REPO_ROOT/Resources/AppIcon.icns"
mkdir -p "$REPO_ROOT/Resources"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "==> Rendering 1024px master…"
/usr/bin/swift - "$TMP/icon-1024.png" <<'SWIFT'
import AppKit

let out = URL(fileURLWithPath: CommandLine.arguments[1])
let canvas = NSSize(width: 1024, height: 1024)
let image = NSImage(size: canvas)
image.lockFocus()
guard let ctx = NSGraphicsContext.current?.cgContext else { exit(1) }

// macOS icon grid: 824x824 body centered on the 1024 canvas, radius ~185.
let body = NSRect(x: 100, y: 100, width: 824, height: 824)
let radius: CGFloat = 185
let shape = NSBezierPath(roundedRect: body, xRadius: radius, yRadius: radius)

// Drop shadow behind the body (soft, downward) — matches system icons.
ctx.saveGState()
ctx.setShadow(offset: CGSize(width: 0, height: -14), blur: 36,
              color: NSColor.black.withAlphaComponent(0.30).cgColor)
NSColor(calibratedRed: 0.16, green: 0.20, blue: 0.55, alpha: 1).setFill()
shape.fill()
ctx.restoreGState()

// Vertical gradient fill: deep indigo (top) -> vivid blue (bottom).
shape.addClip()
let gradient = NSGradient(colors: [
    NSColor(calibratedRed: 0.28, green: 0.20, blue: 0.85, alpha: 1),   // indigo
    NSColor(calibratedRed: 0.13, green: 0.42, blue: 0.98, alpha: 1),   // blue
    NSColor(calibratedRed: 0.10, green: 0.62, blue: 0.99, alpha: 1),   // sky
])!
gradient.draw(in: body, angle: -90)

// Subtle top-edge highlight for depth.
let highlight = NSGradient(colors: [
    NSColor.white.withAlphaComponent(0.22),
    NSColor.white.withAlphaComponent(0.0),
])!
highlight.draw(in: NSRect(x: body.minX, y: body.maxY - 300,
                          width: body.width, height: 300), angle: -90)

// Waveform: rounded bars, symmetric, with a soft shadow.
ctx.setShadow(offset: CGSize(width: 0, height: -8), blur: 22,
              color: NSColor.black.withAlphaComponent(0.28).cgColor)
NSColor.white.setFill()
let heights: [CGFloat] = [150, 260, 420, 560, 420, 260, 150]
let barWidth: CGFloat = 58
let gap: CGFloat = 42
let total = CGFloat(heights.count) * barWidth + CGFloat(heights.count - 1) * gap
var x = (canvas.width - total) / 2
for h in heights {
    let bar = NSRect(x: x, y: (canvas.height - h) / 2, width: barWidth, height: h)
    NSBezierPath(roundedRect: bar, xRadius: barWidth / 2, yRadius: barWidth / 2).fill()
    x += barWidth + gap
}

image.unlockFocus()
guard let tiff = image.tiffRepresentation,
      let rep = NSBitmapImageRep(data: tiff),
      let png = rep.representation(using: .png, properties: [:]) else { exit(1) }
try png.write(to: out)
SWIFT

echo "==> Building iconset…"
ICONSET="$TMP/AppIcon.iconset"
mkdir -p "$ICONSET"
for s in 16 32 128 256 512; do
    sips -z $s $s "$TMP/icon-1024.png" --out "$ICONSET/icon_${s}x${s}.png" >/dev/null
    d=$((s * 2))
    sips -z $d $d "$TMP/icon-1024.png" --out "$ICONSET/icon_${s}x${s}@2x.png" >/dev/null
done

iconutil -c icns "$ICONSET" -o "$OUT_ICNS"
echo "==> Wrote $OUT_ICNS"
