/*
 * main.c — ZLG USBCAN-4E-U <-> Linux SocketCAN 桥。
 *
 *  真实 VCU <==CAN电气==> ZLG盒 <==USB(libusbcan-4e)==> [本程序] <==SocketCAN==> FSD / hil_test
 *
 * 运行(需 root)：sudo ./zlg_can_bridge [--devtype 31] [--devidx 0] [--chn 0]
 *                 [--baud 500000] [--iface can0]
 *                 [--log-level info] [--log-dir log] [--log-frames on]
 *
 * 收到的报文按 ID 分类写入 <log-dir>/<本次运行时间戳>/{vcu2fsd,fsd2vcu}/0xNNN.log，
 * 同时每 2s 在控制台打印各 ID 的周期/抖动统计。详见 can_log.h。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <limits.h>
#include <pthread.h>

#include "zlg_bridge.h"

#define LOG_TAG "main"
#include "logger.h"
#include "can_log.h"

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
            can_log_write(CAN_LOG_FSD2VCU, &f);
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
            can_log_write(CAN_LOG_VCU2FSD, &f);
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
            "          [--log-level info] [--log-dir log] [--log-frames on]\n"
            "  --devtype    ZLG 设备类型: 31=USBCAN-4E-U(默认), 20=USBCAN-E-U\n"
            "  --iface      桥出的 SocketCAN 接口名(默认 can0, 须先由 start_zlg_bridge.sh 建好)\n"
            "  --log-level  控制台日志级别: debug|info|warn|error (默认 info)\n"
            "  --log-dir    报文日志根目录(默认: 可执行文件同级的 log/)\n"
            "  --log-frames 是否按 ID 分文件落盘收到的报文: on|off (默认 on)\n",
            p);
}

/* 默认日志目录 = 可执行文件所在目录下的 log/，与从哪个工作目录启动无关 */
static void default_log_dir(char *buf, size_t n)
{
    ssize_t k = readlink("/proc/self/exe", buf, n - 1);
    if (k <= 0) {
        snprintf(buf, n, "log");
        return;
    }
    buf[k] = '\0';

    char *slash = strrchr(buf, '/');
    if (slash)
        *slash = '\0';

    size_t len = strlen(buf);
    snprintf(buf + len, n - len, "/log");
}

int main(int argc, char **argv)
{
    unsigned devtype = 31, devidx = 0, chn = 0;
    const char *baud = "500000";
    const char *iface = "can0";
    int log_level = LOG_LV_INFO;
    int log_frames = 1;
    char log_dir[PATH_MAX] = "";

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--devtype") && i + 1 < argc) {
            devtype = (unsigned)atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--devidx") && i + 1 < argc) {
            devidx = (unsigned)atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--chn") && i + 1 < argc) {
            chn = (unsigned)atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--baud") && i + 1 < argc) {
            baud = argv[++i];
        } else if (!strcmp(argv[i], "--iface") && i + 1 < argc) {
            iface = argv[++i];
        } else if (!strcmp(argv[i], "--log-dir") && i + 1 < argc) {
            snprintf(log_dir, sizeof log_dir, "%s", argv[++i]);
        } else if (!strcmp(argv[i], "--log-level") && i + 1 < argc) {
            log_level = logger_level_from_str(argv[++i]);
            if (log_level < 0) {
                fprintf(stderr, "未知日志级别: %s\n", argv[i]);
                usage(argv[0]);
                return 2;
            }
        } else if (!strcmp(argv[i], "--log-frames") && i + 1 < argc) {
            const char *v = argv[++i];
            if (!strcmp(v, "on"))       log_frames = 1;
            else if (!strcmp(v, "off")) log_frames = 0;
            else {
                fprintf(stderr, "--log-frames 只接受 on 或 off\n");
                usage(argv[0]);
                return 2;
            }
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    if (log_dir[0] == '\0')
        default_log_dir(log_dir, sizeof log_dir);

    logger_init(log_level);

    if (getuid() != 0) {
        LOG_ERR("需要 root：请用 sudo 运行（打开/绑定 CAN_RAW 套接字需要权限）");
        return 2;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (zlg_bridge_open(devtype, devidx, chn, baud) < 0)
        return 1;
    if (can_side_open(iface) < 0) {
        LOG_ERR("打不开接口 %s：确认已创建并 up（sudo ip link add %s type vcan && sudo ip link set %s up）",
                iface, iface, iface);
        zlg_bridge_close();
        return 1;
    }

    can_log_open(log_dir, log_frames);

    LOG_INFO("就绪: ZLG(type=%u idx=%u chn=%u @%s bps) <-> SocketCAN %s",
             devtype, devidx, chn, baud, iface);
    LOG_INFO("运行中... Ctrl-C 退出。FSD/hil_test 把 can_device/HIL_INTERFACE 指向 %s 即可", iface);

    pthread_t t1, t2;
    pthread_create(&t1, NULL, t_can0_to_zlg, NULL);
    pthread_create(&t2, NULL, t_zlg_to_can0, NULL);

    while (!g_stop) {
        sleep(2);
        LOG_INFO("FSD->VCU:%llu  VCU->FSD:%llu  (err %llu/%llu)  收到帧:%llu/%llu",
                 gs.a2z, gs.z2a, gs.e_a2z, gs.e_z2a,
                 can_log_count(CAN_LOG_FSD2VCU), can_log_count(CAN_LOG_VCU2FSD));
        can_log_report();
    }

    g_stop = 1;
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    can_log_report();   /* 退出前再打一份最终统计 */
    can_log_close();
    can_side_close();
    zlg_bridge_close();
    LOG_INFO("已退出");
    return 0;
}
