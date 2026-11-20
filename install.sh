#!/bin/bash
# Install Frame Screenshot for the current user. SteamOS keeps the system
# drive read-only, so everything lands under the home directory.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
dest="${HOME}/.local/bin/frameshot"
settings_dest="${HOME}/.local/bin/frameshot-settings"
apps="${HOME}/.local/share/applications"
sound_dir="${HOME}/.local/share/frameshot"
unit_dir="${HOME}/.config/systemd/user"
desktop_name="frame-screenshot.desktop"
settings_desktop_name="frame-screenshot-settings.desktop"
unit_name="frameshot-keys.service"

if [[ -z "${XDG_RUNTIME_DIR:-}" && -d "/run/user/$(id -u)" ]]; then
    export XDG_RUNTIME_DIR="/run/user/$(id -u)"
fi

uninstall() {
    systemctl --user disable --now "$unit_name" >/dev/null 2>&1 || true
    rm -f "$dest" "$settings_dest" "$apps/$desktop_name" "$apps/$settings_desktop_name" "$unit_dir/$unit_name" "$sound_dir/shutter.wav"
    rmdir "$sound_dir" >/dev/null 2>&1 || true
    systemctl --user daemon-reload >/dev/null 2>&1 || true
    echo "Frame Screenshot has been removed."
}

if [[ "${1:-}" == "--uninstall" ]]; then
    uninstall
    exit 0
fi

if [[ "$(uname -m)" != "aarch64" ]]; then
    echo "This build is for the Steam Frame (aarch64)." >&2
    exit 1
fi

if [[ ! -x "$root/frameshot" || ! -x "$root/frameshot-settings" ]]; then
    echo "frameshot and frameshot-settings need to be next to install.sh. From a source checkout, run make first." >&2
    exit 1
fi

if [[ ! -f /opt/steamvr/bin/linuxarm64/libopenvr_api.so ]]; then
    echo "SteamVR was not found at /opt/steamvr. Frame Screenshot needs it to take the headset picture." >&2
    exit 1
fi

if [[ ! -f "$root/sounds/shutter.wav" ]]; then
    echo "The built-in shutter sound is missing." >&2
    exit 1
fi

install -d "${HOME}/.local/bin" "$apps" "$unit_dir" "$sound_dir"
install -m 755 "$root/frameshot" "$dest"
install -m 755 "$root/frameshot-settings" "$settings_dest"
install -m 644 "$root/sounds/shutter.wav" "$sound_dir/shutter.wav"
sed "s|@BIN@|${dest}|g" "$root/packaging/frame-screenshot.desktop.in" > "$apps/$desktop_name"
sed "s|@SETTINGS@|${settings_dest}|g" "$root/packaging/frame-screenshot-settings.desktop.in" > "$apps/$settings_desktop_name"
sed "s|@BIN@|${dest}|g" "$root/packaging/frameshot-keys.service.in" > "$unit_dir/$unit_name"

systemctl --user daemon-reload
systemctl --user enable "$unit_name"
systemctl --user restart "$unit_name"

echo "Installed."
echo "Press Ctrl+Shift+2, or open Frame Screenshot Settings to choose another shortcut."
echo "Pictures are saved to ~/Pictures."
