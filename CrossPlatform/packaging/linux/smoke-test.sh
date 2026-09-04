#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "Usage: $0 dist-directory expected-version" >&2
    exit 2
fi

dist_dir="$(realpath "$1")"
version="$2"
script_root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
appimage="${dist_dir}/LocalFlow-x86_64.AppImage"
zsync="${appimage}.zsync"
deb="${dist_dir}/localflow_${version}_amd64.deb"
integration="${dist_dir}/LocalFlow-${version}-Linux-Integration.tar.gz"

for required in "$appimage" "$zsync" "$deb" "$integration" "${dist_dir}/SHA256SUMS"; do
    [[ -s "$required" ]] || { echo "Missing package artifact: $required" >&2; exit 1; }
done

if [[ -s "${dist_dir}/SHA256SUMS.asc" ]]; then
    [[ -s "${dist_dir}/LocalFlow-Linux-signing-key.asc" ]]
    signing_home="$(mktemp -d "${TMPDIR:-/tmp}/localflow-signature-smoke.XXXXXX")"
    chmod 0700 "$signing_home"
    GNUPGHOME="$signing_home" gpg --batch --import \
        "${dist_dir}/LocalFlow-Linux-signing-key.asc" >/dev/null 2>&1
    pinned_fingerprint="$(tr -d '[:space:]' \
        <"${script_root}/release-signing-key.fingerprint" | tr '[:lower:]' '[:upper:]')"
    exported_fingerprint="$(GNUPGHOME="$signing_home" gpg --batch --with-colons \
        --show-keys "${dist_dir}/LocalFlow-Linux-signing-key.asc" \
        | awk -F: '$1 == "fpr" { print toupper($10); exit }')"
    [[ "$exported_fingerprint" == "$pinned_fingerprint" ]] || {
        echo "The exported Linux release key does not match the checked-in identity." >&2
        exit 1
    }
    GNUPGHOME="$signing_home" gpg --batch --verify \
        "${dist_dir}/SHA256SUMS.asc" "${dist_dir}/SHA256SUMS"
    rm -rf -- "$signing_home"
fi

desktop-file-validate "${script_root}/assets/com.localflow.LocalFlow.desktop"
appstreamcli validate --no-net "${script_root}/assets/com.localflow.LocalFlow.metainfo.xml"
bash -n \
    "${script_root}/fetch-appimage-tools.sh" \
    "${script_root}/package.sh" \
    "${script_root}/localflow-autostart" \
    "${script_root}/install-appimage.sh" \
    "${script_root}/uninstall-appimage.sh"

file "$appimage" | grep -q 'x86-64'
grep -Fq 'Filename: LocalFlow-x86_64.AppImage' "$zsync"
expected_update_information='gh-releases-zsync|yonif8|LocalFlow|latest|LocalFlow-x86_64.AppImage.zsync'
actual_update_information="$("$appimage" --appimage-updateinformation)"
[[ "$actual_update_information" == "$expected_update_information" ]] || {
    echo "The AppImage contains unexpected update information." >&2
    exit 1
}

extract_root="$(mktemp -d "${TMPDIR:-/tmp}/localflow-appimage-smoke.XXXXXX")"
deb_root="$(mktemp -d "${TMPDIR:-/tmp}/localflow-deb-smoke.XXXXXX")"
integration_root="$(mktemp -d "${TMPDIR:-/tmp}/localflow-integration-smoke.XXXXXX")"
user_root="$(mktemp -d "${TMPDIR:-/tmp}/localflow-user-smoke.XXXXXX")"
trap 'rm -rf -- "$extract_root" "$deb_root" "$integration_root" "$user_root"' EXIT
(
    cd "$extract_root"
    "$appimage" --appimage-extract >/dev/null
)
[[ -x "${extract_root}/squashfs-root/AppRun" ]]
[[ -x "${extract_root}/squashfs-root/usr/bin/LocalFlow" ]]
[[ -x "${extract_root}/squashfs-root/usr/libexec/localflow/localflow-polish-worker" ]]
[[ -f "${extract_root}/squashfs-root/usr/lib/localflow/asr/libnemo_speech_asr_c.so" ]]
for forbidden in libatomic.so.1 libgcc_s.so.1 libgomp.so.1 libstdc++.so.6; do
    [[ ! -e "${extract_root}/squashfs-root/usr/lib/localflow/asr/${forbidden}" ]]
