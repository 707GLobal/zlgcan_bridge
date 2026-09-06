// USBCAN_E_U.cpp : 定义控制台应用程序的入口点。
//

#include "stdafx.h"
#include "zlgcan.h"
#include <iostream>
#include <windows.h>
#include <thread>
#include <ctime>


// 打印锁
CRITICAL_SECTION print_mutex;


// 打印加锁宏
#define LOCKED_BLOCK(code) do { \
    EnterCriticalSection(&print_mutex); \
    code; \
    LeaveCriticalSection(&print_mutex); \
} while(0)


// 线程运行标识
int g_thd_run = 1;


// 示例 构造CAN报文       
void Construct_CAN_Frame(ZCAN_Transmit_Data& can_data, canid_t id, UINT delay = 0)
{
	memset(&can_data, 0, sizeof(can_data));
	can_data.frame.can_id = MAKE_CAN_ID(id, 0, 0, 0);
	can_data.frame.can_dlc = 8;             // 数据长度
	can_data.transmit_type = 0;             // 正常发送

	for (int i = 0; i < 8; ++i){
		can_data.frame.data[i] = i;
	}
}


// 普通接收线程
void thread_task(CHANNEL_HANDLE chn)
{
	ZCAN_Receive_Data canData[100] = {};
	ZCAN_ReceiveFD_Data canfdData[100] = {};
	int chn_idx = (unsigned int)chn & 0x000000FF;	// 通道号

	while (g_thd_run)
	{
		if (ZCAN_GetReceiveNum(chn, 0))		// 0-CAN
		{
			uint32_t ReceiveNum = ZCAN_Receive(chn, canData, 100, 10);
			for (int i = 0; i < ReceiveNum; i++) {
				LOCKED_BLOCK({
					printf("CHN:%d [%lld] CAN  ", chn_idx, canData[i].timestamp);		// 通道和时间戳

					printf(IS_EFF(canData[i].frame.can_id) ? "扩展帧 " : "标准帧 ");		// 帧类型
					//printf(IS_RTR(canData[i].frame.can_id) ? " 远程帧 " : " 数据帧 ");	// 帧格式

					printf("ID:0x%X Data: ", GET_ID(canData[i].frame.can_id));			// ID 
					for (int j = 0; j < canData[i].frame.can_dlc; j++){					// 数据
						printf("%02X ", canData[i].frame.data[j]);
					}
					printf("\n");
				});
			}
		}

		Sleep(10);
	}
}


// 发送示例
void Send_test( CHANNEL_HANDLE chn){
	const int send_num = 10;
	int send_count = 0;

	// CAN
	ZCAN_Transmit_Data canData[send_num];
	memset(canData, 0, sizeof(canData));
	for (int i = 0; i < send_num; ++i) {
		Construct_CAN_Frame(canData[i], i);
	}
	send_count = ZCAN_Transmit(chn, canData, send_num);

	LOCKED_BLOCK({
		printf("\n发送 %d 条CAN报文\n", send_count);
	});
}


// 定时发送（只有USBCAN-2E-U、USBCAN-4E-U、USBCAN-8E-U、CANalyst-II+支持）
void AutoSend_test(DEVICE_HANDLE dev)
{
	// CAN
	ZCAN_AUTO_TRANSMIT_OBJ auto_can;
	memset(&auto_can, 0, sizeof(auto_can));
	auto_can.index = 0;								// 索引，用来区分不同的定时任务
	auto_can.enable = 1;							// 1-使能 0-关闭
	auto_can.interval = 500;						// 周期，单位ms
	Construct_CAN_Frame(auto_can.obj, 0x100);				// 构造报文，和发送的方式一样
	if (0 == ZCAN_SetValue(dev, "0/auto_send", &auto_can)){
		printf("设置定时发送 CAN 失败\n");
	}
	else{
		printf("设置定时发送 CAN 成功\n");
	}

	//// 定时发送过程中，可以直接覆盖修改
	//Sleep(2000);
	//memset(&auto_can, 0, sizeof(auto_can));
	//auto_can.index = 0;
	//auto_can.enable = 1;
	//auto_can.interval = 1000;
	//Construct_CAN_Frame(auto_can.obj, 0x555);
	//ZCAN_SetValue(dev, "0/auto_send", (const char*)&auto_can);			// 设置定时发送
}


