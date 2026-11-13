/* SPDX-License-Identifier: MPL-2.0 */
/* FM-KICK: the kick voice. A resonator (a state-variable filter at the edge of self-oscillation) struck by a pulse,
 * a pitch envelope of three points, a clean sub, a click of pulse and noise, an amplitude envelope with a hold, four
 * kinds of drive and a tone filter. Mono, 44.1 kHz, single-precision float. */
#pragma once
#include <stdint.h>

/* the sound's 24 parameters, 0..127 each (some are selectors: their own ranges below), in the order of the pages */
enum {
    KP_TUNE, KP_BEND, KP_TIME, KP_CURVE,         /* PITCH: base note (12..72), sweep depth, sweep time, sweep shape */
    KP_TAIL, KP_TTIME, KP_GLIDE, KP_DRIFT,       /* SWEEP: slow drop at the end, its time, slide time, drift */
    KP_ATK, KP_DEC, KP_HOLD, KP_LEVEL,           /* AMP */
    KP_COLOUR, KP_PULSE, KP_NOISE, KP_SNAP,      /* CLICK: pulse colour, pulse level, noise level, click length */
    KP_BODY, KP_SUB, KP_SUBT, KP_RESO,           /* LAYERS: resonator level, sub level, sub interval (0..3), ring */
    KP_DTYPE, KP_DRIVE, KP_SHAPE, KP_TONE,       /* DRIVE: type (0..3), amount, shape, tone */
    KP_N
};
#define KP_TUNE_LO 12
#define KP_TUNE_HI 72
enum { DT_SOFT, DT_HARD, DT_FOLD, DT_CRUSH, DT_N };

typedef struct {
    float p[KP_N];                      /* the parameters as played (the pattern's, moved by the MOD lanes) */
} kparm_t;

typedef struct {
    /* the hit */
    float t;                            /* seconds since the hit */
    float vel;                          /* 0..1 */
    float note, note_from, glide_t;     /* the base note (with the step's pitch), where a slide comes from */
    float sign;                         /* +1, or -1 with the step's bend reversed */
    float drift;                        /* this hit's detune (semitones) */
    float wob;                          /* the slow drift's phase */
    int active;
    /* amplitude */
    float amp;
    /* the resonator, the sub, the click */
    float zr, zi;                       /* the resonator: a phasor that turns at the note and decays at RESO */
    int stage;                          /* the amplitude: 0 attack and hold, 1 decay */
    float exc_x;                        /* the strike's push a sample */
    float sub_ph;
    float exc_left;                     /* samples of the striking pulse left */
    float exc_h;                        /* its height */
    float clk, nse;                     /* the click's and the noise's envelopes */
    float clk_lp, nse_lp;
    uint32_t rng;
    /* the tail of a hit cut short by the next one, faded out */
    float tail;
    float last;
    /* drive, tone, DC */
    float crush_hold;
    float crush_n;
    float tone_lp, dc_x, dc_y;
    /* the per-block coefficients (kdsp_render) */
    float c_bend, c_tinv, c_curve, c_tail, c_ttinv, c_glide;
    float c_atk, c_hold, c_dec, c_level;
    float c_clk, c_nse, c_colour, c_pulse, c_noise;
    float c_body, c_sub, c_subt, c_pole;
    int c_dtype;
    float c_drive, c_shape, c_tone, c_dwob;
    float c_crush_step, c_crush_q;
} kick_t;

void kdsp_init(kick_t *k, uint32_t seed);
/* a hit: semis over the base note, velocity 0..1; slide: no new strike, the note glides; rev: the sweep goes down */
void kdsp_hit(kick_t *k, const kparm_t *p, float semis, float vel, int slide, int rev);
void kdsp_render(kick_t *k, const kparm_t *p, float *out, uint32_t n);   /* n mono samples, added to nothing: written */
float kdsp_pitch_at(const kparm_t *p, float t, int rev);                  /* the note the sweep is at, t seconds in */
float kdsp_length(const kparm_t *p);                                      /* about how long a hit sounds, seconds */
float kdsp_note_hz(float note);
