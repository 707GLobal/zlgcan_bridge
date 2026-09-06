#ifndef CANFRAME_H_
#define CANFRAME_H_

#include "typedef.h"  // UINT/BYTE 等基础类型（zlgcan.h 依赖）

/*
 * ZLG sample 的 can_frame/canfd_frame 位定义与 Linux <linux/can.h> 完全一致，
 * 因此在 Linux 上直接复用系统头文件，避免与 SocketCAN 侧的结构体重定义冲突；
 * 仅保留 ZLG 扩展的宏定义。非 Linux 平台（Windows）保留 sample 原始定义。
 */

#ifdef __linux__

#include <linux/can.h>

/* ZLG 扩展：构造/判断 CAN ID */
#ifndef MAKE_CAN_ID
#define MAKE_CAN_ID(id, eff, rtr, err) (id | (!!(eff) << 31) | (!!(rtr) << 30) | (!!(err) << 29))
#endif
#define IS_EFF(id) (!!(id & CAN_EFF_FLAG))  // 1:扩展帧 0:标准帧
#define IS_RTR(id) (!!(id & CAN_RTR_FLAG))  // 1:远程帧 0:数据帧
#define IS_ERR(id) (!!(id & CAN_ERR_FLAG))  // 1:错误帧 0:正常帧
#ifndef CAN_ID_FLAG
#define CAN_ID_FLAG 0x1FFFFFFFU /* id */
#endif
#define GET_ID(id) (id & CAN_ID_FLAG)

#else /* !__linux__ */

#include "typedef.h"

/* special address description flags for the MAKE_CAN_ID */
#define CAN_EFF_FLAG 0x80000000U /* EFF/SFF is set in the MSB */
#define CAN_RTR_FLAG 0x40000000U /* remote transmission request */
#define CAN_ERR_FLAG 0x20000000U /* error message frame */
#define CAN_ID_FLAG  0x1FFFFFFFU /* id */

/* valid bits in CAN ID for frame formats */
#define CAN_SFF_MASK 0x000007FFU /* standard frame format (SFF) */
#define CAN_EFF_MASK 0x1FFFFFFFU /* extended frame format (EFF) */
#define CAN_ERR_MASK 0x1FFFFFFFU /* omit EFF, RTR, ERR flags */

typedef UINT canid_t;

#define CAN_SFF_ID_BITS 11
#define CAN_EFF_ID_BITS 29

#define MAKE_CAN_ID(id, eff, rtr, err) (id | (!!(eff) << 31) | (!!(rtr) << 30) | (!!(err) << 29))
#define IS_EFF(id) (!!(id & CAN_EFF_FLAG))
#define IS_RTR(id) (!!(id & CAN_RTR_FLAG))
#define IS_ERR(id) (!!(id & CAN_ERR_FLAG))
#define GET_ID(id) (id & CAN_ID_FLAG)

/* CAN payload length and DLC definitions according to ISO 11898-1 */
#define CAN_MAX_DLC 8
#define CAN_MAX_DLEN 8

/* CAN FD payload length and DLC definitions according to ISO 11898-7 */
#define CANFD_MAX_DLC 15
#define CANFD_MAX_DLEN 64

typedef struct {
    canid_t can_id;  /* 32 bit MAKE_CAN_ID + EFF/RTR/ERR flags */
    BYTE    can_dlc; /* frame payload length in byte (0 .. CAN_MAX_DLEN) */
    BYTE    __pad;   /* padding */
    BYTE    __res0;  /* reserved / padding */
    BYTE    __res1;  /* reserved / padding */
    BYTE    data[CAN_MAX_DLEN];
} can_frame;

#define CANFD_BRS 0x01
#define CANFD_ESI 0x02

typedef struct {
    canid_t can_id;
    BYTE    len;
    BYTE    flags;
    BYTE    __res0;
    BYTE    __res1;
    BYTE    data[CANFD_MAX_DLEN];
} canfd_frame;

#define CAN_MTU   (sizeof(struct can_frame))
#define CANFD_MTU (sizeof(struct canfd_frame))

#endif /* __linux__ */

/* TX_DELAY_SEND_FLAG apply to can_frame.__pad and canfd_frame.flags, only apply to tx frames */
#define TX_DELAY_SEND_FLAG 0x80 /* indicat tx frame in delay send mode, 1:send in device queue; 0:send direct to bus */
#define IS_DELAY_SEND(flag) (!!(flag & TX_DELAY_SEND_FLAG))
#define TX_DELAY_SEND_TIME_UNIT_FLAG 0x40 /* indicat tx delay send time unit, 1:time unit is 100us; 0:time unit is 1ms */
#define IS_DELAY_SEND_TIME_UNIT_MS(flag) (!(flag & TX_DELAY_SEND_TIME_UNIT_FLAG))
#define IS_DELAY_SEND_TIME_UNIT_100US(flag) (!!(flag & TX_DELAY_SEND_TIME_UNIT_FLAG))

/* TX_ECHO_FLAG apply to can_frame.__pad and canfd_frame.flags, apply to tx and rx frames */
#define TX_ECHO_FLAG 0x20 /* indicat tx frame will be echoed(recieved) when tx frame trasmit to the bus*/
#define IS_TX_ECHO(flag) (!!(flag & TX_ECHO_FLAG))

#endif // CANFRAME_H_
