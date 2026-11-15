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
| `main.c` | 主程序：参数解析、双线程转发、收发计数、信号退出 |
| `zlg_side.c` | ZLG 侧封装（只 include `<zlgcan/zlgcan.h>`） |
| `can_side.c` | SocketCAN 侧封装（只 include `<linux/can.h>`） |
| `zlg_bridge.h` | 两编译单元间的公共帧定义（普通 C 类型，避免两个头文件的 `struct can_frame` 冲突） |
| `logger.c/.h` | 统一分级日志（时间戳 + 级别 + 模块标签，线程安全，见下） |
| `can_log.c/.h` | 收到的报文按 ID 分文件落盘 |
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

另开终端验证（VCU 上电后应能看到 VCU 发出的 0x501 状态帧）：

```bash
candump can0
```

通过后直接跑测试（接口名 `can0` 不在 `hil_test.yaml` 的 `sim_interfaces=['vcan0']`，
hil_test 自动按**真实接口**处理：不起 vcu_sim、不注入 0x501）：

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

`bridge.yaml`（与本脚本同目录，**改它无需改源码、无需重编译**，脚本每次启动时读取）：

```yaml
device:            # ZLG 盒子与 CAN 通道
  type: 31         # 设备类型: 31=USBCAN-4E-U  20=USBCAN-E-U  34=USBCAN-8E-U
  index: 0         # 同型号设备序号（0=第一台）
  channel: 0       # 使用的 CAN 通道（4E-U 有 0~3）
  baud: 500000     # CAN 波特率，必须与 VCU 一致
iface: can0        # 桥输出的 SocketCAN 接口名；不可用 vcan* 开头（hil_test 会判为仿真接口）
log:               # 日志
  level: info      # 控制台日志级别: debug | info | warn | error
  frames: "on"     # 是否按 ID 分文件落盘收到的报文: on | off（yaml 里 on 会被解析成布尔，故加引号）
  dir: ""          # 报文日志根目录；留空=可执行文件同级的 log/（每次运行自动建时间戳子目录）
```

全栈入口启动本桥时会把 `log.dir` 覆盖为本次运行的 `wuta_logs/<赛项>/<YYYYmmdd_HHMMSS>/zlgCAN_log`；
复用外部已启动的桥时，报文日志路径仍由该桥自己的配置决定。

yaml 字段与命令行参数一一对应（**命名不同，命令行参数保持不变**）：

| yaml 字段 | 命令行参数 |
| --- | --- |
| `device.type` | `--devtype` |
| `device.index` | `--devidx` |
| `device.channel` | `--chn` |
| `device.baud` | `--baud` |
| `iface` | `--iface` |
| `log.level` | `--log-level` |
| `log.frames` | `--log-frames` |
| `log.dir` | `--log-dir` |

yaml 键名写错或文件解析失败时，脚本会打印 `!!` 告警（不再静默回落）。

临时调参不必改文件，命令行覆盖即可：

```bash
sudo ./start_zlg_bridge.sh --chn=1 --log-level=debug   # 只改本次运行的通道与日志级别
sudo ./start_zlg_bridge.sh --cfg=/path/bridge.yaml     # 换用另一份 yaml
sudo ./start_zlg_bridge.sh --help                      # 打印全部可用参数与默认值
```

脚本自身参数：`--devtype` / `--devidx` / `--chn` / `--baud` / `--iface` / `--log-level` / `--log-frames` / `--log-dir` / `--cfg`。
直接用二进制时参数同名（空格分隔）：`./zlg_can_bridge --devtype 31 --chn 0 --baud 500000 --iface can0 --log-level info --log-frames on [--log-dir 路径]`。

### 调参要点

- **`iface` 命名**：以 `vcan` 开头会被 hil_test 判为仿真接口（自动起 vcu_sim、允许注入 0x501、vcu\_sim 模型用例仅在仿真接口运行），真实接入请用 `can0` 这类名字；**改后 hil_test 的 `-i` 必须同步**。脚本也拒绝把桥建在真实 CAN 硬件接口上（提示另取名，如 `--iface=canB`）。
- **`baud` 必须与 VCU 一致**（本项目 500k），否则 `err` 持续增长直至 bus-off；建议**先给 VCU 上电、再起桥**。
- **`chn` 接错通道**时表现为 `VCU->FSD` 恒为 0（ZLG 侧收不到任何帧），可 `--chn=0/1/2/3` 逐个试。
- **`devtype` 要与盒子型号一致**（4E-U=31 / E-U=20 / 8E-U=34），填错的典型现象是 `ZCAN_OpenDevice` 失败。
- **日志三项**（`log.level` / `log.frames` / `log.dir`）用于事后核查：`log.frames: on` 会把收到的报文按 CAN ID 分文件写入 `log/<时间戳>/`，逐帧核对协议时很有用；日常跑测试可置 `off` 减少磁盘写入。
- 读 yaml 依赖 PyYAML：若 `python3 -c "import yaml"` 不可用，脚本会打印告警并回落到内置默认值（此时改 yaml 不生效），请先 `sudo apt install python3-yaml`。

