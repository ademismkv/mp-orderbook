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

// Real POSIX UDP multicast sender/receiver, plus a plain unicast socket
// for the Request/Downstream retransmission exchange — no abstraction
// beyond what's needed to exercise actual socket code for ROADMAP.md's
// "receive UDP multicast" item. There's no live exchange feed to
// subscribe to here, so loopback is the honest stand-in: real socket() /
// sendto() / recvfrom() / IP_ADD_MEMBERSHIP calls, addressed to a
// multicast group reachable over 127.0.0.1 instead of a real NIC.

namespace netfeed {

class UdpSocket {
public:
    UdpSocket() {
        fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd_ < 0) throw std::runtime_error(std::string("socket() failed: ") + std::strerror(errno));
    }
    ~UdpSocket() {
        if (fd_ >= 0) ::close(fd_);
    }
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;
    UdpSocket(UdpSocket&& o) noexcept : fd_(o.fd_) { o.fd_ = -1; }

    int fd() const { return fd_; }

protected:
    int fd_ = -1;
};

// Publishes UDP datagrams to a multicast group — stands in for the real
// exchange's feed publisher.
class McastSender : public UdpSocket {
public:
    McastSender(const std::string& group_ip, uint16_t port) {
        std::memset(&addr_, 0, sizeof(addr_));
        addr_.sin_family = AF_INET;
        addr_.sin_port = htons(port);
        if (::inet_pton(AF_INET, group_ip.c_str(), &addr_.sin_addr) != 1) {
            throw std::runtime_error("McastSender: bad group IP " + group_ip);
        }
        // Loop back to this host's own subscribers — required for a
        // same-machine sender/receiver to see each other at all.
        unsigned char loop = 1;
        ::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));

        // Force the outgoing interface to loopback explicitly rather than
        // let the kernel consult a routing table — sandboxes/containers
        // with no default route (only `lo`, no gateway) can't resolve a
        // multicast destination to an interface otherwise and fail with
        // ENODEV. Real deployments would bind this to the actual NIC.
        in_addr iface{};
        ::inet_pton(AF_INET, "127.0.0.1", &iface);
        ::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_IF, &iface, sizeof(iface));
    }

    void send(const std::vector<uint8_t>& payload) {
        const ssize_t n = ::sendto(fd_, payload.data(), payload.size(), 0,
                                    reinterpret_cast<const sockaddr*>(&addr_), sizeof(addr_));
        if (n < 0 || static_cast<size_t>(n) != payload.size()) {
            throw std::runtime_error(std::string("McastSender::send failed: ") + std::strerror(errno));
        }
    }

private:
    sockaddr_in addr_{};
};

// Subscribes to a multicast group and receives datagrams — the real Feed
// Handler receive path.
class McastReceiver : public UdpSocket {
public:
    McastReceiver(const std::string& group_ip, uint16_t port) {
        int reuse = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        sockaddr_in bind_addr{};
        bind_addr.sin_family = AF_INET;
        bind_addr.sin_port = htons(port);
        bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)) < 0) {
            throw std::runtime_error(std::string("McastReceiver: bind failed: ") + std::strerror(errno));
        }

        ip_mreq mreq{};
        if (::inet_pton(AF_INET, group_ip.c_str(), &mreq.imr_multiaddr) != 1) {
            throw std::runtime_error("McastReceiver: bad group IP " + group_ip);
        }
        // Explicit loopback interface, not INADDR_ANY — see McastSender's
        // IP_MULTICAST_IF comment for why (no default route in this
        // environment for the kernel to resolve INADDR_ANY against).
        ::inet_pton(AF_INET, "127.0.0.1", &mreq.imr_interface);
        if (::setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
            throw std::runtime_error(std::string("McastReceiver: IP_ADD_MEMBERSHIP failed: ") +
                                      std::strerror(errno));
        }
    }

    // Blocking receive of one datagram. Empty vector means the underlying
    // recvfrom() failed (e.g. socket closed out from under it).
    std::vector<uint8_t> receive(size_t max_len = 65536) {
        std::vector<uint8_t> buf(max_len);
        const ssize_t n = ::recvfrom(fd_, buf.data(), buf.size(), 0, nullptr, nullptr);
        if (n < 0) return {};
        buf.resize(static_cast<size_t>(n));
        return buf;
    }
};

// Plain unicast UDP socket, used for the Request/Downstream retransmission
// exchange — MoldUDP64's actual recovery path. A receiver that detects a
// gap sends a Request Packet HERE, to a re-request server, not to the
// multicast group.
class UnicastSocket : public UdpSocket {
public:
    explicit UnicastSocket(uint16_t bind_port = 0) {
        if (bind_port != 0) {
            int reuse = 1;
            ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(bind_port);
            addr.sin_addr.s_addr = htonl(INADDR_ANY);
            if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
                throw std::runtime_error(std::string("UnicastSocket: bind failed: ") + std::strerror(errno));
            }
        }
    }

    void send_to(const std::string& ip, uint16_t port, const std::vector<uint8_t>& payload) {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        ::inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);
        ::sendto(fd_, payload.data(), payload.size(), 0, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    }

    // Receives one datagram and reports the sender's address — how a
    // re-request server knows where to unicast its reply.
    std::vector<uint8_t> receive_from(std::string& out_ip, uint16_t& out_port, size_t max_len = 65536) {
        std::vector<uint8_t> buf(max_len);
        sockaddr_in from{};
        socklen_t fromlen = sizeof(from);
        const ssize_t n =
            ::recvfrom(fd_, buf.data(), buf.size(), 0, reinterpret_cast<sockaddr*>(&from), &fromlen);
        if (n < 0) return {};
        buf.resize(static_cast<size_t>(n));
        char ipbuf[INET_ADDRSTRLEN];
        ::inet_ntop(AF_INET, &from.sin_addr, ipbuf, sizeof(ipbuf));
        out_ip = ipbuf;
        out_port = ntohs(from.sin_port);
        return buf;
    }
};

} // namespace netfeed
