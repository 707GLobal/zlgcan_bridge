#include "zlgcan_api.hpp"

#include <dlfcn.h>

namespace zlgcan_bridge {

namespace {

// 函数指针签名（与 zlgcan.h 中声明保持一致，Linux 下 FUNC_CALL 为空）
// 命名规则：PFN_ + 库符号名，与 ZLG_LOAD_SYM 宏的 PFN_##name 拼接一致
typedef DEVICE_HANDLE (*PFN_ZCAN_OpenDevice)(UINT, UINT, UINT);
typedef UINT (*PFN_ZCAN_CloseDevice)(DEVICE_HANDLE);
typedef UINT (*PFN_ZCAN_SetValue)(DEVICE_HANDLE, const char*, const void*);
typedef UINT (*PFN_ZCAN_IsDeviceOnLine)(DEVICE_HANDLE);
typedef CHANNEL_HANDLE (*PFN_ZCAN_InitCAN)(DEVICE_HANDLE, UINT, ZCAN_CHANNEL_INIT_CONFIG*);
typedef UINT (*PFN_ZCAN_StartCAN)(CHANNEL_HANDLE);
typedef UINT (*PFN_ZCAN_ResetCAN)(CHANNEL_HANDLE);
typedef UINT (*PFN_ZCAN_ClearBuffer)(CHANNEL_HANDLE);
typedef UINT (*PFN_ZCAN_GetReceiveNum)(CHANNEL_HANDLE, BYTE);
typedef UINT (*PFN_ZCAN_Receive)(CHANNEL_HANDLE, ZCAN_Receive_Data*, UINT, int);
typedef UINT (*PFN_ZCAN_Transmit)(CHANNEL_HANDLE, ZCAN_Transmit_Data*, UINT);

}  // namespace

ZlgCanApi::~ZlgCanApi() { unload(); }

bool ZlgCanApi::load(const std::string& library_path) {
    error_.clear();
    if (handle_ != nullptr) return true;

    handle_ = dlopen(library_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle_ == nullptr) {
        error_ = "dlopen(" + library_path + ") 失败: " + dlerror();
        return false;
    }

    bool ok = true;
#define ZLG_LOAD_SYM(var, name)                                       \
    do {                                                              \
        var = reinterpret_cast<PFN_##name>(dlsym(handle_, #name));    \
        if (var == nullptr) {                                         \
            error_ += std::string(error_.empty() ? "" : "; ") +       \
                      "缺少符号 " #name;                              \
            ok = false;                                               \
        }                                                             \
    } while (0)

    PFN_ZCAN_OpenDevice fn_open = nullptr;
    PFN_ZCAN_CloseDevice fn_close = nullptr;
    PFN_ZCAN_SetValue fn_set = nullptr;
    PFN_ZCAN_IsDeviceOnLine fn_online = nullptr;
    PFN_ZCAN_InitCAN fn_init = nullptr;
    PFN_ZCAN_StartCAN fn_start = nullptr;
    PFN_ZCAN_ResetCAN fn_reset = nullptr;
    PFN_ZCAN_ClearBuffer fn_clear = nullptr;
    PFN_ZCAN_GetReceiveNum fn_rxnum = nullptr;
    PFN_ZCAN_Receive fn_recv = nullptr;
    PFN_ZCAN_Transmit fn_send = nullptr;

    ZLG_LOAD_SYM(fn_open, ZCAN_OpenDevice);
    ZLG_LOAD_SYM(fn_close, ZCAN_CloseDevice);
    ZLG_LOAD_SYM(fn_set, ZCAN_SetValue);
    ZLG_LOAD_SYM(fn_online, ZCAN_IsDeviceOnLine);
    ZLG_LOAD_SYM(fn_init, ZCAN_InitCAN);
    ZLG_LOAD_SYM(fn_start, ZCAN_StartCAN);
    ZLG_LOAD_SYM(fn_reset, ZCAN_ResetCAN);
    ZLG_LOAD_SYM(fn_clear, ZCAN_ClearBuffer);
    ZLG_LOAD_SYM(fn_rxnum, ZCAN_GetReceiveNum);
    ZLG_LOAD_SYM(fn_recv, ZCAN_Receive);
    ZLG_LOAD_SYM(fn_send, ZCAN_Transmit);
#undef ZLG_LOAD_SYM

    if (!ok) {
        dlclose(handle_);
        handle_ = nullptr;
        error_ = "动态库符号不完整（请确认 libzlgcan.so 版本与手册一致）: " + error_;
        return false;
    }

    fn_open_ = fn_open;
    fn_close_ = fn_close;
    fn_set_ = fn_set;
    fn_online_ = fn_online;
    fn_init_ = fn_init;
    fn_start_ = fn_start;
    fn_reset_ = fn_reset;
    fn_clear_ = fn_clear;
    fn_rxnum_ = fn_rxnum;
    fn_recv_ = fn_recv;
    fn_send_ = fn_send;
    return true;
}

void ZlgCanApi::unload() {
    if (handle_ != nullptr) {
        dlclose(handle_);
        handle_ = nullptr;
    }
}

DEVICE_HANDLE ZlgCanApi::openDevice(UINT device_type, UINT device_index) {
    return fn_open_(device_type, device_index, 0);
}

UINT ZlgCanApi::closeDevice(DEVICE_HANDLE dev) { return fn_close_(dev); }

UINT ZlgCanApi::setValue(DEVICE_HANDLE dev, const char* path, const void* value) {
    return fn_set_(dev, path, value);
}

UINT ZlgCanApi::isDeviceOnLine(DEVICE_HANDLE dev) { return fn_online_(dev); }

CHANNEL_HANDLE ZlgCanApi::initCan(DEVICE_HANDLE dev, UINT can_index, ZCAN_CHANNEL_INIT_CONFIG* cfg) {
    return fn_init_(dev, can_index, cfg);
}

UINT ZlgCanApi::startCan(CHANNEL_HANDLE chn) { return fn_start_(chn); }

UINT ZlgCanApi::resetCan(CHANNEL_HANDLE chn) { return fn_reset_(chn); }

UINT ZlgCanApi::clearBuffer(CHANNEL_HANDLE chn) { return fn_clear_(chn); }

UINT ZlgCanApi::getReceiveNum(CHANNEL_HANDLE chn, BYTE type) { return fn_rxnum_(chn, type); }

UINT ZlgCanApi::receive(CHANNEL_HANDLE chn, ZCAN_Receive_Data* buf, UINT len, int wait_ms) {
    return fn_recv_(chn, buf, len, wait_ms);
}

UINT ZlgCanApi::transmit(CHANNEL_HANDLE chn, ZCAN_Transmit_Data* buf, UINT len) {
    return fn_send_(chn, buf, len);
}

}  // namespace zlgcan_bridge
