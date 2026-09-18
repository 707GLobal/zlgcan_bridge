/*
 * logger.c — 统一控制台日志实现。
 *
 * 要点：
 *  - 时间戳取自 CLOCK_REALTIME，毫秒精度
 *  - 级别固定 5 字符宽、模块标签固定 4 字符宽，便于对齐扫读
 *  - 全部走 stdout 单流，重定向到文件后顺序不会乱
 *  - stdout 显式设为行缓冲，保证重定向时也逐行落盘
 *  - 一把互斥锁保护整行输出，两个转发线程不会把行撕开
 */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <pthread.h>
#include "logger.h"

static int g_level = LOG_LV_INFO;
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;

/* 固定 5 字符宽，保证 [INFO ] 与 [ERROR] 对齐 */
static const char *const k_lv_name[] = { "ERROR", "WARN ", "INFO ", "DEBUG" };

void logger_init(int level)
{
    g_level = level;
    /* 接终端时 stdout 本来就是行缓冲；重定向到文件时默认变全缓冲，
     * 这里显式设成行缓冲，日志才能实时落盘（也省掉了各处手写 fflush）。 */
    setvbuf(stdout, NULL, _IOLBF, 0);
}

void logger_set_level(int level)
{
    g_level = level;
}

int logger_level_from_str(const char *s)
{
    if (!s)
        return -1;
    if (!strcasecmp(s, "error")) return LOG_LV_ERROR;
    if (!strcasecmp(s, "warn"))  return LOG_LV_WARN;
    if (!strcasecmp(s, "info"))  return LOG_LV_INFO;
    if (!strcasecmp(s, "debug")) return LOG_LV_DEBUG;
    return -1;
}

void logger_emit(int lv, const char *tag, const char *fmt, ...)
{
    if (lv < 0 || lv > LOG_LV_DEBUG || lv > g_level)
        return;

    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    char tbuf[32];
    strftime(tbuf, sizeof tbuf, "%Y-%m-%d %H:%M:%S", &tm);

    char line[1200];
    snprintf(line, sizeof line, "%s.%03ld [%s] [%-4s] %s\n",
             tbuf, ts.tv_nsec / 1000000L, k_lv_name[lv], tag, msg);

    pthread_mutex_lock(&g_mtx);
    fputs(line, stdout);
    pthread_mutex_unlock(&g_mtx);
}
