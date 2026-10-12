#!/usr/bin/env bash
# start_zlg_bridge.sh — 一键启动「ZLG USBCAN -> SocketCAN」桥
#
# 用法:
#   ./start_zlg_bridge.sh                        # 参数取自同目录 bridge.yaml
#   ./start_zlg_bridge.sh --baud=500000 --chn=1  # 命令行覆盖 yaml 中的同名项
#   ./start_zlg_bridge.sh --cfg=/path/bridge.yaml
#   ./start_zlg_bridge.sh --log-level=debug --log-frames=off
#
# 参数优先级: 命令行 > bridge.yaml > 内置默认
#   (devtype=31 devidx=0 chn=0 baud=500000 iface=can0 log_level=info log_frames=on)
#
# 它会:
#   1. 检查盒子(0471:126a)与 libusbcan-4e 是否就绪
#   2. 创建 can0(type vcan)——注意名字是 can0 而非 vcan0，
#      这样 hil_test 的 sim_interfaces=['vcan0'] 不会命中，按“真实接口”处理
#      （不会自动起 vcu_sim、不会注入 0x501）
#   3. 前台运行桥（Ctrl-C 退出）
#
# 之后另开终端跑测试:
#   cd hil_test && ./scripts/hil_test.sh -l L1 -i can0 --no-build
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CFG="$DIR/bridge.yaml"

# 读 yaml 中的点分键（取不到/解析失败则输出空串，由调用方回落到默认值）
cfg_get() {
  python3 - "$1" "$2" <<'PYEOF' 2>/dev/null || true
import sys, yaml
try:
    with open(sys.argv[1]) as f:
        val = yaml.safe_load(f)
except Exception:
    sys.exit(0)
for part in sys.argv[2].split('.'):
    if not isinstance(val, dict) or part not in val:
        sys.exit(0)
    val = val[part]
if val is not None:
    print(val)
PYEOF
}

# 校验 yaml：文件缺失/解析失败/存在未知键时显式告警，避免"改了 yaml 却不生效"的静默失败
cfg_validate() {
  if [ ! -f "$CFG" ]; then
    echo "!! 参数文件不存在: $CFG（将使用内置默认值）" >&2
    return
  fi
  python3 - "$CFG" <<'PYEOF' || echo "!! $CFG 解析失败（缺 PyYAML 或语法错误），已回落到内置默认值" >&2
import sys, yaml
KNOWN = {"device": {"type", "index", "channel", "baud"},
         "iface": None,
         "log": {"level", "frames", "dir"}}
try:
    with open(sys.argv[1]) as f:
        doc = yaml.safe_load(f)
except Exception:
    sys.exit(1)
if not isinstance(doc, dict):
    sys.exit(1)
for k, v in doc.items():
    if k not in KNOWN:
        print(f"!! {sys.argv[1]} 未知键: {k}", file=sys.stderr)
        continue
    allowed = KNOWN[k]
    if allowed is not None and isinstance(v, dict):
        for kk in v:
            if kk not in allowed:
                print(f"!! {sys.argv[1]} 未知键: {k}.{kk}", file=sys.stderr)
PYEOF
}

ARGV=("$@")

# --cfg 需先解析：它决定后面读哪个 yaml
for a in "${ARGV[@]}"; do
  case "$a" in
    --cfg=*) CFG="${a#*=}" ;;
  esac
done

# 内置默认 < bridge.yaml
cfg_validate
BAUD="$(cfg_get "$CFG" device.baud)";       BAUD="${BAUD:-500000}"
IFACE="$(cfg_get "$CFG" iface)";            IFACE="${IFACE:-can0}"
DEVTYPE="$(cfg_get "$CFG" device.type)";    DEVTYPE="${DEVTYPE:-31}"
DEVIDX="$(cfg_get "$CFG" device.index)";    DEVIDX="${DEVIDX:-0}"
CHN="$(cfg_get "$CFG" device.channel)";     CHN="${CHN:-0}"
LOG_LEVEL="$(cfg_get "$CFG" log.level)";    LOG_LEVEL="${LOG_LEVEL:-info}"
LOG_FRAMES="$(cfg_get "$CFG" log.frames)";  LOG_FRAMES="${LOG_FRAMES:-on}"
# yaml 不加引号时 on/off 会被解析成布尔 True/False，这里统一归一化
case "$LOG_FRAMES" in
  [Oo][Nn]|true|True|yes|Yes|1)            LOG_FRAMES=on ;;
  [Oo][Ff][Ff]|false|False|no|No|0)        LOG_FRAMES=off ;;
esac
LOG_DIR="$(cfg_get "$CFG" log.dir)"; LOG_DIR="${LOG_DIR:-}"

