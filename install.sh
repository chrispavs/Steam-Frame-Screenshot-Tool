#!/bin/bash
# Install Frame Screenshot for the current user. SteamOS keeps the system
# drive read-only, so everything lands under the home directory.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
dest="${HOME}/.local/bin/frameshot"
apps="${HOME}/.local/share/applications"
unit_dir="${HOME}/.config/systemd/user"
desktop_name="frame-screenshot.desktop"
unit_name="frameshot-keys.service"

if [[ -z "${XDG_RUNTIME_DIR:-}" && -d "/run/user/$(id -u)" ]]; then
    export XDG_RUNTIME_DIR="/run/user/$(id -u)"
fi

uninstall() {
    systemctl --user disable --now "$unit_name" >/dev/null 2>&1 || true
    rm -f "$dest" "$apps/$desktop_name" "$unit_dir/$unit_name"
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

if [[ ! -x "$root/frameshot" ]]; then
    echo "No frameshot binary next to install.sh. From a source checkout, run make first." >&2
    exit 1
fi

if [[ ! -f /opt/steamvr/bin/linuxarm64/libopenvr_api.so ]]; then
    echo "SteamVR was not found at /opt/steamvr. Frame Screenshot needs it to take the headset picture." >&2
    exit 1
fi

install -d "${HOME}/.local/bin" "$apps" "$unit_dir"
install -m 755 "$root/frameshot" "$dest"
sed "s|@BIN@|${dest}|g" "$root/packaging/frame-screenshot.desktop.in" > "$apps/$desktop_name"
sed "s|@BIN@|${dest}|g" "$root/packaging/frameshot-keys.service.in" > "$unit_dir/$unit_name"

systemctl --user daemon-reload
systemctl --user enable "$unit_name"
systemctl --user restart "$unit_name"

echo "Installed."
echo "Press Ctrl+Shift+2 to save a headset screenshot to ~/Pictures."
echo "Frame Screenshot is also in the application menu."
