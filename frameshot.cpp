// Save what the Steam Frame compositor is showing in the headset.
//
// gamescopectl screenshot writes the flat desktop buffer (1280x720 on this
// session). The image in the headset is the VR compositor's eye view: the
// environment, the panels, and the system UI together. SteamVR takes that
// shot itself when asked for a stereo screenshot and no app has hooked
// screenshots.

#include <openvr.h>

#include <png.h>

#include <csignal>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#include <dirent.h>
#include <cerrno>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <wayland-client.h>
#include "gamescope-action-binding-client-protocol.h"
#include "settings.hpp"

#include <vector>

namespace {

void fail(const std::string &message) {
    std::cerr << message << "\n";
    vr::VR_Shutdown();
    std::exit(1);
}

bool stable_png(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    in.seekg(0, std::ios::end);
    auto size = in.tellg();
    if (size < 32)
        return false;
    in.seekg(0);
    unsigned char magic[8] = {};
    in.read(reinterpret_cast<char *>(magic), 8);
    const unsigned char png[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (std::memcmp(magic, png, 8) != 0)
        return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    std::ifstream again(path, std::ios::binary | std::ios::ate);
    return again && again.tellg() == size;
}

bool wait_for_png(const std::string &path, std::chrono::milliseconds budget) {
    auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        vr::VREvent_t event;
        while (vr::VRSystem()->PollNextEvent(&event, sizeof(event))) {
        }
        if (stable_png(path))
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    return false;
}

// The compositor's stereo file is side by side, with the left eye on the left.
bool crop_half(const std::string &from, const std::string &to, bool right) {
    FILE *in = std::fopen(from.c_str(), "rb");
    if (!in)
        return false;
    png_structp read_png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!read_png) {
        std::fclose(in);
        return false;
    }
    png_infop read_info = png_create_info_struct(read_png);
    if (!read_info) {
        png_destroy_read_struct(&read_png, nullptr, nullptr);
        std::fclose(in);
        return false;
    }

    png_byte *image = nullptr;
    png_bytep *rows = nullptr;
    png_uint_32 width = 0;
    png_uint_32 height = 0;
    png_uint_32 half = 0;
    int channels = 0;
    png_size_t stride = 0;
    png_byte color = PNG_COLOR_TYPE_RGB;
    if (setjmp(png_jmpbuf(read_png))) {
        std::free(image);
        std::free(rows);
        png_destroy_read_struct(&read_png, &read_info, nullptr);
        std::fclose(in);
        return false;
    }
    png_init_io(read_png, in);
    png_read_info(read_png, read_info);
    if (png_get_bit_depth(read_png, read_info) == 16)
        png_set_strip_16(read_png);
    if (png_get_color_type(read_png, read_info) == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(read_png);
    if (png_get_color_type(read_png, read_info) == PNG_COLOR_TYPE_GRAY && png_get_bit_depth(read_png, read_info) < 8)
        png_set_expand_gray_1_2_4_to_8(read_png);
    if (png_get_valid(read_png, read_info, PNG_INFO_tRNS))
        png_set_tRNS_to_alpha(read_png);
    png_set_interlace_handling(read_png);
    png_read_update_info(read_png, read_info);
    width = png_get_image_width(read_png, read_info);
    height = png_get_image_height(read_png, read_info);
    channels = png_get_channels(read_png, read_info);
    stride = png_get_rowbytes(read_png, read_info);
    color = png_get_color_type(read_png, read_info);
    if (width < 2 || height == 0 || channels < 1)
        png_error(read_png, "bad image");
    half = width / 2;
    image = static_cast<png_byte *>(std::malloc(stride * height));
    rows = static_cast<png_bytep *>(std::malloc(sizeof(png_bytep) * height));
    if (!image || !rows)
        png_error(read_png, "out of memory");
    for (png_uint_32 y = 0; y < height; ++y)
        rows[y] = image + y * stride;
    png_read_image(read_png, rows);
    png_read_end(read_png, nullptr);
    png_destroy_read_struct(&read_png, &read_info, nullptr);
    std::fclose(in);

    FILE *out = std::fopen(to.c_str(), "wb");
    if (!out) {
        std::free(image);
        std::free(rows);
        return false;
    }
    png_structp write_png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!write_png) {
        std::fclose(out);
        std::remove(to.c_str());
        std::free(image);
        std::free(rows);
        return false;
    }
    png_infop write_info = png_create_info_struct(write_png);
    if (!write_info) {
        png_destroy_write_struct(&write_png, nullptr);
        std::fclose(out);
        std::remove(to.c_str());
        std::free(image);
        std::free(rows);
        return false;
    }
    if (setjmp(png_jmpbuf(write_png))) {
        png_destroy_write_struct(&write_png, &write_info);
        std::fclose(out);
        std::remove(to.c_str());
        std::free(image);
        std::free(rows);
        return false;
    }
    png_init_io(write_png, out);
    png_set_IHDR(write_png, write_info, half, height, 8, color, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);
    png_write_info(write_png, write_info);
    png_uint_32 origin = right ? half : 0;
    for (png_uint_32 y = 0; y < height; ++y)
        rows[y] = image + y * stride + static_cast<size_t>(origin) * static_cast<size_t>(channels);
    png_write_image(write_png, rows);
    png_write_end(write_png, nullptr);
    png_destroy_write_struct(&write_png, &write_info);
    std::fclose(out);
    std::free(image);
    std::free(rows);
    return true;
}

void copy_file(const std::string &from, const std::string &to) {
    std::ifstream in(from, std::ios::binary);
    std::ofstream out(to, std::ios::binary | std::ios::trunc);
    if (!in || !out)
        fail("Could not save " + to + ".");
    out << in.rdbuf();
    if (!out)
        fail("Could not save " + to + ".");
}

bool regular_file(const std::string &path) {
    if (path.empty())
        return false;
    struct stat info {};
    return stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

std::string executable_dir() {
    char buf[4096];
    ssize_t length = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (length <= 0)
        return {};
    buf[length] = '\0';
    std::string path(buf);
    auto slash = path.find_last_of('/');
    if (slash == std::string::npos)
        return {};
    return path.substr(0, slash);
}

std::string default_sound_path() {
    std::vector<std::string> candidates;
    if (const char *data = std::getenv("XDG_DATA_HOME")) {
        if (data[0])
            candidates.push_back(std::string(data) + "/frameshot/shutter.wav");
    }
    if (const char *home = std::getenv("HOME")) {
        if (home[0])
            candidates.push_back(std::string(home) + "/.local/share/frameshot/shutter.wav");
    }
    std::string dir = executable_dir();
    if (!dir.empty())
        candidates.push_back(dir + "/sounds/shutter.wav");
    for (const std::string &path : candidates) {
        if (regular_file(path))
            return path;
    }
    return {};
}

std::string resolve_sound(const Settings &settings) {
    if (regular_file(settings.sound_file))
        return settings.sound_file;
    std::string fallback = default_sound_path();
    if (!settings.sound_file.empty()) {
        if (fallback.empty())
            std::cerr << "The chosen screenshot sound is missing.\n";
        else
            std::cerr << "The chosen screenshot sound is missing. Using the built-in shutter.\n";
    } else if (fallback.empty()) {
        std::cerr << "No screenshot sound was found.\n";
    }
    return fallback;
}

bool run_player(char *const argv[]) {
    pid_t child = fork();
    if (child == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > 2)
                close(devnull);
        }
        execvp(argv[0], argv);
        _exit(127);
    }
    if (child < 0)
        return false;
    int status = 0;
    if (waitpid(child, &status, 0) < 0)
        return false;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

void play_sound_file(const std::string &path, int volume) {
    if (volume <= 0)
        return;
    if (volume > 100)
        volume = 100;
    char linear[16];
    char pulse[32];
    char filter[32];
    std::snprintf(linear, sizeof(linear), "%.3f", volume / 100.0);
    std::snprintf(pulse, sizeof(pulse), "--volume=%d", volume * 65536 / 100);
    std::snprintf(filter, sizeof(filter), "volume=%.3f", volume / 100.0);
    pid_t pid = fork();
    if (pid != 0)
        return;
    setsid();
    signal(SIGCHLD, SIG_DFL);
    char *file = const_cast<char *>(path.c_str());
    char *pw[] = {const_cast<char *>("pw-play"), const_cast<char *>("--volume"), linear, file, nullptr};
    char *pa[] = {const_cast<char *>("paplay"), pulse, file, nullptr};
    char *ff[] = {const_cast<char *>("ffplay"), const_cast<char *>("-nodisp"), const_cast<char *>("-autoexit"),
                  const_cast<char *>("-loglevel"), const_cast<char *>("error"), const_cast<char *>("-af"), filter, file,
                  nullptr};
    char *ap[] = {const_cast<char *>("aplay"), const_cast<char *>("-q"), file, nullptr};
    if (run_player(pw) || run_player(pa) || run_player(ff) || run_player(ap))
        _exit(0);
    _exit(1);
}

void play_screenshot_sound(const Settings &settings, bool force) {
    if (!force && !settings.sound)
        return;
    std::string path = resolve_sound(settings);
    if (!path.empty())
        play_sound_file(path, settings.sound_volume);
}

std::string default_path() {
    const char *home = std::getenv("HOME");
    if (!home || !home[0])
        home = ".";
    std::time_t now = std::time(nullptr);
    std::tm local {};
    localtime_r(&now, &local);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);
    return std::string(home) + "/Pictures/Frame-" + stamp + ".png";
}

