#ifndef ZLGCAN_API_HPP_
#define ZLGCAN_API_HPP_

#include <string>

#include "zlgcan.h"

namespace zlgcan_bridge {

// libzlgcan.so 动态库加载与 API 封装（dlopen/dlsym）
// 调用顺序须遵循手册：OpenDevice -> SetValue("n/baud_rate") -> InitCAN -> StartCAN
class ZlgCanApi {
public:
    ZlgCanApi() = default;
    ~ZlgCanApi();
    ZlgCanApi(const ZlgCanApi&) = delete;
    ZlgCanApi& operator=(const ZlgCanApi&) = delete;

    // 加载动态库并解析全部所需符号；失败返回 false（error() 给出原因）
    bool load(const std::string& library_path);
    bool loaded() const { return handle_ != nullptr; }
    // 卸载动态库（重连换库路径/进程退出前调用）
    void unload();
    const std::string& error() const { return error_; }

    // ---- 设备/通道操作 ----
    DEVICE_HANDLE openDevice(UINT device_type, UINT device_index);
    UINT closeDevice(DEVICE_HANDLE dev);
    UINT setValue(DEVICE_HANDLE dev, const char* path, const void* value);
    UINT isDeviceOnLine(DEVICE_HANDLE dev);

    CHANNEL_HANDLE initCan(DEVICE_HANDLE dev, UINT can_index, ZCAN_CHANNEL_INIT_CONFIG* cfg);
    UINT startCan(CHANNEL_HANDLE chn);
    UINT resetCan(CHANNEL_HANDLE chn);
    UINT clearBuffer(CHANNEL_HANDLE chn);

    UINT getReceiveNum(CHANNEL_HANDLE chn, BYTE type);
    UINT receive(CHANNEL_HANDLE chn, ZCAN_Receive_Data* buf, UINT len, int wait_ms);
    UINT transmit(CHANNEL_HANDLE chn, ZCAN_Transmit_Data* buf, UINT len);

private:
    void* handle_ = nullptr;
    std::string error_;

    // 解析出的库函数指针（load() 成功后有效）
    DEVICE_HANDLE (*fn_open_)(UINT, UINT, UINT) = nullptr;
    UINT (*fn_close_)(DEVICE_HANDLE) = nullptr;
    UINT (*fn_set_)(DEVICE_HANDLE, const char*, const void*) = nullptr;
    UINT (*fn_online_)(DEVICE_HANDLE) = nullptr;
    CHANNEL_HANDLE (*fn_init_)(DEVICE_HANDLE, UINT, ZCAN_CHANNEL_INIT_CONFIG*) = nullptr;
    UINT (*fn_start_)(CHANNEL_HANDLE) = nullptr;
    UINT (*fn_reset_)(CHANNEL_HANDLE) = nullptr;
    UINT (*fn_clear_)(CHANNEL_HANDLE) = nullptr;
    UINT (*fn_rxnum_)(CHANNEL_HANDLE, BYTE) = nullptr;
    UINT (*fn_recv_)(CHANNEL_HANDLE, ZCAN_Receive_Data*, UINT, int) = nullptr;
    UINT (*fn_send_)(CHANNEL_HANDLE, ZCAN_Transmit_Data*, UINT) = nullptr;
};

}  // namespace zlgcan_bridge

#endif  // ZLGCAN_API_HPP_
