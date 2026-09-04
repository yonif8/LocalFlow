#!/usr/bin/env bash
set -euo pipefail

usage() {
    echo "Usage: $0 /path/to/LocalFlow-x86_64.AppImage [--autostart]" >&2
}

if [[ $# -lt 1 || $# -gt 2 ]]; then
    usage
    exit 2
fi

source_image="$(realpath "$1")"
enable_autostart=false
if [[ $# -eq 2 ]]; then
    [[ "$2" == "--autostart" ]] || { usage; exit 2; }
    enable_autostart=true
fi
if [[ ! -f "$source_image" ]]; then
    echo "AppImage not found: $source_image" >&2
    exit 1
fi

script_root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
data_root="${XDG_DATA_HOME:-${HOME}/.local/share}"
binary_root="${data_root}/localflow"
application_root="${data_root}/applications"
icon_root="${data_root}/icons/hicolor/scalable/apps"
icon_png_root="${data_root}/icons/hicolor/256x256/apps"
user_bin="${HOME}/.local/bin"
installed_image="${binary_root}/LocalFlow.AppImage"
desktop_entry="${application_root}/com.localflow.LocalFlow.desktop"
launcher="${user_bin}/localflow"

if [[ -e "$launcher" || -L "$launcher" ]]; then
    existing_target="$(realpath "$launcher" 2>/dev/null || true)"
    if [[ ! -L "$launcher" || "$existing_target" != "$installed_image" ]]; then
        echo "Refusing to replace an existing command not owned by LocalFlow: $launcher" >&2
        exit 1
    fi
fi

for required in \
    "${script_root}/assets/com.localflow.LocalFlow.svg" \
    "${script_root}/assets/com.localflow.LocalFlow.png" \
    "${script_root}/localflow-autostart" \
    "${script_root}/uninstall-appimage.sh"; do
    if [[ ! -f "$required" ]]; then
        echo "The integration bundle is incomplete: $required is missing." >&2
        exit 1
    fi
done

mkdir -p "$binary_root" "$application_root" "$icon_root" "$icon_png_root" "$user_bin"
install -m 0755 "$source_image" "$installed_image"
install -m 0644 "${script_root}/assets/com.localflow.LocalFlow.svg" \
    "${icon_root}/com.localflow.LocalFlow.svg"
install -m 0644 "${script_root}/assets/com.localflow.LocalFlow.png" \
    "${icon_png_root}/com.localflow.LocalFlow.png"
install -m 0755 "${script_root}/localflow-autostart" "${user_bin}/localflow-autostart"
install -m 0755 "${script_root}/uninstall-appimage.sh" "${binary_root}/uninstall.sh"
ln -sfn "$installed_image" "$launcher"

desktop_executable="${installed_image//\\/\\\\}"
desktop_executable="${desktop_executable//\"/\\\"}"
temporary="${desktop_entry}.tmp.$$"
trap 'rm -f -- "$temporary"' EXIT
{
    echo '[Desktop Entry]'
    echo 'Type=Application'
    echo 'Version=1.0'
    echo 'Name=LocalFlow'
    echo 'GenericName=Voice Dictation'
    echo 'Comment=Private, fully local push-to-talk dictation'
    printf 'Exec="%s"\n' "$desktop_executable"
    printf 'TryExec="%s"\n' "$desktop_executable"
    echo 'Icon=com.localflow.LocalFlow'
    echo 'Terminal=false'
    echo 'Categories=Utility;Accessibility;'
    echo 'StartupNotify=false'
} >"$temporary"
chmod 0644 "$temporary"
mv -f -- "$temporary" "$desktop_entry"
trap - EXIT

if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "$application_root" >/dev/null 2>&1 || true
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -f -t "${data_root}/icons/hicolor" >/dev/null 2>&1 || true
fi
if $enable_autostart; then
    "${user_bin}/localflow-autostart" enable "$installed_image"
fi

echo "LocalFlow installed for this user. Open it from the applications menu or run: localflow"