void usage() {
    std::cerr
        << "Usage: frameshot [options] [output.png]\n"
        << "Save the Steam Frame compositor view.\n"
        << "  --left     left eye (default)\n"
        << "  --right    right eye\n"
        << "  --both     left and right eyes, side by side\n"
        << "  --eye      same as --left\n"
        << "  --view     open the image after saving\n"
        << "  --listen   wait for the shortcut, including during games\n"
        << "  --play-sound  play the screenshot sound and exit\n";
}

}  // namespace

// Remote Play never creates a /dev/input keyboard. Steam injects those keys
// through Gamescope, which matches action bindings before the focused game
// sees them. Shift can change the key's symbol, so both symbols are registered.

enum class RemoteState { Down, Armed, Unsupported };

struct RemoteKeys {
    wl_display *display = nullptr;
    gamescope_action_binding_manager *manager = nullptr;
    gamescope_action_binding *binding = nullptr;
};

bool g_remote_shot = false;

extern "C" void frameshot_remote_triggered(void *, gamescope_action_binding *, uint32_t, uint32_t, uint32_t, uint32_t) {
    g_remote_shot = true;
}

extern "C" void frameshot_remote_global(void *data, wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
    if (version < 1 || std::strcmp(interface, gamescope_action_binding_manager_interface.name) != 0)
        return;
    auto *remote = static_cast<RemoteKeys *>(data);
    remote->manager = static_cast<gamescope_action_binding_manager *>(
        wl_registry_bind(registry, name, &gamescope_action_binding_manager_interface, 1));
}

