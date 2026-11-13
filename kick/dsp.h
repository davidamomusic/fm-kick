/* SPDX-License-Identifier: MPL-2.0 */
/* FM-KICK: the kick voice. Six kinds of body (808, 909, 606, ANALOG, FM, WSHAPER), a pitch sweep in three stages,
 * an amplitude envelope with a drop and a punch, three more layers (click, sub, noise), then a distortion, a filter
 * with its own envelope, a three-band EQ and a clipper. Mono, 44.1 kHz, single-precision float. */
#pragma once
#include <stdint.h>

/* the sound's 44 parameters, 0..127 each unless said (selectors: their own ranges, kp_hi), in the order of the pages */
enum {
    KP_TYPE, KP_TUNE, KP_DEC, KP_LEVEL,          /* KICK: the body's kind (0..5), base note (12..72), decay, level */
    KP_CA, KP_CB, KP_CC, KP_CD,                  /* CHARACTER: four knobs whose meaning depends on the kind */
    KP_BEND, KP_TIME, KP_CURVE, KP_TAIL,         /* PITCH: sweep depth, time, shape, slow drop at the end */
    KP_TTIME, KP_GLIDE, KP_DRIFT, KP_VBEND,      /* SWEEP: the tail's time, slide time, drift, velocity to bend */
    KP_ATK, KP_HOLD, KP_DROP, KP_PUNCH,          /* AMP: attack, hold, volume drop (decay shape), punch */
    KP_CTYPE, KP_CLVL, KP_CTONE, KP_CLEN,        /* CLICK: kind (0..4), level, tone, length */
    KP_SLVL, KP_SINT, KP_SDEC, KP_SDRV,          /* SUB: level, interval (0..3), decay, drive */
    KP_NLVL, KP_NTONE, KP_NDEC, KP_NBW,          /* NOISE: level, centre, decay, width */
    KP_DTYPE, KP_DRIVE, KP_SHAPE, KP_DMIX,       /* DIST: kind (0..5), drive, shape, mix */
    KP_FTYPE, KP_CUT, KP_RES, KP_FENV,           /* FILTER: kind (0..3), cutoff, resonance, envelope (64 none) */
    KP_EQLO, KP_EQMID, KP_EQFRQ, KP_EQHI,        /* EQ: 64 flat */
    KP_BODY, KP_VSENS, KP_ACC, KP_CLIP,          /* OUTPUT: body level, velocity sense, accent, clip */
    KP_N
};
#define KP_TUNE_LO 12
#define KP_TUNE_HI 72
enum { KT_808, KT_909, KT_606, KT_ANALOG, KT_FM, KT_WSHAPER, KT_N };
#define WS_N 8                           /* WSHAPER's curves, CURVE morphs from one to the next */
enum { WS_SOFT, WS_HARD, WS_SFOLD, WS_TFOLD, WS_CHEBY, WS_STAIRS, WS_RECT, WS_SQUARE };
enum { CK_PULSE, CK_NOISE, CK_TICK, CK_ZAP, CK_SNAP, CK_N };
enum { DT_SOFT, DT_HARD, DT_FOLD, DT_CRUSH, DT_TUBE, DT_RECT, DT_N };
enum { FT_LP, FT_HP, FT_BP, FT_NOTCH, FT_N };

int kp_lo(int k);
int kp_hi(int k);

typedef struct {
    float p[KP_N];                      /* the parameters as played (the step's, moved by the MOD lanes) */
} kparm_t;

typedef struct { float b0, b1, b2, a1, a2, z1, z2; } kbq_t;

typedef struct {
    /* the hit */
    float t;                            /* seconds since the hit */
    float vel;
    float note, note_from, glide_t;
    float sign, drift, wob;
    int active, stage;
    float amp, amp2;                    /* the envelope; 808's slower BOOM */
    float ph, ph2, fb;                  /* the body's phases (FM: the modulator, 606: the ring), FM feedback */
    float zr, zi, exc_left, exc_x;      /* ANALOG: the resonator, the strike */
    float menv;                         /* FM: the modulator's envelope */
    float mclk, mclk_lp, mlp2, mnse;    /* the kinds' own clicks */
    float body_lp;
    float sub_ph, sub_amp;
    float nse_env, nse_s1, nse_s2;
    float clk_env, clk_lp, clk_ph, clk_hp;
    uint32_t rng;
    float tail, last;
    float crush_hold, crush_n;
    float f_s1, f_s2;
    kbq_t eq[3];
    float dc_x, dc_y;
    /* the per-block coefficients */
    int c_type, c_ctype, c_dtype, c_ftype;
    float c_bend, c_tinv, c_curve, c_tail, c_ttinv, c_glide, c_dwob;
    float c_atk, c_hold, c_dec, c_dec2, c_drop, c_punch;
    float c_a, c_b, c_c, c_d;           /* the CHARACTER knobs, 0..1 */
    float c_ws_drive, c_ws_bias, c_ws_off;
    float c_ratio, c_mdec, c_pole, c_lp_a, c_mclk_a, c_mclk_d;
    float c_clvl, c_ctone, c_cdec;
    float c_slvl, c_sint, c_sdec, c_sdrv;
    float c_nlvl, c_nf, c_nq, c_ndec;
    float c_drive, c_shape, c_dmix, c_cq, c_cstep;
    float c_cut, c_res, c_fenv;
    float c_body, c_level, c_clip;
} kick_t;

/* the step's flags as the voice reads them */
#define KH_SLIDE 1                      /* no new strike: the note glides there */
#define KH_REV 2                        /* the sweep goes down */
#define KH_ACC 4                        /* an accent */

void kdsp_init(kick_t *k, uint32_t seed);
void kdsp_hit(kick_t *k, const kparm_t *p, float semis, float vel, int flags);
void kdsp_render(kick_t *k, const kparm_t *p, float *out, uint32_t n);
float kdsp_pitch_at(const kparm_t *p, float t, int rev);
float kdsp_length(const kparm_t *p);
float kdsp_note_hz(float note);
/* WSHAPER's transfer: what the curve makes of x (-1..1), with its DRIVE and BIAS (for drawing it) */
float kdsp_ws(const kparm_t *p, float x);
