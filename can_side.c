/*
 * can_side.c — Linux SocketCAN 侧封装。
 * 只 include <linux/can.h> 等系统头，避免与 <zlgcan/zlgcan.h> 的 struct can_frame 冲突。
 *
 * 防回环关键点：本模块只维持“一个”CAN_RAW socket 供收发两方向共用，
 * SocketCAN 默认 CAN_RAW_RECV_OWN_MSGS=0（内核不把本 socket 自己发出的帧再投递回来），
 * 因此 can_side_tx() 注入到 can0 的帧不会被 can_side_rx() 收回来转发回 ZLG，不会死循环。
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <net/if.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include "zlg_bridge.h"

static int g_sock = -1;
static struct sockaddr_can g_addr;

int can_side_open(const char *ifname)
{
    if (g_sock >= 0)
        return -1;

    int s = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (s < 0) {
        perror("[can] socket(PF_CAN)");
        return -1;
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof ifr);
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    if (ioctl(s, SIOCGIFINDEX, &ifr) < 0) {
        perror("[can] ioctl(SIOCGIFINDEX)");
        close(s);
        return -1;
    }

    memset(&g_addr, 0, sizeof g_addr);
    g_addr.can_family = AF_CAN;
    g_addr.can_ifindex = ifr.ifr_ifindex;
    if (bind(s, (struct sockaddr *)&g_addr, sizeof g_addr) < 0) {
        perror("[can] bind");
        close(s);
        return -1;
    }

    /* 收包超时 200ms，让线程能周期醒来检查退出标志 */
    struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    g_sock = s;
    return 0;
}

void can_side_close(void)
{
    if (g_sock >= 0)
        close(g_sock);
    g_sock = -1;
}

int can_side_rx(bridge_frame_t *f)
{
    if (g_sock < 0)
        return -1;
    struct can_frame cf;
    ssize_t n = recv(g_sock, &cf, sizeof cf, 0);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return 0; /* 超时无帧 */
        return -1;
    }
    if (n != (ssize_t)sizeof(struct can_frame))
        return -1;
    f->id = cf.can_id;
    f->dlc = cf.can_dlc > 8 ? 8 : cf.can_dlc;
    memcpy(f->data, cf.data, f->dlc);
    return 1;
}

int can_side_tx(const bridge_frame_t *f)
{
    if (g_sock < 0)
        return -1;
    struct can_frame cf;
    memset(&cf, 0, sizeof cf);
    cf.can_id = f->id;
    cf.can_dlc = f->dlc;
    memcpy(cf.data, f->data, f->dlc);
    ssize_t n = sendto(g_sock, &cf, sizeof cf, 0,
                       (struct sockaddr *)&g_addr, sizeof g_addr);
    return n == (ssize_t)sizeof cf ? 0 : -1;
}
