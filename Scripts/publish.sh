#!/bin/bash
# publish.sh — atomically publish a stable LocalFlow release on all platforms.
#
#   Scripts/publish.sh <version>          e.g. Scripts/publish.sh 1.3.0
#
# Run Scripts/release.sh <version> first. This script commits appcast.xml
# locally, pushes only the immutable release tag, prepares the macOS draft,
# dispatches and waits for the three-platform release gate, and pushes main
# only after GitHub confirms that the complete release is public. It is safe to
# rerun: existing tags and assets must match exactly and are never overwritten.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

VERSION="${1:?usage: Scripts/publish.sh <version>   (e.g. 1.3.0)}"
if [[ ! "$VERSION" =~ ^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$ ]]; then
    echo "error: stable versions must have the exact form X.Y.Z" >&2
    exit 1
fi
IFS=. read -r VERSION_MAJOR VERSION_MINOR VERSION_PATCH <<<"$VERSION"
if (( VERSION_MINOR > 99 || VERSION_PATCH > 99 )); then
    echo "error: minor and patch must be below 100 for the macOS build-number scheme" >&2
    exit 1
fi

TAG="v$VERSION"
REPO="yonif8/LocalFlow"
DMG="$REPO_ROOT/dist/LocalFlow-$VERSION.dmg"
APPCAST="$REPO_ROOT/appcast.xml"
PUBLIC_KEY="$REPO_ROOT/Resources/sparkle-public-ed-key.txt"
NOTES="$REPO_ROOT/dist/release-notes-$VERSION.md"
EXPECTED_DMG="LocalFlow-$VERSION.dmg"
EXPECTED_ASSETS=(
    "$EXPECTED_DMG"
    "appcast.xml"
    "LocalFlow-$VERSION-windows-x64-setup.exe"
    "LocalFlow-$VERSION-windows-x64-setup.exe.sha256"
    "windows-update.json"
    "LocalFlow-x86_64.AppImage"
    "LocalFlow-x86_64.AppImage.zsync"
    "localflow_${VERSION}_amd64.deb"
    "LocalFlow-$VERSION-Linux-Integration.tar.gz"
    "SHA256SUMS"
    "SHA256SUMS.asc"
    "LocalFlow-Linux-signing-key.asc"
)

GH="$(command -v gh || true)"
[[ -n "$GH" ]] || GH="$HOME/.local/bin/gh"
[[ -x "$GH" ]] || { echo "error: gh CLI not found" >&2; exit 1; }
command -v openssl >/dev/null || { echo "error: openssl is required" >&2; exit 1; }
command -v python3 >/dev/null || { echo "error: python3 is required" >&2; exit 1; }
command -v shasum >/dev/null || { echo "error: shasum is required" >&2; exit 1; }
command -v swift >/dev/null || { echo "error: swift is required" >&2; exit 1; }
[[ -s "$DMG" ]] || { echo "error: $DMG missing or empty — run Scripts/release.sh $VERSION first" >&2; exit 1; }
[[ -s "$APPCAST" ]] || { echo "error: appcast.xml missing or empty — run Scripts/release.sh $VERSION first" >&2; exit 1; }
[[ -s "$PUBLIC_KEY" ]] || { echo "error: pinned Sparkle public key is missing" >&2; exit 1; }

if [[ "$(git branch --show-current)" != "main" ]]; then
    echo "error: releases must be published from the local main branch" >&2
    exit 1
fi
if git rev-parse -q --verify MERGE_HEAD >/dev/null \
    || [[ -d "$(git rev-parse --git-path rebase-merge)" ]] \
    || [[ -d "$(git rev-parse --git-path rebase-apply)" ]]; then
    echo "error: finish the current merge or rebase before publishing" >&2
    exit 1
fi

# release.sh is allowed to change only the generated appcast. Everything else
# must already be committed so the tag describes exactly what is being built.
while IFS= read -r status_line; do
    [[ -z "$status_line" ]] && continue
    if [[ "${status_line:3}" != "appcast.xml" ]]; then
        echo "error: unrelated worktree change blocks release: $status_line" >&2
        exit 1
    fi
done < <(git status --porcelain=v1 --untracked-files=all)

