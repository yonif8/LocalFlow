#!/bin/bash
# Shared by local packaging, macOS CI, and the exact-tag release gate.

LF_MACOS_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

lf_macos_preflight() {
    local mode="${1:-local}" require_metal="${2:-false}"
    local required_swift actual_swift required_major required_minor actual_major actual_minor
    # shellcheck source=../macos-toolchain.env
    source "$LF_MACOS_ROOT/Scripts/macos-toolchain.env"
    if [[ "$(uname -s)" != Darwin || "$(uname -m)" != arm64 ]]; then
        echo "error: the macOS build contract requires Apple Silicon macOS" >&2
        return 1
    fi
    if [[ "$mode" == ci ]]; then
        export DEVELOPER_DIR="/Applications/Xcode_${LOCALFLOW_CI_XCODE_VERSION}.app/Contents/Developer"
    elif [[ -z "${DEVELOPER_DIR:-}" ]]; then
        DEVELOPER_DIR="$(xcode-select -p)"
        if [[ "$DEVELOPER_DIR" == /Library/Developer/CommandLineTools ]]; then
            DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
        fi
        export DEVELOPER_DIR
    fi
    if [[ ! -d "$DEVELOPER_DIR" || "$DEVELOPER_DIR" != *.app/Contents/Developer ]]; then
        echo "error: full Xcode is required; selected developer directory: $DEVELOPER_DIR" >&2
        return 1
    fi
    xcodebuild -version || return
    LF_SWIFT_VERSION="$(swift --version)" || return
    printf '%s\n' "$LF_SWIFT_VERSION"
    required_swift="$(sed -nE 's#^// swift-tools-version: ([0-9]+\.[0-9]+).*#\1#p' "$LF_MACOS_ROOT/Package.swift")"
    actual_swift="$(printf '%s\n' "$LF_SWIFT_VERSION" | sed -nE 's/.*Swift version ([0-9]+\.[0-9]+).*/\1/p' | head -1)"
    if [[ ! "$required_swift" =~ ^[0-9]+\.[0-9]+$ || ! "$actual_swift" =~ ^[0-9]+\.[0-9]+$ ]]; then
        echo "error: could not determine required and installed Swift versions" >&2
        return 1
    fi
    IFS=. read -r required_major required_minor <<<"$required_swift"
    IFS=. read -r actual_major actual_minor <<<"$actual_swift"
    if (( actual_major < required_major || (actual_major == required_major && actual_minor < required_minor) )); then
        echo "error: Package.swift requires Swift $required_swift; selected Xcode provides $actual_swift" >&2
        return 1
    fi
    if [[ "$require_metal" == true ]]; then
        xcrun --sdk macosx metal --version >/dev/null || {
            echo "error: install this Xcode's Metal Toolchain before packaging" >&2
            return 1
        }
    fi
}

lf_timed() {
    local label="$1" started elapsed status=0
    shift
    started="$(date +%s)"
    "$@" || status=$?
    elapsed=$(( $(date +%s) - started ))
    printf 'TIMING: %s: %ss (exit %s)\n' "$label" "$elapsed" "$status"
    if [[ -n "${GITHUB_STEP_SUMMARY:-}" ]]; then
        printf '| %s | %ss | %s |\n' "$label" "$elapsed" "$status" >>"$GITHUB_STEP_SUMMARY"
    fi
    return "$status"
}

lf_build_and_test_macos() {
    local scratch="$1"
    if [[ -n "${GITHUB_STEP_SUMMARY:-}" ]]; then
        printf '### macOS build timings\n\n| Stage | Duration | Exit |\n| --- | --- | --- |\n' >>"$GITHUB_STEP_SUMMARY"
    fi
    lf_timed "Resolve pinned dependencies" swift package --scratch-path "$scratch" resolve || return
    git -C "$LF_MACOS_ROOT" diff --exit-code -- Package.resolved || return
    # swift test builds app AND tests with the required testable imports in one
    # plan. A separate product-only build changes flags and recompiles modules.
    # Never skip this command on a cache hit: all tests execute on every run.
    lf_timed "Build app and run all tests once" swift test --configuration release \
        --parallel --scratch-path "$scratch" --disable-automatic-resolution \
        --disable-build-manifest-caching || return
    [[ -x "$scratch/release/LocalFlowApp" ]] || {
        echo "error: the combined build did not produce LocalFlowApp" >&2
        return 1
    }
}
