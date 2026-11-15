/*
 * zlg_side.c — ZLG USBCAN-4E-U (libusbcan-4e) 侧封装。
 * 只 include <zlgcan/zlgcan.h>，避免与 <linux/can.h> 的 struct can_frame 冲突。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <glob.h>
#include <stdatomic.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>
#include <zlgcan/zlgcan.h>
#include "zlg_bridge.h"
#define LOG_TAG "zlg"
#include "logger.h"

static int g_receive_only = 1;
static DEVICE_HANDLE   g_dev = INVALID_DEVICE_HANDLE;
static CHANNEL_HANDLE  g_chn = INVALID_CHANNEL_HANDLE;
static int g_usb_dir = -1;
static unsigned g_usb_number;
static atomic_int g_fault;
static int g_cleanup_failed;
static int g_start_attempted;
static int g_device_lock = -1;
static unsigned long long g_e6_count;
static uint64_t g_e6_last_warning_ns;

/* Retain the original sysfs directory, not just its pathname. A re-enumerated
 * box at the same port must not make the old libusb handle appear healthy. */
#ifndef ZLG_USB_SYSFS
#define ZLG_USB_SYSFS "/sys/bus/usb/devices"
#endif
static int read_usb_number(int directory, const char *name, unsigned *value, int base)
{
    int fd = openat(directory, name, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    char text[32] = {0};
    ssize_t n = read(fd, text, sizeof text - 1);
    close(fd);
    if (n <= 0) return -1;
    char *end;
    unsigned long parsed = strtoul(text, &end, base);
    if (end == text) return -1;
    *value = (unsigned)parsed;
    return 0;
}

static int capture_usb_identity(void)
{
    glob_t paths = {0};
    if (glob(ZLG_USB_SYSFS "/*", 0, NULL, &paths) != 0) {
        globfree(&paths);
        return -1;
    }
    int matches = 0;
    for (size_t i = 0; i < paths.gl_pathc; ++i) {
        int fd = open(paths.gl_pathv[i], O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0) continue;
        unsigned vendor, product, number;
        if (read_usb_number(fd, "idVendor", &vendor, 16) == 0 && vendor == 0x0471 &&
            read_usb_number(fd, "idProduct", &product, 16) == 0 && product == 0x126a) {
            ++matches;
            if (g_usb_dir >= 0) close(g_usb_dir);
            g_usb_dir = fd;
            if (read_usb_number(fd, "devnum", &number, 10) < 0) {
                matches = -1;
                break;
            }
            g_usb_number = number;
        } else close(fd);
    }
    globfree(&paths);
    if (matches != 1) {
        if (g_usb_dir >= 0) close(g_usb_dir);
        g_usb_dir = -1;
        LOG_ERR("要求唯一 USBCAN-4E-U USB 设备，实际匹配=%d，无法可靠绑定设备身份", matches);
        return -1;
    }
    LOG_INFO("USB 连接身份已绑定: devnum=%u（重新枚举后必须重新打开驱动）", g_usb_number);
    return 0;
}

static int usb_connected(void)
{
    unsigned number;
    return g_usb_dir >= 0 &&
        read_usb_number(g_usb_dir, "devnum", &number, 10) == 0 && number == g_usb_number;
}

int zlg_bridge_check_health(void)
{
    if (atomic_load(&g_fault)) return -1;
    if (!ZLG_RUNTIME_HEALTH_CHECKS) return 0;
    if (!usb_connected()) {
        LOG_ERR("ZLG USB 断开或重新枚举，旧句柄失效；停止本次桥接，禁止继续报告发送成功");
        atomic_store(&g_fault, 1);
        return -1;
    }
    /* Installed Linux 4E library: 1=error event, 0=empty, negative=API error.
     * Its ReadChannelStatus is a stub and cannot establish transmission health. */
    for (int i = 0; i < 32; ++i) {
        ZCAN_CHANNEL_ERR_INFO error = {0};
        int result = ZCAN_ReadChannelErrInfo(g_chn, &error);
        if (result < 0) {
            LOG_ERR("驱动错误队列读取失败: ret=%d", result);
            atomic_store(&g_fault, 1);
            return -1;
        }
        if (result == 0) break;
        if (!error.error_code) continue;
        /* 4E Linux manual, p.8 table 2: E6=other CAN bus error, E5=bus-off.
         * Temporary user policy: log E6 at most once per 2 seconds and keep
         * draining events, so a subsequent fatal event is still handled.
         * These are enumerated codes, not the generic header's bit masks. */
        if (error.error_code == 0xE6) {
            ++g_e6_count;
            struct timespec now;
            int timed = clock_gettime(CLOCK_MONOTONIC, &now) == 0;
            uint64_t now_ns = timed ? (uint64_t)now.tv_sec * 1000000000ULL +
                                     (uint64_t)now.tv_nsec : 0;
            if (g_e6_count == 1 || !timed ||
                now_ns - g_e6_last_warning_ns >= 2000000000ULL) {
                LOG_WARN("CAN 总线其他错误: code=0xE6，本会话累计=%llu；按临时策略仅告警、继续收发（不代表报文已送达）",
                         g_e6_count);
                g_e6_last_warning_ns = now_ns;
            }
            continue;
        }
        LOG_ERR("CAN 驱动报告错误: code=0x%x passive=%02x/%02x/%02x arbitration=%02x；停止桥接",
                error.error_code, error.passive_ErrData[0], error.passive_ErrData[1],
                error.passive_ErrData[2], error.arLost_ErrData);
        atomic_store(&g_fault, 1);
        return -1;
    }
    return 0;
}

int zlg_bridge_open(unsigned devtype, unsigned devidx, unsigned chn, const char *baud, int receive_only)
{
    if (g_cleanup_failed) {
        LOG_ERR("此前驱动关闭失败，拒绝在同一进程重复打开，请重新启动");
        return -2;
    }
    if (g_dev != INVALID_DEVICE_HANDLE)
        return -1;
    g_receive_only = receive_only;
    atomic_store(&g_fault, 0);
    g_start_attempted = 0;
    g_e6_count = 0;
    g_e6_last_warning_ns = 0;
    /* The linked libusbcan-4e implementation only supports type 31. */
    if (devtype != ZCAN_USBCAN_4E_U || devidx != 0 || chn >= 4) {
        LOG_ERR("当前 libusbcan-4e 仅支持单个 type=31/index=0 设备及通道0..3");
        return -1;
    }
    g_device_lock = open("/run/lock/zlgcan_bridge.device.lock",
                         O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (g_device_lock < 0 || flock(g_device_lock, LOCK_EX | LOCK_NB) < 0) {
        LOG_ERR("无法独占 ZLG USB 设备，拒绝并发初始化");
        zlg_bridge_close();
        return -1;
    }
    if (capture_usb_identity() < 0) {
        zlg_bridge_close();
        return -1;
    }

    DEVICE_HANDLE dev = ZCAN_OpenDevice(devtype, devidx, 0);
    if (dev == INVALID_DEVICE_HANDLE) {
        LOG_ERR("ZCAN_OpenDevice(type=%u idx=%u) 失败：盒子未插入 / 权限不足(需 sudo) / 驱动未装",
                devtype, devidx);
        zlg_bridge_close();
        return -1;
    }

    ZCAN_DEVICE_INFO di;
    if (ZCAN_GetDeviceInf(dev, &di) == 0)
        LOG_INFO("设备: hw=%u fw=%u drv=%u sn=%s chn=%u",
                 di.hw_Version, di.fw_Version, di.dr_Version,
                 (char *)di.str_Serial_Num, di.can_Num);

    /* Linux vendor sequence: Init -> best-effort Reset -> properties -> Start.
     * The vendor sample does not gate initialization on the pre-reset result.
     * A stopped/new channel can reject that stop command; this alone is not
     * evidence that the subsequent configuration/start will fail. The actual
     * installed library returns 0 on success, unlike its generic header comment.
     * Never call GetValue on an uninitialized channel. */
    g_dev = dev;
    g_chn = ZCAN_InitCAN(dev, chn, NULL);
    IProperty *prop = NULL;
    int result;
    if (g_chn == INVALID_CHANNEL_HANDLE) {
        LOG_ERR("ZCAN_InitCAN(ch%u) 失败", chn);
        goto fail;
    }
    result = ZCAN_ResetCAN(g_chn);
    if (result < 0) {
        LOG_WARN("启动前预复位未确认: ch%u ret=%d；按厂商示例继续检查USB、配置及StartCAN结果", chn, result);
    }
    if (!usb_connected()) {
        LOG_ERR("预复位期间USB连接失效，拒绝继续初始化旧句柄");
        goto fail;
    }
    prop = GetIProperty(dev);
    char path[128];
    snprintf(path, sizeof path, "info/channel/channel_%u/baud_rate", chn);
    result = prop && prop->SetValue ? prop->SetValue(path, baud) : -1;
    if (result < 0) {
        LOG_ERR("设置 %s=%s 失败: ret=%d，拒绝使用未知波特率", path, baud, result);
        goto fail;
    }
    LOG_INFO("%s = %s (ret=%d)", path, baud, result);
    snprintf(path, sizeof path, "info/channel/channel_%u/work_mode", chn);
    result = prop->SetValue(path, "0");
    if (result < 0) {
        LOG_ERR("设置 %s=0 失败: ret=%d", path, result);
        goto fail;
    }
    ReleaseIProperty(prop);
    prop = NULL;
    g_start_attempted = 1;
    result = ZCAN_StartCAN(g_chn);
    if (result < 0) {
        LOG_ERR("ZCAN_StartCAN(ch%u) 失败: ret=%d（USB命令/通道状态异常）", chn, result);
        goto fail;
    }
    if (zlg_bridge_check_health() < 0) goto fail;
    return 0;
fail:
    if (prop) ReleaseIProperty(prop);
    zlg_bridge_close();
    return -1;
}

void zlg_bridge_close(void)
{
    if (g_chn != INVALID_CHANNEL_HANDLE && g_start_attempted) {
        int result = ZCAN_ResetCAN(g_chn);
        if (result < 0) LOG_WARN("关闭时通道复位失败: ret=%d", result);
    }
    if (g_dev != INVALID_DEVICE_HANDLE) {
        int result = ZCAN_CloseDevice(g_dev);
        if (result < 0) {
            LOG_ERR("ZCAN_CloseDevice 失败: ret=%d，禁止同进程重开", result);
            g_cleanup_failed = 1;
        }
    }
    g_chn = INVALID_CHANNEL_HANDLE;
    g_start_attempted = 0;
    g_dev = INVALID_DEVICE_HANDLE;
    if (g_usb_dir >= 0) close(g_usb_dir);
    g_usb_dir = -1;
    if (g_device_lock >= 0) close(g_device_lock);
    g_device_lock = -1;
}

int zlg_bridge_rx(bridge_frame_t *f, int timeout_ms)
{
    if (atomic_load(&g_fault) || g_chn == INVALID_CHANNEL_HANDLE)
        return -1;
    ZCAN_Receive_Data rd;
    memset(&rd, 0, sizeof rd);
    int n = ZCAN_Receive(g_chn, &rd, 1, timeout_ms);
    if (n < 0) {
        LOG_ERR("ZCAN_Receive 失败: ret=%d", n);
        atomic_store(&g_fault, 1);
    }
    if (n <= 0)
        return n; /* 0=超时, <0=错误 */
    f->id = rd.frame.can_id;
    f->dlc = rd.frame.can_dlc > 8 ? 8 : rd.frame.can_dlc;
    memcpy(f->data, rd.frame.data, f->dlc);
    return 1;
}

int zlg_bridge_tx(const bridge_frame_t *f)
{
    if (g_receive_only) return -1;
    if (atomic_load(&g_fault) || g_chn == INVALID_CHANNEL_HANDLE)
        return -1;
    if (ZLG_RUNTIME_HEALTH_CHECKS && !usb_connected()) {
        atomic_store(&g_fault, 1);
        LOG_ERR("USB 连接失效，拒绝向旧驱动句柄提交报文");
        return -1;
    }
    ZCAN_Transmit_Data td;
    memset(&td, 0, sizeof td);
    td.frame.can_id  = f->id;
    td.frame.can_dlc = f->dlc;
    memcpy(td.frame.data, f->data, f->dlc);
    td.transmit_type = 0; /* 正常发送 */
    int n = ZCAN_Transmit(g_chn, &td, 1);
    if (n != 1) {
        LOG_ERR("ZCAN_Transmit 提交失败: id=0x%x ret=%d", f->id, n);
        atomic_store(&g_fault, 1);
        return -1;
    }
    return 0; /* Accepted by driver queue; not a VCU delivery acknowledgement. */
}
