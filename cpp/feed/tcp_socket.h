#pragma once
#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <netinet/in.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

// Minimal real TCP socket wrappers for the snapshot-recovery exchange (see
// cpp/feed/snapshot.h). Real Nasdaq systems use a separate, connection-
// oriented, TCP-based protocol (SoupBinTCP, carrying the GLIMPSE snapshot
// service) for exactly this purpose — distinct from MoldUDP64's UDP
// multicast live feed plus point-to-point unicast gap recovery
// (udp_multicast.h). UDP multicast is the right choice for a feed many
// subscribers all need identically, where losing one old packet doesn't
// matter once a retransmission fills it in; it's the wrong choice for
// "send me your entire current book state," which needs reliable,
// ordered, arbitrarily-sized delivery — exactly what TCP gives for free
// and UDP doesn't. This repo doesn't implement literal SoupBinTCP framing
// (no login sequence, no message-type byte prefix) — just a real TCP
// connection for a length-prefixed request/response exchange,
// GLIMPSE-inspired rather than a byte-for-byte protocol reimplementation
// (same honesty pattern as market_data.h's outbound feed: real transport
// choice, simplified wire format).

namespace netfeed {

class TcpSocket {
public:
    explicit TcpSocket(int fd = -1) : fd_(fd) {}
    ~TcpSocket() {
        if (fd_ >= 0) ::close(fd_);
    }
    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;
    TcpSocket(TcpSocket&& o) noexcept : fd_(o.fd_) { o.fd_ = -1; }
    TcpSocket& operator=(TcpSocket&& o) noexcept {
        if (this != &o) {
            if (fd_ >= 0) ::close(fd_);
            fd_ = o.fd_;
            o.fd_ = -1;
        }
        return *this;
    }

    int fd() const { return fd_; }

    // Sends the exact byte count, looping over short writes — a TCP
    // stream socket has no message boundaries, unlike UDP's sendto().
    void send_all(const std::vector<uint8_t>& data) {
        size_t sent = 0;
        while (sent < data.size()) {
            const ssize_t n = ::send(fd_, data.data() + sent, data.size() - sent, 0);
            if (n <= 0) throw std::runtime_error(std::string("TcpSocket::send_all failed: ") + std::strerror(errno));
            sent += static_cast<size_t>(n);
        }
    }

    // Reads exactly `len` bytes, looping over short reads/partial TCP
    // segments. Throws if the peer closes before delivering `len` bytes.
    std::vector<uint8_t> recv_exact(size_t len) {
        std::vector<uint8_t> buf(len);
        size_t got = 0;
        while (got < len) {
            const ssize_t n = ::recv(fd_, buf.data() + got, len - got, 0);
            if (n <= 0) {
                throw std::runtime_error("TcpSocket::recv_exact: peer closed or error before " +
                                          std::to_string(len) + " bytes arrived (" + std::to_string(got) +
                                          " received)");
            }
            got += static_cast<size_t>(n);
        }
        return buf;
    }

protected:
    int fd_ = -1;
};

class TcpListener : public TcpSocket {
public:
    explicit TcpListener(uint16_t port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) throw std::runtime_error(std::string("socket() failed: ") + std::strerror(errno));
        int reuse = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            throw std::runtime_error(std::string("TcpListener: bind failed: ") + std::strerror(errno));
        }
        if (::listen(fd_, 4) < 0) {
            throw std::runtime_error(std::string("TcpListener: listen failed: ") + std::strerror(errno));
        }
    }

    // Blocking accept of one real TCP connection.
    TcpSocket accept_one() {
        const int cfd = ::accept(fd_, nullptr, nullptr);
        if (cfd < 0) throw std::runtime_error(std::string("TcpListener: accept failed: ") + std::strerror(errno));
        return TcpSocket(cfd);
    }
};

// Connects to a real TCP server — the snapshot client's side.
inline TcpSocket tcp_connect(const std::string& ip, uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) throw std::runtime_error(std::string("socket() failed: ") + std::strerror(errno));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        throw std::runtime_error("tcp_connect: bad IP " + ip);
    }
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        throw std::runtime_error(std::string("tcp_connect: connect failed: ") + std::strerror(errno));
    }
    return TcpSocket(fd);
}

} // namespace netfeed
