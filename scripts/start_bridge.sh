#!/usr/bin/env bash
# ZLGCAN 桥接管理脚本：准备 SocketCAN 接口 + 启停桥接进程
#
# 用法:
#   ./scripts/start_bridge.sh                 # 前台运行（Ctrl-C 退出）
#   ./scripts/start_bridge.sh start           # 后台启动（写 logs/bridge.log 与 bridge.pid）
#   ./scripts/start_bridge.sh stop            # 停止后台桥接
#   ./scripts/start_bridge.sh status          # 查看运行状态
#   ./scripts/start_bridge.sh iface           # 仅准备 can0 接口（供 hil_test.sh 调用）
#   ./scripts/start_bridge.sh selftest        # 虚拟设备自测（无需硬件）
#
# 环境变量:
#   ZLG_CAN_IFACE   SocketCAN 接口名（缺省 can0）
#   ZLG_LOG_FILE    后台日志路径（缺省 <仓库>/bridge.log；hil_test.sh 会指到其批次日志目录）
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BRIDGE_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BIN="$BRIDGE_ROOT/build/zlgcan_bridge"
CONFIG="$BRIDGE_ROOT/config/bridge_config.yaml"
LIB_DIR="$BRIDGE_ROOT/lib"
PID_FILE="$BRIDGE_ROOT/bridge.pid"
LOG_FILE="${ZLG_LOG_FILE:-$BRIDGE_ROOT/bridge.log}"
IFACE="${ZLG_CAN_IFACE:-can0}"

usage() { grep '^#   ' "$0" | sed 's/^#   //'; }

die() { echo "!! $*" >&2; exit 1; }

# ---- 准备本地 vcan 类型接口（缺 can0 时自动创建；vcan0 保留给纯仿真） ----
prepare_iface() {
  if ! command -v ip >/dev/null 2>&1; then
    die "未找到 ip 命令（iproute2）"
  fi
  SUDO=""
  if [ "$(id -u)" -ne 0 ] && command -v sudo >/dev/null 2>&1; then
    SUDO="sudo"
  fi
  if ! ip link show "$IFACE" >/dev/null 2>&1; then
    echo "==> 接口 $IFACE 不存在，创建 vcan 类型接口..."
    $SUDO modprobe vcan 2>/dev/null || true
    $SUDO ip link add dev "$IFACE" type vcan || die "创建 $IFACE 失败（检查 vcan 模块: modprobe vcan）"
    echo "==> 已创建 $IFACE（vcan）"
  else
    # 已存在时校验类型：真实 CAN 口需要波特率与硬件，桥接约定绑定 vcan 类型
    if ip -details link show "$IFACE" | grep -q " vcan"; then
      :
    elif ip -details link show "$IFACE" | grep -qE "[^v]can"; then
      echo "==> 注意: $IFACE 已存在且非 vcan 类型（真实 CAN 口），桥接将直接使用它"
    fi
  fi
  if ! ip link show "$IFACE" | grep -qw "UP"; then
    $SUDO ip link set "$IFACE" up || die "启动 $IFACE 失败"
    echo "==> 已启动 $IFACE"
  fi
  echo "==> 接口 $IFACE 就绪"
}

build_if_needed() {
  if [ ! -x "$BIN" ]; then
    command -v cmake >/dev/null 2>&1 || die "桥接未编译且无 cmake：请手动编译 $BRIDGE_ROOT"
    echo "==> 编译桥接..."
    (cd "$BRIDGE_ROOT" && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null &&
      cmake --build build -j"$(nproc)") || die "编译失败"
  fi
  [ -x "$BIN" ] || die "未找到桥接可执行文件: $BIN"
}

lib_env() {
  if [ -d "$LIB_DIR" ]; then
    echo "$LIB_DIR"
  fi
}

run_foreground() {
  build_if_needed
  prepare_iface
  local ld
  ld="$(lib_env)"
  if [ -n "$ld" ]; then
    LD_LIBRARY_PATH="$ld${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" exec "$BIN" -c "$CONFIG" "$@"
  else
    exec "$BIN" -c "$CONFIG" "$@"
  fi
}

is_running() {
  [ -f "$PID_FILE" ] && kill -0 "$(cat "$PID_FILE" 2>/dev/null)" 2>/dev/null
}

cmd_start() {
  if is_running; then
    echo "==> 桥接已在运行 (pid $(cat "$PID_FILE"))"
    return 0
  fi
  rm -f "$PID_FILE"
  build_if_needed
  prepare_iface
  mkdir -p "$(dirname "$LOG_FILE")"
  echo "==> 后台启动桥接（日志: $LOG_FILE）"
  local ld
  ld="$(lib_env)"
  ( cd "$BRIDGE_ROOT" &&
    if [ -n "$ld" ]; then
      LD_LIBRARY_PATH="$ld${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
        nohup "$BIN" -c "$CONFIG" >>"$LOG_FILE" 2>&1 &
    else
      nohup "$BIN" -c "$CONFIG" >>"$LOG_FILE" 2>&1 &
    fi
    echo $! >"$PID_FILE" )
  sleep 0.3
  if is_running; then
    echo "==> 桥接已启动 (pid $(cat "$PID_FILE"))"
  else
    echo "!! 桥接启动失败，最近日志:" >&2
    tail -n 10 "$LOG_FILE" >&2 || true
    return 1
  fi
}

cmd_stop() {
  if ! is_running; then
    echo "==> 桥接未在运行"
    rm -f "$PID_FILE"
    return 0
  fi
  local pid
  pid="$(cat "$PID_FILE")"
  echo "==> 停止桥接 (pid $pid)"
  kill "$pid" 2>/dev/null || true
  for _ in $(seq 1 30); do
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.1
  done
  kill -0 "$pid" 2>/dev/null && kill -9 "$pid" 2>/dev/null
  rm -f "$PID_FILE"
  echo "==> 桥接已停止"
}

cmd_status() {
  if is_running; then
    echo "running (pid $(cat "$PID_FILE"))"
  else
    echo "stopped"
    return 1
  fi
}

# ---- 主入口 ----
CMD="${1:-foreground}"
[ $# -gt 0 ] && shift

case "$CMD" in
  start)   cmd_start "$@" ;;
  stop)    cmd_stop ;;
  status)  cmd_status ;;
  iface)   prepare_iface ;;
  selftest)
    build_if_needed
    local_ld="$(lib_env)"
    if [ -n "$local_ld" ]; then
      LD_LIBRARY_PATH="$local_ld${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" exec "$BIN" --selftest "$@"
    else
      exec "$BIN" --selftest "$@"
    fi
    ;;
  -h|--help|help) usage ;;
  foreground|"") run_foreground "$@" ;;
  *) die "未知命令: $CMD（见 --help）" ;;
esac
