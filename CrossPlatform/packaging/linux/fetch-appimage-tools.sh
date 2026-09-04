#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "Usage: $0 destination-directory" >&2
    exit 2
fi

destination="$1"
mkdir -p "$destination"

download_verified() {
    local name="$1"
    local url="$2"
    local expected_sha256="$3"
    local output="${destination}/${name}"

    if [[ -f "$output" ]] && printf '%s  %s\n' "$expected_sha256" "$output" | sha256sum --check --status; then
        chmod 0755 "$output"
        return
    fi

    local temporary="${output}.download.$$"
    trap 'rm -f -- "$temporary"' RETURN
    curl --fail --location --proto '=https' --tlsv1.2 \
        --retry 3 --retry-all-errors --output "$temporary" "$url"
    printf '%s  %s\n' "$expected_sha256" "$temporary" | sha256sum --check --status || {
        echo "SHA-256 verification failed for ${name}." >&2
        exit 1
    }
    chmod 0755 "$temporary"
    mv -f -- "$temporary" "$output"
    trap - RETURN
}

# Versioned upstream releases plus fixed checksums make the packaging toolchain
# reproducible and prevent a mutable "continuous" asset from executing in CI.
download_verified \
    linuxdeploy-x86_64.AppImage \
    https://github.com/linuxdeploy/linuxdeploy/releases/download/1-alpha-20251107-1/linuxdeploy-x86_64.AppImage \
    c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d

download_verified \
    linuxdeploy-plugin-qt-x86_64.AppImage \
    https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/1-alpha-20250213-1/linuxdeploy-plugin-qt-x86_64.AppImage \
    15106be885c1c48a021198e7e1e9a48ce9d02a86dd0a1848f00bdbf3c1c92724

download_verified \
    appimagetool-x86_64.AppImage \
    https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage \
    ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0

# appimagetool otherwise downloads the mutable "continuous" runtime at build
# time. Supplying this reviewed runtime makes the produced ELF reproducible.
download_verified \
    runtime-x86_64 \
    https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64 \
    2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d

download_verified \
    AppImage-runtime-LICENSE \
    https://raw.githubusercontent.com/AppImage/type2-runtime/dd6cebedcbddde9c82f89b011e8e1d40b6e43868/LICENSE \
    aa154fc9070614bbe7921f89db11efd1dba7a1f3a41685958110e2230f9c0ca1

# Apache-2.0 licensed English LSTM data. This exact tagged artifact is bundled
# privately so OCR is deterministic and does not depend on the host distro.
download_verified \
    eng.traineddata \
    https://github.com/tesseract-ocr/tessdata_fast/raw/refs/tags/4.1.0/eng.traineddata \
    7d4322bd2a7749724879683fc3912cb542f19906c83bcc1a52132556427170b2

download_verified \
    tessdata-LICENSE \
    https://raw.githubusercontent.com/tesseract-ocr/tessdata_fast/4.1.0/LICENSE \
    cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30

download_verified \
    Leptonica-LICENSE \
    https://raw.githubusercontent.com/DanBloomberg/leptonica/f4138265b390f1921b9891d6669674d3157887d8/leptonica-license.txt \
    87829abb5bbb00b55a107365da89e9a33f86c4250169e5a1e5588505be7d5806

# The command-line updater supports an explicit in-place update and is launched
# with APPIMAGE_EXTRACT_AND_RUN=1, so updating also works without FUSE.
download_verified \
    appimageupdatetool-x86_64.AppImage \
    https://github.com/AppImageCommunity/AppImageUpdate/releases/download/2.0.0-alpha-1-20251018/appimageupdatetool-x86_64.AppImage \
    d976cdac667b03dee8cb23fb95ef74b042c406c5cbab3ff294d2b16efeaff84f

download_verified \
    AppImageUpdate-LICENSE.txt \
    https://raw.githubusercontent.com/AppImageCommunity/AppImageUpdate/a211784/LICENSE.txt \
    39089eaef4a8262516be1a9dd33bcb9837b3bedb5fc74fcd0c1448c5cca2367b

# Kept as a packaging-only tool. It performs the cryptographic validation of
# the signature appimagetool embeds; it is not shipped in LocalFlow.
download_verified \
    validate-x86_64.AppImage \
    https://github.com/AppImageCommunity/AppImageUpdate/releases/download/2.0.0-alpha-1-20251018/validate-x86_64.AppImage \
    b10c8d39a0a917432af185afc92f1cd54b7f68aa70deda927acacf38ded84990
