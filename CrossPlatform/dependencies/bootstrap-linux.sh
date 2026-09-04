#!/usr/bin/env bash

set -Eeuo pipefail
IFS=$'\n\t'
umask 022

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
readonly LOCK_MANIFEST="${SCRIPT_DIR}/runtime-lock.json"

readonly NEMO_VERSION="0.1.0"
readonly LLAMA_VERSION="b10794"
readonly LLAMA_COMMIT="f9f09f02cc44d87d842dbd2d578857d92d4bb63b"

readonly LLAMA_SOURCE_FILENAME="llama.cpp-${LLAMA_COMMIT}.tar.gz"
readonly LLAMA_SOURCE_URL="https://codeload.github.com/ggml-org/llama.cpp/tar.gz/${LLAMA_COMMIT}"
readonly LLAMA_SOURCE_SIZE="37295615"
readonly LLAMA_SOURCE_SHA256="50b96e851f70552ae3bb7bd5192107a052e6148ccc45a180db08fc1fe7b5bc4f"

log() {
    printf '[localflow-deps] %s\n' "$*" >&2
}

die() {
    log "error: $*"
    exit 1
}

usage() {
    cat <<'EOF'
Usage: bootstrap-linux.sh [--prefix ABSOLUTE_DIRECTORY] [--arch ARCHITECTURE]

Install the exact CPU SDK/runtime versions recorded in runtime-lock.json.
Supported architectures: x86_64 (amd64) and aarch64 (arm64).
EOF
}

PREFIX="${SCRIPT_DIR}/.runtime"
REQUESTED_ARCH="$(uname -m)"