done
[[ -f "${extract_root}/squashfs-root/usr/libexec/localflow/libllama.so" ]]
[[ -s "${extract_root}/squashfs-root/usr/share/localflow/tessdata/eng.traineddata" ]]
[[ -x "${extract_root}/squashfs-root/usr/libexec/localflow/appimageupdatetool-x86_64.AppImage" ]]
for notice in \
    Qt/SOURCE.md Qt/MANIFEST.sha256 Qt/qtbase/LGPL-3.0-only.txt \
    Qt/qtdeclarative/LGPL-3.0-only.txt Qt/qtshadertools/LGPL-3.0-only.txt \
    Qt/qtwayland/LGPL-3.0-only.txt \
    nemo-speech/LICENSE nemo-speech/NOTICE nemo-speech/THIRD_PARTY_NOTICES.md \
    llama.cpp/LICENSE \
    tesseract/LICENSE leptonica/leptonica-license.txt \
    tessdata-fast/LICENSE AppImageUpdate/LICENSE.txt AppImageRuntime/LICENSE; do
    [[ -s "${extract_root}/squashfs-root/usr/share/licenses/${notice}" ]] || {
        echo "The AppImage is missing a redistribution notice: $notice" >&2
        exit 1
    }
done
updater_help="$(APPIMAGE_EXTRACT_AND_RUN=1 \
    "${extract_root}/squashfs-root/usr/libexec/localflow/appimageupdatetool-x86_64.AppImage" \
    --help 2>&1)"
grep -Fq -- '--check-for-update' <<<"$updater_help"
grep -Fq -- '--overwrite' <<<"$updater_help"
if find "${extract_root}/squashfs-root/usr/lib" -maxdepth 1 \
    \( -name 'libggml*.so*' -o -name 'libnemo_speech_asr*.so*' \) \
    -print -quit | grep -q .; then
    echo "The AppImage exposes an inference library in its shared ABI directory." >&2
    exit 1
fi
[[ -f "${extract_root}/squashfs-root/usr/share/applications/com.localflow.LocalFlow.desktop" ]]
cmp --silent "${script_root}/assets/com.localflow.LocalFlow.svg" \
    "${extract_root}/squashfs-root/usr/share/icons/hicolor/scalable/apps/com.localflow.LocalFlow.svg"
cmp --silent "${script_root}/../../app/assets/LocalFlow.png" \
    "${extract_root}/squashfs-root/usr/share/icons/hicolor/256x256/apps/com.localflow.LocalFlow.png"
[[ -f "${extract_root}/squashfs-root/usr/plugins/platforms/libqxcb.so" ]]
[[ -f "${extract_root}/squashfs-root/usr/plugins/platforms/libqwayland-egl.so" ]]
[[ -f "${extract_root}/squashfs-root/usr/qml/QtQuick/Controls/Basic/qmldir" ]]
desktop_root="${extract_root}/squashfs-root"
if LD_LIBRARY_PATH="${desktop_root}/usr/lib/localflow/asr:${desktop_root}/usr/lib" \
    ldd "${desktop_root}/usr/bin/LocalFlow" | grep -F 'not found'; then
    echo "The AppImage payload has unresolved shared libraries." >&2
    exit 1
fi
desktop_probe="$("${desktop_root}/AppRun" --probe-runtime)"
grep -Fq '"ready":true' <<<"$desktop_probe"
xvfb-run --auto-servernum env \
    "${desktop_root}/AppRun" --smoke-ui
if ldd "${extract_root}/squashfs-root/usr/libexec/localflow/localflow-polish-worker" | grep -F 'not found'; then
    echo "The AppImage polish worker has unresolved shared libraries." >&2
    exit 1
