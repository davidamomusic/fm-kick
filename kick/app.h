/* SPDX-License-Identifier: MPL-2.0 */
/* FM-KICK: a kick drum synthesizer and generative sequencer for the FM-1. */
#pragma once
#include <stdint.h>
#include "plat.h"

#define APP_NAME "FM-KICK"
#define APP_VERSION "0.5.1"
#define APP_SPLASH_RGB RGB(255, 72, 28)
#define APP_REDRAW                       /* the screen mirror asks for the screen again when a slow link lost a part */
void app_redraw(void);
#ifndef PLAT_HOST
#define MRQ 8192u                        /* the mirror's frame ring (felucca-bsp usb.c; 4096 by default) */
#endif
#define APP_CPU_KHZ                      /* plat_cpu_khz: the SYSTEM page shows the clock */
#define APP_BLOCK 256u                   /* frames per audio block: 5.8 ms at 44.1 kHz (the clock is per sample) */

/* the store: the settings with the effects, the sixteen patterns in four objects of four (each with its sound, its
 * p-locks and its MOD lanes), the 32 user kicks, the panel calibration (the platform's); then the four projects, nine
 * objects each (a head: the tempo, the effects, the pattern and bank in use; then the 48 patterns of banks A-C, six an
 * object), pooled in the data area's first 41 sectors (storage.c's pool: one sector a copy, not a pair) */
#define NPROJ 4
#define PJ_NOBJ 9
/* (P4..P11: banks B and C) */
enum { OBJ_SET, OBJ_P0, OBJ_PANEL, OBJ_P1, OBJ_P2, OBJ_P3, OBJ_USER,
       OBJ_P4, OBJ_P5, OBJ_P6, OBJ_P7, OBJ_P8, OBJ_P9, OBJ_P10, OBJ_P11,
       OBJ_PJ0, OBJ_NOBJ = OBJ_PJ0 + NPROJ * PJ_NOBJ };
#define PJ_OBJ(p, j) (OBJ_PJ0 + (p) * PJ_NOBJ + (j))
#define PJX 0x97000u                     /* (the pooled objects' entries: not used) */
#define APP_STORE_MAP {0xC0000u, 0xC2000u, 0xDC000u, 0xC4000u, 0xC6000u, 0xC8000u, 0xCA000u, \
                       0xCC000u, 0xCE000u, 0xD0000u, 0xD2000u, 0xD4000u, 0xD6000u, 0xD8000u, 0xDA000u, \
                       PJX, PJX, PJX, PJX, PJX, PJX, PJX, PJX, PJX, PJX, PJX, PJX, PJX, PJX, PJX, PJX, PJX, PJX, \
                       PJX, PJX, PJX, PJX, PJX, PJX, PJX, PJX, PJX, PJX, PJX, PJX, PJX, PJX, PJX, PJX, PJX, PJX}
#define APP_STORE_POOL
#define APP_STORE_POOL_BASE 0x97000u
#define APP_STORE_POOL_N 41u             /* 0x97000..0xBFFFF: 36 objects, five sectors to spare */
#define APP_STORE_POOL_OLD 41u
#define APP_STORE_POOL_OBJ0 OBJ_PJ0
#define APP_STORE_POOL_OBJN (NPROJ * PJ_NOBJ)

/* backup and restore over USB (fm1.toml backup = true): the mirror page's Projects and backup, and its Render Audio */
#define APP_BACKUP_OBJS {OBJ_SET, OBJ_P0, OBJ_P1, OBJ_P2, OBJ_P3, OBJ_P4, OBJ_P5, OBJ_P6, OBJ_P7, OBJ_P8, OBJ_P9, OBJ_P10, \
                         OBJ_P11, OBJ_USER, \
                         OBJ_PJ0 + 0, OBJ_PJ0 + 1, OBJ_PJ0 + 2, OBJ_PJ0 + 3, OBJ_PJ0 + 4, OBJ_PJ0 + 5, OBJ_PJ0 + 6, \
                         OBJ_PJ0 + 7, OBJ_PJ0 + 8, OBJ_PJ0 + 9, OBJ_PJ0 + 10, OBJ_PJ0 + 11, OBJ_PJ0 + 12, OBJ_PJ0 + 13, \
                         OBJ_PJ0 + 14, OBJ_PJ0 + 15, OBJ_PJ0 + 16, OBJ_PJ0 + 17, OBJ_PJ0 + 18, OBJ_PJ0 + 19, \
                         OBJ_PJ0 + 20, OBJ_PJ0 + 21, OBJ_PJ0 + 22, OBJ_PJ0 + 23, OBJ_PJ0 + 24, OBJ_PJ0 + 25, \
                         OBJ_PJ0 + 26, OBJ_PJ0 + 27, OBJ_PJ0 + 28, OBJ_PJ0 + 29, OBJ_PJ0 + 30, OBJ_PJ0 + 31, \
                         OBJ_PJ0 + 32, OBJ_PJ0 + 33, OBJ_PJ0 + 34, OBJ_PJ0 + 35}
#define APP_BACKUP_MAGIC 0x424B4D46u     /* "FMKB" */
#define APP_BACKUP_VERSION 1u
#define APP_BACKUP_WHAT "replaces all saved data"
int app_backup_playing(void);
void app_backup_flush(void);
void app_backup_hold(int on);
void app_backup_restored(void);
#define APP_WEB                          /* the browser pages' own exports (kick/web.c): Render Audio */

void app_init(void);
void app_render(int32_t *lr, uint32_t n, uint32_t master_q12);
void app_panic(void);
void app_ui_init(void);
void app_frame(void);
void app_input_only(void);
void app_say(const char *a, const char *b);

#define APP_HOST_EXPECT
int app_expect(const char *what, int *got);
