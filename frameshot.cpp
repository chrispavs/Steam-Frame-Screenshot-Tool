// Save what the Steam Frame compositor is showing in the headset.
//
// gamescopectl screenshot writes the flat desktop buffer (1280x720 on this
// session). The image in the headset is the VR compositor's eye view: the
// environment, the panels, and the system UI together. SteamVR takes that
// shot itself when asked for a stereo screenshot and no app has hooked
// screenshots.

#include <openvr.h>

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

void copy_file(const std::string &from, const std::string &to) {
    std::ifstream in(from, std::ios::binary);
    std::ofstream out(to, std::ios::binary | std::ios::trunc);
    if (!in || !out)
        fail("Could not save " + to + ".");
    out << in.rdbuf();
    if (!out)
        fail("Could not save " + to + ".");
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
        << "  --eye      one eye, the view you see (default)\n"
        << "  --both     left and right eyes, side by side\n"
        << "  --view     open the image after saving\n"
        << "  --listen   wait for Ctrl+Shift+2, including during games\n";
}

}  // namespace

// Remote Play never creates a /dev/input keyboard. Steam injects those keys
// through Gamescope, which matches action bindings before the focused game
// sees them. With Shift already held, the 2 key is @, so both symbols are
// registered. The plain 2 covers pressing it before Shift.
constexpr uint32_t k_ctrl_l = 0xffe3;
constexpr uint32_t k_ctrl_r = 0xffe4;
constexpr uint32_t k_shift_l = 0xffe1;
constexpr uint32_t k_shift_r = 0xffe2;
constexpr uint32_t k_key_2 = 0x0032;
constexpr uint32_t k_at = 0x0040;

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

void add_chord(gamescope_action_binding *binding, uint32_t first, uint32_t second, uint32_t third) {
    uint32_t chord[3] = {first, second, third};
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

void remote_close(RemoteKeys &remote) {
    if (remote.display)
        wl_display_disconnect(remote.display);
    remote.display = nullptr;
    remote.manager = nullptr;
    remote.binding = nullptr;
}

RemoteState remote_open(RemoteKeys &remote) {
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
    gamescope_action_binding_set_description(remote.binding, "Frame Screenshot");
    const uint32_t ctrls[] = {k_ctrl_l, k_ctrl_r};
    const uint32_t shifts[] = {k_shift_l, k_shift_r};
    const uint32_t digits[] = {k_at, k_key_2};
    for (uint32_t ctrl : ctrls)
        for (uint32_t shift : shifts)
            for (uint32_t digit : digits)
                add_chord(remote.binding, ctrl, shift, digit);
    gamescope_action_binding_arm(remote.binding, 0);
    if (wl_display_roundtrip(remote.display) < 0) {
        remote_close(remote);
        return RemoteState::Down;
    }
    std::cerr << "Remote Play keyboard: Ctrl+Shift+2\n";
    return RemoteState::Armed;
}

bool key_bit(int fd, int code) {
    unsigned char bits[(KEY_MAX / 8) + 1] = {};
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) < 0)
        return false;
    return bits[code / 8] & (1u << (code % 8));
}

// Gamescope reads every keyboard for the focused game, without taking the
// device exclusively, so this sees Ctrl+Shift+2 in desktop mode and in
// an immersive scene.
int listen_for_shortcut() {
    signal(SIGCHLD, SIG_IGN);
    int notify = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (notify < 0 || inotify_add_watch(notify, "/dev/input", IN_CREATE | IN_ATTRIB) < 0) {
        std::cerr << "Could not watch for keyboards.\n";
        return 1;
    }

    struct Keyboard {
        int fd;
        bool ctrl;
        bool shift;
    };
    std::vector<Keyboard> keys;
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
            if (!key_bit(fd, KEY_2) ||
                !(key_bit(fd, KEY_LEFTCTRL) || key_bit(fd, KEY_RIGHTCTRL)) ||
                !(key_bit(fd, KEY_LEFTSHIFT) || key_bit(fd, KEY_RIGHTSHIFT))) {
                close(fd);
                continue;
            }
            char name[128] = {};
            ioctl(fd, EVIOCGNAME(sizeof(name)), name);
            std::cerr << "Watching " << (name[0] ? name : path.c_str()) << "\n";
            keys.push_back({fd, false, false});
        }
        closedir(dir);
    };
    open_keyboards();
    std::cerr << "Ctrl+Shift+2 saves a headset screenshot\n";
    RemoteKeys remote;
    RemoteState remote_state = remote_open(remote);

    auto last_shot = std::chrono::steady_clock::time_point {};
    auto shoot = [&] {
        auto now = std::chrono::steady_clock::now();
        if (now - last_shot < std::chrono::milliseconds(800))
            return;
        last_shot = now;
        std::cerr << "Ctrl+Shift+2\n";
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
        if (remote_state == RemoteState::Down)
            remote_state = remote_open(remote);
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
                    bool ctrl = false;
                    bool shift = false;
                    for (const Keyboard &keyboard : keys) {
                        ctrl = ctrl || keyboard.ctrl;
                        shift = shift || keyboard.shift;
                    }
                    if (event.code == KEY_2 && event.value == 1 && ctrl && shift)
                        shoot();
                }
            }
            ++i;
        }
    }
}

int main(int argc, char **argv) {
    bool both = false;
    bool view = false;
    std::string path;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--both")
            both = true;
        else if (arg == "--eye")
            both = false;
        else if (arg == "--view")
            view = true;
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
    if (!wait_for_png(eye_file, std::chrono::seconds(5)))
        fail("The compositor did not produce an image.");
    wait_for_png(both_file, std::chrono::seconds(2));
    if (both && !stable_png(both_file))
        fail("The compositor did not produce the side-by-side image.");

    std::string source = both ? both_file : eye_file;
    std::string folder = path.substr(0, path.find_last_of('/'));
    if (!folder.empty())
        mkdir(folder.c_str(), 0755);
    copy_file(source, path);
    std::remove(source.c_str());
    std::remove((stem + "-eye.png").c_str());
    std::remove((stem + "-both.png").c_str());
    vr::VR_Shutdown();

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
