/*
 * main.c — ZLG USBCAN-4E-U <-> Linux SocketCAN 桥。
 *
 *  真实 VCU <==CAN电气==> ZLG盒 <==USB(libusbcan-4e)==> [本程序] <==SocketCAN==> FSD / hil_test
 *
 * 运行(需 root)：sudo ./zlg_can_bridge [--devtype 31] [--devidx 0] [--chn 0]
 *                 [--baud 500000] [--iface can0]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include "zlg_bridge.h"

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int s)
{
    (void)s;
    g_stop = 1;
}

typedef struct {
    unsigned long long a2z;   /* can0 -> ZLG(发往 VCU) 帧数 */
    unsigned long long z2a;   /* ZLG(来自 VCU) -> can0 帧数 */
    unsigned long long e_a2z;
    unsigned long long e_z2a;
} STATS;
static STATS gs;

static void *t_can0_to_zlg(void *arg)
{
    (void)arg;
    while (!g_stop) {
        bridge_frame_t f;
        int r = can_side_rx(&f);
        if (r == 1) {
            if (zlg_bridge_tx(&f) == 0)
                gs.a2z++;
            else
                gs.e_a2z++;
        } else if (r < 0) {
            gs.e_a2z++;
            usleep(2000);
        }
    }
    return NULL;
}

static void *t_zlg_to_can0(void *arg)
{
    (void)arg;
    while (!g_stop) {
        bridge_frame_t f;
        int r = zlg_bridge_rx(&f, 100); /* 每次最多阻塞 100ms 攒批 */
        if (r > 0) {
            if (can_side_tx(&f) == 0)
                gs.z2a++;
            else
                gs.e_z2a++;
        } else if (r < 0) {
            gs.e_z2a++;
            usleep(2000);
        }
    }
    return NULL;
}

static void usage(const char *p)
{
    fprintf(stderr,
            "用法: %s [--devtype 31] [--devidx 0] [--chn 0] [--baud 500000] [--iface can0]\n"
            "  --devtype   ZLG 设备类型: 31=USBCAN-4E-U(默认), 20=USBCAN-E-U\n"
            "  --iface     桥出的 SocketCAN 接口名(默认 can0, 须先由 start_zlg_bridge.sh 建好)\n",
            p);
}

int main(int argc, char **argv)
{
    unsigned devtype = 31, devidx = 0, chn = 0;
    const char *baud = "500000";
    const char *iface = "can0";

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--devtype") && i + 1 < argc)      devtype = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--devidx") && i + 1 < argc)  devidx  = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--chn") && i + 1 < argc)     chn     = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--baud") && i + 1 < argc)    baud    = argv[++i];
        else if (!strcmp(argv[i], "--iface") && i + 1 < argc)   iface   = argv[++i];
        else { usage(argv[0]); return 2; }
    }

    if (getuid() != 0) {
        fprintf(stderr, "需要 root：请用 sudo 运行（打开/绑定 CAN_RAW 套接字需要权限）\n");
        return 2;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (zlg_bridge_open(devtype, devidx, chn, baud) < 0)
        return 1;
    if (can_side_open(iface) < 0) {
        fprintf(stderr, "[main] 打不开接口 %s：确认已创建并 up（sudo ip link add %s type vcan && sudo ip link set %s up）\n",
                iface, iface, iface);
        zlg_bridge_close();
        return 1;
    }

    printf("[bridge] 就绪: ZLG(type=%u idx=%u chn=%u @%s bps) <-> SocketCAN %s\n",
           devtype, devidx, chn, baud, iface);
    printf("[bridge] 运行中... Ctrl-C 退出。FSD/hil_test 把 can_device/HIL_INTERFACE 指向 %s 即可\n",
           iface);
    fflush(stdout);

    pthread_t t1, t2;
    pthread_create(&t1, NULL, t_can0_to_zlg, NULL);
    pthread_create(&t2, NULL, t_zlg_to_can0, NULL);

    while (!g_stop) {
        sleep(2);
        printf("[bridge] FSD->VCU:%llu  VCU->FSD:%llu  (err %llu/%llu)\n",
               gs.a2z, gs.z2a, gs.e_a2z, gs.e_z2a);
        fflush(stdout);
    }

    g_stop = 1;
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    can_side_close();
    zlg_bridge_close();
    printf("[bridge] 已退出\n");
    return 0;
}