## 日志系统介绍

控制台每行格式：`2026-09-18 16:33:00.123 [INFO ] [main] 就绪: ...`，
时间戳 + 级别 + 模块标签（`main`/`zlg`/`can`/`log`），两个转发线程共用一个锁，
整行原子输出，不会互相撕行。帧级明细**不打到控制台**，只落盘。

落盘的报文按「方向 + CAN-ID」分文件，每次运行新建时间戳目录（所以多次运行的数据不混）：

```text
log/20260918_163300/vcu2fsd/0x501.log   # VCU → 工控机
log/20260918_163300/fsd2vcu/0x210.log   # 工控机 → VCU
```

文件是纯文本，一行一帧，时间戳用 `CLOCK_REALTIME`（便于与 hil_test / dmesg 日志对齐）：

```text
# id=0x501  dir=vcu2fsd  start=2026-09-18 16:33:00.123
2026-09-18 16:33:00.123  [8]  01 02 00 00 00 00 00 00
2026-09-18 16:33:00.224  [8]  01 03 00 00 00 00 00 00
```

## 问题排查

先看桥终端每 2 秒打印的收发计数，它把问题定位到「桥→VCU」还是「FSD→桥」：

```text
2026-09-18 16:33:02.300 [INFO ] [main] FSD->VCU:129  VCU->FSD:0  (err 0/0)
```

- `FSD->VCU(queue)`：驱动队列接受的提交数，不代表VCU实际收到；`VCU->FSD`：转入SocketCAN的帧数（`VCU->FSD` 恒 0 = ZLG 侧一帧都没收到）；
- `err a/b`：两方向发送/接收错误计数（持续增长 → 波特率或接线问题）。

| 症状 | 原因 | 处理 |
| --- | --- | --- |
| 步骤 1/3 报 `未检测到 USBCAN-4E-U(0471:126a)` | 盒子未插好 / USB 口或线缆故障 / 虚拟机未把设备直通给工控机 | `lsusb -d 0471:126a` 复核；换 USB 口与线缆；虚拟机做 USB 直通 |
| 步骤 1/3 报 `未找到 libusbcan-4e.so` | ZLG 厂商 Linux 驱动未装（或装到非 `/lib`、`/usr/lib` 路径） | 按上方「编译」的前置步骤运行 ZLG 官方安装器；`ls -l /usr/lib/libusbcan-4e.so` 复核 |
| 步骤 3/3 起桥报 `ZCAN_OpenDevice 失败` | 未用 sudo / 设备被其他进程占用（**同一 ZLG 设备仅一个进程可打开**）/ `devtype` 与盒子型号不符 | `sudo` 运行脚本；`pgrep -af zlg_can_bridge` 杀残留实例；关闭 CANTest 等厂商工具后重试；核对 `devtype` |
| 步骤 2/3 报 `创建 <iface> 失败` | 无 CAP_NET_ADMIN（容器 / 权限受限） | 提权运行；容器需 `--privileged`（或加 CAP_NET_ADMIN 并加载 vcan 模块） |
| 步骤 2/3 提示 `已存在且挂在真实设备上…拒绝复用`（疑似真实 CAN 硬件） | 接口名与内核真实 CAN 网卡重名 | 换独立虚拟接口名：`sudo ./start_zlg_bridge.sh --iface=canB`，并把 hil_test 的 `-i` 同步改为 `canB` |
| 桥终端 `VCU->FSD` 恒为 0 | ZLG 侧一帧都没收到：`chn` 接错通道 / VCU 未上电未发 0x501 / 接线（CAN_H、CAN_L 接反或未共地）/ 缺 120Ω 终端电阻 / `baud` 与 VCU 不一致 | 确认 VCU 上电并在发 0x501，再 `--chn=0/1/2/3` 逐个试；核查接线与终端电阻；确认两端均为 500k |
| 桥终端 `err` 持续增长直至 `bus-off` | 波特率不匹配 / 接线错误；或桥先于 VCU 上电，发送无应答不断累积错误 | Ctrl-C 停桥，**先给 VCU 上电、再起桥**（当前桥检测到驱动错误后退出，无 bus-off 自动恢复） |
| `candump <iface>` 无输出，但桥计数在增长 | `bridge.yaml` 的 `iface` 改过，终端 / 测试仍用旧接口名 | 统一接口名：`candump <新名>`，hil_test 用 `-i <新名>` |
| hil_test 提示 `can_interface 未运行` 或收不到 VCU 状态帧 | FSD 未编译 / 未 source workspace；真实接口下还需 VCU 在发 0x501 | 先不带 `--no-build` 跑一次编译；看 `logs/latest/<层级>/can_interface_node.log`；`candump <iface>` 确认 0x501 |


## 退出