extern "C" void frameshot_remote_global_remove(void *, wl_registry *, uint32_t) {}

const gamescope_action_binding_listener k_binding_listener = {frameshot_remote_triggered};
const wl_registry_listener k_registry_listener = {frameshot_remote_global, frameshot_remote_global_remove};

void add_chord(gamescope_action_binding *binding, const std::vector<uint32_t> &chord) {
    wl_array keys;
    wl_array_init(&keys);
    for (uint32_t key : chord) {
        auto *slot = static_cast<uint32_t *>(wl_array_add(&keys, sizeof(key)));
        if (slot)
            *slot = key;
    }
    gamescope_action_binding_add_keyboard_trigger(binding, &keys);
    wl_array_release(&keys);
}

void bind_shortcut(gamescope_action_binding *binding, const Shortcut &shortcut) {
    gamescope_action_binding_set_description(binding, "Frame Screenshot");
    gamescope_action_binding_clear_triggers(binding);
    std::vector<std::vector<uint32_t>> chords = gamescope_chords(shortcut);
    if (chords.empty())
        std::cerr << "Could not translate the shortcut for Remote Play.\n";
    for (const std::vector<uint32_t> &chord : chords)
        add_chord(binding, chord);
    gamescope_action_binding_arm(binding, 0);
}

void remote_close(RemoteKeys &remote) {
    if (remote.display)
        wl_display_disconnect(remote.display);
    remote.display = nullptr;
    remote.manager = nullptr;
    remote.binding = nullptr;
}

