#include "socketcan_port.hpp"

#include <errno.h>
#include <net/if.h>
#include <poll.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace zlgcan_bridge {

SocketCanPort::~SocketCanPort() { close(); }

bool SocketCanPort::open(const std::string& ifname, std::string* err) {
    close();
    fd_ = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (fd_ < 0) {
        if (err) *err = "socket(PF_CAN, SOCK_RAW, CAN_RAW) 失败: " + std::string(strerror(errno));
        return false;
    }

    ifreq ifr{};
    strncpy(ifr.ifr_name, ifname.c_str(), IFNAMSIZ - 1);
    if (ioctl(fd_, SIOCGIFINDEX, &ifr) < 0) {
        if (err)
            *err = "接口 " + ifname + " 不存在（ioctl SIOCGIFINDEX 失败: " + strerror(errno) +
                   "）；请先创建/启动接口（见 scripts/start_bridge.sh）";
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    sockaddr_can addr{};
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;
    if (bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        if (err) *err = "bind(" + ifname + ") 失败: " + strerror(errno);
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    ifname_ = ifname;
    return true;
}

void SocketCanPort::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    ifname_.clear();
}

int SocketCanPort::recv(struct can_frame& frame, int timeout_ms) {
    if (fd_ < 0) return -1;
    pollfd pfd{fd_, POLLIN, 0};
    int pr = poll(&pfd, 1, timeout_ms);
    if (pr < 0) return (errno == EINTR) ? 0 : -1;
    if (pr == 0) return 0;
    ssize_t n = ::read(fd_, &frame, sizeof(frame));
    if (n < 0) return (errno == EAGAIN || errno == EINTR) ? 0 : -1;
    return (n == sizeof(frame)) ? 1 : -1;
}

int SocketCanPort::recvNonblock(struct can_frame& frame) {
    if (fd_ < 0) return -1;
    ssize_t n = ::read(fd_, &frame, sizeof(frame));
    if (n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
    return (n == sizeof(frame)) ? 1 : -1;
}

bool SocketCanPort::send(const struct can_frame& frame) {
    if (fd_ < 0) return false;
    // vcan 上一般不会 ENOBUFS，真实 can 接口满载时可能出现：短暂重试
    for (int retry = 0; retry < 3; ++retry) {
        ssize_t n = ::write(fd_, &frame, sizeof(frame));
        if (n == sizeof(frame)) return true;
        if (n < 0 && (errno == EINTR || errno == ENOBUFS)) {
            usleep(1000);
            continue;
        }
        return false;
    }
    return false;
}

}  // namespace zlgcan_bridge
