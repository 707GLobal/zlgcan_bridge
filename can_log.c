/*
 * can_log.c — 报文按 ID 分类落盘 + 周期/抖动统计。
 *
 * 设计考虑：
 *  - 纯文本、一行一帧、hex 字节用空格分开：人眼优先，tail -f / grep 都直接可用
 *  - (+100.32ms) 列把「距同 ID 上一帧的间隔」直接印在行里，扫一眼就知道周期对不对
 *  - 落盘用 CLOCK_REALTIME（便于和 hil_test / dmesg 对齐），
 *    统计用 CLOCK_MONOTONIC（不受系统时间跳变影响）
 *  - 统计始终开启；只有落盘受 --log-frames 控制
 *  - 表只由一把互斥锁保护：帧率极低（10Hz 级），加锁开销可忽略
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <pthread.h>

#include "can_log.h"

#define LOG_TAG "log"
#include "logger.h"

#define MAX_IDS    64      /* 同时记录的 ID 上限；超出只告警一次并丢弃 */
#define PERIOD_TOL 0.20    /* 周期容差 ±20%，与 hil_test 的 period_tolerance_ratio 一致 */

/*
 * 期望周期表：只有列在这里的 ID 才判定「超差」，其余 ID 只报实测值。
 * 数值来自 hil_test/config/protocol.yaml（0x210 / 0x501 均为 10Hz 保活）。
 */
static const struct {
    can_log_dir_t dir;
    uint32_t      id;
    int           period_ms;
} k_expect[] = {
    { CAN_LOG_VCU2FSD, 0x501, 100 },   /* VCU → 工控机 心跳 */
    { CAN_LOG_FSD2VCU, 0x210, 100 },   /* 工控机 → VCU 心跳 */
};

typedef struct {
    int      used;
    uint32_t id;              /* 原始 CAN-ID（含 EFF/RTR 等标志位） */

    FILE    *fp;              /* NULL = 不落盘（--log-frames off 或打开失败） */

    /* ---- 周期统计 ---- */
    unsigned long long n;      /* 帧数 */
    unsigned long long n_iv;   /* 间隔样本数 */
    int                has_prev;
    struct timespec    prev;   /* 上一帧的单调时间 */
    double             sum_ms;  /* 间隔之和，用于均值 */
    double             sum2_ms; /* 间隔平方和，用于标准差 */
    double             min_ms;
    double             max_ms;
    int                exp_ms;  /* 期望周期(ms)，0 = 未知 */
    unsigned long long n_bad;   /* 超出 ±PERIOD_TOL 的帧数 */
} log_entry_t;

static log_entry_t  g_tab[CAN_LOG_DIR_NUM][MAX_IDS];
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;

static const char  *k_dir_name[CAN_LOG_DIR_NUM] = { "vcu2fsd", "fsd2vcu" };

static int   g_enabled = 0;       /* 是否落盘 */
static char  g_run_dir[512];      /* 本次运行的时间戳目录 */
static int   g_overflow_warned = 0;

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

static int expect_period(can_log_dir_t dir, uint32_t raw_id)
{
    uint32_t id = raw_id & 0x1FFFFFFFu;
    for (size_t i = 0; i < sizeof k_expect / sizeof k_expect[0]; i++) {
        if (k_expect[i].dir == dir && k_expect[i].id == id)
            return k_expect[i].period_ms;
    }
    return 0;
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

    slot->used   = 1;
    slot->id     = raw_id;
    slot->min_ms = 1e9;
    slot->exp_ms = expect_period(dir, raw_id);

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
        LOG_WARN("打开 %s 失败: %s（该 ID 只统计、不落盘）", path, strerror(errno));
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
            LOG_WARN("创建日志目录 %s 失败: %s（本次不落盘，仅统计）",
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
            LOG_WARN("创建日志子目录 %s 失败: %s（本次不落盘，仅统计）",
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

    pthread_mutex_lock(&g_mtx);

    log_entry_t *e = find_or_create(dir, f->id);
    if (!e) {
        pthread_mutex_unlock(&g_mtx);
        return;
    }

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    double dt = -1.0;                       /* <0 表示这是该 ID 的第一帧 */
    if (e->has_prev) {
        dt = (double)(now.tv_sec  - e->prev.tv_sec) * 1000.0
           + (double)(now.tv_nsec - e->prev.tv_nsec) / 1e6;
        if (dt >= 0.0) {
            e->n_iv++;
            e->sum_ms  += dt;
            e->sum2_ms += dt * dt;
            if (dt < e->min_ms) e->min_ms = dt;
            if (dt > e->max_ms) e->max_ms = dt;
            if (e->exp_ms > 0) {
                double tol = e->exp_ms * PERIOD_TOL;
                if (dt < e->exp_ms - tol || dt > e->exp_ms + tol)
                    e->n_bad++;
            }
        }
    }
    e->prev     = now;
    e->has_prev = 1;
    e->n++;

    if (!e->fp) {
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

    /* 间隔列固定 10 字符宽，首帧无前序帧时用等宽占位，保证列对齐 */
    char dtbuf[32];
    if (dt < 0.0)
        snprintf(dtbuf, sizeof dtbuf, "      --  ");
    else
        snprintf(dtbuf, sizeof dtbuf, "+%7.2fms", dt);

    fprintf(e->fp, "%s.%03ld  (%s)  [%u]  %s\n",
            tbuf, wall.tv_nsec / 1000000L, dtbuf, (unsigned)f->dlc, hex);
    fflush(e->fp);

    pthread_mutex_unlock(&g_mtx);
}

unsigned long long can_log_count(can_log_dir_t dir)
{
    if (dir < 0 || dir >= CAN_LOG_DIR_NUM)
        return 0;

    unsigned long long n = 0;
    pthread_mutex_lock(&g_mtx);
    for (int i = 0; i < MAX_IDS; i++)
        n += g_tab[dir][i].n;
    pthread_mutex_unlock(&g_mtx);
    return n;
}

void can_log_report(void)
{
    pthread_mutex_lock(&g_mtx);

    for (int d = 0; d < CAN_LOG_DIR_NUM; d++) {
        for (int i = 0; i < MAX_IDS; i++) {
            log_entry_t *e = &g_tab[d][i];
            if (!e->used || e->n == 0)
                continue;

            char idstr[16];
            id_to_str(e->id, idstr, sizeof idstr);

            if (e->n_iv == 0) {
                logger_emit(LOG_LV_INFO, "stat", "%-7s %s  n=%llu  (帧数不足，尚无周期)",
                            k_dir_name[d], idstr, e->n);
                continue;
            }

            double mean = e->sum_ms / (double)e->n_iv;
            double var  = e->sum2_ms / (double)e->n_iv - mean * mean;
            double std  = var > 0.0 ? sqrt(var) : 0.0;

            if (e->exp_ms > 0) {
                logger_emit(LOG_LV_INFO, "stat",
                            "%-7s %s  n=%-6llu 周期 %.2fms ±%.2f [%.1f~%.1f]  超差 %llu/%llu (期望 %dms ±%d%%)",
                            k_dir_name[d], idstr, e->n, mean, std,
                            e->min_ms, e->max_ms,
                            e->n_bad, e->n_iv,
                            e->exp_ms, (int)(PERIOD_TOL * 100));
            } else {
                logger_emit(LOG_LV_INFO, "stat",
                            "%-7s %s  n=%-6llu 周期 %.2fms ±%.2f [%.1f~%.1f]",
                            k_dir_name[d], idstr, e->n, mean, std,
                            e->min_ms, e->max_ms);
            }
        }
    }

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
