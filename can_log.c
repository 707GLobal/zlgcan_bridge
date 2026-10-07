/*
 * can_log.c — 收到的报文按 ID 分文件落盘。
 *
 * 设计考虑：
 *  - 纯文本、一行一帧、hex 字节用空格分开：人眼优先，tail -f / grep 都直接可用
 *  - 时间戳用 CLOCK_REALTIME（便于和 hil_test / dmesg 对齐）
 *  - 表只由一把互斥锁保护：帧率极低（10Hz 级），加锁开销可忽略
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <pthread.h>

#include "can_log.h"

#define LOG_TAG "log"
#include "logger.h"

#define MAX_IDS 64      /* 同时记录的 ID 上限；超出只告警一次并丢弃 */

typedef struct {
    int      used;
    uint32_t id;              /* 原始 CAN-ID（含 EFF/RTR 等标志位） */
    FILE    *fp;              /* NULL = 不落盘（--log-frames off 或打开失败） */
} log_entry_t;

static log_entry_t  g_tab[CAN_LOG_DIR_NUM][MAX_IDS];
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;

static const char *k_dir_name[CAN_LOG_DIR_NUM] = { "vcu2fsd", "fsd2vcu" };

static int  g_enabled = 0;       /* 是否落盘 */
static char g_run_dir[512];      /* 本次运行的时间戳目录 */
static int  g_overflow_warned = 0;

/* ---- 小工具 ---- */

/* 逐级创建目录；已存在视为成功。 */
static int mkdir_p(const char *path)
{
    char buf[512];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof buf)
        return -1;

    memcpy(buf, path, len + 1);
    if (buf[len - 1] == '/')
        buf[len - 1] = '\0';

    for (char *p = buf + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(buf, 0775) < 0 && errno != EEXIST)
            return -1;
        *p = '/';
    }
    if (mkdir(buf, 0775) < 0 && errno != EEXIST)
        return -1;
    return 0;
}

/*
 * ID 转字符串：标准帧 0x501，扩展帧 0x18FF50E5。
 * 低 29 位是真正的 ID，bit31 是 EFF 标志，显示前先剥掉。
 */
static void id_to_str(uint32_t raw_id, char *out, size_t n)
{
    uint32_t id = raw_id & 0x1FFFFFFFu;
    if (raw_id & 0x80000000u)
        snprintf(out, n, "0x%08X", id);
    else
        snprintf(out, n, "0x%03X", id);
}

/* 查表；首次遇到该 ID 则占一个槽位并（按需）建文件。调用前须持有 g_mtx。 */
static log_entry_t *find_or_create(can_log_dir_t dir, uint32_t raw_id)
{
    log_entry_t *slot = NULL;

    for (int i = 0; i < MAX_IDS; i++) {
        log_entry_t *e = &g_tab[dir][i];
        if (e->used) {
            if (e->id == raw_id)
                return e;
        } else if (!slot) {
            slot = e;
        }
    }

    if (!slot) {
        if (!g_overflow_warned) {
            LOG_WARN("ID 数量已达上限 %d 个，后续新 ID 不再记录", MAX_IDS);
            g_overflow_warned = 1;
        }
        return NULL;
    }

    slot->used = 1;
    slot->id   = raw_id;

    if (!g_enabled)
        return slot;

    char idstr[16], path[700];
    id_to_str(raw_id, idstr, sizeof idstr);
    snprintf(path, sizeof path, "%s/%s/%s.log", g_run_dir, k_dir_name[dir], idstr);

    struct timespec wall;
    clock_gettime(CLOCK_REALTIME, &wall);
    struct tm tm;
    localtime_r(&wall.tv_sec, &tm);
    char tbuf[32];
    strftime(tbuf, sizeof tbuf, "%Y-%m-%d %H:%M:%S", &tm);

    slot->fp = fopen(path, "a");
    if (!slot->fp) {
        LOG_WARN("打开 %s 失败: %s（该 ID 不落盘）", path, strerror(errno));
        return slot;
    }

    fprintf(slot->fp, "# id=%s  dir=%s  start=%s.%03ld\n",
            idstr, k_dir_name[dir], tbuf, wall.tv_nsec / 1000000L);
    fflush(slot->fp);
    LOG_INFO("落盘 %s", path);
    return slot;
}

