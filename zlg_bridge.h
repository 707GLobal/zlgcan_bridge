#ifndef ZLG_BRIDGE_H_
#define ZLG_BRIDGE_H_

#include <stdint.h>

/*
 * 最小公共帧表示：在两个编译单元之间传递，字段与 Linux SocketCAN can_id 位定义一致
 * （bit31=EFF, bit30=RTR, bit29=ERR，低 29 位为 ID）。
 * ZLG(libusbcan-4e) 的 can_frame 与该布局一致，故可逐字段透传。
 */
typedef struct {
    uint32_t id;     /* CAN-ID + EFF/RTR/ERR 标志 */
    uint8_t  dlc;    /* 数据长度 0..8 */
    uint8_t  data[8];
} bridge_frame_t;

/* ---- zlg_side.c：ZLG USBCAN 侧（libusbcan-4e），只 include <zlgcan/zlgcan.h> ---- */
int  zlg_bridge_open(unsigned devtype, unsigned devidx, unsigned chn, const char *baud);
void zlg_bridge_close(void);
/* 返回 1=收到帧并填入 f; 0=超时无帧; <0=错误 */
int  zlg_bridge_rx(bridge_frame_t *f, int timeout_ms);
/* 返回 0=成功; <0=失败 */
int  zlg_bridge_tx(const bridge_frame_t *f);

/* ---- can_side.c：Linux SocketCAN 侧，只 include <linux/can.h> ---- */
int  can_side_open(const char *ifname);
void can_side_close(void);
/* 返回 1=收到帧; 0=超时(无帧); <0=错误 */
int  can_side_rx(bridge_frame_t *f);
/* 返回 0=成功; <0=失败 */
int  can_side_tx(const bridge_frame_t *f);

#endif /* ZLG_BRIDGE_H_ */