fi
worker_root="${extract_root}/squashfs-root/usr/libexec/localflow"
worker_qt_root="${extract_root}/squashfs-root/usr/lib"
probe_output="$(LD_LIBRARY_PATH="${worker_root}:${worker_qt_root}" \
    "${worker_root}/localflow-polish-worker" --probe-runtime)"
grep -Fq '"ready":true' <<<"$probe_output"

dpkg-deb --info "$deb" >/dev/null
dpkg-deb --extract "$deb" "$deb_root"
[[ -L "${deb_root}/usr/bin/localflow" ]]
[[ -x "${deb_root}/opt/localflow/AppRun" ]]
[[ -x "${deb_root}/opt/localflow/usr/libexec/localflow/localflow-polish-worker" ]]
[[ -f "${deb_root}/opt/localflow/usr/lib/localflow/asr/libnemo_speech_asr_c.so" ]]
for forbidden in libatomic.so.1 libgcc_s.so.1 libgomp.so.1 libstdc++.so.6; do
    [[ ! -e "${deb_root}/opt/localflow/usr/lib/localflow/asr/${forbidden}" ]]
done
[[ -f "${deb_root}/opt/localflow/usr/libexec/localflow/libllama.so" ]]
[[ -s "${deb_root}/opt/localflow/usr/share/localflow/tessdata/eng.traineddata" ]]
[[ ! -e "${deb_root}/opt/localflow/usr/libexec/localflow/appimageupdatetool-x86_64.AppImage" ]]
for notice in \
    Qt/SOURCE.md Qt/MANIFEST.sha256 Qt/qtbase/LGPL-3.0-only.txt \
    Qt/qtdeclarative/LGPL-3.0-only.txt Qt/qtshadertools/LGPL-3.0-only.txt \
    Qt/qtwayland/LGPL-3.0-only.txt \
    nemo-speech/LICENSE nemo-speech/NOTICE nemo-speech/THIRD_PARTY_NOTICES.md \
    llama.cpp/LICENSE \
    tesseract/LICENSE leptonica/leptonica-license.txt \
    tessdata-fast/LICENSE AppImageUpdate/LICENSE.txt AppImageRuntime/LICENSE; do
    [[ -s "${deb_root}/opt/localflow/usr/share/licenses/${notice}" ]] || {
        echo "The Debian package is missing a redistribution notice: $notice" >&2
        exit 1
    }
done
[[ -f "${deb_root}/usr/share/applications/com.localflow.LocalFlow.desktop" ]]
cmp --silent "${script_root}/assets/com.localflow.LocalFlow.svg" \
    "${deb_root}/usr/share/icons/hicolor/scalable/apps/com.localflow.LocalFlow.svg"
cmp --silent "${script_root}/../../app/assets/LocalFlow.png" \
    "${deb_root}/usr/share/icons/hicolor/256x256/apps/com.localflow.LocalFlow.png"
deb_probe="$("${deb_root}/opt/localflow/AppRun" --probe-runtime)"
grep -Fq '"ready":true' <<<"$deb_probe"
xvfb-run --auto-servernum env \
    "${deb_root}/opt/localflow/AppRun" --smoke-ui
[[ "$(dpkg-deb --field "$deb" Package)" == "localflow" ]]
[[ "$(dpkg-deb --field "$deb" Version)" == "$version" ]]
[[ "$(dpkg-deb --field "$deb" Architecture)" == "amd64" ]]
cmp --silent "${desktop_root}/usr/bin/LocalFlow" \
    "${deb_root}/opt/localflow/usr/bin/LocalFlow"
cmp --silent "${desktop_root}/usr/libexec/localflow/localflow-polish-worker" \
    "${deb_root}/opt/localflow/usr/libexec/localflow/localflow-polish-worker"

