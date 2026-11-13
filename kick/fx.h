/* SPDX-License-Identifier: MPL-2.0 */
/* FM-KICK: the effects, one rack for every pattern. Twelve LIVE effects, on while their white key is held, then five
 * FINAL ones, on or off, at the end of the chain. The kick (mono) in, stereo out, in this order. */
#pragma once
#include <stdint.h>

enum {
    /* LIVE: held */
    FX_DIST, FX_CRUSH, FX_FILT, FX_RING, FX_CHORUS, FX_PHASER, FX_FLANGER, FX_DELAY, FX_VERB, FX_STUTTER, FX_GATE,
    FX_WIDTH,
    /* FINAL: on or off */
    FX_TRANS, FX_COMP, FX_EQ, FX_CLIP, FX_LIMIT,
    FX_N
};
#define FX_NLIVE 12
#define FX_NFINAL (FX_N - FX_NLIVE)

typedef struct {
    uint32_t on;                         /* bit i: FINAL effect i on (the LIVE ones are held, not kept) */
    uint8_t p[FX_N][4];                  /* each effect's four knobs, 0..127 */
} fxset_t;

#define FX_NDIV 8                        /* DELAY's times */
#define FX_NGATE 4                       /* GATE's rates: 1/32, 1/16, 1/8, 1/4 */
#define FX_NSLICE 4                      /* STUTTER's slices: 1/16, 1/32, 1/64, 1/128 */
#define FX_NREP 4                        /* STUTTER: a new slice every 1, 2, 4, 8 steps */
static inline int fx_idx(uint8_t v, int n) { int i = v * n / 128; return i < n ? i : n - 1; }

void fx_defaults(fxset_t *s);
void fx_reset(void);
/* one block: the kick in, the rack out. live: the LIVE effects held (bit i). tick: the sequencer's clock (960 a beat)
 * at the block's start, inc: its advance a sample; running: the sequencer plays. */
void fx_process(const fxset_t *s, uint32_t live, const float *in, float *l, float *r, uint32_t n, float bpm, float tick,
                float inc, int running);
void fx_step(uint32_t sixteenth);        /* each sixteenth of the sequencer: STUTTER takes a new slice on its beat */
