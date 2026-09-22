#include "output.hpp"

#include <algorithm>
#include <stdexcept>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

namespace obs2led {

std::string validate(const OutputConfig &c)
{
    if (c.width < 1 || c.height < 1 || c.width > max_dimension || c.height > max_dimension)
        return "Width and height must be between 1 and 512.";
    if (c.fps < 1 || c.fps > 60)
        return "Frame rate must be between 1 and 60.";
    if (c.transport == Transport::usb) {
        if (c.device.empty()) return "Select a USB serial device, then refresh the status.";
        if (c.baud != 115200 && c.baud != 230400 && c.baud != 460800 && c.baud != 921600 &&
            c.baud != 1000000 && c.baud != 2000000 && c.baud != 3000000)
            return "Choose a supported serial baud rate.";
    } else {
        if (c.address.empty()) return "Enter a destination IP address.";
        in_addr v4{};
        in6_addr v6{};
        const bool ipv4 = inet_pton(AF_INET, c.address.c_str(), &v4) == 1;
        const bool ipv6 = inet_pton(AF_INET6, c.address.c_str(), &v6) == 1;
        if (!ipv4 && !ipv6) return "Enter a numeric IPv4 or IPv6 address (no URL or hostname).";
        if ((ipv4 && v4.s_addr == 0) ||
            (ipv6 && std::all_of(std::begin(v6.s6_addr), std::end(v6.s6_addr), [](auto b) { return b == 0; })))
            return "The destination cannot be an unspecified address.";
        if (c.port < 1 || c.port > 65535) return "Port must be between 1 and 65535.";
        if (static_cast<size_t>(c.width) * c.height * 3 > max_udp_payload)
            return "Raw UDP frames must fit in 65,507 bytes. Reduce the output resolution.";
    }
    return {};
}

std::vector<uint8_t> rgb_from_rgba(const uint8_t *rgba, uint32_t stride,
                                 uint32_t width, uint32_t height)
{
    if (!rgba || width > max_dimension || height > max_dimension || stride < width * 4)
        throw std::invalid_argument("Invalid RGBA readback");
    std::vector<uint8_t> rgb(static_cast<size_t>(width) * height * 3);
    for (uint32_t y = 0; y < height; ++y) {
        const auto *row = rgba + static_cast<size_t>(y) * stride;
        auto *out = rgb.data() + static_cast<size_t>(y) * width * 3;
        for (uint32_t x = 0; x < width; ++x) {
            // OBS's premultiplied RGB is already composited against black.
            std::copy_n(row + x * 4, 3, out + x * 3);
        }
    }
    return rgb;
}

uint32_t crc32(std::span<const uint8_t> bytes)
{
    uint32_t crc = 0xffffffff;
    for (const auto byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

std::vector<uint8_t> usb_packet(std::span<const uint8_t> rgb, uint16_t width,
                                uint16_t height, uint32_t sequence)
{
    if (!width || !height || width > max_dimension || height > max_dimension ||
        rgb.size() != static_cast<size_t>(width) * height * 3)
        throw std::invalid_argument("Invalid USB RGB frame");
    std::vector<uint8_t> packet(usb_header_size + rgb.size());
    auto put = [&packet](size_t offset, uint32_t value, size_t bytes) {
        for (size_t i = 0; i < bytes; ++i) packet[offset + i] = static_cast<uint8_t>(value >> (8 * i));
    };
    packet[0] = 'O'; packet[1] = '2'; packet[2] = 'L'; packet[3] = 'F';
    packet[4] = 1; // Protocol version; flags at byte 5 remain zero.
    put(6, usb_header_size, 2);
    put(8, width, 2); put(10, height, 2);
    put(12, sequence, 4); put(16, static_cast<uint32_t>(rgb.size()), 4);
    put(20, crc32(rgb), 4);
    std::copy(rgb.begin(), rgb.end(), packet.begin() + usb_header_size);
    return packet;
}

Output::Output() : worker_([this] { run(); }) {}
Output::~Output()
{
    stop_ = true;
    wake_.notify_all();
    worker_.join();
}

uint64_t Output::configure(const OutputConfig &config)
{
    std::lock_guard lock(mutex_);
    if (generation_ != 0 && config_ == config) return generation_;
    config_ = config;
    pending_.reset();
    ready_ = false;
    status_ = validate(config);
    if (status_.empty()) status_ = "Connecting...";
    const auto generation = ++generation_;
    wake_.notify_one();
    return generation;
}

void Output::set_active(bool active) noexcept
{
    active_ = active;
    if (!active) {
        std::unique_lock lock(mutex_, std::try_to_lock);
        if (lock) pending_.reset();
    }
    wake_.notify_one();
}

void Output::submit(std::vector<uint8_t> rgb, uint64_t generation)
{
    // Never wait for the I/O thread while inside OBS's graphics context.
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock || !ready_ || !active_ || generation != generation_) return;
    if (rgb.size() != static_cast<size_t>(config_.width) * config_.height * 3) return;
    pending_ = Frame{std::move(rgb), generation, std::chrono::steady_clock::now()};
    wake_.notify_one();
}

std::string Output::status() const
{
    std::lock_guard lock(mutex_);
    return status_;
}

void Output::set_status(std::string text, uint64_t generation)
{
    std::lock_guard lock(mutex_);
    if (generation == generation_) status_ = std::move(text);
}

void Output::run()
{
    using namespace std::chrono_literals;
    OutputConfig config;
    uint64_t generation = 0;
    uint32_t sequence = 0;
    std::unique_ptr<Connection> connection;
    auto retry = std::chrono::steady_clock::now();
    bool valid = false;
    while (!stop_) {
        try {
            std::optional<Frame> frame;
            {
                std::unique_lock lock(mutex_);
                wake_.wait_for(lock, 50ms, [&] {
                    return stop_ || generation != generation_ || pending_.has_value();
                });
                if (stop_) break;
                if (generation != generation_) {
                    config = config_;
                    generation = generation_;
                    valid = validate(config).empty();
                    connection.reset();
                    ready_ = false;
                    retry = std::chrono::steady_clock::now();
                }
                frame = std::move(pending_);
                pending_.reset();
            }
            if (!valid || !active_) {
                // Disabling either output must release USB so another output
                // (or a firmware uploader) can use the same serial device.
                connection.reset();
                ready_ = false;
                continue;
            }
            if (!connection && std::chrono::steady_clock::now() >= retry) {
                connection = config.transport == Transport::udp ? open_udp(config) : open_serial(config);
                {
                    std::lock_guard lock(mutex_);
                    if (generation != generation_) { connection.reset(); continue; }
                    ready_ = true;
                    status_ = "Ready; waiting for this source to render.";
                }
            }
            if (!connection || !frame || frame->generation != generation ||
                std::chrono::steady_clock::now() - frame->captured > 250ms) continue;
            const auto cancelled = [&] { return stop_ || generation != generation_ || !active_; };
            if (cancelled()) continue;
            if (config.transport == Transport::udp) {
                connection->write(frame->rgb, cancelled);
            } else {
                const auto packet = usb_packet(frame->rgb, static_cast<uint16_t>(config.width),
                                               static_cast<uint16_t>(config.height), sequence++);
                connection->write(packet, cancelled);
            }
            if (!cancelled()) set_status(config.transport == Transport::udp
                ? "Sending UDP (delivery is not acknowledged)." : "Sending RGB frames over USB serial.", generation);
        } catch (const std::exception &error) {
            ready_ = false;
            connection.reset();
            retry = std::chrono::steady_clock::now() + 1s;
            if (!stop_) set_status(std::string(error.what()) + " Retrying...", generation);
        }
    }
    ready_ = false;
}

} // namespace obs2led
