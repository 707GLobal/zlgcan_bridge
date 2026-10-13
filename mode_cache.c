#define _GNU_SOURCE
#include "mode_cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#define LOG_TAG "cache"
#include "logger.h"
static int lock_fd = -1;
static char path[512], interface[64], boot[64], session[64];
static unsigned long long start_ticks, sequence;
static time_t last_warning;
static unsigned long long ns(clockid_t clock) {
    struct timespec t; clock_gettime(clock, &t);
    return (unsigned long long)t.tv_sec * 1000000000ULL + t.tv_nsec;
}
static int write_snapshot(int valid, unsigned mode) {
    char temp[600]; snprintf(temp, sizeof temp, "%s.tmp.%ld", path, (long)getpid());
    int fd = open(temp, O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC, 0644);
    if (fd < 0) return -1;
    FILE *out = fdopen(fd, "w");
    if (!out) { close(fd); unlink(temp); return -1; }
    int rc = fprintf(out,
        "version=1\nboot_id=%s\nsession_id=%s\npid=%ld\nprocess_start_ticks=%llu\n"
        "iface=%s\ncan_id=1281\nvalid=%d\nmode=%u\nsequence=%llu\n"
        "received_monotonic_ns=%llu\nreceived_unix_ns=%llu\n",
        boot, session, (long)getpid(), start_ticks, interface, valid, mode, sequence,
        valid ? ns(CLOCK_MONOTONIC) : 0, valid ? ns(CLOCK_REALTIME) : 0);
    if (fchmod(fd, 0644) < 0) rc = -1;
    if (fclose(out) != 0) rc = -1;
    if (rc >= 0 && rename(temp, path) == 0) return 0;
    unlink(temp); return -1;
}
int mode_cache_open(const char *directory, const char *iface) {
    if (!*iface || strlen(iface) >= sizeof interface) return -1;
    for (const unsigned char *p=(const unsigned char *)iface; *p; ++p)
        if (!isalnum(*p) && *p!='_' && *p!='-' && *p!='.') return -1;
    if (!strcmp(iface,".") || !strcmp(iface,"..")) return -1;
    int created = mkdir(directory,0755);
    if (created < 0 && errno != EEXIST) return -1;
    if (created == 0 && chmod(directory,0755) < 0) return -1;
    struct stat st;
    if (lstat(directory,&st) < 0 || !S_ISDIR(st.st_mode)) return -1;
    if (snprintf(path,sizeof path,"%s/%s.mode",directory,iface) >= (int)sizeof path) return -1;
    char lock[600]; snprintf(lock,sizeof lock,"%s.lock",path);
    lock_fd=open(lock,O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0644);
    if (lock_fd < 0) return -1;
    if (flock(lock_fd,LOCK_EX|LOCK_NB) < 0) { close(lock_fd); lock_fd=-1; return -1; }
    FILE *f=fopen("/proc/sys/kernel/random/boot_id","r");
    if (!f) goto fail;
    int ok=fscanf(f,"%63s",boot); fclose(f); if (ok!=1) goto fail;
    f=fopen("/proc/self/stat","r"); if (!f) goto fail;
    char line[4096]; char *r=fgets(line,sizeof line,f); fclose(f); if (!r) goto fail;
    r=strrchr(line,')'); if (!r) goto fail;
    char *save=NULL; r=strtok_r(r+2," ",&save);
    for (int field=3; field<22 && r; ++field) r=strtok_r(NULL," ",&save);
    if (!r) goto fail;
    start_ticks=strtoull(r,NULL,10);
    f=fopen("/proc/sys/kernel/random/uuid","r"); if (!f) goto fail;
    ok=fscanf(f,"%63s",session); fclose(f); if (ok!=1) goto fail;
    snprintf(interface,sizeof interface,"%s",iface); sequence=0;
    if (write_snapshot(0,0)<0) goto fail;
    LOG_INFO("模式快照: %s (session=%s)",path,session); return 0;
fail:
    unlink(path); close(lock_fd); lock_fd=-1; return -1;
}
void mode_cache_update(const bridge_frame_t *frame) {
    if (lock_fd<0 || frame->id!=0x501 || frame->dlc<1 || frame->dlc>8) return;
    ++sequence;
    if (write_snapshot(1,frame->data[0])<0) {
        unlink(path); /* 禁止使用此前已失效的模式 */
        time_t now=time(NULL);
        if (now-last_warning>=5) { LOG_WARN("模式快照更新失败，实时转发继续: %s",strerror(errno)); last_warning=now; }
    }
}
void mode_cache_close(void) {
    if (lock_fd<0) return;
    unlink(path); close(lock_fd); lock_fd=-1;
}