while (($# > 0)); do
    case "$1" in
        --prefix)
            (($# >= 2)) || die "--prefix requires a value"
            PREFIX="$2"
            shift 2
            ;;
        --arch)
            (($# >= 2)) || die "--arch requires a value"
            REQUESTED_ARCH="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            die "unknown argument: $1"
            ;;
    esac
done

[[ "$(uname -s)" == "Linux" ]] || die "this bootstrap supports Linux only"
[[ -f "$LOCK_MANIFEST" ]] || die "missing lock manifest: $LOCK_MANIFEST"

case "${REQUESTED_ARCH,,}" in
    x86_64|amd64)
        ARCH="x86_64"
        NEMO_FILENAME="nemo-speech-0.1.0-linux-x86_64-cpu.tar.gz"
        NEMO_URL="https://github.com/NVIDIA/NeMo-Speech.cpp/releases/download/v0.1.0/${NEMO_FILENAME}"
        NEMO_SIZE="4583913"
        NEMO_SHA256="0f74131d631ad2c694cf0ec53490866bb6461147959589a69fb6fc231944065b"
        LLAMA_FILENAME="llama-b10794-bin-ubuntu-x64.tar.gz"
        LLAMA_URL="https://github.com/ggml-org/llama.cpp/releases/download/b10794/${LLAMA_FILENAME}"
        LLAMA_SIZE="16734681"
        LLAMA_SHA256="175312e0aad5e4eac77eb0736c4ddefe482f04386741f59b0222be158932c29f"
        LLAMA_CPU_LIBS=(
            libggml-cpu-alderlake.so
            libggml-cpu-cannonlake.so
            libggml-cpu-cascadelake.so
            libggml-cpu-cooperlake.so
            libggml-cpu-haswell.so
            libggml-cpu-icelake.so
            libggml-cpu-ivybridge.so
            libggml-cpu-piledriver.so
            libggml-cpu-sandybridge.so
            libggml-cpu-sapphirerapids.so
            libggml-cpu-skylakex.so
            libggml-cpu-sse42.so
            libggml-cpu-x64.so
            libggml-cpu-zen4.so
        )
        ;;
    aarch64|arm64)
        ARCH="aarch64"
        NEMO_FILENAME="nemo-speech-0.1.0-linux-aarch64-cpu.tar.gz"
        NEMO_URL="https://github.com/NVIDIA/NeMo-Speech.cpp/releases/download/v0.1.0/${NEMO_FILENAME}"
        NEMO_SIZE="4328117"
        NEMO_SHA256="0e4112255d566de7bdd142f239e984995c4447103ba8feb41f2bb5c559d561d3"
        LLAMA_FILENAME="llama-b10794-bin-ubuntu-arm64.tar.gz"
        LLAMA_URL="https://github.com/ggml-org/llama.cpp/releases/download/b10794/${LLAMA_FILENAME}"
        LLAMA_SIZE="13374927"
        LLAMA_SHA256="d1e1256e90425c82477ed1fc9bdafed4422fe4e16e0a4ff9ed3a47123ab0e7d5"
        LLAMA_CPU_LIBS=(
            libggml-cpu-armv8.0_1.so
            libggml-cpu-armv8.2_1.so
            libggml-cpu-armv8.2_2.so
            libggml-cpu-armv8.2_3.so
            libggml-cpu-armv8.6_1.so
            libggml-cpu-armv8.6_2.so
            libggml-cpu-armv9.2_1.so
            libggml-cpu-armv9.2_2.so
        )
        ;;
    *)
        die "unsupported Linux architecture '${REQUESTED_ARCH}'; expected x86_64/amd64 or aarch64/arm64"
        ;;
esac

case "$PREFIX" in
    /*) ;;
    *) die "--prefix must be an absolute path" ;;
esac

for required_command in curl tar stat mktemp cp mv mkdir; do
    command -v "$required_command" >/dev/null 2>&1 || die "required command not found: $required_command"
done
if command -v sha256sum >/dev/null 2>&1; then
    SHA_COMMAND="sha256sum"
elif command -v shasum >/dev/null 2>&1; then
    SHA_COMMAND="shasum"
else
    die "required command not found: sha256sum or shasum"
fi

mkdir -p -- "$PREFIX"
PREFIX="$(cd -- "$PREFIX" && pwd -P)"
readonly PREFIX
readonly PLATFORM_ROOT="${PREFIX}/linux-${ARCH}"
readonly DOWNLOAD_ROOT="${PREFIX}/downloads"
readonly NEMO_ROOT="${PLATFORM_ROOT}/nemo-speech-${NEMO_VERSION}-cpu"
readonly LLAMA_ROOT="${PLATFORM_ROOT}/llama-${LLAMA_VERSION}-cpu"
readonly LOCK_PATH="${PLATFORM_ROOT}/.bootstrap.lock"

mkdir -p -- "$PLATFORM_ROOT" "$DOWNLOAD_ROOT"
if ! mkdir -- "$LOCK_PATH" 2>/dev/null; then
    die "another bootstrap is using ${PLATFORM_ROOT}; remove ${LOCK_PATH} only if no bootstrap is running"
fi

WORK_ROOT=""
cleanup() {
    local status=$?
    trap - EXIT INT TERM
    if [[ -n "${WORK_ROOT:-}" && "$WORK_ROOT" == "${PLATFORM_ROOT}/.bootstrap-work."* && -d "$WORK_ROOT" ]]; then
        rm -rf -- "$WORK_ROOT"
    fi
    rmdir -- "$LOCK_PATH" 2>/dev/null || true
    exit "$status"
}
trap cleanup EXIT INT TERM

WORK_ROOT="$(mktemp -d "${PLATFORM_ROOT}/.bootstrap-work.XXXXXX")"

sha256_file() {
    if [[ "$SHA_COMMAND" == "sha256sum" ]]; then
        sha256sum -- "$1" | awk '{print tolower($1)}'
    else
        shasum -a 256 -- "$1" | awk '{print tolower($1)}'
    fi
}

file_matches_lock() {
    local path="$1"
    local expected_size="$2"
    local expected_sha="$3"
    local actual_size actual_sha

    [[ -f "$path" ]] || return 1
    actual_size="$(stat -c '%s' -- "$path")"
    [[ "$actual_size" == "$expected_size" ]] || return 1
    actual_sha="$(sha256_file "$path")"
    [[ "$actual_sha" == "$expected_sha" ]]
}

download_locked() {
    local filename="$1"
    local url="$2"
    local expected_size="$3"
    local expected_sha="$4"
    local destination="${DOWNLOAD_ROOT}/${filename}"
    local temporary

    if file_matches_lock "$destination" "$expected_size" "$expected_sha"; then
        log "using verified cache: $filename"
        printf '%s\n' "$destination"
        return 0
    fi

    if [[ -e "$destination" ]]; then
        log "discarding cache entry that does not match the lock: $filename"
        rm -f -- "$destination"
    fi

    temporary="$(mktemp "${DOWNLOAD_ROOT}/.${filename}.download.XXXXXX")"
    log "downloading locked asset: $filename"
    if ! curl \
        --proto '=https' \
        --proto-redir '=https' \
        --tlsv1.2 \
        --fail \
        --location \
        --silent \
        --show-error \
        --retry 3 \
        --retry-delay 1 \
        --output "$temporary" \
        "$url"; then
        rm -f -- "$temporary"
        die "download failed: $url"
    fi

    if ! file_matches_lock "$temporary" "$expected_size" "$expected_sha"; then
        local actual_size actual_sha
        actual_size="$(stat -c '%s' -- "$temporary")"
        actual_sha="$(sha256_file "$temporary")"
        rm -f -- "$temporary"
        die "integrity check failed for ${filename} (size ${actual_size}, sha256 ${actual_sha}); expected size ${expected_size}, sha256 ${expected_sha}"
    fi

    mv -- "$temporary" "$destination"
    printf '%s\n' "$destination"
}

validate_tar_paths() {
    local archive="$1"
    local listing component entry
    listing="$(mktemp "${WORK_ROOT}/archive-list.XXXXXX")"

    tar -tzf "$archive" >"$listing" || die "cannot read archive: $archive"
    while IFS= read -r entry; do
        [[ -n "$entry" ]] || continue
        [[ "$entry" != /* ]] || die "archive contains an absolute path: $entry"
        IFS='/' read -r -a components <<<"$entry"
        for component in "${components[@]}"; do
            [[ "$component" != ".." ]] || die "archive contains a parent traversal: $entry"
        done
    done <"$listing"
}

marker_matches() {
    local root="$1"
    local expected="$2"
    [[ -f "${root}/.localflow-runtime-lock" ]] || return 1
    [[ "$(cat -- "${root}/.localflow-runtime-lock")" == "$expected" ]]
}

NEMO_MARKER="$(printf '%s\n' \
    'component=nemo-speech-cpp' \
    "version=${NEMO_VERSION}" \
    'platform=linux' \
    "architecture=${ARCH}" \
    "archive_sha256=${NEMO_SHA256}")"

LLAMA_MARKER="$(printf '%s\n' \
    'component=llama-cpp' \
    "version=${LLAMA_VERSION}" \
    "commit=${LLAMA_COMMIT}" \
    'platform=linux' \
    "architecture=${ARCH}" \
    "runtime_sha256=${LLAMA_SHA256}" \
    "source_sha256=${LLAMA_SOURCE_SHA256}")"

nemo_root_is_valid() {
    marker_matches "$NEMO_ROOT" "$NEMO_MARKER" &&
        [[ -f "${NEMO_ROOT}/include/nemo_speech/asr.h" ]] &&
        [[ -f "${NEMO_ROOT}/lib/libnemo_speech_asr_c.so" ]] &&
        [[ -f "${NEMO_ROOT}/lib/libnemo_speech_asr.so" ]] &&
        [[ -f "${NEMO_ROOT}/share/licenses/nemo-speech/LICENSE" ]] &&
        [[ -f "${NEMO_ROOT}/share/licenses/nemo-speech/THIRD_PARTY_NOTICES.md" ]]
}

llama_root_is_valid() {
    local library
    marker_matches "$LLAMA_ROOT" "$LLAMA_MARKER" || return 1
    [[ -f "${LLAMA_ROOT}/include/llama.h" ]] || return 1
    [[ -f "${LLAMA_ROOT}/include/ggml.h" ]] || return 1
    [[ -f "${LLAMA_ROOT}/lib/libllama.so" ]] || return 1
    [[ -f "${LLAMA_ROOT}/lib/libggml.so" ]] || return 1
    [[ -f "${LLAMA_ROOT}/lib/libggml-base.so" ]] || return 1
    [[ -f "${LLAMA_ROOT}/share/licenses/llama.cpp/LICENSE" ]] || return 1
    for library in "${LLAMA_CPU_LIBS[@]}"; do
        [[ -f "${LLAMA_ROOT}/lib/${library}" ]] || return 1
    done
}

if [[ -e "$NEMO_ROOT" ]] && ! nemo_root_is_valid; then
    die "refusing to overwrite an incomplete or differently locked root: $NEMO_ROOT"
fi
if [[ -e "$LLAMA_ROOT" ]] && ! llama_root_is_valid; then
    die "refusing to overwrite an incomplete or differently locked root: $LLAMA_ROOT"
fi

if ! nemo_root_is_valid; then
    nemo_archive="$(download_locked "$NEMO_FILENAME" "$NEMO_URL" "$NEMO_SIZE" "$NEMO_SHA256")"
    validate_tar_paths "$nemo_archive"

    nemo_stage="${WORK_ROOT}/nemo-stage"
    mkdir -- "$nemo_stage"
    tar -xzf "$nemo_archive" -C "$nemo_stage" --strip-components=1
    printf '%s\n' "$NEMO_MARKER" >"${nemo_stage}/.localflow-runtime-lock"

    [[ -f "${nemo_stage}/include/nemo_speech/asr.h" ]] || die "locked NeMo SDK is missing include/nemo_speech/asr.h"
    [[ -f "${nemo_stage}/lib/libnemo_speech_asr_c.so" ]] || die "locked NeMo SDK is missing libnemo_speech_asr_c.so"
    [[ -f "${nemo_stage}/share/licenses/nemo-speech/LICENSE" ]] || die "locked NeMo SDK is missing its license"

    mv -- "$nemo_stage" "$NEMO_ROOT"
    log "installed NeMo-Speech.cpp ${NEMO_VERSION}: $NEMO_ROOT"
else
    log "verified existing NeMo-Speech.cpp root: $NEMO_ROOT"
fi

if ! llama_root_is_valid; then
    llama_archive="$(download_locked "$LLAMA_FILENAME" "$LLAMA_URL" "$LLAMA_SIZE" "$LLAMA_SHA256")"
    llama_source_archive="$(download_locked "$LLAMA_SOURCE_FILENAME" "$LLAMA_SOURCE_URL" "$LLAMA_SOURCE_SIZE" "$LLAMA_SOURCE_SHA256")"
    validate_tar_paths "$llama_archive"
    validate_tar_paths "$llama_source_archive"

    llama_runtime_raw="${WORK_ROOT}/llama-runtime"
    llama_source_raw="${WORK_ROOT}/llama-source"
    llama_stage="${WORK_ROOT}/llama-stage"
    mkdir -- "$llama_runtime_raw" "$llama_source_raw" "$llama_stage"
    tar -xzf "$llama_archive" -C "$llama_runtime_raw" --strip-components=1
    tar -xzf "$llama_source_archive" -C "$llama_source_raw" --strip-components=1

    mkdir -p -- "${llama_stage}/include" "${llama_stage}/lib" "${llama_stage}/share/licenses/llama.cpp"
    cp -- "${llama_source_raw}/include/"*.h "${llama_stage}/include/"
    cp -- "${llama_source_raw}/ggml/include/"*.h "${llama_stage}/include/"

    LLAMA_COMMON_LIBS=(
        libllama.so libllama.so.0 libllama.so.0.3.0
        libggml.so libggml.so.0 libggml.so.0.22.0
        libggml-base.so libggml-base.so.0 libggml-base.so.0.22.0
    )
    for library in "${LLAMA_COMMON_LIBS[@]}" "${LLAMA_CPU_LIBS[@]}"; do
        [[ -e "${llama_runtime_raw}/${library}" || -L "${llama_runtime_raw}/${library}" ]] || die "locked llama runtime is missing ${library}"
        cp -a -- "${llama_runtime_raw}/${library}" "${llama_stage}/lib/"
    done

    [[ -f "${llama_source_raw}/LICENSE" ]] || die "locked llama source is missing LICENSE"
    [[ -f "${llama_runtime_raw}/LICENSE" ]] || die "locked llama runtime is missing LICENSE"
    cmp -s -- "${llama_source_raw}/LICENSE" "${llama_runtime_raw}/LICENSE" || die "llama source and runtime license files differ"
    cp -- "${llama_source_raw}/LICENSE" "${llama_stage}/share/licenses/llama.cpp/LICENSE"
    printf '%s\n' "$LLAMA_MARKER" >"${llama_stage}/.localflow-runtime-lock"

    mv -- "$llama_stage" "$LLAMA_ROOT"
    log "installed llama.cpp ${LLAMA_VERSION}: $LLAMA_ROOT"
else
    log "verified existing llama.cpp root: $LLAMA_ROOT"
fi

activation_temp="$(mktemp "${PLATFORM_ROOT}/.activate.sh.XXXXXX")"
{
    printf '# Generated by bootstrap-linux.sh from runtime-lock.json.\n'
    printf 'export NEMO_SPEECH_ROOT=%q\n' "$NEMO_ROOT"
    printf 'export LLAMA_ROOT=%q\n' "$LLAMA_ROOT"
} >"$activation_temp"
chmod 0644 -- "$activation_temp"
mv -f -- "$activation_temp" "${PLATFORM_ROOT}/activate.sh"

export NEMO_SPEECH_ROOT="$NEMO_ROOT"
export LLAMA_ROOT

printf 'NEMO_SPEECH_ROOT=%s\n' "$NEMO_SPEECH_ROOT"
printf 'LLAMA_ROOT=%s\n' "$LLAMA_ROOT"
printf 'For a later shell: source %q\n' "${PLATFORM_ROOT}/activate.sh"