// 初始化 USBCAN 通道
CHANNEL_HANDLE Init_chn_USBCAN_E_U(DEVICE_HANDLE dev, int chn_idx)
{
	// 请按照以下顺序配置设备
	CHANNEL_HANDLE chn = nullptr;
	char path[24] = {};

	// 设置通道 波特率
	sprintf_s(path, 24, "%d/baud_rate", chn_idx);
	if (0 == ZCAN_SetValue(dev, path, "500000")) {
		printf("设置波特率失败\n");
	}

	// USBCAN-E-U系列电阻需要自行外接。

	// 初始化通道
	ZCAN_CHANNEL_INIT_CONFIG config;	// 通道结构体
	memset(&config, 0, sizeof(config));
	config.can_type = 0;				// 0 = CAN，CAN设备必须是初始化CAN!!
	config.can.mode = 0;				// 0-正常模式，1-只听模式
	//config.can.filter = 1;				//开启滤波
	config.can.acc_code = 0;			// 验收码（USBCAN-8E-U 必须设置验收码和屏蔽码，本系列其他型号可忽略）
	config.can.acc_mask = 0xffffffff;	// 屏蔽码

	chn = ZCAN_InitCAN(dev, chn_idx, &config);
	if (chn == INVALID_CHANNEL_HANDLE) {
		printf("初始化通道失败\n");
		return nullptr;
	}

	// 滤波部分（USBCAN-8E-U 不支持通过这种方式设置滤波）
#if 0
	// 清除滤波
	sprintf_s(path, "%d/filter_clear", chn_idx);
	if (ZCAN_SetValue(dev, path, "0") == STATUS_ERR) {
		printf("清除滤波失败\n");
		return nullptr;
	}

	// 设置第一组滤波，只接收 ID 范围之间的标准帧
	sprintf_s(path, "%d/filter_mode", chn_idx);
	if (ZCAN_SetValue(dev, path, "0") == STATUS_ERR) {		// 0-标准帧
		printf("设置标准帧滤波失败\n");
		return nullptr;
	}
	sprintf_s(path, "%d/filter_start", chn_idx);
	if (ZCAN_SetValue(dev, path, "0x01") == STATUS_ERR) {	// 起始 ID
		printf("设置标准帧起始 ID 失败\n");
		return nullptr;
	}
	sprintf_s(path, "%d/filter_end", chn_idx);
	if (ZCAN_SetValue(dev, path, "0x10") == STATUS_ERR) {	// 结束 ID
		printf("设置标准帧结束 ID 失败\n");
		return nullptr;
	}

	// 设置第二组滤波，只接收 ID 范围在 0x1FFFF-0x2FFFF 之间的扩展帧
	sprintf_s(path, "%d/filter_mode", chn_idx);
	if (ZCAN_SetValue(dev, path, "1") == STATUS_ERR) {		// 1-扩展帧
		printf("设置扩展帧滤波失败\n");
		return nullptr;
	}
	sprintf_s(path, "%d/filter_start", chn_idx);
	if (ZCAN_SetValue(dev, path, "0x1FF") == STATUS_ERR) {
		printf("设置扩展帧起始 ID 失败\n");
		return nullptr;
	}
	sprintf_s(path, "%d/filter_end", chn_idx);
	if (ZCAN_SetValue(dev, path, "0x2FF") == STATUS_ERR) {
		printf("设置扩展帧结束 ID 失败\n");
		return nullptr;
	}

	// 使能滤波
	sprintf_s(path, "%d/filter_ack", chn_idx);
	if (ZCAN_SetValue(dev, path, "0") == STATUS_ERR) {
		printf("使能滤波失败\n");
		return nullptr;
	}
#endif

	// 启动通道
	if (ZCAN_StartCAN(chn) == STATUS_ERR) {
		printf("开启通道失败\n");
		return nullptr;
	}

	return chn;
}


int _tmain(int argc, _TCHAR* argv[])
{
	InitializeCriticalSection(&print_mutex);

	const int chn_max = 2;				// 通道数量
	std::thread thd_busload;			// 总线利用率线程
	std::thread thd_handle[chn_max];	// 接收线程
	CHANNEL_HANDLE chn[chn_max] = {};	// 通道句柄
	bool isMergeRec = 0;				// 是否合并接收

	// 打开设备
	DEVICE_HANDLE dev = ZCAN_OpenDevice(ZCAN_USBCAN_2E_U, 0, 0);
	if (dev == INVALID_DEVICE_HANDLE) {
		printf("打开设备失败\n");
		system("pause");
		return 0;
	}

	// 初始化通道
	for (int i = 0; i < chn_max; i++) {
		chn[i] = Init_chn_USBCAN_E_U(dev, i);		// 一定要一个通道初始化完，再初始化另外一个通道！
		if (chn[i] == nullptr){
			printf("初始化通道失败\n");
			goto end;
		}

		thd_handle[i] = std::thread(thread_task, chn[i]);		// 每个通道一个线程接收
	}

	//  发送示例
	Send_test(chn[0]);

	//// 定时发送示例（USBCAN-2E-U开启定时发送期间不能再调用ZCAN_Transmit 进行普通发送）
	//AutoSend_test(dev);

end:
	getchar();											// 回车结束
	ZCAN_SetValue(dev, "0/clear_auto_send", "0");		// 清除定时发送

	g_thd_run = 0;										// 线程运行标识
	if (thd_busload.joinable())							// 总线利用率线程
		thd_busload.join();

	for (int i = 0; i < chn_max; i++) {
		if (thd_handle[i].joinable())					// 接收线程
			thd_handle[i].join();

		if (STATUS_ERR == ZCAN_ResetCAN(chn[i]))		// 关闭通道
			printf("关闭通道失败");
	}
	if (STATUS_ERR == ZCAN_CloseDevice(dev))			// 关闭设备
		printf("关闭设备失败");

	//system("pause");
	DeleteCriticalSection(&print_mutex);
	return 0;
}

