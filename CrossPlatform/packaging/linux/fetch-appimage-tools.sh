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
