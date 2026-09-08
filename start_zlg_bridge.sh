#!/usr/bin/env bash
# start_zlg_bridge.sh — 一键启动「ZLG USBCAN-4E-U -> SocketCAN」桥
#
# 用法:
#   ./start_zlg_bridge.sh                 # 默认: 波特率 500000, 桥接口 can0
#   ./start_zlg_bridge.sh --baud=500000 --iface=can0
#
# 它会:
#   1. 检查盒子(0471:126a)与 libusbcan-4e 是否就绪
#   2. 创建 can0(type vcan)——注意名字是 can0 而非 vcan0，
#      这样 hil_test 的 sim_interfaces=['vcan0'] 不会命中，按“真实接口”处理
#      （不会自动起 vcu_sim、不会注入 0x501、L0 会被拒绝）
#   3. 前台运行桥（Ctrl-C 退出）
#
# 之后另开终端跑测试:
#   cd hil_test && ./scripts/hil_test.sh -l L1 -i can0 --no-build
#   cd hil_test && ./scripts/hil_test.sh -l L2 -i can0 --no-build
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BAUD="500000"
IFACE="can0"
DEVTYPE="31"

for a in "$@"; do
  case "$a" in
    --baud=*)   BAUD="${a#*=}" ;;
    --iface=*)  IFACE="${a#*=}" ;;
    --devtype=*) DEVTYPE="${a#*=}" ;;
    -h|--help)
      echo "用法: $0 [--baud=500000] [--iface=can0] [--devtype=31]"
      echo "  --devtype: 31=USBCAN-4E-U(默认) 20=USBCAN-E-U"
      exit 0 ;;
    *) echo "!! 未知参数: $a"; exit 1 ;;
  esac
done

echo "==> 1/3 检查硬件与驱动"
if ! lsusb 2>/dev/null | grep -qi "0471:126a"; then
  echo "!! 未检测到 USBCAN-4E-U(0471:126a)，请插入盒子后重试" >&2
  exit 1
fi
if [ ! -f /usr/lib/libusbcan-4e.so ] && [ ! -f /lib/libusbcan-4e.so ]; then
  echo "!! 未找到 libusbcan-4e.so，先安装 ZLG Linux 驱动:"
  echo "   sudo /path/to/usbcan-4e_x86_64_20220629_installer" >&2
  exit 1
fi

echo "==> 2/3 准备 SocketCAN 接口 $IFACE"
if ip link show "$IFACE" >/dev/null 2>&1; then
  # 区分虚拟(vcan)与真实 CAN 硬件：虚拟网卡没有 /sys/class/net/<if>/device 链接
  if [ -e "/sys/class/net/$IFACE/device" ]; then
    echo "!! $IFACE 已存在且挂在真实设备上 ($(readlink -f /sys/class/net/$IFACE/device))" >&2
    echo "   疑似真实 CAN 硬件，拒绝复用为桥接口。桥请用独立虚拟接口名，如 --iface=canB" >&2
    exit 1
  fi
  echo "==> $IFACE 已存在（虚拟 CAN），复用"
else
  echo "==> 创建 $IFACE (type vcan, 需要 sudo)"
  if ! sudo ip link add "$IFACE" type vcan; then
    echo "!! 创建 $IFACE 失败（提示 File exists 说明接口已被占用，重跑本脚本即可复用）" >&2
    exit 1
  fi
fi
sudo ip link set "$IFACE" up || true
echo "==> 接口就绪: $(ip -br link show "$IFACE")"

echo "==> 3/3 编译并启动桥"
cd "$DIR"
if [ ! -x ./zlg_can_bridge ] || [ Makefile -nt ./zlg_can_bridge ]; then
  make
fi
echo "============================================================"
echo " 桥启动中: ZLG(USBCAN devtype=$DEVTYPE @${BAUD}bps) <-> $IFACE"
echo " 保持此终端运行。测试在另一个终端执行:"
echo "   cd hil_test && ./scripts/hil_test.sh -l L1 -i $IFACE --no-build"
echo " 排查: candump $IFACE  应能抓到 VCU 的 0x501 心跳"
echo "============================================================"
exec sudo ./zlg_can_bridge --devtype "$DEVTYPE" --baud "$BAUD" --iface "$IFACE"
