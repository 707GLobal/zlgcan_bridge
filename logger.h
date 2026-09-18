#ifndef LOGGER_H_
#define LOGGER_H_

/*
 * logger.h — 统一控制台日志：时间戳 + 级别 + 模块标签。
 *
 * 用法：在 .c 文件里先 #define LOG_TAG "模块名"，再 include 本头文件，
 *       然后用 LOG_ERR / LOG_WARN / LOG_INFO / LOG_DBG 打印。
 *
 * 输出示例：
 *   2026-09-18 16:33:00.123 [INFO ] [main] 就绪: ZLG(type=31 ...) <-> SocketCAN can0
 */

enum {
    LOG_LV_ERROR = 0,
    LOG_LV_WARN  = 1,
    LOG_LV_INFO  = 2,
    LOG_LV_DEBUG = 3,
};

/* 初始化：低于 level 的日志被丢弃。会顺带把 stdout 设为行缓冲。 */
void logger_init(int level);
void logger_set_level(int level);

/* 解析 "debug"/"info"/"warn"/"error"（不区分大小写）；无法识别返回 -1。 */
int  logger_level_from_str(const char *s);

/* 一般不用直接调，用下面的宏。 */
void logger_emit(int lv, const char *tag, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

#ifndef LOG_TAG
#define LOG_TAG "app"
#endif

#define LOG_ERR(...)  logger_emit(LOG_LV_ERROR, LOG_TAG, __VA_ARGS__)
#define LOG_WARN(...) logger_emit(LOG_LV_WARN,  LOG_TAG, __VA_ARGS__)
#define LOG_INFO(...) logger_emit(LOG_LV_INFO,  LOG_TAG, __VA_ARGS__)
#define LOG_DBG(...)  logger_emit(LOG_LV_DEBUG, LOG_TAG, __VA_ARGS__)

#endif /* LOGGER_H_ */
