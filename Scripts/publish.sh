#!/bin/bash
# publish.sh — publish an already-built LocalFlow release to GitHub.
#
#   Scripts/publish.sh <version>          e.g. Scripts/publish.sh 1.0.0
#
# Run Scripts/release.sh <version> first (builds the DMG + appcast.xml).
# This script:
#   1. creates the public repo yonif8/LocalFlow if it doesn't exist
#   2. commits appcast.xml if it changed, pushes main
#   3. creates GitHub release v<version> with the DMG attached
#
# Requires gh (looked up in PATH, then ~/.local/bin) authenticated with repo
# scope. Idempotent: existing repo/release steps are skipped, not duplicated.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

VERSION="${1:?usage: Scripts/publish.sh <version>   (e.g. 1.0.0)}"
TAG="v$VERSION"
REPO="yonif8/LocalFlow"
DMG="$REPO_ROOT/dist/LocalFlow-$VERSION.dmg"

GH="$(command -v gh || true)"
[[ -n "$GH" ]] || GH="$HOME/.local/bin/gh"
[[ -x "$GH" ]] || { echo "error: gh CLI not found" >&2; exit 1; }

[[ -f "$DMG" ]] || { echo "error: $DMG missing — run Scripts/release.sh $VERSION first" >&2; exit 1; }
[[ -f "$REPO_ROOT/appcast.xml" ]] || { echo "error: appcast.xml missing — run Scripts/release.sh $VERSION first" >&2; exit 1; }
grep -q "LocalFlow-$VERSION.dmg" "$REPO_ROOT/appcast.xml" || {
    echo "error: appcast.xml does not reference LocalFlow-$VERSION.dmg — re-run Scripts/release.sh $VERSION" >&2
    exit 1; }

echo "==> [1/5] Ensuring GitHub repo $REPO exists…"
if "$GH" repo view "$REPO" > /dev/null 2>&1; then
    echo "    repo exists."
    if ! git remote get-url origin > /dev/null 2>&1; then
        echo "    adding origin remote…"
        git remote add origin "https://github.com/$REPO.git"
    fi
else
    echo "    creating public repo and pushing…"
    "$GH" repo create "$REPO" --public --source "$REPO_ROOT" --push \
        --description "Fully-local dictation for macOS — hold a key, speak, release"
fi

echo "==> [2/5] Committing appcast.xml if it changed…"
if [[ -n "$(git status --porcelain -- appcast.xml)" ]]; then
    git add appcast.xml
    git commit -m "Appcast for $TAG"
    echo "    committed."
else
    echo "    appcast.xml already committed."
fi
if [[ -n "$(git status --porcelain)" ]]; then
    echo "    WARNING: other uncommitted changes exist (not touched):"
    git status --porcelain | sed 's/^/      /'
fi

echo "==> [3/5] Pushing main…"
git push -u origin main

echo "==> [4/5] Writing release notes…"
NOTES="$REPO_ROOT/dist/release-notes-$VERSION.md"
if [[ ! -f "$NOTES" ]]; then
    cat > "$NOTES" <<EOF
LocalFlow $VERSION — fully-local dictation for macOS.

- Hold-to-talk dictation, transcribed entirely on your Mac (Parakeet ASR, on-device)
- Optional on-device polish pass (S1-mini by Superwhisper)
- Auto-updates via Sparkle

Install: download the DMG below, drag LocalFlow to Applications, and follow
the first-run steps in the README (self-signed app: one-time
"Open Anyway" in System Settings > Privacy & Security).
EOF
    echo "    wrote $NOTES (edit it and re-run to customize)."
else
    echo "    using existing $NOTES."
fi

echo "==> [5/5] Creating GitHub release $TAG…"
if "$GH" release view "$TAG" --repo "$REPO" > /dev/null 2>&1; then
    echo "    release exists; uploading DMG (clobber)…"
    "$GH" release upload "$TAG" "$DMG" --repo "$REPO" --clobber
else
    "$GH" release create "$TAG" "$DMG" --repo "$REPO" \
        --title "LocalFlow $VERSION" --notes-file "$NOTES"
fi

echo ""
echo "==> Published: https://github.com/$REPO/releases/tag/$TAG"
echo "    Update feed: https://raw.githubusercontent.com/yonif8/LocalFlow/main/appcast.xml"
