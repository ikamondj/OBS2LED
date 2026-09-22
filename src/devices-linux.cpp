#include "output.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>

namespace obs2led {
namespace {
std::string read(const std::filesystem::path &path)
{
    std::ifstream stream(path);
    std::string value;
    std::getline(stream, value);
    return value;
}
} // namespace

std::vector<SerialDevice> serial_devices()
{
    namespace fs = std::filesystem;
    std::map<fs::path, std::string> stable_paths;
    std::error_code error;
    for (const auto &entry : fs::directory_iterator("/dev/serial/by-id", error)) {
        const auto target = fs::canonical(entry.path(), error);
        if (!error) stable_paths[target] = entry.path().string();
    }
    std::vector<SerialDevice> devices;
    error.clear();
    for (const auto &entry : fs::directory_iterator("/sys/class/tty", error)) {
        auto usb = fs::canonical(entry.path() / "device", error);
        if (error) continue;
        while (!usb.empty() && usb != usb.root_path()) {
            if (fs::exists(usb / "idVendor", error) && fs::exists(usb / "idProduct", error)) {
                const auto path = fs::path("/dev") / entry.path().filename();
                if (!fs::exists(path, error)) break;
                auto label = read(usb / "product");
                if (label.empty()) label = "USB serial";
                label += " [" + read(usb / "idVendor") + ":" + read(usb / "idProduct") + "] " + path.string();
                const auto stable = stable_paths.find(path);
                devices.push_back({stable != stable_paths.end() ? stable->second : path.string(), label});
                break;
            }
            usb = usb.parent_path();
        }
    }
    std::sort(devices.begin(), devices.end(), [](const auto &a, const auto &b) { return a.path < b.path; });
    return devices;
}
} // namespace obs2led
