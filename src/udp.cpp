#include "output.hpp"

#include <cstring>
#include <stdexcept>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace obs2led {
namespace {
#ifdef _WIN32
struct Winsock {
    Winsock() { WSADATA data{}; if (WSAStartup(MAKEWORD(2, 2), &data)) throw std::runtime_error("WSAStartup failed"); }
    ~Winsock() { WSACleanup(); }
};
using Socket = SOCKET;
constexpr Socket invalid_socket = INVALID_SOCKET;
void close_socket(Socket socket) { closesocket(socket); }
int socket_error() { return WSAGetLastError(); }
#else
using Socket = int;
constexpr Socket invalid_socket = -1;
void close_socket(Socket socket) { close(socket); }
int socket_error() { return errno; }
#endif

class Udp final : public Connection {
public:
    explicit Udp(const OutputConfig &config)
    {
        sockaddr_storage address{};
        auto *v4 = reinterpret_cast<sockaddr_in *>(&address);
        auto *v6 = reinterpret_cast<sockaddr_in6 *>(&address);
        int length;
        if (inet_pton(AF_INET, config.address.c_str(), &v4->sin_addr) == 1) {
            v4->sin_family = AF_INET; v4->sin_port = htons(static_cast<uint16_t>(config.port));
            length = sizeof(sockaddr_in);
        } else {
            v6->sin6_family = AF_INET6; v6->sin6_port = htons(static_cast<uint16_t>(config.port));
            if (inet_pton(AF_INET6, config.address.c_str(), &v6->sin6_addr) != 1)
                throw std::runtime_error("Invalid UDP address");
            length = sizeof(sockaddr_in6);
        }
        socket_ = socket(address.ss_family, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_ == invalid_socket) fail("UDP socket");
        try {
            // One raw frame is one datagram. Permit IP fragmentation for frames
            // above the route MTU; the receiver must support IP reassembly.
#if defined(__linux__)
            const int discovery = IP_PMTUDISC_DONT;
            const int level = address.ss_family == AF_INET ? IPPROTO_IP : IPPROTO_IPV6;
            const int option = address.ss_family == AF_INET ? IP_MTU_DISCOVER : IPV6_MTU_DISCOVER;
            if (setsockopt(socket_, level, option, &discovery, sizeof(discovery))) fail("UDP fragmentation mode");
#elif defined(_WIN32)
            const DWORD dont_fragment = 0;
            const int level = address.ss_family == AF_INET ? IPPROTO_IP : IPPROTO_IPV6;
            const int option = address.ss_family == AF_INET ? IP_DONTFRAGMENT : IPV6_DONTFRAG;
            if (setsockopt(socket_, level, option, reinterpret_cast<const char *>(&dont_fragment), sizeof(dont_fragment)))
                fail("UDP fragmentation mode");
#elif defined(__APPLE__)
            if (address.ss_family == AF_INET6) {
                const int dont_fragment = 0;
                if (setsockopt(socket_, IPPROTO_IPV6, IPV6_DONTFRAG, &dont_fragment, sizeof(dont_fragment)))
                    fail("UDP fragmentation mode");
            }
#endif
#ifdef _WIN32
            u_long nonblocking = 1;
            if (ioctlsocket(socket_, FIONBIO, &nonblocking)) fail("UDP nonblocking mode");
#else
            if (fcntl(socket_, F_SETFL, O_NONBLOCK) < 0) fail("UDP nonblocking mode");
            fcntl(socket_, F_SETFD, FD_CLOEXEC);
#endif
            if (connect(socket_, reinterpret_cast<sockaddr *>(&address), length) != 0) fail("UDP destination");
        } catch (...) { close_socket(socket_); socket_ = invalid_socket; throw; }
    }
    ~Udp() override { if (socket_ != invalid_socket) close_socket(socket_); }
    void write(std::span<const uint8_t> bytes, const Cancelled &cancelled) override
    {
        if (cancelled()) return;
        if (bytes.size() > max_udp_payload) throw std::runtime_error("UDP frame is too large");
        const auto sent = send(socket_, reinterpret_cast<const char *>(bytes.data()), static_cast<int>(bytes.size()), 0);
        if (sent != static_cast<decltype(sent)>(bytes.size())) fail("UDP send");
    }
private:
    [[noreturn]] static void fail(const char *operation)
    { throw std::runtime_error(std::string(operation) + " failed (OS error " + std::to_string(socket_error()) + ")."); }
#ifdef _WIN32
    Winsock winsock_;
#endif
    Socket socket_ = invalid_socket;
};
} // namespace

std::unique_ptr<Connection> open_udp(const OutputConfig &config) { return std::make_unique<Udp>(config); }
} // namespace obs2led
