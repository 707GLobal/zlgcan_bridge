#ifndef CAN_LOG_H_
#define CAN_LOG_H_

#include "zlg_bridge.h"

/*
 * can_log.h — 收到的报文按 ID 分文件落盘。
 *
 * 目录（每次运行在其下新建一个时间戳子目录，避免多次运行的数据混在一起）：
 *   log/<YYYYmmdd_HHMMSS>/vcu2fsd/0x501.log   ZLG→can0，即 VCU 发出的帧
 *   log/<YYYYmmdd_HHMMSS>/fsd2vcu/0x210.log   can0→ZLG，即 FSD 发出的帧
 *
 * 文件内容为纯文本（人眼优先），一行一帧；时间戳用 CLOCK_REALTIME，
 * 便于与 hil_test / dmesg 对齐：
 *   # id=0x501  dir=vcu2fsd  start=2026-09-18 16:33:00.123
 *   2026-09-18 16:33:00.123  [8]  01 02 00 00 00 00 00 00
 *   2026-09-18 16:33:00.224  [8]  01 03 00 00 00 00 00 00
 */

typedef enum {
    CAN_LOG_VCU2FSD = 0,   /* 从 ZLG 收到的（VCU → 工控机） */
    CAN_LOG_FSD2VCU = 1,   /* 从 SocketCAN 收到的（工控机 → VCU） */
    CAN_LOG_DIR_NUM
} can_log_dir_t;

/*
 * 打开日志根目录，并在其下建本次运行的时间戳子目录。
 * enabled=0 时不落盘（记录直接丢弃）。
 * 返回 0 成功；<0 表示落盘不可用（不影响帧转发）。
 */
int  can_log_open(const char *root_dir, int enabled);
void can_log_close(void);

/* 记一帧：按 ID 追加一行到对应文件。在两个转发线程里直接调用。 */
void can_log_write(can_log_dir_t dir, const bridge_frame_t *f);

#endif /* CAN_LOG_H_ */
