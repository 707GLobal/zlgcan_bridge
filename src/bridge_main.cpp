// ZLGCAN <-> SocketCAN 桥接
//
// 将周立功 USBCAN-xE-U（厂商私有协议，无内核 SocketCAN 驱动）与本地 SocketCAN
// 接口（vcan 类型，默认 can0）双向转发，使 FSD can_interface（C++）与
// hil_test（Python）按标准 SocketCAN 语义访问真实 CAN 总线。
//
// 初始化流程严格遵循《接口函数使用手册》：
//   ZCAN_OpenDevice -> ZCAN_SetValue("n/baud_rate") -> ZCAN_InitCAN -> ZCAN_StartCAN
//   ... 收发 ... -> ZCAN_ResetCAN -> ZCAN_CloseDevice
// 在线检测：ZCAN_IsDeviceOnLine 返回 STATUS_ONLINE(2)/STATUS_OFFLINE(3)，
// 非 ONLINE 视为掉线并自动重连（SocketCAN 侧 socket 保持打开，不中断 FSD）。
//
// 用法:
//   zlgcan_bridge [-c <config.yaml>] [--lib <libzlgcan.so>] [-i <iface>] [-v]
//   zlgcan_bridge --selftest [--lib <libzlgcan.so>]   # 虚拟设备(ZCAN_VIRTUAL_DEVICE)自测

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <poll.h>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include "socketcan_port.hpp"
#include "zlgcan_api.hpp"