# Exercise the actual per-user integration scripts with XDG paths containing
# spaces and Exec-reserved characters. The installer must copy exact bytes,
# create a valid startup entry, and remove only LocalFlow-owned integration
# while preserving app data.
tar -xzf "$integration" -C "$integration_root"
bundle="${integration_root}/LocalFlow-Linux-Integration"
cmp --silent "${script_root}/assets/com.localflow.LocalFlow.svg" \
    "${bundle}/assets/com.localflow.LocalFlow.svg"
cmp --silent "${script_root}/../../app/assets/LocalFlow.png" \
    "${bundle}/assets/com.localflow.LocalFlow.png"
home_dir="${user_root}/Home With Spaces"
data_dir="${user_root}/Data With Spaces \$ cash % percent \` tick"
config_dir="${user_root}/Config With Spaces"
cache_dir="${user_root}/Cache With Spaces"
mkdir -p "$home_dir" "$data_dir" "$config_dir/LocalFlow" "$cache_dir"
preserved_settings="${config_dir}/LocalFlow/LocalFlow.conf"
printf '%s\n' 'release-smoke-userdata=keep' >"$preserved_settings"
env HOME="$home_dir" XDG_DATA_HOME="$data_dir" \
    XDG_CONFIG_HOME="$config_dir" XDG_CACHE_HOME="$cache_dir" \
    "${bundle}/install-appimage.sh" "$appimage" --autostart
installed_image="${data_dir}/localflow/LocalFlow.AppImage"
launcher="${home_dir}/.local/bin/localflow"
installed_desktop="${data_dir}/applications/com.localflow.LocalFlow.desktop"
autostart_entry="${config_dir}/autostart/com.localflow.LocalFlow.desktop"
[[ -x "$installed_image" && -L "$launcher" && -f "$installed_desktop" ]]
[[ "$(realpath "$launcher")" == "$installed_image" ]]
cmp --silent "$appimage" "$installed_image"
cmp --silent "${script_root}/../../app/assets/LocalFlow.png" \
    "${data_dir}/icons/hicolor/256x256/apps/com.localflow.LocalFlow.png"
desktop-file-validate "$installed_desktop"
desktop-file-validate "$autostart_entry"
for entry in "$installed_desktop" "$autostart_entry"; do
    ! grep -Fq 'TryExec=' "$entry"
    grep -Fq 'Exec="' "$entry"
    grep -Fq 'LocalFlow.AppImage' "$entry"
    grep -Fq '\$' "$entry"
    grep -Fq '\`' "$entry"
    grep -Fq '%%' "$entry"
done
installed_probe="$(env HOME="$home_dir" XDG_DATA_HOME="$data_dir" \
    XDG_CONFIG_HOME="$config_dir" XDG_CACHE_HOME="$cache_dir" \
    APPIMAGE_EXTRACT_AND_RUN=1 "$installed_image" --probe-runtime)"
grep -Fq '"ready":true' <<<"$installed_probe"
xvfb-run --auto-servernum env HOME="$home_dir" XDG_DATA_HOME="$data_dir" \
    XDG_CONFIG_HOME="$config_dir" XDG_CACHE_HOME="$cache_dir" \
    APPIMAGE_EXTRACT_AND_RUN=1 \
    "$installed_image" --smoke-ui
env HOME="$home_dir" XDG_DATA_HOME="$data_dir" \
    XDG_CONFIG_HOME="$config_dir" XDG_CACHE_HOME="$cache_dir" \
    "${data_dir}/localflow/uninstall.sh"
for removed in \
    "$installed_image" "$launcher" "$installed_desktop" "$autostart_entry" \
    "${data_dir}/icons/hicolor/scalable/apps/com.localflow.LocalFlow.svg" \
    "${data_dir}/icons/hicolor/256x256/apps/com.localflow.LocalFlow.png" \
    "${home_dir}/.local/bin/localflow-autostart"; do
    [[ ! -e "$removed" && ! -L "$removed" ]] || {
        echo "AppImage uninstall left an owned integration file: $removed" >&2
        exit 1
    }
done
grep -Fxq 'release-smoke-userdata=keep' "$preserved_settings"

(
    cd "$dist_dir"
    sha256sum --check SHA256SUMS
)

echo "Linux package smoke tests passed."
