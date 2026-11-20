# Frame Screenshot

Frame Screenshot saves a picture of what you are seeing in a Steam Frame headset.

The shot is the compositor's view of one eye: the environment, the panels, and the Steam UI together.

Pictures are saved in `~/Pictures`.

## Install

On the Frame, open Desktop Mode and download `frameshot-linux-arm64.tar.gz` from the [latest release](https://github.com/chrispavs/Steam-Frame-Screenshot-Tool/releases). Then:

```bash
tar -xzf frameshot-linux-arm64.tar.gz
cd frameshot-linux-arm64
./install.sh
```

The installer copies the program to `~/.local/bin`, adds **Frame Screenshot** to the application menu, and starts the keyboard shortcut with the headset session.

To remove it, run `./install.sh --uninstall` from that same directory.

## Use

Take a screenshot with **Ctrl+Shift+2** on a keyboard connected to the Frame!

A sound will play when a screenshot is taken.

This shortcut works anywhere, including when a game is running. The picture is written to `~/Pictures/Frame-YYYYMMDD-HHMMSS.png`.

### Settings

**Frame Screenshot Settings** is a settings menu where you can change things like the shortcut, which eye is used for the screenshot (left, right, or both eyes), or the screenshot sound.

### CLI

You can also take a screenshot from a terminal:

```bash
frameshot                      # screenshot one eye
frameshot --view               # screenshot one eye and open it
frameshot --both               # screenshot both eyes, side by side
frameshot ~/Pictures/shot.png  # take a screenshot and save it as shot.png
```

## How to build

The release already contains a build for the Frame. Use this only if you want to compile it yourself.

The Frame needs `g++`, `make`, and `wayland-scanner`. SteamVR is already installed and supplies the OpenVR headers and library. If `make` cannot find a compiler or `wayland-scanner`, install them with:

```bash
sudo steamos-readonly disable
sudo pacman -S --needed base-devel wayland
```

Then:

```bash
git clone https://github.com/chrispavs/Steam-Frame-Screenshot-Tool.git
cd Steam-Frame-Screenshot-Tool
make
./install.sh
```

`make` builds `frameshot` in this directory. `./install.sh` installs that build the same way as the release package. After later changes, run `make` and `./install.sh` again.
