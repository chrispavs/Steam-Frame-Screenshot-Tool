#include "settings.hpp"

#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>

#include <linux/input-event-codes.h>
#include <sys/stat.h>

namespace {

struct Xkb {
    xkb_context *context = nullptr;
    xkb_keymap *keymap = nullptr;

    Xkb() {
        context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        if (context)
            keymap = xkb_keymap_new_from_names(context, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);
    }

    ~Xkb() {
        if (keymap)
            xkb_keymap_unref(keymap);
        if (context)
            xkb_context_unref(context);
    }
};

Xkb &xkb() {
    static Xkb keys;
    return keys;
}

xkb_keysym_t keysym_with(int evdev, bool shift, bool alt) {
    if (!xkb().keymap || evdev <= 0 || evdev > KEY_MAX)
        return XKB_KEY_NoSymbol;
    xkb_state *state = xkb_state_new(xkb().keymap);
    if (!state)
        return XKB_KEY_NoSymbol;
    if (shift)
        xkb_state_update_key(state, KEY_LEFTSHIFT + 8, XKB_KEY_DOWN);
    if (alt)
        xkb_state_update_key(state, KEY_LEFTALT + 8, XKB_KEY_DOWN);
    xkb_keysym_t sym = xkb_state_key_get_one_sym(state, static_cast<xkb_keycode_t>(evdev + 8));
    xkb_state_unref(state);
    return sym;
}

std::string pretty_keysym(xkb_keysym_t sym) {
    char raw[128] = {};
    if (xkb_keysym_get_name(sym, raw, sizeof raw) <= 0)
        return "Unknown";
    std::string name = raw;
    if (name == "Print")
        return "Print Screen";
    if (name == "Return" || name == "KP_Enter")
        return "Enter";
    if (name == "BackSpace")
        return "Backspace";
    if (name == "Prior" || name == "Page_Up")
        return "Page Up";
    if (name == "Next" || name == "Page_Down")
        return "Page Down";
    if (name == "Escape")
        return "Esc";
    if (name == "space")
        return "Space";
    if (name == "Tab" || name == "ISO_Left_Tab")
        return "Tab";
    if (name.size() == 1 && std::isalpha(static_cast<unsigned char>(name[0])))
        return std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(name[0]))));
    for (char &character : name)
        if (character == '_')
            character = ' ';
    return name;
}

bool parse_bool(const std::string &value) {
    return value == "1" || value == "true" || value == "yes" || value == "on";
}

std::string trim(const std::string &text) {
    size_t start = 0;
    while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start])))
        ++start;
    size_t end = text.size();
    while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1])))
        --end;
    return text.substr(start, end - start);
}

void add_sym(std::vector<xkb_keysym_t> &syms, xkb_keysym_t sym) {
    if (sym == XKB_KEY_NoSymbol)
        return;
    sym = xkb_keysym_to_upper(sym);
    if (std::find(syms.begin(), syms.end(), sym) == syms.end())
        syms.push_back(sym);
}

struct ModPair {
    bool enabled;
    uint32_t left;
    uint32_t right;
};

void emit_chords(std::vector<std::vector<uint32_t>> &chords, std::vector<uint32_t> &prefix,
                 const std::vector<ModPair> &mods, size_t index, uint32_t sym) {
    if (index == mods.size()) {
        std::vector<uint32_t> chord = prefix;
        chord.push_back(sym);
        chords.push_back(std::move(chord));
        return;
    }
    if (!mods[index].enabled) {
        emit_chords(chords, prefix, mods, index + 1, sym);
        return;
    }
    prefix.push_back(mods[index].left);
    emit_chords(chords, prefix, mods, index + 1, sym);
    prefix.back() = mods[index].right;
    emit_chords(chords, prefix, mods, index + 1, sym);
    prefix.pop_back();
}

}  // namespace

std::string settings_path() {
    const char *home = std::getenv("HOME");
    if (!home || !home[0])
        home = ".";
    const char *config = std::getenv("XDG_CONFIG_HOME");
    if (config && config[0])
        return std::string(config) + "/frameshot/settings";
    return std::string(home) + "/.config/frameshot/settings";
}

Settings load_settings() {
    Settings settings;
    std::ifstream in(settings_path());
    if (!in)
        return settings;
    bool saw_eye = false;
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#')
            continue;
        auto split = line.find('=');
        if (split == std::string::npos)
            continue;
        std::string key = trim(line.substr(0, split));
        std::string value = trim(line.substr(split + 1));
        if (key == "evdev") {
            int code = std::atoi(value.c_str());
            if (code > 0 && code <= KEY_MAX)
                settings.shortcut.evdev = code;
        } else if (key == "ctrl") {
            settings.shortcut.ctrl = parse_bool(value);
        } else if (key == "shift") {
            settings.shortcut.shift = parse_bool(value);
        } else if (key == "alt") {
            settings.shortcut.alt = parse_bool(value);
        } else if (key == "super") {
            settings.shortcut.super_key = parse_bool(value);
        } else if (key == "eye") {
            saw_eye = true;
            if (value == "right")
                settings.eye = EyeView::Right;
            else if (value == "both")
                settings.eye = EyeView::Both;
            else
                settings.eye = EyeView::Left;
        } else if (key == "both") {
            if (!saw_eye)
                settings.eye = parse_bool(value) ? EyeView::Both : EyeView::Left;
        } else if (key == "open_after") {
            settings.open_after = parse_bool(value);
        } else if (key == "sound") {
            settings.sound = parse_bool(value);
        } else if (key == "sound_file") {
            settings.sound_file = value;
        } else if (key == "sound_volume") {
            int volume = std::atoi(value.c_str());
            if (volume < 0)
                volume = 0;
            if (volume > 100)
                volume = 100;
            settings.sound_volume = volume;
        }
    }
    return settings;
}

std::string shortcut_label(const Shortcut &shortcut) {
    std::string label;
    if (shortcut.ctrl)
        label += "Ctrl+";
    if (shortcut.alt)
        label += "Alt+";
    if (shortcut.shift)
        label += "Shift+";
    if (shortcut.super_key)
        label += "Super+";
    xkb_keysym_t sym = keysym_with(shortcut.evdev, false, false);
    if (sym == XKB_KEY_NoSymbol)
        label += "Key " + std::to_string(shortcut.evdev);
    else
        label += pretty_keysym(sym);
    return label;
}

std::vector<std::vector<uint32_t>> gamescope_chords(const Shortcut &shortcut) {
    std::vector<xkb_keysym_t> syms;
    add_sym(syms, keysym_with(shortcut.evdev, false, false));
    if (shortcut.shift)
        add_sym(syms, keysym_with(shortcut.evdev, true, false));
    if (shortcut.alt)
        add_sym(syms, keysym_with(shortcut.evdev, false, true));
    if (shortcut.shift && shortcut.alt)
        add_sym(syms, keysym_with(shortcut.evdev, true, true));
    if (syms.empty())
        return {};

    std::vector<ModPair> mods = {
        {shortcut.ctrl, XKB_KEY_Control_L, XKB_KEY_Control_R},
        {shortcut.alt, XKB_KEY_Alt_L, XKB_KEY_Alt_R},
        {shortcut.shift, XKB_KEY_Shift_L, XKB_KEY_Shift_R},
        {shortcut.super_key, XKB_KEY_Super_L, XKB_KEY_Super_R},
    };
    std::vector<std::vector<uint32_t>> chords;
    std::vector<uint32_t> prefix;
    for (xkb_keysym_t sym : syms)
        emit_chords(chords, prefix, mods, 0, sym);
    return chords;
}
