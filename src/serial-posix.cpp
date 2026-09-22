#include "output.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <poll.h>
#include <stdexcept>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#ifdef __APPLE__
#include <IOKit/serial/ioss.h>
#endif

namespace obs2led {
namespace {
[[noreturn]] void fail(const char *operation)
{ throw std::runtime_error(std::string(operation) + ": " + std::strerror(errno)); }

#ifndef __APPLE__
speed_t baud_speed(int baud)
{
    switch (baud) {
    case 115200: return B115200;
    case 230400: return B230400;
    case 460800: return B460800;
    case 921600: return B921600;
    case 1000000: return B1000000;
    case 2000000: return B2000000;
    case 3000000: return B3000000;
    default: throw std::runtime_error("Unsupported baud rate");
    }
}
#endif

class Serial final : public Connection {
public:
    explicit Serial(const OutputConfig &config)
    {
        // Saved settings must identify a serial device, never a regular file.
        std::error_code error;
        const auto path = std::filesystem::canonical(config.device, error);
        if (error || !path.string().starts_with("/dev/"))
            throw std::runtime_error("USB serial device is not connected.");
        struct stat info{};
        if (stat(path.c_str(), &info) != 0 || !S_ISCHR(info.st_mode))
            throw std::runtime_error("Selected USB endpoint is not a character device.");
        fd_ = open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
        if (fd_ < 0) fail("Open USB serial device (check permissions / other applications)");
        try {
            termios state{};
            if (tcgetattr(fd_, &state) != 0) fail("Read serial settings");
            if (ioctl(fd_, TIOCEXCL) != 0) fail("Claim USB serial device");
            cfmakeraw(&state);
            state.c_cflag |= CLOCAL | CREAD;
            state.c_cflag &= ~(CSTOPB | PARENB | CRTSCTS | CSIZE);
            state.c_cflag |= CS8;
            state.c_cc[VMIN] = 0; state.c_cc[VTIME] = 0;
#ifdef __APPLE__
            const speed_t initial = B9600;
#else
            const speed_t initial = baud_speed(config.baud);
#endif
            if (cfsetispeed(&state, initial) || cfsetospeed(&state, initial) || tcsetattr(fd_, TCSANOW, &state))
                fail("Configure serial baud rate / 8N1");
#ifdef __APPLE__
            speed_t baud = static_cast<speed_t>(config.baud);
            if (ioctl(fd_, IOSSIOSPEED, &baud) != 0) fail("Set USB serial baud rate");
#endif
            int bits = TIOCM_DTR;
            // Some tty drivers do not expose modem-control bits.
            if (ioctl(fd_, TIOCMBIS, &bits) != 0 && errno != ENOTTY && errno != EINVAL)
                fail("Assert USB serial DTR");
            tcflush(fd_, TCIOFLUSH);
        } catch (...) { close(fd_); fd_ = -1; throw; }
    }
    ~Serial() override { if (fd_ >= 0) close(fd_); }
    void write(std::span<const uint8_t> bytes, const Cancelled &cancelled) override
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        size_t offset = 0;
        while (offset < bytes.size()) {
            if (cancelled() || std::chrono::steady_clock::now() >= deadline) {
                tcflush(fd_, TCOFLUSH);
                throw std::runtime_error("USB transfer cancelled or timed out.");
            }
            pollfd item{fd_, POLLOUT, 0};
            const int polled = poll(&item, 1, 20);
            if (polled < 0 && errno == EINTR) continue;
            if (polled < 0) fail("Poll USB serial device");
            if (!polled) continue;
            if (item.revents & (POLLERR | POLLHUP | POLLNVAL)) throw std::runtime_error("USB device disconnected.");
            const auto count = ::write(fd_, bytes.data() + offset, std::min<size_t>(4096, bytes.size() - offset));
            if (count < 0 && (errno == EAGAIN || errno == EINTR)) continue;
            if (count <= 0) fail("USB write");
            offset += static_cast<size_t>(count);
        }
    }
private:
    int fd_ = -1;
};
} // namespace

std::unique_ptr<Connection> open_serial(const OutputConfig &config) { return std::make_unique<Serial>(config); }
} // namespace obs2led