RemoteState remote_open(RemoteKeys &remote, const Shortcut &shortcut) {
    const char *name = std::getenv("GAMESCOPE_WAYLAND_DISPLAY");
    if (!name || !name[0])
        name = "gamescope-0";
    remote.display = wl_display_connect(name);
    if (!remote.display)
        return RemoteState::Down;

    wl_registry *registry = wl_display_get_registry(remote.display);
    if (!registry || wl_registry_add_listener(registry, &k_registry_listener, &remote) != 0) {
        remote_close(remote);
        return RemoteState::Down;
    }
    if (wl_display_roundtrip(remote.display) < 0) {
        remote_close(remote);
        return RemoteState::Down;
    }
    if (!remote.manager) {
        std::cerr << "Gamescope has no shortcut hook, so the Remote Play keyboard cannot take a screenshot.\n";
        remote_close(remote);
        return RemoteState::Unsupported;
    }

    remote.binding = gamescope_action_binding_manager_create_action_binding(remote.manager);
    if (!remote.binding) {
        remote_close(remote);
        return RemoteState::Down;
    }
    gamescope_action_binding_add_listener(remote.binding, &k_binding_listener, nullptr);
    bind_shortcut(remote.binding, shortcut);
    if (wl_display_roundtrip(remote.display) < 0) {
        remote_close(remote);
        return RemoteState::Down;
    }
    std::cerr << "Remote Play keyboard: " << shortcut_label(shortcut) << "\n";
    return RemoteState::Armed;
}

bool key_bit(int fd, int code) {
    unsigned char bits[(KEY_MAX / 8) + 1] = {};
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) < 0)
        return false;
    return bits[code / 8] & (1u << (code % 8));
}

bool device_has_shortcut(int fd, const Shortcut &shortcut) {
    if (!key_bit(fd, shortcut.evdev))
        return false;
    if (shortcut.ctrl && !key_bit(fd, KEY_LEFTCTRL) && !key_bit(fd, KEY_RIGHTCTRL))
        return false;
    if (shortcut.shift && !key_bit(fd, KEY_LEFTSHIFT) && !key_bit(fd, KEY_RIGHTSHIFT))
        return false;
    if (shortcut.alt && !key_bit(fd, KEY_LEFTALT) && !key_bit(fd, KEY_RIGHTALT))
        return false;
    if (shortcut.super_key && !key_bit(fd, KEY_LEFTMETA) && !key_bit(fd, KEY_RIGHTMETA))
        return false;
    return true;
}

