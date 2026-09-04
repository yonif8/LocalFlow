#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat >&2 <<'EOF'
Usage: package.sh --build-dir DIR --dist-dir DIR --version VERSION [--release]

--release requires an imported secret key in GNUPGHOME plus:
  LOCALFLOW_GPG_FINGERPRINT
  LOCALFLOW_GPG_PASSPHRASE
EOF
}

build_dir=""
dist_dir=""
version=""
release=false
while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir) build_dir="${2:-}"; shift 2 ;;
        --dist-dir) dist_dir="${2:-}"; shift 2 ;;
        --version) version="${2:-}"; shift 2 ;;
        --release) release=true; shift ;;
        *) usage; exit 2 ;;
    esac
done

if [[ -z "$build_dir" || -z "$dist_dir" || -z "$version" ]]; then
    usage
    exit 2
fi
if [[ ! "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+([+~.-][0-9A-Za-z.-]+)?$ ]]; then
    echo "Invalid Linux package version: $version" >&2
    exit 2
fi
if $release && [[ ! "$version" =~ ^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$ ]]; then
    echo "Production Linux packages require an exact stable X.Y.Z version." >&2
    exit 2
fi
if [[ ! -d "$build_dir" || ! -f "$build_dir/CMakeCache.txt" ]]; then
    echo "CMake build directory not found: $build_dir" >&2
    exit 1
fi

script_root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repository_root="$(cd "${script_root}/../../.." && pwd)"
build_dir="$(realpath "$build_dir")"
mkdir -p "$dist_dir"
dist_dir="$(realpath "$dist_dir")"
if [[ "$dist_dir" == "/" || "$dist_dir" == "$repository_root" ]]; then
    echo "Refusing unsafe distribution directory: $dist_dir" >&2
    exit 1
fi

work_root="$(mktemp -d "${TMPDIR:-/tmp}/localflow-linux-package.XXXXXX")"
trap 'rm -rf -- "$work_root"' EXIT
app_dir="${work_root}/LocalFlow.AppDir"
tools_dir="${work_root}/tools"
mkdir -p "$app_dir" "$tools_dir"

"${script_root}/fetch-appimage-tools.sh" "$tools_dir"
export PATH="${tools_dir}:${PATH}"
export APPIMAGE_EXTRACT_AND_RUN=1

# CMake owns the application payload; packaging owns Linux integration files.
DESTDIR="$app_dir" cmake --install "$build_dir" --prefix /usr --strip
if [[ ! -x "${app_dir}/usr/bin/LocalFlow" ]]; then
    echo "The localflow_desktop install did not produce usr/bin/LocalFlow." >&2
    exit 1
fi
if [[ ! -x "${app_dir}/usr/libexec/localflow/localflow-polish-worker" ]]; then
    echo "The install did not produce usr/libexec/localflow/localflow-polish-worker." >&2
    exit 1
fi
: "${NEMO_SPEECH_ROOT:?NEMO_SPEECH_ROOT must point at the locked bootstrap root}"
: "${LLAMA_ROOT:?LLAMA_ROOT must point at the locked bootstrap root}"
python3 "${repository_root}/CrossPlatform/dependencies/stage-runtime.py" \
    --platform linux \
    --arch x86_64 \
    --stage "$app_dir" \
    --nemo-root "$NEMO_SPEECH_ROOT" \
    --llama-root "$LLAMA_ROOT"

# The OCR library does not carry its recognition data. Bundle the exact
# English file privately so AppImage and Debian installs behave identically.
tessdata_source="${LOCALFLOW_TESSDATA_DIR:-$tools_dir}"
if [[ -z "$tessdata_source" || ! -s "${tessdata_source}/eng.traineddata" ]]; then
    echo "English Tesseract data was not found; refusing to package OCR as available." >&2
    exit 1
fi
install -Dm0644 "${tessdata_source}/eng.traineddata" \
    "${app_dir}/usr/share/localflow/tessdata/eng.traineddata"
install -Dm0644 "${tools_dir}/tessdata-LICENSE" \
    "${app_dir}/usr/share/licenses/tessdata-fast/LICENSE"
install -Dm0644 "${tools_dir}/tessdata-LICENSE" \
    "${app_dir}/usr/share/licenses/tesseract/LICENSE"
install -Dm0644 "${tools_dir}/Leptonica-LICENSE" \
    "${app_dir}/usr/share/licenses/leptonica/leptonica-license.txt"
install -Dm0755 "${tools_dir}/appimageupdatetool-x86_64.AppImage" \
    "${app_dir}/usr/libexec/localflow/appimageupdatetool-x86_64.AppImage"
install -Dm0644 "${tools_dir}/AppImageUpdate-LICENSE.txt" \
    "${app_dir}/usr/share/licenses/AppImageUpdate/LICENSE.txt"
install -Dm0644 "${tools_dir}/AppImage-runtime-LICENSE" \
    "${app_dir}/usr/share/licenses/AppImageRuntime/LICENSE"
ln -sfn LocalFlow "${app_dir}/usr/bin/localflow"
install -Dm0644 "${script_root}/assets/com.localflow.LocalFlow.desktop" \
    "${app_dir}/usr/share/applications/com.localflow.LocalFlow.desktop"
install -Dm0644 "${script_root}/assets/com.localflow.LocalFlow.svg" \
    "${app_dir}/usr/share/icons/hicolor/scalable/apps/com.localflow.LocalFlow.svg"
brand_png="${repository_root}/CrossPlatform/app/assets/LocalFlow.png"
if [[ ! -s "$brand_png" ]]; then
    echo "The canonical LocalFlow PNG icon is missing: $brand_png" >&2
    exit 1
fi
install -Dm0644 "$brand_png" \
    "${app_dir}/usr/share/icons/hicolor/256x256/apps/com.localflow.LocalFlow.png"
install -Dm0644 "${script_root}/assets/com.localflow.LocalFlow.metainfo.xml" \
    "${app_dir}/usr/share/metainfo/com.localflow.LocalFlow.metainfo.xml"
# appimagetool 1.9.1 still looks for the legacy .appdata.xml basename.
# Keep the modern metainfo name too; both point at the same validated content.
install -Dm0644 "${script_root}/assets/com.localflow.LocalFlow.metainfo.xml" \
    "${app_dir}/usr/share/metainfo/com.localflow.LocalFlow.appdata.xml"
install -Dm0755 "${script_root}/localflow-autostart" \
    "${app_dir}/usr/bin/localflow-autostart"

export QML_SOURCES_PATHS="${repository_root}/CrossPlatform/app/qml"
export EXTRA_PLATFORM_PLUGINS="libqwayland-egl.so;libqwayland-generic.so"
if [[ -z "${QMAKE:-}" ]] && command -v qmake6 >/dev/null 2>&1; then
    QMAKE="$(command -v qmake6)"
    export QMAKE
fi
if [[ -z "${QMAKE:-}" || ! -x "$QMAKE" ]]; then
    echo "QMAKE must name the exact Qt deployment used for packaging." >&2
    exit 1
fi
qt_license_source="${repository_root}/CrossPlatform/packaging/licenses/Qt"
for required_license in \
    SOURCE.md MANIFEST.sha256 \
    qtbase/LGPL-3.0-only.txt \
    qtbase/GPL-3.0-only.txt \
    qtdeclarative/LGPL-3.0-only.txt \
    qtshadertools/LGPL-3.0-only.txt \
    qtwayland/LGPL-3.0-only.txt; do
    if [[ ! -s "${qt_license_source}/${required_license}" ]]; then
        echo "The reviewed Qt notice bundle is missing ${required_license}." >&2
        exit 1
    fi
done
(
    cd "$qt_license_source"
    [[ "$(wc -l <MANIFEST.sha256)" -eq 74 ]]
    sha256sum --check --strict MANIFEST.sha256
    find qtbase qtdeclarative qtshadertools qtwayland -type f -printf '%p\n' \
        | sort >"${work_root}/qt-license-actual"
    sed -E 's/^[0-9a-f]{64}  //' MANIFEST.sha256 \
        | sort >"${work_root}/qt-license-expected"
    cmp --silent "${work_root}/qt-license-expected" "${work_root}/qt-license-actual" || {
        echo "The reviewed Qt license inventory is incomplete or contains an unreviewed file." >&2
        exit 1
    }
)
mkdir -p "${app_dir}/usr/share/licenses/Qt"
cp -a "${qt_license_source}/." "${app_dir}/usr/share/licenses/Qt/"

for required_notice in \
    "${app_dir}/usr/share/licenses/nemo-speech/LICENSE" \
    "${app_dir}/usr/share/licenses/nemo-speech/NOTICE" \
    "${app_dir}/usr/share/licenses/nemo-speech/THIRD_PARTY_NOTICES.md" \
    "${app_dir}/usr/share/licenses/llama.cpp/LICENSE" \
    "${app_dir}/usr/share/licenses/tesseract/LICENSE" \
    "${app_dir}/usr/share/licenses/leptonica/leptonica-license.txt" \
    "${app_dir}/usr/share/licenses/AppImageUpdate/LICENSE.txt" \
    "${app_dir}/usr/share/licenses/AppImageRuntime/LICENSE"; do
    if [[ ! -s "$required_notice" ]]; then
        echo "A required bundled-runtime notice is missing: $required_notice" >&2
        exit 1
    fi
done

"${tools_dir}/linuxdeploy-x86_64.AppImage" \
    --appdir "$app_dir" \
    --executable "${app_dir}/usr/bin/LocalFlow" \
    --executable "${app_dir}/usr/libexec/localflow/localflow-polish-worker" \
    --desktop-file "${script_root}/assets/com.localflow.LocalFlow.desktop" \
    --icon-file "${script_root}/assets/com.localflow.LocalFlow.svg" \
    --plugin qt

# linuxdeploy follows the desktop executable's NeMo dependencies and otherwise
# copies one ggml ABI into the common directory. That can collide with the
# worker's different llama ggml ABI. AppRun selects the private ASR directory.
rm -f -- \
    "${app_dir}"/usr/lib/libnemo_speech_asr.so* \
    "${app_dir}"/usr/lib/libnemo_speech_asr_c.so* \
    "${app_dir}"/usr/lib/libggml.so* \
    "${app_dir}"/usr/lib/libggml-base.so* \
    "${app_dir}"/usr/lib/libggml-cpu.so*
install -Dm0755 "${script_root}/AppRun" "${app_dir}/AppRun"

appimage_output="${dist_dir}/LocalFlow-x86_64.AppImage"
update_information="${LOCALFLOW_APPIMAGE_UPDATE_INFORMATION:-gh-releases-zsync|yonif8|LocalFlow|latest|LocalFlow-x86_64.AppImage.zsync}"
appimage_arguments=(
    --updateinformation "$update_information"
    --runtime-file "${tools_dir}/runtime-x86_64"
)

if $release; then
    : "${GNUPGHOME:?GNUPGHOME is required for a signed release}"
    : "${LOCALFLOW_GPG_PASSPHRASE:?LOCALFLOW_GPG_PASSPHRASE is required for a signed release}"
    fingerprint_file="${script_root}/release-signing-key.fingerprint"
    expected_fingerprint="$(tr -d '[:space:]' <"$fingerprint_file" | tr '[:lower:]' '[:upper:]')"
    if [[ ! "$expected_fingerprint" =~ ^[0-9A-F]{40}$ ]]; then
        echo "The checked-in Linux release fingerprint is invalid." >&2
        exit 1
    fi
    configured_fingerprint="$(tr -d '[:space:]' <<<"${LOCALFLOW_GPG_FINGERPRINT:-}" | tr '[:lower:]' '[:upper:]')"
    if [[ -n "$configured_fingerprint" && "$configured_fingerprint" != "$expected_fingerprint" ]]; then
        echo "LOCALFLOW_GPG_FINGERPRINT does not match the checked-in release identity." >&2
        exit 1
    fi
    actual_fingerprint="$(gpg --batch --with-colons --list-secret-keys "$expected_fingerprint" | awk -F: '$1 == "fpr" { print toupper($10); exit }')"
    if [[ -z "$actual_fingerprint" || "$actual_fingerprint" != "$expected_fingerprint" ]]; then
        echo "The imported release key does not match LOCALFLOW_GPG_FINGERPRINT." >&2
        exit 1
    fi
    export APPIMAGETOOL_SIGN_PASSPHRASE="$LOCALFLOW_GPG_PASSPHRASE"
    appimage_arguments+=(--sign --sign-key "$expected_fingerprint")
fi

(
    cd "$dist_dir"
    ARCH=x86_64 VERSION="$version" APPIMAGETOOL_APP_NAME=LocalFlow \
        "${tools_dir}/appimagetool-x86_64.AppImage" \
        "${appimage_arguments[@]}" "$app_dir" "$(basename "$appimage_output")"
)
chmod 0755 "$appimage_output"

zsync_output="${appimage_output}.zsync"
if [[ ! -s "$zsync_output" ]]; then
    echo "appimagetool did not create the required zsync metadata." >&2
    exit 1
fi

if $release; then
    signature_section="${work_root}/appimage-signature"
    public_key_section="${work_root}/appimage-public-key"
    # objcopy rewrites an input in place when no output path is given, which
    # would discard the SquashFS appended to an AppImage. Probe a disposable
    # copy so signature validation can never corrupt the release artifact.
    signature_probe="${work_root}/signature-probe.AppImage"
    cp "$appimage_output" "$signature_probe"
    objcopy --dump-section .sha256_sig="$signature_section" "$signature_probe"
    objcopy --dump-section .sig_key="$public_key_section" "$signature_probe"
    if [[ ! -s "$signature_section" || ! -s "$public_key_section" ]]; then
        echo "The release AppImage does not contain its expected embedded signature." >&2
        exit 1
    fi
    validator_output="$(APPIMAGE_EXTRACT_AND_RUN=1 \
        "${tools_dir}/validate-x86_64.AppImage" "$appimage_output" 2>&1)" || {
        printf '%s\n' "$validator_output" >&2
        echo "Cryptographic validation of the embedded AppImage signature failed." >&2
        exit 1
    }
    printf '%s\n' "$validator_output"
    validation_fingerprints="$(
        grep -oE '[0-9A-Fa-f]{40}' <<<"$validator_output" \
            | tr '[:lower:]' '[:upper:]' | sort -u || true
    )"
    if [[ "$validation_fingerprints" != "$expected_fingerprint" ]]; then
        echo "The AppImage signature is not anchored to the checked-in Linux release key." >&2
        exit 1
    fi
fi

# Reuse the exact deployed AppDir for the Debian package. This intentionally
# carries the same Qt runtime and avoids a second, drifting dependency recipe.
deb_root="${work_root}/deb-root"
mkdir -p "${deb_root}/DEBIAN" "${deb_root}/opt/localflow" \
    "${deb_root}/usr/bin" "${deb_root}/usr/share/applications" \
    "${deb_root}/usr/share/icons/hicolor/scalable/apps" \
    "${deb_root}/usr/share/metainfo"
cp -a "${app_dir}/." "${deb_root}/opt/localflow/"
# Debian installations update through the package/release handoff, so do not
# carry the AppImage-only updater inside the .deb.
rm -f -- "${deb_root}/opt/localflow/usr/libexec/localflow/appimageupdatetool-x86_64.AppImage"
ln -s /opt/localflow/AppRun "${deb_root}/usr/bin/localflow"
install -m0755 "${script_root}/localflow-autostart" \
    "${deb_root}/usr/bin/localflow-autostart"
install -m0644 "${script_root}/assets/com.localflow.LocalFlow.desktop" \
    "${deb_root}/usr/share/applications/com.localflow.LocalFlow.desktop"
install -m0644 "${script_root}/assets/com.localflow.LocalFlow.svg" \
    "${deb_root}/usr/share/icons/hicolor/scalable/apps/com.localflow.LocalFlow.svg"
install -Dm0644 "$brand_png" \
    "${deb_root}/usr/share/icons/hicolor/256x256/apps/com.localflow.LocalFlow.png"
install -m0644 "${script_root}/assets/com.localflow.LocalFlow.metainfo.xml" \
    "${deb_root}/usr/share/metainfo/com.localflow.LocalFlow.metainfo.xml"
sed "s/@VERSION@/${version}/g" "${script_root}/debian/control.in" >"${deb_root}/DEBIAN/control"
install -m0755 "${script_root}/debian/postinst" "${deb_root}/DEBIAN/postinst"
install -m0755 "${script_root}/debian/postrm" "${deb_root}/DEBIAN/postrm"
installed_kib="$(du -sk "$deb_root" | awk '{print $1}')"
printf 'Installed-Size: %s\n' "$installed_kib" >>"${deb_root}/DEBIAN/control"

deb_output="${dist_dir}/localflow_${version}_amd64.deb"
fakeroot dpkg-deb --root-owner-group --build "$deb_root" "$deb_output"

# AppImage users get reversible, per-user desktop integration. It is separate
# from the image so an updater can replace only LocalFlow.AppImage.
integration_root="${work_root}/LocalFlow-Linux-Integration"
mkdir -p "${integration_root}/assets"
install -m0755 "${script_root}/install-appimage.sh" "${integration_root}/install-appimage.sh"
install -m0755 "${script_root}/uninstall-appimage.sh" "${integration_root}/uninstall-appimage.sh"
install -m0755 "${script_root}/localflow-autostart" "${integration_root}/localflow-autostart"
install -m0644 "${script_root}/assets/com.localflow.LocalFlow.svg" \
    "${integration_root}/assets/com.localflow.LocalFlow.svg"
install -m0644 "$brand_png" \
    "${integration_root}/assets/com.localflow.LocalFlow.png"
install -m0644 "${script_root}/README.md" "${integration_root}/README.md"
tar -C "$work_root" -czf \
    "${dist_dir}/LocalFlow-${version}-Linux-Integration.tar.gz" \
    LocalFlow-Linux-Integration

public_key_output="${dist_dir}/LocalFlow-Linux-signing-key.asc"
rm -f -- "${dist_dir}/SHA256SUMS" "${dist_dir}/SHA256SUMS.asc" \
    "${dist_dir}/UNSIGNED-NOT-FOR-DISTRIBUTION.txt" "$public_key_output"
checksum_files=(
    "$(basename "$appimage_output")"
    "$(basename "$zsync_output")"
    "$(basename "$deb_output")"
    "LocalFlow-${version}-Linux-Integration.tar.gz"
)
if $release; then
    gpg --batch --armor --export "$expected_fingerprint" >"$public_key_output"
    [[ -s "$public_key_output" ]] || {
        echo "Could not export the Linux release verification key." >&2
        exit 1
    }
    checksum_files+=("$(basename "$public_key_output")")
fi
(
    cd "$dist_dir"
    sha256sum "${checksum_files[@]}" >SHA256SUMS
)

if $release; then
    printf '%s' "$LOCALFLOW_GPG_PASSPHRASE" | gpg \
        --batch --yes --pinentry-mode loopback --passphrase-fd 0 \
        --local-user "$expected_fingerprint" --armor --detach-sign \
        --output "${dist_dir}/SHA256SUMS.asc" "${dist_dir}/SHA256SUMS"
    gpg --batch --verify "${dist_dir}/SHA256SUMS.asc" "${dist_dir}/SHA256SUMS"
else
    cat >"${dist_dir}/UNSIGNED-NOT-FOR-DISTRIBUTION.txt" <<'EOF'
This is an unsigned CI smoke artifact. It is for automated validation only.
Do not publish it or ask end users to install it.
EOF
fi

echo "Linux packages written to: $dist_dir"
