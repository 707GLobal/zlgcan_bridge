/*
 * zlg_side.c — ZLG USBCAN-4E-U (libusbcan-4e) 侧封装。
 * 只 include <zlgcan/zlgcan.h>，避免与 <linux/can.h> 的 struct can_frame 冲突。
 */
#include <stdio.h>
#include <string.h>
#include <zlgcan/zlgcan.h>
#include "zlg_bridge.h"

static DEVICE_HANDLE   g_dev = INVALID_DEVICE_HANDLE;
static CHANNEL_HANDLE  g_chn = INVALID_CHANNEL_HANDLE;

int zlg_bridge_open(unsigned devtype, unsigned devidx, unsigned chn, const char *baud)
{
    if (g_dev != INVALID_DEVICE_HANDLE)
        return -1;

    DEVICE_HANDLE dev = ZCAN_OpenDevice(devtype, devidx, 0);
    if (dev == INVALID_DEVICE_HANDLE) {
        fprintf(stderr,
                "[zlg] ZCAN_OpenDevice(type=%u idx=%u) 失败：盒子未插入 / 权限不足(需 sudo) / 驱动未装\n",
                devtype, devidx);
        return -1;
    }

    ZCAN_DEVICE_INFO di;
    if (ZCAN_GetDeviceInf(dev, &di) == 0)
        fprintf(stderr, "[zlg] 设备: hw=%u fw=%u drv=%u sn=%s chn=%u\n",
                di.hw_Version, di.fw_Version, di.dr_Version,
                (char *)di.str_Serial_Num, di.can_Num);

    /* 通过属性接口配置通道 0：波特率(协议 500k) + 正常模式。
     * 注意：仅用 SetValue，不要在此处调 GetValue——通道未初始化时 ZLG 属性接口
     * 的 GetValue 会段错误（实测）。波特率是否生效看 set 返回值即可。 */
    IProperty *prop = GetIProperty(dev);
    char path[128];
    snprintf(path, sizeof path, "info/channel/channel_%u/baud_rate", chn);
    if (!prop || prop->SetValue(path, baud) != 1)
        fprintf(stderr, "[zlg] 警告: 无法把 %s 设为 %s（若盒默认已是 %s 可忽略）\n",
                path, baud, baud);
    else
        fprintf(stderr, "[zlg] %s = %s\n", path, baud);
    snprintf(path, sizeof path, "info/channel/channel_%u/work_mode", chn);
    if (prop)
        prop->SetValue(path, "0"); /* 0=正常模式 */

    CHANNEL_HANDLE h = ZCAN_InitCAN(dev, chn, NULL);
    if (h == INVALID_CHANNEL_HANDLE) {
        fprintf(stderr, "[zlg] ZCAN_InitCAN(ch%u) 失败\n", chn);
        ZCAN_CloseDevice(dev);
        return -1;
    }
    if (ZCAN_StartCAN(h) < 0) {
        fprintf(stderr, "[zlg] ZCAN_StartCAN 失败（VCU 未上电 / 总线错误?）\n");
        ZCAN_ResetCAN(h);
        ZCAN_CloseDevice(dev);
        return -1;
    }
    g_dev = dev;
    g_chn = h;
    return 0;
}

void zlg_bridge_close(void)
{
    if (g_chn != INVALID_CHANNEL_HANDLE)
        ZCAN_ResetCAN(g_chn);
    if (g_dev != INVALID_DEVICE_HANDLE)
        ZCAN_CloseDevice(g_dev);
    g_chn = INVALID_CHANNEL_HANDLE;
    g_dev = INVALID_DEVICE_HANDLE;
}

int zlg_bridge_rx(bridge_frame_t *f, int timeout_ms)
{
    if (g_chn == INVALID_CHANNEL_HANDLE)
        return -1;
    ZCAN_Receive_Data rd;
    memset(&rd, 0, sizeof rd);
    int n = ZCAN_Receive(g_chn, &rd, 1, timeout_ms);
    if (n <= 0)
        return n; /* 0=超时, <0=错误 */
    f->id = rd.frame.can_id;
    f->dlc = rd.frame.can_dlc > 8 ? 8 : rd.frame.can_dlc;
    memcpy(f->data, rd.frame.data, f->dlc);
    return 1;
}

int zlg_bridge_tx(const bridge_frame_t *f)
{
    if (g_chn == INVALID_CHANNEL_HANDLE)
        return -1;
    ZCAN_Transmit_Data td;
    memset(&td, 0, sizeof td);
    td.frame.can_id  = f->id;
    td.frame.can_dlc = f->dlc;
    memcpy(td.frame.data, f->data, f->dlc);
    td.transmit_type = 0; /* 正常发送 */
    int n = ZCAN_Transmit(g_chn, &td, 1);
    return n > 0 ? 0 : -1;
}