// Gamescope reads every keyboard for the focused game, without taking the
// device exclusively, so this sees the shortcut in desktop mode and in
// an immersive scene.
int listen_for_shortcut() {
    signal(SIGCHLD, SIG_IGN);
    int notify = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (notify < 0 || inotify_add_watch(notify, "/dev/input", IN_CREATE | IN_ATTRIB) < 0) {
        std::cerr << "Could not watch for keyboards.\n";
        return 1;
    }

    Settings settings = load_settings();
    struct timespec settings_time {};
    struct stat settings_stat {};
    if (stat(settings_path().c_str(), &settings_stat) == 0)
        settings_time = settings_stat.st_mtim;

    struct Keyboard {
        int fd;
        bool ctrl;
        bool shift;
        bool alt;
        bool super_key;
    };
    std::vector<Keyboard> keys;
    auto close_keyboards = [&] {
        for (const Keyboard &keyboard : keys)
            close(keyboard.fd);
        keys.clear();
    };
    auto open_keyboards = [&] {
        DIR *dir = opendir("/dev/input");
        if (!dir)
            return;
        while (dirent *entry = readdir(dir)) {
            if (std::strncmp(entry->d_name, "event", 5) != 0)
                continue;
            std::string path = std::string("/dev/input/") + entry->d_name;
            bool known = false;
            for (const Keyboard &keyboard : keys) {
                char other[64] = {};
                std::snprintf(other, sizeof(other), "/proc/self/fd/%d", keyboard.fd);
                char link[256] = {};
                ssize_t n = readlink(other, link, sizeof(link) - 1);
                if (n > 0 && path == std::string(link, link + n))
                    known = true;
            }
            if (known)
                continue;
            int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0)
                continue;
            if (!device_has_shortcut(fd, settings.shortcut)) {
                close(fd);
                continue;
            }
            char name[128] = {};
            ioctl(fd, EVIOCGNAME(sizeof(name)), name);
            std::cerr << "Watching " << (name[0] ? name : path.c_str()) << "\n";
            keys.push_back({fd, false, false, false, false});
        }
        closedir(dir);
    };
    open_keyboards();
    std::cerr << shortcut_label(settings.shortcut) << " saves a headset screenshot\n";
    RemoteKeys remote;
    RemoteState remote_state = remote_open(remote, settings.shortcut);

    auto last_shot = std::chrono::steady_clock::time_point {};
    auto shoot = [&] {
        auto now = std::chrono::steady_clock::now();
        if (now - last_shot < std::chrono::milliseconds(800))
            return;
        last_shot = now;
        std::cerr << shortcut_label(settings.shortcut) << "\n";
        pid_t child = fork();
        if (child == 0) {
            setsid();
            execl("/proc/self/exe", "frameshot", nullptr);
            _exit(127);
        }
        if (child < 0)
            std::cerr << "Could not start the screenshot.\n";
    };

    while (true) {
        if (stat(settings_path().c_str(), &settings_stat) == 0 &&
            (settings_stat.st_mtim.tv_sec != settings_time.tv_sec ||
             settings_stat.st_mtim.tv_nsec != settings_time.tv_nsec)) {
            settings_time = settings_stat.st_mtim;
            Settings updated = load_settings();
            bool shortcut_changed = !(updated.shortcut == settings.shortcut);
            settings = updated;
            if (shortcut_changed) {
                std::cerr << shortcut_label(settings.shortcut) << " saves a headset screenshot\n";
                close_keyboards();
                open_keyboards();
                if (remote.binding) {
                    bind_shortcut(remote.binding, settings.shortcut);
                    if (wl_display_roundtrip(remote.display) < 0) {
                        remote_close(remote);
                        remote_state = RemoteState::Down;
                    } else {
                        std::cerr << "Remote Play keyboard: " << shortcut_label(settings.shortcut) << "\n";
                    }
                }
            }
        }
        if (remote_state == RemoteState::Down)
            remote_state = remote_open(remote, settings.shortcut);
        bool reading = false;
        if (remote.display) {
            while (wl_display_prepare_read(remote.display) != 0) {
                if (wl_display_dispatch_pending(remote.display) < 0) {
                    remote_close(remote);
                    remote_state = RemoteState::Down;
                    break;
                }
            }
            reading = remote.display != nullptr;
            if (reading && wl_display_flush(remote.display) < 0 && errno != EAGAIN) {
                wl_display_cancel_read(remote.display);
                remote_close(remote);
                remote_state = RemoteState::Down;
                reading = false;
            }
        }

        std::vector<pollfd> pollfds;
        pollfds.push_back({notify, POLLIN, 0});
        for (const Keyboard &keyboard : keys)
            pollfds.push_back({keyboard.fd, POLLIN, 0});
        if (reading)
            pollfds.push_back({wl_display_get_fd(remote.display), POLLIN, 0});
        poll(pollfds.data(), pollfds.size(), 1000);

        if (reading) {
            short wayland_events = pollfds.back().revents;
            if (wayland_events & (POLLIN | POLLHUP | POLLERR)) {
                if (wl_display_read_events(remote.display) < 0) {
                    remote_close(remote);
                    remote_state = RemoteState::Down;
                }
            } else {
                wl_display_cancel_read(remote.display);
            }
            if (remote.display && wl_display_dispatch_pending(remote.display) < 0) {
                remote_close(remote);
                remote_state = RemoteState::Down;
            }
            if (g_remote_shot) {
                g_remote_shot = false;
                shoot();
            }
        }

        if (pollfds[0].revents & POLLIN) {
            char buffer[4096];
            while (read(notify, buffer, sizeof(buffer)) > 0) {
            }
            open_keyboards();
        }
        for (size_t i = 0; i < keys.size();) {
            short revents = pollfds[i + 1].revents;
            if (revents & (POLLERR | POLLHUP | POLLNVAL)) {
                close(keys[i].fd);
                keys.erase(keys.begin() + i);
                continue;
            }
            if (revents & POLLIN) {
                input_event event {};
                while (read(keys[i].fd, &event, sizeof(event)) == static_cast<ssize_t>(sizeof(event))) {
                    if (event.type != EV_KEY)
                        continue;
                    if (event.code == KEY_LEFTCTRL || event.code == KEY_RIGHTCTRL)
                        keys[i].ctrl = event.value != 0;
                    if (event.code == KEY_LEFTSHIFT || event.code == KEY_RIGHTSHIFT)
                        keys[i].shift = event.value != 0;
                    if (event.code == KEY_LEFTALT || event.code == KEY_RIGHTALT)
                        keys[i].alt = event.value != 0;
                    if (event.code == KEY_LEFTMETA || event.code == KEY_RIGHTMETA)
                        keys[i].super_key = event.value != 0;
                    bool ctrl = false;
                    bool shift = false;
                    bool alt = false;
                    bool super_key = false;
                    for (const Keyboard &keyboard : keys) {
                        ctrl = ctrl || keyboard.ctrl;
                        shift = shift || keyboard.shift;
                        alt = alt || keyboard.alt;
                        super_key = super_key || keyboard.super_key;
                    }
                    bool mods = (!settings.shortcut.ctrl || ctrl) && (!settings.shortcut.shift || shift) &&
                                (!settings.shortcut.alt || alt) && (!settings.shortcut.super_key || super_key);
                    if (event.code == settings.shortcut.evdev && event.value == 1 && mods)
                        shoot();
                }
            }
            ++i;
        }
    }
}

