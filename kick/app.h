/* SPDX-License-Identifier: MPL-2.0 */
/* FM-KICK: a kick drum synthesizer and generative sequencer for the FM-1. */
#pragma once
#include <stdint.h>
#include "plat.h"

#define APP_NAME "FM-KICK"
#define APP_VERSION "0.1.0"
#define APP_SPLASH_RGB RGB(255, 72, 28)
#define APP_BLOCK 256u                   /* frames per audio block: 5.8 ms at 44.1 kHz (the clock is per sample) */

/* the store: the settings, the sixteen patterns (each with its sound), the panel calibration (the platform's) */
enum { OBJ_SET, OBJ_PATS, OBJ_PANEL, OBJ_NOBJ };
#define APP_STORE_MAP {0xC0000u, 0xC2000u, 0xDC000u}

void app_init(void);
void app_render(int32_t *lr, uint32_t n, uint32_t master_q12);
void app_panic(void);
void app_ui_init(void);
void app_frame(void);
void app_input_only(void);
void app_say(const char *a, const char *b);

#define APP_HOST_EXPECT
int app_expect(const char *what, int *got);
