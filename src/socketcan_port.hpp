#ifndef SOCKETCAN_PORT_HPP_
#define SOCKETCAN_PORT_HPP_

#include <linux/can.h>

#include <string>

namespace zlgcan_bridge {

// SocketCAN(CAN_RAW) 端口封装：绑定本地 vcan/can 接口，与 FSD / hil_test 互通
class SocketCanPort {
public:
    SocketCanPort() = default;
    ~SocketCanPort();
    SocketCanPort(const SocketCanPort&) = delete;
    SocketCanPort& operator=(const SocketCanPort&) = delete;

    // 创建 CAN_RAW socket 并绑定接口；失败返回 false（err 输出原因）
    bool open(const std::string& ifname, std::string* err);
    void close();
    bool isOpen() const { return fd_ >= 0; }
    const std::string& ifname() const { return ifname_; }
    int fd() const { return fd_; }

    // 读取一帧；timeout_ms < 0 表示永久阻塞
    // 返回：1=读到一帧；0=超时；-1=错误
    int recv(struct can_frame& frame, int timeout_ms);

    // 非阻塞读取一帧；返回：1=读到；0=暂无数据；-1=错误
    int recvNonblock(struct can_frame& frame);

    // 发送一帧（阻塞式 write，内部对 EINTR/ENOBUFS 做短暂重试）
    // 返回：true=成功；false=失败
    bool send(const struct can_frame& frame);

private:
    int fd_ = -1;
    std::string ifname_;
};

}  // namespace zlgcan_bridge

#endif  // SOCKETCAN_PORT_HPP_