WORK_DIR="$(mktemp -d)"
MOUNT_DIR="$WORK_DIR/dmg-mount"
DMG_ATTACHED=0
cleanup() {
    if [[ $DMG_ATTACHED -eq 1 ]]; then
        hdiutil detach "$MOUNT_DIR" -force >/dev/null 2>&1 || true
    fi
    rm -rf -- "$WORK_DIR"
}
trap cleanup EXIT

echo "==> [1/8] Validating the macOS artifact and appcast…"
hdiutil verify "$DMG" >/dev/null
ED_SIG="$(python3 - "$APPCAST" "$DMG" "$VERSION" "$REPO" <<'PY'
import os
import sys
import xml.etree.ElementTree as ET

appcast_path, dmg_path, version, repo = sys.argv[1:]
sparkle = "http://www.andymatuschak.org/xml-namespaces/sparkle"
expected_url = f"https://github.com/{repo}/releases/download/v{version}/LocalFlow-{version}.dmg"
major, minor, patch = map(int, version.split("."))
expected_build = str(major * 10000 + minor * 100 + patch)
root = ET.parse(appcast_path).getroot()
items = root.findall("./channel/item")
if not items or items[0].findtext(f"{{{sparkle}}}shortVersionString") != version:
    raise SystemExit(f"error: {version} must be the newest appcast item")
matches = []
for item in items:
    short_version = item.findtext(f"{{{sparkle}}}shortVersionString")
    enclosure = item.find("enclosure")
    if short_version == version and enclosure is not None:
        matches.append(enclosure)
if len(matches) != 1:
    raise SystemExit(f"error: appcast.xml must contain exactly one item for {version}")
enclosure = matches[0]
item = next(item for item in items if item.findtext(f"{{{sparkle}}}shortVersionString") == version)
checks = {
    "build number": (item.findtext(f"{{{sparkle}}}version"), expected_build),
    "minimum macOS version": (item.findtext(f"{{{sparkle}}}minimumSystemVersion"), "15.0"),
    "hardware requirement": (item.findtext(f"{{{sparkle}}}hardwareRequirements"), "arm64"),
    "download URL": (enclosure.get("url"), expected_url),
    "DMG length": (enclosure.get("length"), str(os.path.getsize(dmg_path))),
}
for label, (actual, expected) in checks.items():
    if actual != expected:
        raise SystemExit(f"error: appcast {label} mismatch: expected {expected!r}, got {actual!r}")
signature = enclosure.get(f"{{{sparkle}}}edSignature")
if not signature:
    raise SystemExit("error: the appcast item has no Sparkle EdDSA signature")
print(signature)
PY
)"
swift "$REPO_ROOT/Scripts/verify-sparkle-signature.swift" "$DMG" "$ED_SIG" "$PUBLIC_KEY"

mkdir "$MOUNT_DIR"
hdiutil attach -readonly -nobrowse -mountpoint "$MOUNT_DIR" "$DMG" >/dev/null
DMG_ATTACHED=1
printf '%s\n' Applications LocalFlow.app | sort >"$WORK_DIR/expected-dmg-contents"
find "$MOUNT_DIR" -mindepth 1 -maxdepth 1 -exec basename {} \; | sort >"$WORK_DIR/actual-dmg-contents"
if ! diff -u "$WORK_DIR/expected-dmg-contents" "$WORK_DIR/actual-dmg-contents"; then
    echo "error: the DMG contains missing or unexpected top-level entries" >&2
    exit 1
