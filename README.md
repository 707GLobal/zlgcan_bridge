# ZLG USBCAN-4E-U → SocketCAN 桥

让 FSD / hil_test **零代码改动**地用上 ZLG（周立功）USBCAN-4E-U 真实 CAN 盒。

## 为什么需要它

- ZLG 盒在 Linux 上**不是 SocketCAN 设备**：由用户态库 `libusbcan-4e.so`（libusb）驱动，
  永远不生成 `can0`；内核 `gs_usb` 也不认它（实测 `new_id` 绑定失败 err=-32，固件非 gs_usb 协议）。
- FSD `can_interface` 与 hil_test 全部基于 **Linux SocketCAN**（`AF_CAN` raw socket）。
- 本桥把两者接起来：真实 VCU ↔ ZLG 盒 ↔ 本程序 ↔ `can0`(vcan 类型) ↔ FSD/hil_test。

```text
真实 VCU <==CAN电气==> ZLG盒 <==USB(libusbcan-4e)==> [zlg_can_bridge] <==can0(socketcan)==> FSD / hil_test
```

## 目录

| 文件 | 作用 |
| --- | --- |
| `main.c` | 主程序：参数解析、双线程转发、统计、信号退出 |
| `zlg_side.c` | ZLG 侧封装（只 include `<zlgcan/zlgcan.h>`） |
| `can_side.c` | SocketCAN 侧封装（只 include `<linux/can.h>`） |
| `zlg_bridge.h` | 两编译单元间的公共帧定义（普通 C 类型，避免两个头文件的 `struct can_frame` 冲突） |
| `logger.c/.h` | 统一分级日志（时间戳 + 级别 + 模块标签，线程安全，见下） |
| `can_log.c/.h` | 报文按 ID 分文件落盘 + 周期/抖动实时统计 |
| `start_zlg_bridge.sh` | 一键脚本：检查硬件 → 建 can0(vcan) → 编译 → 前台运行 |
| `log/` | 运行产物（已 gitignore） |

## 编译

```bash
cd hil_test/scripts/zlg_bridge
make            # 依赖: gcc + libusbcan-4e.so(/lib 或 /usr/lib) + /usr/include/zlgcan/*
```

前置（ZLG 官方 Linux 驱动，需先装一次）：
```bash
sudo apt install libusb-1.0-0
sudo /path/to/USBCAN-4E-U_251017/usbcan-4e_x86_64_20220629_installer
# 产物: /usr/lib/libusbcan-4e.so ；头文件装到 /usr/include/zlgcan/
```

## 运行

```bash
sudo ./start_zlg_bridge.sh                 # 默认波特率 500000、桥接口 can0
```

另开终端验证（VCU 上电应能持续看到 0x501，周期 100ms）：

```bash
candump can0
```

通过后直接跑测试（接口名 `can0` 不在 `hil_test.yaml` 的 `sim_interfaces=['vcan0']`，
hil_test 自动按**真实接口**处理：不起 vcu_sim、不注入 0x501、拒绝 L0）：

```bash
cd /home/czh/WUTA_HIL_TEST/hil_test
./scripts/hil_test.sh -l L1 -i can0 --no-build
./scripts/hil_test.sh -l L2 -i can0 --no-build
```

> 桥接口**必须**叫 `can0`（vcan 类型）而非 `vcan0`——否则 hil_test 会误判为仿真接口，
> 自动起 vcu_sim 并向真实 VCU 注入 0x501，两者冲突。

## 参数

```text
./zlg_can_bridge [--devtype 31] [--devidx 0] [--chn 0] [--baud 500000] [--iface can0]
                 [--log-level info] [--log-dir log] [--log-frames on]
  --devtype    ZLG 设备类型: 31=USBCAN-4E-U(默认), 20=USBCAN-E-U
  --log-level  控制台日志级别: debug|info|warn|error（默认 info）
  --log-dir    报文日志根目录（默认: 可执行文件同级的 log/）
  --log-frames 是否按 ID 分文件落盘收到的报文: on|off（默认 on）
```

也可以用 `bridge.yaml` 配 `log_level` / `log_frames` / `log_dir`，由 `start_zlg_bridge.sh` 透传。

## 日志

控制台每行格式：`2026-09-18 16:33:00.123 [INFO ] [main] 就绪: ...`，
时间戳 + 级别 + 模块标签（`main`/`zlg`/`can`/`log`/`stat`），两个转发线程共用一个锁，
整行原子输出，不会互相撕行。帧级明细**不打到控制台**，只落盘。

落盘的报文按「方向 + CAN-ID」分文件，每次运行新建时间戳目录（所以多次运行的数据不混）：

```text
log/20260918_163300/vcu2fsd/0x501.log   # VCU → 工控机
log/20260918_163300/fsd2vcu/0x210.log   # 工控机 → VCU
```

文件是纯文本，一行一帧，`(+100.12ms)` 是距同 ID 上一帧的间隔，肉眼即可看出周期与抖动：

```text
# id=0x501  dir=vcu2fsd  start=2026-09-18 16:33:00.123
2026-09-18 16:33:00.123  (      --  )  [8]  01 02 00 00 00 00 00 00
2026-09-18 16:33:00.224  (+ 100.12ms)  [8]  01 03 00 00 00 00 00 00
```

同时每 2s 在控制台打印各 ID 的周期统计（`0x501`/`0x210` 的期望周期来自协议约定，
容差 ±20%，与 `hil_test.yaml` 的 `period_tolerance_ratio` 一致）：

```text
2026-09-18 16:33:02.300 [INFO ] [stat] vcu2fsd 0x501  n=20  周期 100.21ms ±0.35 [99.8~101.2]  超差 0/19 (期望 100ms ±20%)
```

周期统计用 `CLOCK_MONOTONIC`（不受系统时钟跳变影响），落盘时间戳用 `CLOCK_REALTIME`
（便于与 hil_test / dmesg 日志对齐）。

## 设计要点 / 已知限制

- **防回环**：can0 侧只用一个 CAN_RAW socket 供收发共用。SocketCAN 默认
  `CAN_RAW_RECV_OWN_MSGS=0`（不把本 socket 自己发的帧投递回来），因此桥注入 can0 的帧
  不会再被收回来转发回 ZLG，避免 `ZLG→can0→ZLG` 死循环。
- **零转换**：ZLG `can_frame` 与 Linux SocketCAN `struct can_frame` 布局一致
  （bit31=EFF/bit30=RTR/bit29=ERR + 低 29 位 ID），桥只做逐字段透传，无重编码。
- **性能/时序**：帧路径 = 电气→USB→libusb→用户态→vcan，有毫秒级抖动。
  L1 心跳(100ms±20%)通常可过；**L2/L3 验收级安全时序请谨慎**，最好仍用原生
  gs_usb/slcan 适配器做最终验收。本桥适合开发期验证链路与协议。
- 未处理 CANFD（本协议仅 2.0，8 字节）。

## 退出

前台 Ctrl-C（SIGINT/SIGTERM）自动 `ZCAN_ResetCAN` + `ZCAN_CloseDevice` + 关闭 socket。
`can0`(vcan) 接口保留，可 `sudo ip link del can0` 手动删除。