# 命令行覆盖 yaml
for a in "${ARGV[@]}"; do
  case "$a" in
    --baud=*)    BAUD="${a#*=}" ;;
    --iface=*)   IFACE="${a#*=}" ;;
    --devtype=*) DEVTYPE="${a#*=}" ;;
    --devidx=*)  DEVIDX="${a#*=}" ;;
    --chn=*)     CHN="${a#*=}" ;;
    --log-level=*)  LOG_LEVEL="${a#*=}" ;;
    --log-frames=*) LOG_FRAMES="${a#*=}" ;;
    --log-dir=*)    LOG_DIR="${a#*=}" ;;
    --cfg=*)     ;;
    -h|--help)
      echo "用法: $0 [--devtype=31] [--devidx=0] [--chn=0] [--baud=500000] [--iface=can0]"
      echo "          [--log-level=info] [--log-frames=on] [--log-dir=路径] [--cfg=bridge.yaml]"
      echo "  --devtype: 31=USBCAN-4E-U(默认) 20=USBCAN-E-U 34=USBCAN-8E-U"
      echo "  --log-level:  debug|info|warn|error"
      echo "  --log-frames: on|off，是否按 ID 分文件落盘收到的报文"
      echo "  --log-dir:    报文日志根目录，留空=可执行文件同级的 log/"
      echo "  缺省值读取 $CFG，命令行参数优先级最高"
      exit 0 ;;
    *) echo "!! 未知参数: $a"; exit 1 ;;
  esac
done

echo "==> 参数来源: $CFG（命令行优先）"
echo "    devtype=$DEVTYPE devidx=$DEVIDX chn=$CHN baud=$BAUD iface=$IFACE"
echo "    日志: level=$LOG_LEVEL frames=$LOG_FRAMES dir=${LOG_DIR:-(默认 log/)}"

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

# Authenticate before interface setup. The full-stack launcher may already
# have elevated this process before detaching it from the terminal.
PRIVILEGE=()
if (( EUID != 0 )); then
  if ! command -v sudo >/dev/null 2>&1; then
    echo "!! 需要 root 权限，但未找到 sudo；请以 root 运行本脚本" >&2
    exit 1
  fi
  if ! sudo -n -v 2>/dev/null; then
    if [[ ! -t 0 ]]; then
      echo "!! 需要 sudo 密码，但当前进程没有交互终端；尚未创建或启用 $IFACE" >&2
      printf '   请在交互终端运行：sudo bash %q' "$0" >&2
      if (( ${#ARGV[@]} )); then printf ' %q' "${ARGV[@]}" >&2; fi
      printf '\n' >&2
      exit 1
    fi
    sudo -v || { echo "!! sudo 认证失败，无法启动 CAN 桥" >&2; exit 1; }
  fi
  PRIVILEGE=(sudo -n)
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
  echo "==> 创建 $IFACE (type vcan, 需要 root 权限)"
  if ! "${PRIVILEGE[@]}" ip link add "$IFACE" type vcan; then
    echo "!! 创建 $IFACE 失败，请检查上方 ip / 权限错误" >&2
    exit 1
  fi
fi
if ! "${PRIVILEGE[@]}" ip link set "$IFACE" up; then
  echo "!! 启用 $IFACE 失败，请检查上方权限或接口错误" >&2
  exit 1
fi
echo "==> 接口就绪: $(ip -br link show "$IFACE")"

echo "==> 3/3 编译并启动桥"
cd "$DIR"
if [ ! -x ./zlg_can_bridge ] || [ Makefile -nt ./zlg_can_bridge ]; then
  make
fi
echo "============================================================"
echo " 桥启动中: ZLG(USBCAN devtype=$DEVTYPE devidx=$DEVIDX chn=$CHN @${BAUD}bps) <-> $IFACE"
echo " 保持此终端运行。测试在另一个终端执行:"
echo "   cd hil_test && ./scripts/hil_test.sh -l L1 -i $IFACE --no-build"
echo " 排查: candump $IFACE  应能抓到 VCU 的 0x501 状态帧（事件驱动，无固定周期）"
echo "============================================================"
ARGS=(--devtype "$DEVTYPE" --devidx "$DEVIDX" --chn "$CHN" --baud "$BAUD" --iface "$IFACE"
      --log-level "$LOG_LEVEL" --log-frames "$LOG_FRAMES")
if [ -n "$LOG_DIR" ]; then
  ARGS+=(--log-dir "$LOG_DIR")
fi
exec "${PRIVILEGE[@]}" ./zlg_can_bridge "${ARGS[@]}"
