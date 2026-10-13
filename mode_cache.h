#ifndef MODE_CACHE_H
#define MODE_CACHE_H
#include "zlg_bridge.h"
int mode_cache_open(const char *directory, const char *iface);
void mode_cache_update(const bridge_frame_t *frame);
void mode_cache_close(void);
#endif