/* ---- 对外接口 ---- */

int can_log_open(const char *root_dir, int enabled)
{
    if (!root_dir || !*root_dir)
        return -1;

    g_enabled = enabled ? 1 : 0;
    if (!g_enabled)
        return 0;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    char stamp[32];
    strftime(stamp, sizeof stamp, "%Y%m%d_%H%M%S", &tm);

    /* 同一秒内重复启动时加序号后缀，避免两次运行的数据混进同一个目录 */
    char run[512];
    int ok = 0;
    for (int i = 0; i < 100; i++) {
        if (i == 0)
            snprintf(run, sizeof run, "%s/%s", root_dir, stamp);
        else
            snprintf(run, sizeof run, "%s/%s_%d", root_dir, stamp, i);

        struct stat st;
        if (stat(run, &st) == 0)
            continue;                     /* 已被占用，换下一个后缀 */

        if (mkdir_p(run) < 0) {
            LOG_WARN("创建日志目录 %s 失败: %s（本次不落盘）",
                     run, strerror(errno));
            g_enabled = 0;
            return -1;
        }
        ok = 1;
        break;
    }
    if (!ok) {
        LOG_WARN("无法为本次运行分配日志目录（%s 下同秒目录已满）", root_dir);
        g_enabled = 0;
        return -1;
    }

    for (int d = 0; d < CAN_LOG_DIR_NUM; d++) {
        char sub[600];
        snprintf(sub, sizeof sub, "%s/%s", run, k_dir_name[d]);
        if (mkdir_p(sub) < 0) {
            LOG_WARN("创建日志子目录 %s 失败: %s（本次不落盘）",
                     sub, strerror(errno));
            g_enabled = 0;
            return -1;
        }
    }

    snprintf(g_run_dir, sizeof g_run_dir, "%s", run);
    LOG_INFO("报文日志目录: %s", g_run_dir);
    return 0;
}

void can_log_write(can_log_dir_t dir, const bridge_frame_t *f)
{
    if (dir < 0 || dir >= CAN_LOG_DIR_NUM || !f)
        return;
    if (!g_enabled)
        return;

    pthread_mutex_lock(&g_mtx);

    log_entry_t *e = find_or_create(dir, f->id);
    if (!e || !e->fp) {
        pthread_mutex_unlock(&g_mtx);
        return;
    }

    struct timespec wall;
    clock_gettime(CLOCK_REALTIME, &wall);
    struct tm tm;
    localtime_r(&wall.tv_sec, &tm);
    char tbuf[32];
    strftime(tbuf, sizeof tbuf, "%Y-%m-%d %H:%M:%S", &tm);

    /* 注意：snprintf 每次都会补 '\0'，所以必须用游标推进，
     * 不能按 i*3 定位——那样第一个字节的 '\0' 会把整串截断。 */
    char hex[3 * 8 + 1];
    hex[0] = '\0';
    size_t off = 0;
    for (int i = 0; i < f->dlc && i < 8; i++) {
        if (i)
            hex[off++] = ' ';
        off += (size_t)snprintf(hex + off, sizeof hex - off, "%02X", f->data[i]);
    }

    fprintf(e->fp, "%s.%03ld  [%u]  %s\n",
            tbuf, wall.tv_nsec / 1000000L, (unsigned)f->dlc, hex);
    fflush(e->fp);

    pthread_mutex_unlock(&g_mtx);
}

void can_log_close(void)
{
    pthread_mutex_lock(&g_mtx);
    for (int d = 0; d < CAN_LOG_DIR_NUM; d++) {
        for (int i = 0; i < MAX_IDS; i++) {
            log_entry_t *e = &g_tab[d][i];
            if (e->fp) {
                fclose(e->fp);
                e->fp = NULL;
            }
        }
    }
    pthread_mutex_unlock(&g_mtx);
}
