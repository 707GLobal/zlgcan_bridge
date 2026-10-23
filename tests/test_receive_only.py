"""Run the real bridge main with fake device/socket IO; never open USB or CAN."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def test_bridge_receives_logs_and_never_reads_outbound_socket(tmp_path):
    stub = tmp_path / 'io.c'
    stub.write_text(r'''
#include "zlg_bridge.h"
#include "can_log.h"
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
static int received, logged, forwarded;
uid_t getuid(void) {return 0;}
int zlg_bridge_open(unsigned a, unsigned b, unsigned c, const char *d, int receive_only) {
  (void)a;(void)b;(void)c;(void)d; if (!receive_only) abort(); return 0;
}
void zlg_bridge_close(void) {if (logged != 1 || forwarded != 1) abort();}
int zlg_bridge_tx(const bridge_frame_t *f) {(void)f; abort();}
int zlg_bridge_rx(bridge_frame_t *f, int timeout) {
  (void)timeout;
  if (!received++) {f->id=0x501; f->dlc=1; f->data[0]=1; return 1;}
  raise(SIGTERM); return 0;
}
int can_side_open(const char *name) {(void)name; return 0;}
void can_side_close(void) {}
int can_side_rx(bridge_frame_t *f) {(void)f; abort();}
int can_side_tx(const bridge_frame_t *f) {if (f->id != 0x501 || f->data[0] != 1) abort(); ++forwarded; return 0;}
int mode_cache_open(const char *d, const char *i) {(void)d;(void)i;return 0;}
void mode_cache_update(const bridge_frame_t *f) {(void)f;}
void mode_cache_close(void) {}
int can_log_open(const char *d, int enabled) {(void)d;if (!enabled) abort();return 0;}
void can_log_close(void) {}
void can_log_write(can_log_dir_t d, const bridge_frame_t *f) {
  if (d != CAN_LOG_VCU2FSD || f->id != 0x501) abort(); ++logged;
}
''')
    binary = tmp_path / 'bridge'
    subprocess.run(['cc', '-Wall', '-Wextra', '-I', str(ROOT), str(ROOT/'main.c'),
                    str(ROOT/'logger.c'), str(stub), '-pthread', '-o', str(binary)], check=True, capture_output=True)
    result = subprocess.run([str(binary), '--receive-only'], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stdout + result.stderr
    assert 'RECEIVE ONLY' in result.stdout
    assert 'FSD->VCU:0' in result.stdout


def test_device_send_guard_and_normal_mode(tmp_path):
    stub = tmp_path / 'vendor.c'
    stub.write_text(r'''
#include <zlgcan/zlgcan.h>
static int transmissions;
DEVICE_HANDLE ZCAN_OpenDevice(UINT a, UINT b, UINT c) {(void)a;(void)b;(void)c;return (DEVICE_HANDLE)1;}
INT ZCAN_CloseDevice(DEVICE_HANDLE h) {(void)h;return 0;}
INT ZCAN_GetDeviceInf(DEVICE_HANDLE h, ZCAN_DEVICE_INFO *i) {(void)h;(void)i;return -1;}
IProperty *GetIProperty(DEVICE_HANDLE h) {(void)h;return 0;}
CHANNEL_HANDLE ZCAN_InitCAN(DEVICE_HANDLE h, UINT c, ZCAN_CHANNEL_INIT_CONFIG *i) {(void)h;(void)c;(void)i;return (CHANNEL_HANDLE)2;}
INT ZCAN_StartCAN(CHANNEL_HANDLE h) {(void)h;return 0;}
INT ZCAN_ResetCAN(CHANNEL_HANDLE h) {(void)h;return 0;}
INT ZCAN_Receive(CHANNEL_HANDLE h, ZCAN_Receive_Data *f, UINT n, INT t) {(void)h;(void)f;(void)n;(void)t;return 0;}
INT ZCAN_Transmit(CHANNEL_HANDLE h, ZCAN_Transmit_Data *f, UINT n) {(void)h;(void)f;++transmissions;return n;}
/* Keep vendor and Linux frame headers in separate compilation units. */
int vendor_transmissions(void) {return transmissions;}
''')
    probe = tmp_path / 'probe.c'
    probe.write_text(r'''
#include "zlg_bridge.h"
int vendor_transmissions(void);
int main(void) {
  bridge_frame_t frame = {.id=0x210, .dlc=8};
  if (zlg_bridge_open(31,0,0,"500000",1)) return 1;
  if (zlg_bridge_tx(&frame) != -1 || vendor_transmissions()) return 2;
  /* A failed duplicate open cannot weaken the active receive-only session. */
  if (zlg_bridge_open(31,0,0,"500000",0) != -1) return 3;
  if (zlg_bridge_tx(&frame) != -1 || vendor_transmissions()) return 4;
  zlg_bridge_close();
  if (zlg_bridge_open(31,0,0,"500000",0)) return 5;
  if (zlg_bridge_tx(&frame) || vendor_transmissions() != 1) return 6;
  zlg_bridge_close(); return 0;
}
''')
    binary = tmp_path / 'device'
    subprocess.run(['cc', '-Wall', '-Wextra', '-I', str(ROOT), str(ROOT/'zlg_side.c'),
                    str(ROOT/'logger.c'), str(stub), str(probe), '-pthread', '-o', str(binary)], check=True, capture_output=True)
    subprocess.run([str(binary)], check=True, capture_output=True, timeout=5)
