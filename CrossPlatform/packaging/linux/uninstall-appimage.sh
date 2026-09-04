#!/usr/bin/env bash
set -euo pipefail

data_root="${XDG_DATA_HOME:-${HOME}/.local/share}"
config_root="${XDG_CONFIG_HOME:-${HOME}/.config}"
binary_root="${data_root}/localflow"
application_root="${data_root}/applications"
icon_root="${data_root}/icons/hicolor/scalable/apps"
user_bin="${HOME}/.local/bin"
installed_image="${binary_root}/LocalFlow.AppImage"
launcher="${user_bin}/localflow"

# Do not remove a command that the user repointed after installation.
if [[ -L "$launcher" ]]; then
    launcher_target="$(realpath "$launcher" 2>/dev/null || true)"
    if [[ "$launcher_target" == "$installed_image" ]]; then
        rm -f -- "$launcher"
    fi
fi

# Remove only integration files owned by the per-user AppImage installer.
# Dictation history, settings, learned terminology, and downloaded models are
# intentionally preserved so reinstalling or upgrading cannot destroy data.
rm -f -- \
    "${config_root}/autostart/com.localflow.LocalFlow.desktop" \
    "${application_root}/com.localflow.LocalFlow.desktop" \
    "${icon_root}/com.localflow.LocalFlow.svg" \
    "${user_bin}/localflow-autostart" \
    "$installed_image" \
    "${binary_root}/uninstall.sh"
rmdir --ignore-fail-on-non-empty -- "$binary_root" 2>/dev/null || true

if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "$application_root" >/dev/null 2>&1 || true
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -f -t "${data_root}/icons/hicolor" >/dev/null 2>&1 || true
fi

echo "LocalFlow was uninstalled. Your settings, terminology, history, and models were kept."
