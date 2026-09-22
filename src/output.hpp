#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace obs2led {

enum class Transport { udp, usb };
enum class Scaling { nearest, bilinear, bicubic, lanczos, area };

struct OutputConfig {
    Transport transport = Transport::udp;
    std::string address;
    int port = 9090;
    std::string device;
    int baud = 2000000;
    int width = 64;
    int height = 32;
    int fps = 30;
    Scaling scaling = Scaling::nearest;
    bool operator==(const OutputConfig &) const = default;
};

inline constexpr int max_dimension = 512;
inline constexpr size_t max_udp_payload = 65507;
inline constexpr size_t usb_header_size = 24;

// Empty means valid. Validation never resolves DNS or opens a device.
std::string validate(const OutputConfig &config);
std::vector<uint8_t> rgb_from_rgba(const uint8_t *rgba, uint32_t stride,
                                 uint32_t width, uint32_t height);
uint32_t crc32(std::span<const uint8_t> bytes);
std::vector<uint8_t> usb_packet(std::span<const uint8_t> rgb, uint16_t width,
                                uint16_t height, uint32_t sequence);

struct SerialDevice { std::string path; std::string label; };
std::vector<SerialDevice> serial_devices();

using Cancelled = std::function<bool()>;
class Connection {
public:
    virtual ~Connection() = default;
    virtual void write(std::span<const uint8_t> bytes, const Cancelled &cancelled) = 0;
};
std::unique_ptr<Connection> open_udp(const OutputConfig &config);
std::unique_ptr<Connection> open_serial(const OutputConfig &config);

// Owns all I/O. The rendering thread only hands over the newest complete frame.
class Output {
public:
    Output();
    ~Output();
    Output(const Output &) = delete;
    Output &operator=(const Output &) = delete;
    uint64_t configure(const OutputConfig &config);
    bool ready() const noexcept { return ready_.load(); }
    void set_active(bool active) noexcept;
    void submit(std::vector<uint8_t> rgb, uint64_t generation);
    std::string status() const;

private:
    struct Frame {
        std::vector<uint8_t> rgb;
        uint64_t generation;
        std::chrono::steady_clock::time_point captured;
    };
    void run();
    void set_status(std::string text, uint64_t generation);
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    OutputConfig config_;
    std::optional<Frame> pending_;
    std::string status_ = "Enter a destination IP address.";
    std::atomic<uint64_t> generation_{0};
    std::atomic<bool> ready_{false};
    std::atomic<bool> active_{true};
    std::atomic<bool> stop_{false};
    // Explicitly joined for compatibility with Apple libc++ before Xcode 26.
    std::thread worker_;
};

} // namespace obs2led