namespace {

using zlgcan_bridge::SocketCanPort;
using zlgcan_bridge::ZlgCanApi;

// ---------------- 配置 ----------------

struct BridgeConfig {
    std::string zlgcan_library = "libzlgcan.so";
    UINT        device_type    = ZCAN_USBCAN_2E_U;  // 21
    UINT        device_index   = 0;
    UINT        channel        = 0;
    UINT        baud_rate      = 500000;
    std::string socketcan_if   = "can0";
    int         offline_check_ms  = 1000;   // 设备在线检测周期
    int         reconnect_ms      = 2000;   // 重连间隔
    int         stats_interval_ms = 5000;   // 统计打印周期
    int         rx_wait_ms        = 10;     // ZCAN_Receive 等待时间
    int         socket_poll_ms    = 10;     // SocketCAN 收帧轮询
    UINT        batch             = 100;    // 单次收/发批量
};

// 极简 YAML 解析：支持两层嵌套的 "key: value"（# 注释、引号值），
// 解析为点分键（如 zlgcan.baud_rate）。仅覆盖桥接配置所需子集。
bool loadYaml(const std::string& path, std::map<std::string, std::string>& out, std::string* err) {
    std::ifstream in(path);
    if (!in) {
        if (err) *err = "无法打开配置文件: " + path;
        return false;
    }
    std::map<int, std::string> level_key;  // 缩进层级 -> 该层键名
    std::string line;
    while (std::getline(in, line)) {
        auto hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        if (line.find_first_not_of(" \t\r\n") == std::string::npos) continue;

        size_t indent = line.find_first_not_of(' ');
        std::string body = line;
        body.erase(0, indent);
        while (!body.empty() && (body.back() == ' ' || body.back() == '\r')) body.pop_back();

        auto colon = body.find(':');
        if (colon == std::string::npos) continue;
        std::string key = body.substr(0, colon);
        std::string val = body.substr(colon + 1);
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
        while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.erase(0, 1);
        while (!val.empty() && (val.back() == ' ' || val.back() == '\r')) val.pop_back();
        if (!val.empty() && val.front() == '"' && val.back() == '"' && val.size() >= 2)
            val = val.substr(1, val.size() - 2);

        int level = static_cast<int>(indent / 2);
        level_key[level] = key;
        std::string prefix;
        for (int lv = 0; lv < level; ++lv) {
            auto it = level_key.find(lv);
            if (it == level_key.end()) continue;
            if (!prefix.empty()) prefix += ".";
            prefix += it->second;
        }
        if (!prefix.empty()) prefix += ".";
        if (val.empty()) continue;  // 中间节点
        out[prefix + key] = val;
    }
    return true;
}

template <typename T>
bool parseNum(const std::string& s, T* out) {
    try {
        size_t pos = 0;
        unsigned long long v = std::stoull(s, &pos, 0);
        if (pos != s.size()) return false;
        *out = static_cast<T>(v);
        return true;
    } catch (...) {
        return false;
    }
}

BridgeConfig applyOverrides(const std::map<std::string, std::string>& kv, BridgeConfig c) {
    auto get = [&](const char* k) -> const std::string* {
        auto it = kv.find(k);
        return it == kv.end() ? nullptr : &it->second;
    };
    if (auto* v = get("zlgcan.library")) c.zlgcan_library = *v;
    if (auto* v = get("zlgcan.device_type")) parseNum(*v, &c.device_type);
    if (auto* v = get("zlgcan.device_index")) parseNum(*v, &c.device_index);
    if (auto* v = get("zlgcan.channel")) parseNum(*v, &c.channel);
    if (auto* v = get("zlgcan.baud_rate")) parseNum(*v, &c.baud_rate);
    if (auto* v = get("socketcan.interface")) c.socketcan_if = *v;
    if (auto* v = get("bridge.offline_check_ms")) parseNum(*v, &c.offline_check_ms);
    if (auto* v = get("bridge.reconnect_ms")) parseNum(*v, &c.reconnect_ms);
    if (auto* v = get("bridge.stats_interval_ms")) parseNum(*v, &c.stats_interval_ms);
    if (auto* v = get("bridge.rx_wait_ms")) parseNum(*v, &c.rx_wait_ms);
    if (auto* v = get("bridge.socket_poll_ms")) parseNum(*v, &c.socket_poll_ms);
    if (auto* v = get("bridge.batch")) parseNum(*v, &c.batch);
    return c;
}

// ---------------- 日志 ----------------

std::atomic<bool> g_running{true};
std::atomic<bool> g_verbose{false};

void nowStr(char* buf, size_t n) {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    std::strftime(buf, n, "%Y-%m-%d %H:%M:%S", &tm);
}

void logf(const char* level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void logf(const char* level, const char* fmt, ...) {
    char ts[32];
    nowStr(ts, sizeof(ts));
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    std::fprintf(stdout, "[%s] [%s] %s\n", ts, level, msg);
    std::fflush(stdout);
}

void logThrottled(std::string* last, std::chrono::steady_clock::time_point* last_tp,
                  const std::string& msg) {
    auto now = std::chrono::steady_clock::now();
    if (*last != msg || now - *last_tp > std::chrono::seconds(30)) {
        logf("WARN", "%s", msg.c_str());
        *last = msg;
        *last_tp = now;
    }
}

void onSignal(int) { g_running = false; }

// ---------------- 运行状态 ----------------

struct Stats {
    std::atomic<uint64_t> dev2bus{0};   // 设备 -> SocketCAN
    std::atomic<uint64_t> bus2dev{0};   // SocketCAN -> 设备
    std::atomic<uint64_t> drops{0};     // 发送失败丢弃
    std::atomic<uint64_t> reconnects{0};
} g_stats;

ZlgCanApi g_api;
SocketCanPort g_sock;
std::atomic<DEVICE_HANDLE> g_dev{nullptr};
std::atomic<CHANNEL_HANDLE> g_chn{nullptr};
std::atomic<bool> g_forward_run{false};

BridgeConfig g_cfg;

// 设备 -> SocketCAN 转发线程
void rxThread() {
    std::vector<ZCAN_Receive_Data> buf(g_cfg.batch);
    while (g_forward_run) {
        CHANNEL_HANDLE chn = g_chn.load();
        if (chn == nullptr) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        UINT n = g_api.receive(chn, buf.data(), g_cfg.batch, g_cfg.rx_wait_ms);
        for (UINT i = 0; i < n; ++i) {
            if (g_verbose.load()) {
                logf("TRACE", "dev->bus id=0x%X dlc=%d", buf[i].frame.can_id & 0x1FFFFFFFU,
                     buf[i].frame.can_dlc);
            }
            if (g_sock.send(buf[i].frame)) {
                g_stats.dev2bus.fetch_add(1);
            } else {
                g_stats.drops.fetch_add(1);
            }
        }
    }
}

// SocketCAN -> 设备 转发线程
void txThread() {
    std::vector<ZCAN_Transmit_Data> buf(g_cfg.batch);
    while (g_forward_run) {
        CHANNEL_HANDLE chn = g_chn.load();
        if (chn == nullptr) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        pollfd pfd{g_sock.fd(), POLLIN, 0};
        int pr = poll(&pfd, 1, g_cfg.socket_poll_ms);
        if (pr <= 0) continue;
        UINT k = 0;
        struct can_frame f;
        while (k < g_cfg.batch && g_sock.recvNonblock(f) == 1) {
            std::memset(&buf[k], 0, sizeof(ZCAN_Transmit_Data));
            buf[k].frame = f;  // can_id/dlc/data 域与 SocketCAN 完全一致，直接透传
            ++k;
        }
        if (k == 0) continue;
        UINT sent = g_api.transmit(chn, buf.data(), k);
        if (g_verbose.load()) {
            logf("TRACE", "bus->dev 批量 %u 帧，已发 %u", k, sent);
        }
        if (sent < k) {
            g_stats.drops.fetch_add(k - sent);
            static std::string last;
            static std::chrono::steady_clock::time_point last_tp;
            logThrottled(&last, &last_tp, "设备发送失败（部分帧丢弃），已发 " +
                                              std::to_string(sent) + "/" + std::to_string(k));
        }
        g_stats.bus2dev.fetch_add(sent);
    }
}

void startForwardThreads(std::thread* rx, std::thread* tx) {
    g_forward_run = true;
    *rx = std::thread(rxThread);
    *tx = std::thread(txThread);
}

void stopForwardThreads(std::thread* rx, std::thread* tx) {
    g_forward_run = false;
    if (rx->joinable()) rx->join();
    if (tx->joinable()) tx->join();
}

// 断开链路（线程已在调用方停止）
void teardownDevice(DEVICE_HANDLE dev, CHANNEL_HANDLE chn) {
    if (chn != nullptr) g_api.resetCan(chn);  // 忽略返回值
    if (dev != nullptr) g_api.closeDevice(dev);
    g_chn = nullptr;
    g_dev = nullptr;
}

// 建立链路：OpenDevice -> SetValue(baud_rate) -> InitCAN -> StartCAN
bool tryConnect(std::string* hint) {
    hint->clear();
    if (!g_api.loaded()) {
        if (!g_api.load(g_cfg.zlgcan_library)) {
            *hint = g_api.error() +
                    "；请将 Linux 版 libzlgcan.so 放入 zlgcan_bridge/lib/ 或用 --lib 指定路径";
            return false;
        }
        logf("INFO", "SDK 加载成功: %s", g_cfg.zlgcan_library.c_str());
    }

    DEVICE_HANDLE dev = g_api.openDevice(g_cfg.device_type, g_cfg.device_index);
    if (dev == INVALID_DEVICE_HANDLE) {
        *hint = "ZCAN_OpenDevice 失败（type=" + std::to_string(g_cfg.device_type) +
                ", index=" + std::to_string(g_cfg.device_index) +
                "）：请检查设备是否接入/驱动是否安装";
        return false;
    }

    char path[32];
    std::snprintf(path, sizeof(path), "%u/baud_rate", g_cfg.channel);
    UINT ret = g_api.setValue(dev, path, std::to_string(g_cfg.baud_rate).c_str());
    if (ret != STATUS_OK) {
        *hint = "ZCAN_SetValue(" + std::string(path) + "=" +
                std::to_string(g_cfg.baud_rate) + ") 失败";
        g_api.closeDevice(dev);
        return false;
    }

    ZCAN_CHANNEL_INIT_CONFIG cfg;
    std::memset(&cfg, 0, sizeof(cfg));
    cfg.can_type = TYPE_CAN;
    cfg.can.acc_code = 0;
    cfg.can.acc_mask = 0xFFFFFFFF;  // 不滤波，全收
    cfg.can.filter = 0;
    cfg.can.mode = 0;  // 正常模式
    CHANNEL_HANDLE chn = g_api.initCan(dev, g_cfg.channel, &cfg);
    if (chn == INVALID_CHANNEL_HANDLE) {
        *hint = "ZCAN_InitCAN(通道 " + std::to_string(g_cfg.channel) + ") 失败";
        g_api.closeDevice(dev);
        return false;
    }

    ret = g_api.startCan(chn);
    if (ret != STATUS_OK) {
        *hint = "ZCAN_StartCAN 失败";
        g_api.resetCan(chn);
        g_api.closeDevice(dev);
        return false;
    }

    g_api.clearBuffer(chn);
    g_dev = dev;
    g_chn = chn;
    return true;
}

// ---------------- 自测 ----------------

// 虚拟设备(ZCAN_VIRTUAL_DEVICE=99)自测：验证 SDK 加载与调用链完整性，无需真实硬件
int selftest() {
    logf("INFO", "自测开始（虚拟设备 type=%d）", ZCAN_VIRTUAL_DEVICE);
    ZlgCanApi api;
    if (!api.load(g_cfg.zlgcan_library)) {
        logf("ERROR", "%s", api.error().c_str());
        return 1;
    }
    logf("INFO", "[1/5] SDK 加载成功: %s", g_cfg.zlgcan_library.c_str());

    DEVICE_HANDLE dev = api.openDevice(ZCAN_VIRTUAL_DEVICE, 0);
    if (dev == INVALID_DEVICE_HANDLE) {
        logf("ERROR", "[2/5] ZCAN_OpenDevice(虚拟设备) 失败");
        return 1;
    }
    logf("INFO", "[2/5] ZCAN_OpenDevice 成功");

    ZCAN_CHANNEL_INIT_CONFIG cfg;
    std::memset(&cfg, 0, sizeof(cfg));
    cfg.can_type = TYPE_CAN;
    cfg.can.acc_code = 0;
    cfg.can.acc_mask = 0xFFFFFFFF;
    cfg.can.mode = 0;

    char path[32];
    std::snprintf(path, sizeof(path), "%u/baud_rate", g_cfg.channel);
    UINT ret = api.setValue(dev, path, std::to_string(g_cfg.baud_rate).c_str());
    if (ret != STATUS_OK) {
        logf("ERROR", "[3/5] ZCAN_SetValue(%s) 失败", path);
        api.closeDevice(dev);
        return 1;
    }
    logf("INFO", "[3/5] ZCAN_SetValue(%s=%u) 成功", path, g_cfg.baud_rate);

    CHANNEL_HANDLE chn = api.initCan(dev, g_cfg.channel, &cfg);
    if (chn == INVALID_CHANNEL_HANDLE) {
        logf("ERROR", "[4/5] ZCAN_InitCAN 失败");
        api.closeDevice(dev);
        return 1;
    }
    if (api.startCan(chn) != STATUS_OK) {
        logf("ERROR", "[4/5] ZCAN_StartCAN 失败");
        api.resetCan(chn);
        api.closeDevice(dev);
        return 1;
    }
    logf("INFO", "[4/5] ZCAN_InitCAN + ZCAN_StartCAN 成功");

    // 虚拟设备不一定回环发送帧，收发探测结果不作 PASS 依据
    ZCAN_Transmit_Data tf;
    std::memset(&tf, 0, sizeof(tf));
    tf.frame.can_id = 0x210;
    tf.frame.can_dlc = 1;
    tf.frame.data[0] = 0x55;
    UINT sent = api.transmit(chn, &tf, 1);
    ZCAN_Receive_Data rf;
    UINT recvd = api.receive(chn, &rf, 1, 100);
    logf("INFO", "[5/5] 虚拟设备收发探测: 发送=%u 帧, 接收=%u 帧（仅探测，不判定）", sent, recvd);

    api.resetCan(chn);
    api.closeDevice(dev);
    logf("INFO", "自测通过：SDK 与调用链完整，可接入真实设备");
    return 0;
}

// ---------------- main ----------------

void usage(const char* argv0) {
    std::printf(
        "用法: %s [选项]\n"
        "  -c, --config <文件>      配置文件（缺省使用内置默认值）\n"
        "      --lib <路径>         libzlgcan.so 路径（覆盖配置）\n"
        "  -i, --interface <名称>   SocketCAN 接口（覆盖配置，缺省 can0）\n"
        "      --selftest           虚拟设备自测（无需真实硬件与 CAN 接口）\n"
        "  -v, --verbose            打印逐帧转发日志\n"
        "  -h, --help               帮助\n",
        argv0);
}

}  // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    std::signal(SIGPIPE, SIG_IGN);

