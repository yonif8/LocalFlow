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

for required in "$appimage" "$zsync" "$deb" "${dist_dir}/SHA256SUMS"; do
    [[ -s "$required" ]] || { echo "Missing package artifact: $required" >&2; exit 1; }
done

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
grep -Fq 'gh-releases-zsync|yonif8|LocalFlow|latest|' <(
    "$appimage" --appimage-updateinformation
)

extract_root="$(mktemp -d "${TMPDIR:-/tmp}/localflow-appimage-smoke.XXXXXX")"
deb_root="$(mktemp -d "${TMPDIR:-/tmp}/localflow-deb-smoke.XXXXXX")"
trap 'rm -rf -- "$extract_root" "$deb_root"' EXIT
(
    cd "$extract_root"
    "$appimage" --appimage-extract >/dev/null
)
[[ -x "${extract_root}/squashfs-root/AppRun" ]]
[[ -x "${extract_root}/squashfs-root/usr/bin/LocalFlow" ]]
[[ -x "${extract_root}/squashfs-root/usr/libexec/localflow/localflow-polish-worker" ]]
[[ -f "${extract_root}/squashfs-root/usr/share/applications/com.localflow.LocalFlow.desktop" ]]
[[ -f "${extract_root}/squashfs-root/usr/plugins/platforms/libqxcb.so" ]]
[[ -f "${extract_root}/squashfs-root/usr/plugins/platforms/libqwayland-egl.so" ]]
[[ -f "${extract_root}/squashfs-root/usr/qml/QtQuick/Controls/Basic/qmldir" ]]
if ldd "${extract_root}/squashfs-root/usr/bin/LocalFlow" | grep -F 'not found'; then
    echo "The AppImage payload has unresolved shared libraries." >&2
    exit 1
fi
if ldd "${extract_root}/squashfs-root/usr/libexec/localflow/localflow-polish-worker" | grep -F 'not found'; then
    echo "The AppImage polish worker has unresolved shared libraries." >&2
    exit 1
fi

dpkg-deb --info "$deb" >/dev/null
dpkg-deb --extract "$deb" "$deb_root"
[[ -L "${deb_root}/usr/bin/localflow" ]]
[[ -x "${deb_root}/opt/localflow/AppRun" ]]
[[ -x "${deb_root}/opt/localflow/usr/libexec/localflow/localflow-polish-worker" ]]
[[ -f "${deb_root}/usr/share/applications/com.localflow.LocalFlow.desktop" ]]

(
    cd "$dist_dir"
    sha256sum --check SHA256SUMS
)

echo "Linux package smoke tests passed."