int main(int argc, char **argv) {
    Settings settings = load_settings();
    EyeView eye = settings.eye;
    bool view = settings.open_after;
    bool play_only = false;
    std::string path;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--both")
            eye = EyeView::Both;
        else if (arg == "--right")
            eye = EyeView::Right;
        else if (arg == "--left" || arg == "--eye")
            eye = EyeView::Left;
        else if (arg == "--view")
            view = true;
        else if (arg == "--play-sound")
            play_only = true;
        else if (arg == "--listen") {
            return listen_for_shortcut();
        }
        else if (arg == "--help" || arg == "-h") {
            usage();
            return 0;
        } else if (!arg.empty() && arg[0] != '-') {
            path = arg;
        } else {
            usage();
            return 2;
        }
    }
    if (play_only) {
        play_screenshot_sound(settings, true);
        return 0;
    }
    if (path.empty())
        path = default_path();

    vr::EVRInitError init_error = vr::VRInitError_None;
    vr::VR_Init(&init_error, vr::VRApplication_Background);
    if (init_error != vr::VRInitError_None) {
        const char *why = vr::VR_GetVRInitErrorAsEnglishDescription(init_error);
        std::cerr << "SteamVR is not available: " << (why ? why : "unknown error") << "\n";
        return 1;
    }

    vr::IVRScreenshots *shots = vr::VRScreenshots();
    if (!shots)
        fail("The compositor screenshot interface is not available.");

    std::string stem = "/tmp/frameshot-" + std::to_string(getpid());
    vr::ScreenshotHandle_t handle = 0;
    vr::EVRScreenshotError shot_error = shots->RequestScreenshot(
        &handle, vr::VRScreenshotType_Stereo, (stem + "-eye").c_str(), (stem + "-both").c_str());
    if (shot_error != vr::VRScreenshotError_None)
        fail("The compositor did not take a screenshot (error " + std::to_string(shot_error) + ").");

    std::string eye_file = stem + "-eye.png";
    std::string both_file = stem + "-both.png";
    if (!wait_for_png(both_file, std::chrono::seconds(5)))
        fail("The compositor did not produce an image.");

    std::string folder = path.substr(0, path.find_last_of('/'));
    if (!folder.empty())
        mkdir(folder.c_str(), 0755);
    if (eye == EyeView::Both)
        copy_file(both_file, path);
    else if (!crop_half(both_file, path, eye == EyeView::Right))
        fail("Could not save " + path + ".");
    std::remove(eye_file.c_str());
    std::remove(both_file.c_str());
    vr::VR_Shutdown();
    play_screenshot_sound(settings, false);

    std::cout << path << "\n";
    if (view) {
        pid_t child = fork();
        if (child == 0) {
            execlp("xdg-open", "xdg-open", path.c_str(), nullptr);
            _exit(127);
        }
    }
    return 0;
}
