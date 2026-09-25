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
cd zlgcan_bridge
make            # 依赖: gcc + libusbcan-4e.so(/lib 或 /usr/lib) + /usr/include/zlgcan/*
```

前置（ZLG 官方 Linux 驱动，需先装一次）：
<https://fy63ozvxdv.feishu.cn/file/UC8IbcxTeo1a83x4wZbcAa26n5E>
```bash
sudo apt install libusb-1.0-0
sudo /path/to/USBCAN-4E-U_251017/usbcan-4e_x86_64_20220629_installer
# 产物: /usr/lib/libusbcan-4e.so ；头文件装到 /usr/include/zlgcan/
```

## 运行

先确认盒子已插上并被系统识别（脚本第 1/3 步会做同样检查，这里是手动复核方法）：

```bash
lsusb -d 0471:126a          # 精确查找 USBCAN-4E-U
lsusb | grep -i 0471        # 等价的模糊查找（盒子型号不确定 / 记不住 PID 时用）
```

`0471` 是 ZLG 的 USB 厂商号，`126a` 是 USBCAN-4E-U 的产品号，命中时会打印一行，例如：

```text
Bus 001 Device 007: ID 0471:126a ZLG USBCAN-4E-U
```

- **能打印出这行 = 盒子已插好、USB 枚举成功**，可以起桥了。
- 打印不出来时逐项排查：换 USB 口 / 换线缆；虚拟机需把该 USB 设备**直通**给工控机（要在工控机里执行 `lsusb` 看）；`dmesg | tail` 看有没有插入/错误事件；必要时 `lsusb -t` 看该设备挂在哪个总线上。
- 它**不会**出现在 `ip link` 或 `dmesg` 的 CAN 网卡列表中（ZLG 盒没有内核驱动，属正常）——`can0` 由桥启动后创建，见下方「参数」的 `iface`。

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
cd hil_test
./scripts/hil_test.sh -l L1 -i can0 --no-build
./scripts/hil_test.sh -l L2 -i can0 --no-build
```

> 桥接口**必须**叫 `can0`（vcan 类型）而非 `vcan0`——否则 hil_test 会误判为仿真接口，
> 自动起 vcu_sim 并向真实 VCU 注入 0x501，两者冲突。

## 如何调参

`start_zlg_bridge.sh` 的取值优先级：**命令行 > `bridge.yaml` > 脚本内置默认**
（内置默认 `devtype=31 devidx=0 chn=0 baud=500000 iface=can0 log_level=info log_frames=on`）。

`bridge.yaml`（与本脚本同目录，**改它无需改源码、无需重编译**，脚本每次启动时读取；每项都对应一个同名命令行参数 `--key=值`）：

```yaml
devtype: 31        # ZLG 设备类型: 31=USBCAN-4E-U  20=USBCAN-E-U  34=USBCAN-8E-U
devidx: 0          # 同型号设备序号（0=第一台）
chn: 0             # 使用的 CAN 通道（4E-U 有 0~3）
baud: 500000       # CAN 波特率，必须与 VCU 一致
iface: can0        # 桥输出的 SocketCAN 接口名；不可用 vcan* 开头（hil_test 会判为仿真接口）
log_level: info    # 控制台日志级别: debug | info | warn | error
log_frames: "on"   # 是否按 ID 分文件落盘收到的报文: on | off（yaml 里 on 会被解析成布尔，故加引号）
log_dir: ""        # 报文日志根目录；留空=可执行文件同级的 log/（每次运行自动建时间戳子目录）
```

临时调参不必改文件，命令行覆盖即可：

```bash
sudo ./start_zlg_bridge.sh --chn=1 --log-level=debug   # 只改本次运行的通道与日志级别
sudo ./start_zlg_bridge.sh --cfg=/path/bridge.yaml     # 换用另一份 yaml
sudo ./start_zlg_bridge.sh --help                      # 打印全部可用参数与默认值
```

脚本自身参数：`--devtype` / `--devidx` / `--chn` / `--baud` / `--iface` / `--log-level` / `--log-frames` / `--log-dir` / `--cfg`。
直接用二进制时参数同名（空格分隔）：`./zlg_can_bridge --devtype 31 --chn 0 --baud 500000 --iface can0 --log-level info --log-frames on [--log-dir 路径]`。

### 调参要点

