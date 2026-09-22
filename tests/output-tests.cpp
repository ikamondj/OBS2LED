#include "output.hpp"
#include "../examples/usb-receiver/rgb-receiver.hpp"

#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using Socket = SOCKET;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
using Socket = int;
#endif

using namespace obs2led;
using namespace std::chrono_literals;

static void require(bool value, const char *message)
{ if (!value) throw std::runtime_error(message); }

class Receiver {
public:
    Receiver()
    {
        socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        require(socket_ != static_cast<Socket>(-1), "receiver socket");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        require(bind(socket_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0, "receiver bind");
#ifdef _WIN32
        int size = sizeof(address);
#else
        socklen_t size = sizeof(address);
#endif
        require(getsockname(socket_, reinterpret_cast<sockaddr *>(&address), &size) == 0, "receiver port");
        port = ntohs(address.sin_port);
    }
    ~Receiver()
    {
#ifdef _WIN32
        closesocket(socket_);
#else
        close(socket_);
#endif
    }
    std::vector<uint8_t> receive(int milliseconds = 1000)
    {
        fd_set set;
        FD_ZERO(&set); FD_SET(socket_, &set);
        timeval wait{milliseconds / 1000, (milliseconds % 1000) * 1000};
        if (select(static_cast<int>(socket_ + 1), &set, nullptr, nullptr, &wait) <= 0) return {};
        std::vector<uint8_t> bytes(65536);
        const auto size = recv(socket_, reinterpret_cast<char *>(bytes.data()), static_cast<int>(bytes.size()), 0);
        require(size >= 0, "receiver read");
        bytes.resize(static_cast<size_t>(size));
        return bytes;
    }
    int port;
private:
    Socket socket_;
};

static void wait_ready(Output &output)
{
    const auto until = std::chrono::steady_clock::now() + 2s;
    while (!output.ready() && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(5ms);
    require(output.ready(), output.status().c_str());
}

static void validation_and_pixels()
{
    OutputConfig c;
    require(!validate(c).empty(), "empty destination must not send");
    c.address = "127.0.0.1";
    require(validate(c).empty(), "IPv4 destination");
    c.address = "::1";
    require(validate(c).empty(), "IPv6 destination");
    for (const auto *address : {"localhost", "udp://1.2.3.4", "999.0.0.1", "0.0.0.0", "::"}) {
        c.address = address;
        require(!validate(c).empty(), "invalid destination rejected");
    }
    c.address = "127.0.0.1";
    c.width = 256; c.height = 256;
    require(!validate(c).empty(), "oversize UDP rejected");
    c.transport = Transport::usb; c.device = "COM1";
    require(validate(c).empty(), "USB accepts frames larger than UDP limit");
    c.width = 0;
    require(!validate(c).empty(), "zero dimensions rejected");
    c.width = 64; c.fps = 0;
    require(!validate(c).empty(), "zero FPS rejected");

    const std::array<uint8_t, 24> padded{255,0,0,255, 0,255,0,255, 99,99,99,99,
                                       0,0,255,255, 1,2,3,128, 99,99,99,99};
    require(rgb_from_rgba(padded.data(), 12, 2, 2) ==
        std::vector<uint8_t>({255,0,0, 0,255,0, 0,0,255, 1,2,3}), "RGB byte order, top-down rows, stride, and alpha");
    bool threw = false;
    try { rgb_from_rgba(padded.data(), 4, 2, 2); } catch (const std::invalid_argument &) { threw = true; }
    require(threw, "short readback stride rejected");
    const std::array<uint8_t, 9> check{'1','2','3','4','5','6','7','8','9'};
    require(crc32(check) == 0xcbf43926, "standard CRC32 test vector");
    const std::array<uint8_t, 6> rgb{10,20,30,40,50,60};
    auto packet = usb_packet(rgb, 2, 1, 0x12345678);
    require(packet.size() == 30 && std::memcmp(packet.data(), "O2LF", 4) == 0, "USB packet envelope");
    const std::array<uint8_t, 16> header{1,0,24,0,2,0,1,0,0x78,0x56,0x34,0x12,6,0,0,0};
    require(std::equal(header.begin(), header.end(), packet.begin() + 4), "little-endian USB header");
    require(std::equal(rgb.begin(), rgb.end(), packet.begin() + 24), "uncompressed USB RGB payload");
    RgbReceiver<6144> decoder;
    int decoded = 0;
    auto got_frame = [&](uint32_t width, uint32_t height, uint32_t sequence, const uint8_t *pixels) {
        require(width == 2 && height == 1 && sequence == 0x12345678, "firmware receives dimensions and sequence");
        require(std::equal(rgb.begin(), rgb.end(), pixels), "firmware receives RGB bytes");
        ++decoded;
    };
    for (auto b : {uint8_t(7), uint8_t('O'), uint8_t('2'), uint8_t('O')}) decoder.feed(b, got_frame);
    for (auto b : packet) decoder.feed(b, got_frame);
    require(decoded == 1, "stream synchronization after noise");
    packet.back() ^= 1;
    for (auto b : packet) decoder.feed(b, got_frame);
    require(decoded == 1, "corrupt payload rejected by firmware CRC");
    packet.back() ^= 1;
    auto bad_header = packet;
    bad_header[16] = 255;
    for (auto b : bad_header) decoder.feed(b, got_frame);
    for (auto b : packet) decoder.feed(b, got_frame);
    require(decoded == 2, "firmware recovers from malformed header");
}

static void udp_and_worker()
{
    Receiver first, second;
    OutputConfig c;
    c.address = "127.0.0.1"; c.port = first.port;
    std::vector<uint8_t> frame(64 * 32 * 3);
    for (size_t i = 0; i < frame.size(); ++i) frame[i] = static_cast<uint8_t>(i);
    auto udp = open_udp(c);
    udp->write(frame, [] { return false; });
    require(first.receive() == frame, "one complete raw RGB frame per datagram");
    udp->write(frame, [] { return true; });
    require(first.receive(30).empty(), "cancelled UDP sends nothing");

    Output output;
    const auto old = output.configure(c);
    wait_ready(output);
    output.submit(frame, old);
    require(first.receive() == frame, "background sender transmits");
    c.port = second.port;
    const auto current = output.configure(c);
    require(current != old, "new configuration generation");
    wait_ready(output);
    output.submit(frame, old);
    require(second.receive(60).empty(), "stale configuration frames dropped");
    output.submit({1,2,3}, current);
    require(second.receive(60).empty(), "incomplete frame dropped");
    output.submit(frame, current);
    require(second.receive() == frame && first.receive(30).empty(), "destination switched without leaking old frames");
    output.set_active(false);
    output.submit(frame, current);
    require(second.receive(60).empty(), "disabled filter stops output");
    require(!output.ready(), "disabled output releases its connection");
    output.set_active(true);
    wait_ready(output);
    output.submit(frame, current);
    require(second.receive() == frame, "re-enabled output resumes");
    c.address = "invalid";
    const auto invalid = output.configure(c);
    output.submit(frame, invalid);
    require(!output.ready() && second.receive(60).empty(), "invalid settings stop existing output");
}

#if defined(__linux__)
static void serial_pty()
{
    const int master = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
    require(master >= 0 && grantpt(master) == 0 && unlockpt(master) == 0, "create pseudo-terminal");
    OutputConfig c;
    c.transport = Transport::usb; c.device = ptsname(master);
    auto serial = open_serial(c);
    const std::array<uint8_t, 6> rgb{0,10,13,255,128,42};
    const auto bytes = usb_packet(rgb, 2, 1, 123);
    serial->write(bytes, [] { return false; });
    std::vector<uint8_t> received(bytes.size());
    size_t offset = 0;
    const auto until = std::chrono::steady_clock::now() + 1s;
    while (offset < received.size() && std::chrono::steady_clock::now() < until) {
        const auto count = read(master, received.data() + offset, received.size() - offset);
        if (count > 0) offset += static_cast<size_t>(count);
        else std::this_thread::sleep_for(2ms);
    }
    require(received == bytes, "serial raw mode preserves every header/payload byte");
    close(master);
    bool failed = false;
    try { serial->write(bytes, [] { return false; }); } catch (const std::exception &) { failed = true; }
    require(failed, "serial disconnect detected");
}
#endif

int main()
{
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data)) return 1;
#endif
    try {
        validation_and_pixels();
        udp_and_worker();
#if defined(__linux__)
        serial_pty();
#endif
        (void)serial_devices(); // Discovery must also work with no supported devices attached.
        std::cout << "PASS: validation, RGB packing, USB framing, UDP loopback, reconfiguration and lifecycle\n";
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
#ifdef _WIN32
    WSACleanup();
#endif
}
