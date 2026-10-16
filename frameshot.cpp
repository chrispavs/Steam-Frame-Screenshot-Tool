// Save what the Steam Frame compositor is showing in the headset.
//
// gamescopectl screenshot writes the flat desktop buffer (1280x720 on this
// session). The image in the headset is the VR compositor's eye view: the
// environment, the panels, and the system UI together. SteamVR takes that
// shot itself when asked for a stereo screenshot and no app has hooked
// screenshots.

#include <openvr.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#include <sys/stat.h>
#include <unistd.h>

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
        << "  --eye    one eye, the view you see (default)\n"
        << "  --both   left and right eyes, side by side\n"
        << "  --view   open the image after saving\n";
}

}  // namespace

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