- **`iface` 命名**：以 `vcan` 开头会被 hil_test 判为仿真接口（自动起 vcu_sim、允许注入 0x501、允许跑 L0），真实接入请用 `can0` 这类名字；**改后 hil_test 的 `-i` 必须同步**。脚本也拒绝把桥建在真实 CAN 硬件接口上（提示另取名，如 `--iface=canB`）。
- **`baud` 必须与 VCU 一致**（本项目 500k），否则 `err` 持续增长直至 bus-off；建议**先给 VCU 上电、再起桥**。
- **`chn` 接错通道**时表现为 `VCU->FSD` 恒为 0（ZLG 侧收不到任何帧），可 `--chn=0/1/2/3` 逐个试。
- **`devtype` 要与盒子型号一致**（4E-U=31 / E-U=20 / 8E-U=34），填错的典型现象是 `ZCAN_OpenDevice` 失败。
- **日志三项**（`log_level` / `log_frames` / `log_dir`）用于事后核查：`log_frames: on` 会把收到的报文按 CAN ID 分文件写入 `log/<时间戳>/`，逐帧核对协议时很有用；日常跑测试可置 `off` 减少磁盘写入。
- 读 yaml 依赖 PyYAML：若 `python3 -c "import yaml"` 不可用，脚本会**静默回落到内置默认值**（此时改 yaml 不生效），请先 `sudo apt install python3-yaml`。

## 日志系统介绍

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

## 问题排查

先看桥终端每 2 秒打印的收发统计，它把问题定位到「桥→VCU」还是「FSD→桥」：

```text
2026-09-18 16:33:02.300 [INFO ] [main] FSD->VCU:129  VCU->FSD:0  (err 0/0)  收到帧:129/0
```

- `FSD->VCU` / `VCU->FSD`：两方向已转发帧数（`VCU->FSD` 恒 0 = ZLG 侧一帧都没收到）；
- `err a/b`：两方向发送/接收错误计数（持续增长 → 波特率或接线问题）；
- `收到帧 x/y`：入库（落盘）的帧数，正常应与 `FSD->VCU` / `VCU->FSD` 一致。

| 症状 | 原因 | 处理 |
| --- | --- | --- |
| 步骤 1/3 报 `未检测到 USBCAN-4E-U(0471:126a)` | 盒子未插好 / USB 口或线缆故障 / 虚拟机未把设备直通给工控机 | `lsusb -d 0471:126a` 复核；换 USB 口与线缆；虚拟机做 USB 直通 |
| 步骤 1/3 报 `未找到 libusbcan-4e.so` | ZLG 厂商 Linux 驱动未装（或装到非 `/lib`、`/usr/lib` 路径） | 按上方「编译」的前置步骤运行 ZLG 官方安装器；`ls -l /usr/lib/libusbcan-4e.so` 复核 |
| 步骤 3/3 起桥报 `ZCAN_OpenDevice 失败` | 未用 sudo / 设备被其他进程占用（**同一 ZLG 设备仅一个进程可打开**）/ `devtype` 与盒子型号不符 | `sudo` 运行脚本；`pgrep -af zlg_can_bridge` 杀残留实例；关闭 CANTest 等厂商工具后重试；核对 `devtype` |
| 步骤 2/3 报 `创建 <iface> 失败` | 无 CAP_NET_ADMIN（容器 / 权限受限） | 提权运行；容器需 `--privileged`（或加 CAP_NET_ADMIN 并加载 vcan 模块） |
| 步骤 2/3 提示 `已存在且挂在真实设备上…拒绝复用`（疑似真实 CAN 硬件） | 接口名与内核真实 CAN 网卡重名 | 换独立虚拟接口名：`sudo ./start_zlg_bridge.sh --iface=canB`，并把 hil_test 的 `-i` 同步改为 `canB` |
| 桥终端 `VCU->FSD` 恒为 0 | ZLG 侧一帧都没收到：`chn` 接错通道 / VCU 未上电未发 0x501 / 接线（CAN_H、CAN_L 接反或未共地）/ 缺 120Ω 终端电阻 / `baud` 与 VCU 不一致 | 确认 VCU 上电并在发 0x501，再 `--chn=0/1/2/3` 逐个试；核查接线与终端电阻；确认两端均为 500k |
| 桥终端 `err` 持续增长直至 `bus-off` | 波特率不匹配 / 接线错误；或桥先于 VCU 上电，发送无应答不断累积错误 | Ctrl-C 停桥，**先给 VCU 上电、再起桥**（当前桥无 bus-off 自动恢复） |
| `candump <iface>` 无输出，但桥计数在增长 | `bridge.yaml` 的 `iface` 改过，终端 / 测试仍用旧接口名 | 统一接口名：`candump <新名>`，hil_test 用 `-i <新名>` |
| hil_test 提示 `can_interface 未运行` 或心跳超时 | FSD 未编译 / 未 source workspace；真实接口下还需 VCU 在发 0x501 | 先不带 `--no-build` 跑一次编译；看 `logs/latest/<层级>/can_interface_node.log`；`candump <iface>` 确认 0x501 |


## 退出

前台 Ctrl-C（SIGINT/SIGTERM）自动 `ZCAN_ResetCAN` + `ZCAN_CloseDevice` + 关闭 socket。
`can0`(vcan) 接口保留，可 `sudo ip link del can0` 手动删除。
