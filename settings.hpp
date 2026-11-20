#pragma once

#include <cstdint>
#include <string>
#include <vector>

// ~/.config/frameshot/settings
//   evdev=<linux key code>
//   ctrl=0|1
//   shift=0|1
//   alt=0|1
//   super=0|1
//   eye=left|right|both
//   open_after=0|1
//   sound=0|1
//   sound_file=<path>   empty uses the built-in shutter
//   sound_volume=0..100
// Older files use both=0|1. both=1 means both eyes, and both=0 means the left eye.

struct Shortcut {
    int evdev = 3;
    bool ctrl = true;
    bool shift = true;
    bool alt = false;
    bool super_key = false;

    bool operator==(const Shortcut &other) const {
        return evdev == other.evdev && ctrl == other.ctrl && shift == other.shift && alt == other.alt &&
               super_key == other.super_key;
    }
};

enum class EyeView { Left, Right, Both };

struct Settings {
    Shortcut shortcut;
    EyeView eye = EyeView::Left;
    bool open_after = false;
    bool sound = false;
    std::string sound_file;
    int sound_volume = 75;
};

std::string settings_path();
Settings load_settings();
std::string shortcut_label(const Shortcut &shortcut);
std::vector<std::vector<uint32_t>> gamescope_chords(const Shortcut &shortcut);