fi
[[ -L "$MOUNT_DIR/Applications" && "$(readlink "$MOUNT_DIR/Applications")" == "/Applications" ]] || {
    echo "error: the DMG's Applications link is missing or invalid" >&2
    exit 1
}
MOUNTED_APP="$MOUNT_DIR/LocalFlow.app"
INFO_PLIST="$MOUNTED_APP/Contents/Info.plist"
EXECUTABLE="$MOUNTED_APP/Contents/MacOS/LocalFlow"
METALLIB="$MOUNTED_APP/Contents/MacOS/mlx.metallib"
[[ -f "$INFO_PLIST" && -x "$EXECUTABLE" && -s "$METALLIB" ]] || {
    echo "error: the DMG does not contain a complete LocalFlow app" >&2
    exit 1
}
EXPECTED_BUILD=$((10#$VERSION_MAJOR * 10000 + 10#$VERSION_MINOR * 100 + 10#$VERSION_PATCH))
plist_value() { /usr/libexec/PlistBuddy -c "Print :$1" "$INFO_PLIST"; }
[[ "$(plist_value CFBundleIdentifier)" == "com.localflow.app" ]] || { echo "error: macOS bundle identifier mismatch" >&2; exit 1; }
[[ "$(plist_value CFBundleShortVersionString)" == "$VERSION" ]] || { echo "error: macOS bundle version mismatch" >&2; exit 1; }
[[ "$(plist_value CFBundleVersion)" == "$EXPECTED_BUILD" ]] || { echo "error: macOS build number mismatch" >&2; exit 1; }
[[ "$(plist_value LSMinimumSystemVersion)" == "15.0" ]] || { echo "error: macOS minimum-system version mismatch" >&2; exit 1; }
[[ "$(plist_value SUFeedURL)" == "https://raw.githubusercontent.com/$REPO/main/appcast.xml" ]] || { echo "error: Sparkle feed URL mismatch" >&2; exit 1; }
EXPECTED_PUBLIC_KEY="$(tr -d '[:space:]' <"$PUBLIC_KEY")"
[[ "$(plist_value SUPublicEDKey)" == "$EXPECTED_PUBLIC_KEY" ]] || { echo "error: embedded Sparkle public key mismatch" >&2; exit 1; }
[[ "$(lipo -archs "$EXECUTABLE")" == "arm64" ]] || { echo "error: macOS release executable is not thin arm64" >&2; exit 1; }
file "$METALLIB" | grep -Fq "MetalLib executable" || { echo "error: packaged mlx.metallib is invalid" >&2; exit 1; }
[[ -d "$MOUNTED_APP/Contents/Frameworks/Sparkle.framework" ]] || { echo "error: Sparkle.framework is missing" >&2; exit 1; }
codesign --verify --deep --strict --verbose=2 "$MOUNTED_APP"
codesign -dv --verbose=4 "$MOUNTED_APP" 2>&1 | grep -Fx "Authority=LocalFlow Signing" >/dev/null || {
    echo "error: macOS release is not signed with the stable LocalFlow identity" >&2
    exit 1
}
hdiutil detach "$MOUNT_DIR" >/dev/null
DMG_ATTACHED=0

echo "==> [2/8] Checking GitHub and remote main…"
"$GH" auth status --hostname github.com >/dev/null
if ! "$GH" repo view "$REPO" >/dev/null 2>&1; then
    echo "error: GitHub repository $REPO does not exist or is not accessible" >&2
    exit 1
fi
"$GH" api --paginate "repos/$REPO/releases?per_page=100" \
    --jq '.[] | [.tag_name, .draft, .prerelease] | @tsv' >"$WORK_DIR/github-releases.tsv"
python3 - "$WORK_DIR/github-releases.tsv" "$VERSION" <<'PY'
import pathlib
import re
import sys

path, requested = sys.argv[1:]
requested_version = tuple(map(int, requested.split(".")))
newer = []
for line in pathlib.Path(path).read_text(encoding="utf-8").splitlines():
    tag, draft, prerelease = line.split("\t")
    match = re.fullmatch(r"v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)", tag)
    if not match or draft == "true" or prerelease == "true":
        continue
    version = tuple(map(int, match.groups()))
    if version > requested_version:
        newer.append(tag)
if newer:
    raise SystemExit(
        "error: refusing to publish an older stable version; newer public release(s) exist: "
        + ", ".join(sorted(newer))
    )
PY
origin_url="$(git remote get-url origin 2>/dev/null || true)"
if [[ "$origin_url" != "https://github.com/$REPO.git" \
    && "$origin_url" != "git@github.com:${REPO}.git" \
    && "$origin_url" != "ssh://git@github.com/${REPO}.git" ]]; then
    echo "error: origin does not point to github.com/$REPO" >&2
    exit 1
fi
git fetch --no-tags origin main
remote_main="$(git rev-parse refs/remotes/origin/main)"
if ! git merge-base --is-ancestor "$remote_main" HEAD; then
    echo "error: local main is behind or diverged from origin/main; reconcile it before publishing" >&2
    exit 1
fi

echo "==> [3/8] Committing appcast.xml locally…"
if [[ -n "$(git status --porcelain=v1 -- appcast.xml)" ]]; then
    git add -- appcast.xml
    staged_paths="$(git diff --cached --name-only)"
    if [[ "$staged_paths" != "appcast.xml" ]]; then
        echo "error: the index contains changes other than appcast.xml" >&2
        exit 1
    fi
    git commit -m "Appcast for $TAG"
else
    echo "    appcast.xml is already committed."
fi
if [[ -n "$(git status --porcelain=v1 --untracked-files=all)" ]]; then
    echo "error: the release worktree is not clean after committing appcast.xml" >&2
    git status --porcelain=v1 >&2
    exit 1
fi
RELEASE_COMMIT="$(git rev-parse HEAD)"

echo "==> [4/8] Creating and explicitly pushing ${TAG}…"
if git show-ref --verify --quiet "refs/tags/$TAG"; then
    if [[ "$(git rev-parse "$TAG^{commit}")" != "$RELEASE_COMMIT" ]]; then
        echo "error: local tag $TAG points at a different commit" >&2
        exit 1
    fi
else
    git tag -a "$TAG" -m "LocalFlow $VERSION"
fi
remote_tag_lines="$(git ls-remote --tags origin "refs/tags/$TAG" "refs/tags/$TAG^{}")"
if [[ -n "$remote_tag_lines" ]]; then
    remote_tag_commit="$(awk '$2 ~ /\^\{\}$/ { print $1 }' <<<"$remote_tag_lines")"
    [[ -n "$remote_tag_commit" ]] || remote_tag_commit="$(awk 'NR == 1 { print $1 }' <<<"$remote_tag_lines")"
    if [[ "$remote_tag_commit" != "$RELEASE_COMMIT" ]]; then
        echo "error: remote tag $TAG already points at a different commit" >&2
        exit 1
    fi
else
    git push origin "refs/tags/$TAG:refs/tags/$TAG"
fi
remote_tag_lines="$(git ls-remote --tags origin "refs/tags/$TAG" "refs/tags/$TAG^{}")"
remote_tag_commit="$(awk '$2 ~ /\^\{\}$/ { print $1 }' <<<"$remote_tag_lines")"
[[ -n "$remote_tag_commit" ]] || remote_tag_commit="$(awk 'NR == 1 { print $1 }' <<<"$remote_tag_lines")"
if [[ "$remote_tag_commit" != "$RELEASE_COMMIT" ]]; then
    echo "error: GitHub did not retain $TAG at the expected commit" >&2
    exit 1
fi

echo "==> [5/8] Creating or verifying the macOS release draft…"
if [[ ! -f "$NOTES" ]]; then
    printf '%s\n' \
        "LocalFlow $VERSION — fully-local dictation for macOS, Windows, and Linux." \
        "" \
        "- Hold-to-talk dictation, transcribed entirely on your computer" \
        "- Optional on-device polish pass" \
        "- Screen-aware terminology and a bounded local terminology bank" \
        "- Signed, in-app updates on macOS, Windows, and the recommended Linux AppImage" \
        "" \
        "Choose the DMG (macOS), setup EXE (Windows), or AppImage (recommended for Linux). The DEB is available for package-managed installs." \
        >"$NOTES"
fi

if ! "$GH" release view "$TAG" --repo "$REPO" >/dev/null 2>&1; then
    "$GH" release create "$TAG" "$DMG" "$APPCAST" --repo "$REPO" --verify-tag --draft \
        --title "LocalFlow $VERSION" --notes-file "$NOTES"
fi

RELEASE_JSON="$WORK_DIR/release.json"
DOWNLOAD_DIR="$WORK_DIR/release-downloads"
mkdir "$DOWNLOAD_DIR"
"$GH" release view "$TAG" --repo "$REPO" \
    --json tagName,isDraft,isPrerelease,assets,url >"$RELEASE_JSON"
release_public="$(python3 - "$RELEASE_JSON" "$TAG" "${EXPECTED_ASSETS[@]}" <<'PY'
import json
import pathlib
import sys

path, expected_tag, *expected_assets = sys.argv[1:]
release = json.loads(pathlib.Path(path).read_text(encoding="utf-8"))
if release["tagName"] != expected_tag or release["isPrerelease"]:
    raise SystemExit("error: existing release metadata does not match this stable release")
assets = release["assets"]
names = [asset["name"] for asset in assets]
if len(names) != len(set(names)):
    raise SystemExit("error: the release contains duplicate asset names")
unexpected = sorted(set(names) - set(expected_assets))
if unexpected:
    raise SystemExit("error: unexpected assets exist on the release: " + ", ".join(unexpected))
if not release["isDraft"] and set(names) != set(expected_assets):
    missing = sorted(set(expected_assets) - set(names))
    raise SystemExit("error: the public release is incomplete: " + ", ".join(missing))
print("true" if not release["isDraft"] else "false")
PY
)"
"$GH" release view "$TAG" --repo "$REPO" --json assets --jq '.assets[].name' \
    | sort >"$WORK_DIR/remote-assets"
for local_asset in "$DMG" "$APPCAST"; do
    asset_name="$(basename "$local_asset")"
    if grep -Fxq "$asset_name" "$WORK_DIR/remote-assets"; then
        "$GH" release download "$TAG" --repo "$REPO" --pattern "$asset_name" --dir "$DOWNLOAD_DIR"
        if ! cmp --silent "$local_asset" "$DOWNLOAD_DIR/$asset_name"; then
            echo "error: the release already contains a different $asset_name" >&2
            exit 1
        fi
    elif [[ "$release_public" == "true" ]]; then
        echo "error: the public release is missing $asset_name and cannot be repaired in place" >&2
        exit 1
    else
        "$GH" release upload "$TAG" "$local_asset" --repo "$REPO"
    fi
done
"$GH" release view "$TAG" --repo "$REPO" --json assets \
    --jq '.assets[] | [.name, .size] | @tsv' >"$WORK_DIR/remote-assets-with-sizes"
python3 - "$WORK_DIR/remote-assets-with-sizes" "$EXPECTED_DMG" <<'PY'
import pathlib
import sys

path, expected_dmg = sys.argv[1:]
assets = {}
for line in pathlib.Path(path).read_text(encoding="utf-8").splitlines():
    name, size = line.split("\t")
    if name in assets:
        raise SystemExit(f"error: duplicate release asset: {name}")
    assets[name] = int(size)
for name in (expected_dmg, "appcast.xml"):
    if assets.get(name, 0) <= 0:
        raise SystemExit(f"error: release must contain one non-empty {name}")
PY
DMG_SHA256="$(shasum -a 256 "$DMG" | awk '{print $1}')"
APPCAST_SHA256="$(shasum -a 256 "$APPCAST" | awk '{print $1}')"

if [[ "$release_public" != "true" ]]; then
    echo "==> [6/8] Dispatching the exact-tag, three-platform release gate on ${TAG}…"
    REQUEST_ID="$(openssl rand -hex 16)"
    run_output="$("$GH" workflow run release.yml --repo "$REPO" --ref "$TAG" \
        -f "tag=$TAG" -f "request_id=$REQUEST_ID" -f "dmg_sha256=$DMG_SHA256" \
        -f "appcast_sha256=$APPCAST_SHA256")"
    RUN_ID=""
    if [[ "$run_output" =~ /actions/runs/([0-9]+) ]]; then
        RUN_ID="${BASH_REMATCH[1]}"
    fi
    for _ in {1..30}; do
        [[ -n "$RUN_ID" ]] && break
        RUN_ID="$("$GH" run list --repo "$REPO" --workflow release.yml \
            --event workflow_dispatch --commit "$RELEASE_COMMIT" --limit 100 \
            --json databaseId,displayTitle \
            --jq ".[] | select(.displayTitle == \"Release $TAG ($REQUEST_ID)\") | .databaseId" \
            | head -1)"
        [[ -n "$RUN_ID" ]] || sleep 2
    done
    if [[ -z "$RUN_ID" ]]; then
        echo "error: GitHub accepted the dispatch but its workflow run could not be identified" >&2
        exit 1
    fi
    echo "    workflow: https://github.com/$REPO/actions/runs/$RUN_ID"
    "$GH" run watch "$RUN_ID" --repo "$REPO" --exit-status
else
    echo "==> [6/8] The complete release is already public; skipping a duplicate build."
fi

echo "==> [7/8] Verifying the public three-platform release…"
"$GH" release view "$TAG" --repo "$REPO" \
    --json tagName,isDraft,isPrerelease,assets,url >"$RELEASE_JSON"
RELEASE_URL="$(python3 - "$RELEASE_JSON" "$TAG" "${EXPECTED_ASSETS[@]}" <<'PY'
import json
import pathlib
import sys

path, expected_tag, *expected_assets = sys.argv[1:]
release = json.loads(pathlib.Path(path).read_text(encoding="utf-8"))
actual = {asset["name"] for asset in release["assets"]}
expected = set(expected_assets)
if release["tagName"] != expected_tag or release["isDraft"] or release["isPrerelease"]:
    raise SystemExit("error: the release workflow did not produce a public stable release")
if actual != expected:
    raise SystemExit("error: the public release asset inventory is incomplete or unexpected")
if any(int(asset.get("size", 0)) <= 0 for asset in release["assets"]):
    raise SystemExit("error: the public release contains an empty asset")
print(release["url"])
PY
)"
FINAL_INPUT_DIR="$WORK_DIR/final-release-inputs"
mkdir "$FINAL_INPUT_DIR"
"$GH" release download "$TAG" --repo "$REPO" --pattern "$EXPECTED_DMG" --dir "$FINAL_INPUT_DIR"
"$GH" release download "$TAG" --repo "$REPO" --pattern "appcast.xml" --dir "$FINAL_INPUT_DIR"
cmp --silent "$DMG" "$FINAL_INPUT_DIR/$EXPECTED_DMG" || {
    echo "error: the public macOS DMG changed during publication" >&2
    exit 1
}
cmp --silent "$APPCAST" "$FINAL_INPUT_DIR/appcast.xml" || {
    echo "error: the public appcast changed during publication" >&2
    exit 1
}