    BridgeConfig cfg;
    std::string config_path;
    bool selftest_mode = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : ""; };
        if (a == "-c" || a == "--config") {
            config_path = next();
        } else if (a == "--lib") {
            cfg.zlgcan_library = next();
        } else if (a == "-i" || a == "--interface") {
            cfg.socketcan_if = next();
        } else if (a == "--selftest") {
            selftest_mode = true;
        } else if (a == "-v" || a == "--verbose") {
            g_verbose = true;
        } else if (a == "-h" || a == "--help") {
            usage(argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "未知参数: %s\n", a.c_str());
            usage(argv[0]);
            return 2;
        }
    }

    if (!config_path.empty()) {
        std::map<std::string, std::string> kv;
        std::string err;
        if (!loadYaml(config_path, kv, &err)) {
            std::fprintf(stderr, "[CONFIG] %s\n", err.c_str());
            return 2;
        }
        cfg = applyOverrides(kv, cfg);
    }
    g_cfg = cfg;

    if (selftest_mode) return selftest();

    logf("INFO", "ZLGCAN 桥接启动: 设备类型=%u 索引=%u 通道=%u 波特率=%u -> SocketCAN %s",
         cfg.device_type, cfg.device_index, cfg.channel, cfg.baud_rate, cfg.socketcan_if.c_str());

    // SocketCAN 侧先就绪（跨重连保持打开，不影响 FSD/hil_test）
    std::string err;
    if (!g_sock.open(cfg.socketcan_if, &err)) {
        logf("ERROR", "%s", err.c_str());
        return 1;
    }
    logf("INFO", "SocketCAN %s 就绪", cfg.socketcan_if.c_str());

    std::thread rx, tx;
    bool connected = false;
    auto last_stats_tp = std::chrono::steady_clock::now();
    uint64_t last_d2b = 0, last_b2d = 0;
    std::string last_hint;
    std::chrono::steady_clock::time_point last_hint_tp;

    while (g_running) {
        if (!connected) {
            std::string hint;
            if (tryConnect(&hint)) {
                connected = true;
                g_stats.reconnects.fetch_add(1);
                logf("INFO", "设备链路已建立（第 %llu 次连接）",
                     static_cast<unsigned long long>(g_stats.reconnects.load()));
                startForwardThreads(&rx, &tx);
            } else {
                logThrottled(&last_hint, &last_hint_tp, hint);
                for (int waited = 0; g_running && waited < cfg.reconnect_ms; waited += 100)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            continue;
        }

        // 已连接：周期统计 + 在线检测
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        auto now = std::chrono::steady_clock::now();
        if (now - last_stats_tp >= std::chrono::milliseconds(cfg.stats_interval_ms)) {
            auto sec = std::chrono::duration_cast<std::chrono::duration<double>>(now - last_stats_tp).count();
            uint64_t d2b = g_stats.dev2bus.load();
            uint64_t b2d = g_stats.bus2dev.load();
            logf("INFO", "统计: 设备->总线 %.1f 帧/s (累计 %llu), 总线->设备 %.1f 帧/s (累计 %llu), 丢帧 %llu",
                 (d2b - last_d2b) / sec, static_cast<unsigned long long>(d2b),
                 (b2d - last_b2d) / sec, static_cast<unsigned long long>(b2d),
                 static_cast<unsigned long long>(g_stats.drops.load()));
            last_d2b = d2b;
            last_b2d = b2d;
            last_stats_tp = now;
        }

        DEVICE_HANDLE dev = g_dev.load();
        if (dev != nullptr && g_api.isDeviceOnLine(dev) != STATUS_ONLINE) {
            logf("WARN", "ZLG 设备掉线（ZCAN_IsDeviceOnLine != STATUS_ONLINE），准备重连");
            stopForwardThreads(&rx, &tx);
            teardownDevice(dev, g_chn.load());
            connected = false;
        }
    }

    if (connected) {
        stopForwardThreads(&rx, &tx);
        teardownDevice(g_dev.load(), g_chn.load());
    }
    g_sock.close();
    logf("INFO", "桥接退出: 设备->总线 %llu 帧, 总线->设备 %llu 帧, 丢帧 %llu",
         static_cast<unsigned long long>(g_stats.dev2bus.load()),
         static_cast<unsigned long long>(g_stats.bus2dev.load()),
         static_cast<unsigned long long>(g_stats.drops.load()));
    return 0;
}