前台 Ctrl-C（SIGINT/SIGTERM）自动 `ZCAN_ResetCAN` + `ZCAN_CloseDevice` + 关闭 socket。USB连接失效或驱动错误也会结束本次桥进程，不在运行中自动重连恢复发送。
`can0`(vcan) 接口保留，可 `sudo ip link del can0` 手动删除。


## VCU 模式快照

桥收到标准数据帧 `0x501`（DLC 1～8）时，将 `data[0]` 原子写入
`/run/zlgcan_bridge/<iface>.mode`。日志开关不影响快照。所有模式值都会覆盖，
包括有人驾驶和未知值；RES、GO、急停及控制帧均不缓存。

桥每次启动生成新会话并清空有效标记，正常退出删除快照。异常退出留下的
文件由 FSD 根据 boot ID、PID 和进程启动时间拒绝。目录 0755、快照 0644；
写入失败告警并删除旧快照，实时桥接继续。接口锁阻止两个缓存写入器同时工作。

更新后必须重启旧桥进程；运行中的旧版本不会自动产生快照。
快照表示当前桥会话最后收到的模式，不代表 VCU 当前在线。

## 只收桥（2026-10-22）

`start_zlg_bridge.sh --receive-only`（二进制同名参数）只启动ZLG→SocketCAN线程，继续原VCU报文日志和模式缓存；设备层 `zlg_bridge_tx` 拒绝发送。不启动SocketCAN→ZLG线程，外部向虚拟接口注入的数据也不会经此桥转到硬件。默认不传参数时保持双向桥。

人工驾驶记录由主仓库CLI M键选择的有人跑动记录模式独占启动本桥，额外强制 `--log-frames=on` 并使用当次运行的zlgCAN_log目录，禁止复用外部桥。正常模式不能复用只收桥。设备仍使用原work_mode=0，未配置电气静默监听；这里只承诺软件不调用硬件数据帧发送。无硬件回归：`python3 -m pytest tests/test_receive_only.py`，以替代IO执行真实main及设备层函数，不打开USB/CAN。

## 每次初始化与USB失效处理（2026-11-11）

宿主start_fsd_manager.sh→start_full_fsd.sh每次先核对并终止同接口唯一旧桥，确认退出后创建新桥；独立本脚本不替用户杀进程，设备独占冲突则失败。当前实际链接的libusbcan-4e仅支持唯一type31/index0设备（0471:126a），通道0..3；上文其他型号仅为历史参数说明，不表示此二进制支持。

初始化按厂商Linux示例执行Open→Init→尽力预复位→SetValue(波特率/正常模式)→Start。此Linux库成功返回0、失败为负，不能按通用头文件的SetValue成功1注释误报；初始化失败关闭后0.5秒重试，最多3次。

保留原USB连接sysfs目录，每250ms检查设备身份及ZCAN_ReadChannelErrInfo，发送前也检查USB身份；断开/重新枚举或驱动错误锁定故障，停止使用旧句柄并非零退出，清理模式缓存。不在运行中自动重新发送驾驶指令。单纯无接收帧不作为USB断开的证据。日志fsd2vcu保留SocketCAN输入，不证明硬件发送成功；FSD->VCU(queue)只表示驱动接受提交，实际送达需VCU反馈确认。

本轮按用户要求未构建、未执行测试及硬件验证。

2026-11-11预复位门槛修正：厂商Linux示例不检查启动前ZCAN_ResetCAN的返回值。预复位负值仅告警，USB身份必须仍有效，再继续严格检查属性配置、StartCAN和启动后健康状态；未尝试StartCAN的初始化失败路径不重复复位，仍关闭设备。该调整不放宽USB失效检测或驱动运行错误退出；按用户要求未构建或验证。

2026-11-11临时覆盖（按用户要求）：zlgcan_bridge/zlg_bridge.h中ZLG_RUNTIME_HEALTH_CHECKS=0，暂停运行时USB身份检查、驱动错误队列读取及发送前USB身份检查；原检查实现保留，将宏改回1可恢复。启动时独占/USB身份绑定、预复位后的USB存在检查、配置与StartCAN结果、驱动收发接口直接失败及进程监管仍保留。未构建、未验证、未重启当前进程；下一次启动编译后生效。

2026-11-13临时错误处理（覆盖此前任意非零事件退出的说明）：依据USBCAN-4E-U Linux专用手册正文第8页表2，0xE6为CAN总线其他错误，0xE5为总线关闭。zlg_side.c:zlg_bridge_check_health对0xE6按每2秒最多一次告警，记录本次设备打开会话的累计次数并继续读取错误队列，不置g_fault、不触发退出；其他非零事件、USB身份变化及驱动收发/错误读取调用失败沿原处理。错误码按枚举比较，不按通用头文件位掩码拆解；其余诊断字段对4E无有效性保证。当前源码健康检查宏为1，本条是开启检查时的分类策略，不整体关闭检查。按用户要求未运行测试或硬件验证，未重启当前进程；下次启动桥脚本编译后生效。