"$GH" api --paginate "repos/$REPO/releases?per_page=100" \
    --jq '.[] | [.tag_name, .draft, .prerelease] | @tsv' >"$WORK_DIR/final-github-releases.tsv"
python3 - "$WORK_DIR/final-github-releases.tsv" "$VERSION" <<'PY'
import pathlib
import re
import sys

path, requested = sys.argv[1:]
requested_version = tuple(map(int, requested.split(".")))
for line in pathlib.Path(path).read_text(encoding="utf-8").splitlines():
    tag, draft, prerelease = line.split("\t")
    match = re.fullmatch(r"v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)", tag)
    if match and draft == "false" and prerelease == "false":
        if tuple(map(int, match.groups())) > requested_version:
            raise SystemExit(f"error: newer public release {tag} appeared during publication")
PY

echo "==> [8/8] Publishing main and the Sparkle appcast…"
if [[ "$(git rev-parse HEAD)" != "$RELEASE_COMMIT" \
    || -n "$(git status --porcelain=v1 --untracked-files=all)" ]]; then
    echo "error: the local release checkout changed while the workflow was running; main was not pushed" >&2
    exit 1
fi
git fetch --no-tags origin main
if ! git merge-base --is-ancestor refs/remotes/origin/main "$RELEASE_COMMIT"; then
    echo "error: origin/main advanced incompatibly while the release built; main was not pushed" >&2
    exit 1
fi
git push -u origin "HEAD:refs/heads/main"
if [[ "$(git ls-remote origin refs/heads/main | awk 'NR == 1 { print $1 }')" != "$RELEASE_COMMIT" ]]; then
    echo "error: origin/main does not point at the released commit" >&2
    exit 1
fi

echo
echo "==> LocalFlow $VERSION is public on all three platforms:"
echo "    $RELEASE_URL"
echo "    Sparkle feed: https://raw.githubusercontent.com/$REPO/main/appcast.xml"
