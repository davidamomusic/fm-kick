/* SPDX-License-Identifier: MPL-2.0 */
/* FM-KICK: a kick drum synthesizer and generative sequencer for the FM-1.
 *
 * The sound (dsp.c): a resonator struck by a pulse, a three-point pitch sweep, a click, a sub, four drives and a tone.
 * The sequencer: a TRIG lane of sixteen steps (trig, accent, pitch, ratchet, slide, reversed sweep) played by hand or
 * by one of nine generators, and three MOD lanes, each with its own length, division and direction, that move the
 * sound. Sixteen patterns, each with its own sound.
 *
 * Part of the unity build (device/unity.c, host/plat_host.c), after the platform: gfx.c and announce.c are in scope. */
#include "fastmath.h"
#include "dsp.h"

#define NPAT 16
#define NST 16
#define NMOD 3
#define PPQ 960u                         /* clock ticks a quarter note */

/* ------------------------------------------------------------------ colours --- */
#define K_BG 0x0000u
#define K_INK RGB(236, 230, 218)         /* bone: values, what is there */
#define K_ACC RGB(255, 72, 28)           /* hot orange: the kick, accents, the playhead */
#define K_ACC2 RGB(92, 24, 8)            /* its shadow: fills */
#define K_FILL RGB(132, 36, 12)          /* the scope's body */
#define K_LOCK RGB(255, 176, 32)          /* a p-lock: amber */
#define K_GRID RGB(40, 37, 35)           /* rules, empty cells */
#define K_MUTE RGB(124, 116, 106)        /* labels */
#define K_DIM RGB(70, 64, 58)            /* inactive */
static const uint16_t MOD_COL[NMOD] = {RGB(255, 176, 32), RGB(255, 104, 160), RGB(110, 200, 255)};

/* ------------------------------------------------------------------- data --- */
typedef struct {
    uint8_t trig;                        /* 0 off, 1 on, 2 accent */
    uint8_t fl;                          /* FL_* */
    int8_t pit;                          /* -24 .. +24 semitones */
    uint8_t rat;                         /* 0..3: one to four hits in the step */
} step_t;
enum { FL_SLIDE = 1, FL_REV = 2 };

typedef struct {
    uint8_t tgt, shape, len, div, dir, smooth;
    int8_t depth;                        /* -100 .. +100 % */
    uint8_t seed;
    int8_t val[NST];                     /* -64 .. +63: the MANUAL shape's values */
} mod_t;

/* a step's p-locks: the kick (a factory sound in place of the pattern's), then up to four parameters set to a value */
#define NLOCK 4
typedef struct {
    uint8_t kit;                         /* 0 none, 1..NPRE: that preset's sound for this step */
    uint8_t id[NLOCK];                   /* 0 none, 1 + KP_*: the parameter */
    uint8_t val[NLOCK];                  /* its value (the parameter's own range) */
} lock_t;

typedef struct {
    uint8_t snd[KP_N];                   /* the pattern's sound */
    step_t st[NST];
    uint8_t len, div, dir, swing, prob, human, algo, preset;
    uint8_t ga[4];                       /* the generator's four parameters */
    mod_t mod[NMOD];
    lock_t lk[NST];                      /* each step's p-locks */
    uint8_t rsv[4];
} pat_t;
_Static_assert(sizeof(pat_t) == 320, "pat_t: 320 bytes");

#define SET_MAGIC 0x4B434B31u            /* "KCK1" */
#define PATS_MAGIC1 0x4B505431u          /* "KPT1": 0.1.0, sixteen patterns of 176 bytes in one object, no p-locks */
#define PATS_MAGIC 0x4B505432u           /* "KPT2": eight patterns an object, with p-locks */
typedef struct {
    uint32_t magic;
    uint16_t bpm10;                      /* tempo x 10 */
    uint8_t cur;                         /* the pattern playing */
    uint8_t mch;                         /* MIDI in channel: 0 all, 1..16 */
    uint8_t clk;                         /* 0 internal, 1 MIDI clock in */
    uint8_t clkout;                      /* MIDI clock out */
    uint8_t notes_out;                   /* the hits out as notes */
    uint8_t rsv[5];
} set_t;
typedef struct {
    uint32_t magic;
    pat_t p[NPAT / 2];
} pats_t;                                /* one flash object: half the patterns (a sector holds 3840 bytes) */
_Static_assert(sizeof(pats_t) <= 3840u, "pats_t: one sector's payload");

static set_t set;
static pats_t pk[2];
#define pats(i) (pk[(i) >> 3].p[(i) & 7])

/* ------------------------------------------------------------------ tables --- */
static const char *const PNAME[KP_N] = {"TUNE", "BEND", "TIME", "CURVE", "TAIL", "T.TIME", "GLIDE", "DRIFT",
                                        "ATTACK", "DECAY", "HOLD", "LEVEL", "COLOUR", "PULSE", "NOISE", "SNAP",
                                        "BODY", "SUB", "SUB INT", "RESO", "TYPE", "DRIVE", "SHAPE", "TONE"};
static const char *const SND_PAGE[6] = {"PITCH", "SWEEP", "AMP", "CLICK", "LAYERS", "DRIVE"};
static const char *const DT_NAME[DT_N] = {"SOFT", "HARD", "FOLD", "CRUSH"};
static const char *const SUBT_NAME[4] = {"-12", "0", "+7", "+12"};

#define NDIV 9
static const char *const DIV_NAME[NDIV] = {"1/32", "1/16T", "1/16", "1/8T", "1/8", "1/4T", "1/4", "1/2", "1 BAR"};
static const uint16_t DIV_T[NDIV] = {120, 160, 240, 320, 480, 640, 960, 1920, 3840};
#define NDIR 5
static const char *const DIR_NAME[NDIR] = {"FWD", "REV", "PING", "RAND", "WALK"};

enum { AL_MAN, AL_EUCL, AL_CLAVE, AL_BREAK, AL_WALK, AL_CELL, AL_GOLD, AL_POLY, AL_LOGI, AL_WAVE, NALGO };
static const char *const ALGO_NAME[NALGO] = {"MANUAL", "EUCLID", "CLAVE", "BREAK", "WALK", "CELL", "GOLDEN", "POLY",
                                             "LOGISTIC", "WAVE"};
static const char *const ALGO_P[NALGO][4] = {
    {"-", "-", "-", "-"},           {"HITS", "ROTATE", "ACCENT", "FILL"},  {"TYPE", "ROTATE", "ACCENT", "FILL"},
    {"TYPE", "VARY", "ACCENT", "FILL"}, {"DENSITY", "STEP", "BIAS", "SEED"}, {"RULE", "SEED", "GENS", "ACCENT"},
    {"DENSITY", "PHASE", "ACCENT", "FILL"}, {"EVERY A", "EVERY B", "SHIFT B", "ACCENT"},
    {"CHAOS", "THRESH", "SEED", "ACCENT"}, {"FREQ A", "FREQ B", "AMOUNT", "PHASE"}};

#define NCLAVE 10
static const char *const CLAVE_NAME[NCLAVE] = {"FLOOR", "SON 3-2", "SON 2-3", "RUMBA", "BOSSA", "TRESILLO", "GAHU",
                                               "SHIKO", "SOUKOUS", "HALF"};
static const uint16_t CLAVE_BITS[NCLAVE] = {
    0x1111u, (1u << 0) | (1u << 3) | (1u << 6) | (1u << 10) | (1u << 12),
    (1u << 2) | (1u << 4) | (1u << 8) | (1u << 11) | (1u << 14), (1u << 0) | (1u << 3) | (1u << 7) | (1u << 10) | (1u << 12),
    (1u << 0) | (1u << 3) | (1u << 6) | (1u << 10) | (1u << 13), (1u << 0) | (1u << 3) | (1u << 6) | (1u << 8) | (1u << 11) | (1u << 14),
    (1u << 0) | (1u << 3) | (1u << 6) | (1u << 10) | (1u << 14), (1u << 0) | (1u << 4) | (1u << 6) | (1u << 10) | (1u << 12),
    (1u << 0) | (1u << 3) | (1u << 6) | (1u << 10) | (1u << 11), (1u << 0) | (1u << 8)};
#define NBREAK 8
static const char *const BREAK_NAME[NBREAK] = {"BREAK 1", "FUNK", "THINK", "2-STEP", "ROLLER", "BOOM BAP", "GARAGE",
                                               "ELECTRO"};
static const uint16_t BREAK_BITS[NBREAK] = {
    (1u << 0) | (1u << 2) | (1u << 10) | (1u << 11), (1u << 0) | (1u << 3) | (1u << 8) | (1u << 10),
    (1u << 0) | (1u << 6) | (1u << 10), (1u << 0) | (1u << 7) | (1u << 10), (1u << 0) | (1u << 10),
    (1u << 0) | (1u << 7) | (1u << 8), (1u << 0) | (1u << 3) | (1u << 9) | (1u << 12),
    (1u << 0) | (1u << 6) | (1u << 8) | (1u << 11) | (1u << 14)};

/* MOD lanes: their targets and shapes */
static const int8_t MOD_TGT_KP[] = {-1, KP_TUNE, KP_BEND, KP_TIME, KP_CURVE, KP_TAIL, KP_DEC, KP_HOLD, KP_COLOUR,
                                    KP_PULSE, KP_NOISE, KP_BODY, KP_SUB, KP_RESO, KP_DRIVE, KP_SHAPE, KP_TONE, KP_LEVEL};
#define NTGT ((int)sizeof MOD_TGT_KP)
enum { SH_MAN, SH_UP, SH_DOWN, SH_TRI, SH_SINE, SH_SQR, SH_RAND, SH_WALK, NSHAPE };
static const char *const SHAPE_NAME[NSHAPE] = {"MANUAL", "RAMP UP", "RAMP DN", "TRI", "SINE", "SQUARE", "RANDOM",
                                               "WALK"};

/* ------------------------------------------------------------------ presets --- */
#define NPRE 12
static const char *const PRE_NAME[NPRE] = {"DEEP", "PUNCH", "RUMBLE", "HARD", "GABBA", "TOM", "DUB", "CRUSH", "HOUSE",
                                           "ZAP", "SOFT", "FOLD"};
static const uint8_t PRE[NPRE][KP_N] = {
    {31, 50, 62, 64, 0, 64, 40, 0, 0, 88, 0, 110, 70, 40, 0, 40, 110, 40, 1, 100, 0, 10, 0, 110},
    {34, 70, 52, 80, 8, 80, 40, 0, 0, 66, 20, 110, 95, 70, 10, 35, 115, 20, 1, 90, 0, 35, 10, 105},
    {29, 40, 60, 50, 6, 100, 40, 30, 0, 105, 30, 108, 50, 25, 0, 45, 105, 60, 1, 115, 0, 70, 30, 70},
    {36, 90, 56, 100, 20, 85, 40, 0, 0, 80, 50, 104, 110, 80, 20, 30, 120, 0, 1, 120, 1, 95, 80, 95},
    {38, 100, 60, 90, 24, 70, 40, 0, 0, 70, 60, 100, 120, 90, 30, 30, 120, 0, 1, 127, 1, 120, 110, 90},
    {45, 24, 70, 40, 4, 60, 60, 10, 0, 60, 0, 110, 60, 30, 15, 50, 110, 0, 1, 70, 0, 15, 0, 100},
    {28, 30, 75, 30, 10, 110, 60, 50, 0, 110, 40, 106, 40, 15, 0, 50, 100, 70, 1, 105, 0, 30, 20, 60},
    {33, 60, 55, 64, 0, 64, 40, 0, 0, 75, 10, 108, 90, 50, 10, 40, 115, 30, 1, 95, 3, 40, 90, 120},
    {33, 55, 58, 70, 0, 64, 40, 0, 0, 72, 10, 110, 85, 55, 5, 40, 112, 30, 1, 95, 0, 20, 0, 110},
    {40, 127, 40, 110, 0, 64, 40, 0, 0, 50, 0, 108, 127, 60, 0, 30, 110, 0, 1, 80, 2, 30, 40, 115},
    {32, 36, 66, 40, 0, 64, 40, 0, 4, 78, 0, 112, 45, 20, 0, 55, 100, 50, 1, 85, 0, 0, 0, 90},
    {30, 60, 60, 70, 12, 90, 40, 10, 0, 92, 30, 104, 80, 40, 0, 40, 110, 20, 1, 110, 2, 60, 70, 100},
};

static int kp_lo(int k) { return k == KP_TUNE ? KP_TUNE_LO : 0; }
static int kp_hi(int k) { return k == KP_TUNE ? KP_TUNE_HI : k == KP_SUBT ? 3 : k == KP_DTYPE ? DT_N - 1 : 127; }

/* --------------------------------------------------------------- randomness --- */
static uint8_t ui_pat_dirty_boot;        /* the patterns came from an older layout: save them */
static uint32_t rng_ui = 0x9E3779B9u, rng_au = 0x7F4A7C15u;
static uint32_t xr32(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *s = x;
}
static int rr(uint32_t *s, int lo, int hi) { return lo + (int)(xr32(s) % (uint32_t)(hi - lo + 1)); }   /* lo..hi */
static uint32_t hash3(uint32_t a, uint32_t b, uint32_t c)
{
    uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA6Bu ^ (c + 0x165667B1u) * 0xC2B2AE35u;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    h *= 0x297A2D39u;
    return h ^ (h >> 15);
}

/* ------------------------------------------------------------------ the voice --- */
static kick_t voice;
static kparm_t eff;                      /* the playing pattern's sound, moved by the MOD lanes (each block) */

/* ---------------------------------------------------------------- sequencer --- */
#define NEV 24
typedef struct {
    uint32_t at;                         /* the tick it sounds at */
    int8_t semis;
    uint8_t fl;
    uint8_t step;                        /* its p-locks (0xFF: none) */
    float vel;
} ev_t;

static struct {
    volatile uint8_t running;
    uint32_t tick;
    float frac, inc;                     /* ticks: the fraction, a sample's worth */
    uint8_t first;                       /* tick 0 still to play */
    uint32_t lc[1 + NMOD];               /* each lane's steps so far */
    int8_t lpos[1 + NMOD];               /* each lane's step now (-1: none yet) */
    int8_t pdir[1 + NMOD];
    float mval[NMOD], mtgt[NMOD];
    volatile uint8_t cur, next;          /* the pattern playing, the one queued (0xFF: none) */
    ev_t ev[NEV];
    uint8_t nev;
    volatile uint32_t hits;              /* the hits so far (shown, and tests) */
    volatile uint8_t gen_dirty;
    uint32_t gcycle;
    /* the generators' state, carried across cycles */
    float walk_e, logi_x;
    uint16_t cell_row;
    /* the clock in */
    uint8_t ext_wait;
    int32_t ext_c;                       /* clocks since the start (-1 none yet) */
    uint32_t smp, ext_last;
    float ext_per;
    volatile uint16_t ext_bpm10;
    /* MIDI out */
    int8_t note_out;                     /* the note sent last (-1 none) */
    /* auditions from the panel */
    volatile int8_t aud_semis;
    volatile uint8_t aud_vel, aud_req, aud_step;   /* aud_step: whose p-locks (0xFF none) */
} sq;
static uint8_t gen_trig[NST];            /* the generator's TRIG lane: 0, 1, 2 */
static volatile uint32_t midi_seen;

static uint32_t lane_len(const pat_t *p, int l) { return l == 0 ? p->len : p->mod[l - 1].len; }
static uint32_t lane_div(const pat_t *p, int l) { return DIV_T[l == 0 ? p->div : p->mod[l - 1].div]; }
static uint32_t lane_dir(const pat_t *p, int l) { return l == 0 ? p->dir : p->mod[l - 1].dir; }

static void tempo_set(void)
{
    float bpm = sq.ext_bpm10 && set.clk ? (float)sq.ext_bpm10 * 0.1f : (float)set.bpm10 * 0.1f;
    if (!set.clk || !sq.ext_per)
        sq.inc = bpm * (float)PPQ / 60.0f / 44100.0f;
}

/* the TRIG lane's generators: a pattern of L steps (0 off, 1 on, 2 accent) */
static void gen_run(const pat_t *p, uint32_t cycle)
{
    uint32_t L = p->len ? p->len : 1u, i, k;
    const uint8_t *g = p->ga;
    uint8_t out[NST] = {0};
    uint32_t fill = cycle % 4u == 3u ? g[3] : 0u;   /* FILL: in the fourth bar, extra hits */
    int acc_mode = g[2] / 32;
    uint32_t nh = 0;
    switch (p->algo) {
    case AL_EUCL: {
        uint32_t hits = 1u + (uint32_t)g[0] * (L - 1u) / 127u, rot = (uint32_t)g[1] * L / 128u;
        for (i = 0; i < L; i++) {
            uint32_t j = (i + rot) % L;
            if ((j * hits) % L < hits)
                out[i] = 1;
        }
        break;
    }
    case AL_CLAVE:
    case AL_BREAK: {
        uint32_t t = p->algo == AL_CLAVE ? (uint32_t)g[0] * NCLAVE / 128u : (uint32_t)g[0] * NBREAK / 128u;
        uint16_t bits = p->algo == AL_CLAVE ? CLAVE_BITS[t] : BREAK_BITS[t];
        uint32_t rot = p->algo == AL_CLAVE ? (uint32_t)g[1] * L / 128u : 0u;
        for (i = 0; i < L; i++) {
            uint32_t s = ((i + rot) % L) * 16u / L;
            if (bits >> s & 1u)
                out[i] = 1;
        }
        if (p->algo == AL_BREAK && g[1]) {          /* VARY: hits move or drop, a new way every bar */
            for (i = 1; i < L; i++)
                if (out[i] && hash3(cycle, i, 11u) % 127u < g[1]) {
                    uint32_t h = hash3(cycle, i, 12u) % 3u;
                    out[i] = 0;
                    if (h == 1u && i + 1u < L)
                        out[i + 1u] = 1;
                    else if (h == 2u && i > 1u && !out[i - 1u])
                        out[i - 1u] = 1;
                }
        }
        break;
    }
    case AL_WALK: {                      /* a walk that drifts back to its middle: DENSITY sets how often it is high */
        float th = 0.6f - (float)g[0] / 127.0f * 1.2f, stp = 0.2f + (float)g[1] / 127.0f * 0.8f;
        float bias = ((float)g[2] - 64.0f) / 64.0f * 0.3f;
        uint32_t seed = g[3];
        if (cycle == 0u)
            sq.walk_e = 0.0f;
        for (i = 0; i < L; i++) {
            float r = (float)(int32_t)hash3(seed, cycle, i) * (1.0f / 2147483648.0f);
            sq.walk_e = sq.walk_e * 0.65f + r * stp + bias * 0.5f;
            if (sq.walk_e > 1.0f)
                sq.walk_e = 1.0f;
            if (sq.walk_e < -1.0f)
                sq.walk_e = -1.0f;
            if (sq.walk_e > th)
                out[i] = sq.walk_e > th + 0.45f ? 2 : 1;
        }
        break;
    }
    case AL_CELL: {
        uint32_t rule = (uint32_t)g[0] * 2u, gens = g[2] / 8u, row, n, m = L >= 16u ? 0xFFFFu : (1u << L) - 1u;
        if (cycle == 0u || !sq.cell_row) {   /* the start: the seed's row, GENS generations on */
            sq.cell_row = (uint16_t)((hash3(g[1], 7u, 7u) & m) | 1u);
            n = gens + 1u;
        } else {
            n = 1u;                          /* then one generation a bar */
        }
        row = sq.cell_row;
        for (k = 0; k < n; k++) {
            uint32_t nr = 0;
            for (i = 0; i < L; i++) {
                uint32_t l = row >> ((i + L - 1u) % L) & 1u, c = row >> i & 1u, r = row >> ((i + 1u) % L) & 1u;
                nr |= (rule >> (l << 2 | c << 1 | r) & 1u) << i;
            }
            row = nr & m;
            if (!row)
                row = (uint32_t)(hash3(g[1], cycle, k) & m) | 1u;
        }
        sq.cell_row = (uint16_t)row;
        for (i = 0; i < L; i++)
            if (row >> i & 1u)
                out[i] = hash3(g[3], i, 3u) % 127u < g[3] ? 2 : 1;
        acc_mode = 0;
        fill = 0;
        break;
    }
    case AL_GOLD: {
        float d = (float)g[0] / 127.0f, ph = (float)g[1] / 127.0f * (float)L;
        for (i = 0; i < L; i++) {
            float x = ((float)i + ph) * 0.6180340f;
            if (x - fm_floorf(x) < d)
                out[i] = 1;
        }
        break;
    }
    case AL_POLY: {
        uint32_t a = 1u + (uint32_t)g[0] * 7u / 127u, b = 1u + (uint32_t)g[1] * 7u / 127u, sh = (uint32_t)g[2] * L / 128u;
        for (i = 0; i < L; i++) {
            int x = i % a == 0u, y = (i + L - sh) % L % b == 0u;
            if (x || y)
                out[i] = (x && y && g[3] > 40u) ? 2 : 1;
        }
        acc_mode = 0;
        break;
    }
    case AL_LOGI: {
        float r = 3.4f + (float)g[0] / 127.0f * 0.6f, th = 0.3f + (float)g[1] / 127.0f * 0.6f;
        float at = 1.0f - (float)g[3] / 127.0f * 0.25f;
        if (cycle == 0u || sq.logi_x <= 0.0f || sq.logi_x >= 1.0f)
            sq.logi_x = 0.1f + (float)g[2] / 127.0f * 0.8f;
        for (i = 0; i < L; i++) {
            sq.logi_x = r * sq.logi_x * (1.0f - sq.logi_x);
            if (sq.logi_x > th)
                out[i] = g[3] && sq.logi_x > at ? 2 : 1;
        }
        acc_mode = 0;
        break;
    }
    case AL_WAVE: {
        float fa = 1.0f + (float)(g[0] * 7 / 127), fb = 1.0f + (float)(g[1] * 7 / 127);
        float th = 1.6f - (float)g[2] / 127.0f * 3.2f, ph = (float)g[3] / 127.0f;
        for (i = 0; i < L; i++) {
            float s = fm_sin_turns((float)i * fa / (float)L + ph) + fm_sin_turns((float)i * fb / (float)L);
            if (s > th)
                out[i] = s > 1.5f ? 2 : 1;
        }
        acc_mode = 0;
        break;
    }
    default:
        break;
    }
    if (fill)                            /* FILL */
        for (i = 0; i < L; i++)
            if (!out[i] && hash3(cycle, i, 99u) % 127u < fill)
                out[i] = 1;
    if (acc_mode)                        /* ACCENT: the first hit, every other hit, the off-beat hits */
        for (i = 0; i < L; i++)
            if (out[i]) {
                if ((acc_mode == 1 && nh == 0u) || (acc_mode == 2 && nh % 2u == 0u) || (acc_mode == 3 && (i & 1u)))
                    out[i] = 2;
                nh++;
            }
    for (i = 0, k = 0; i < L; i++)
        k |= out[i];
    if (!k && p->algo != AL_MAN)         /* never a silent bar: the first step at least */
        out[0] = 1;
    for (i = 0; i < NST; i++)
        gen_trig[i] = i < L ? out[i] : 0;
}

/* the lanes' position for count c */
static int lane_pos(int l, uint32_t c, uint32_t L, uint32_t dir)
{
    if (L <= 1u)
        return 0;
    switch (dir) {
    default:
    case 0:
        return (int)(c % L);
    case 1:
        return (int)(L - 1u - c % L);
    case 2: {
        uint32_t per = 2u * L - 2u, m = c % per;
        return (int)(m < L ? m : per - m);
    }
    case 3:
        return (int)(xr32(&rng_au) % L);
    case 4: {
        int p = sq.lpos[l] < 0 ? 0 : sq.lpos[l];
        if (c) {
            p += rr(&rng_au, -1, 1);
            if (p < 0)
                p = (int)L - 1;
            if (p >= (int)L)
                p = 0;
        }
        return p;
    }
    }
}

static void ev_add(uint32_t at, int8_t semis, float vel, uint8_t fl, uint8_t step)
{
    if (sq.nev < NEV) {
        sq.ev[sq.nev].step = step;
        sq.ev[sq.nev].at = at;
        sq.ev[sq.nev].semis = semis;
        sq.ev[sq.nev].vel = vel;
        sq.ev[sq.nev].fl = fl;
        sq.nev++;
    }
}

static float mod_value(const mod_t *m, int l, int pos)
{
    float L = (float)(m->len > 1u ? m->len : 2u);
    float x = (float)pos;
    switch (m->shape) {
    default:
    case SH_MAN:
        return (float)m->val[pos] / 63.0f;
    case SH_UP:
        return x / (L - 1.0f) * 2.0f - 1.0f;
    case SH_DOWN:
        return 1.0f - x / (L - 1.0f) * 2.0f;
    case SH_TRI: {
        float u = x / L * 2.0f;
        return u < 1.0f ? u * 2.0f - 1.0f : 3.0f - u * 2.0f;
    }
    case SH_SINE:
        return fm_sin_turns(x / L);
    case SH_SQR:
        return x < L * 0.5f ? 1.0f : -1.0f;
    case SH_RAND:
        return (float)(int32_t)hash3(m->seed, sq.lc[l], (uint32_t)pos) * (1.0f / 2147483648.0f);
    case SH_WALK: {
        float v = sq.mtgt[l - 1] + (float)(int32_t)hash3(m->seed, sq.lc[l], 5u) * (0.35f / 2147483648.0f);
        return v > 1.0f ? 2.0f - v : v < -1.0f ? -2.0f - v : v;
    }
    }
}

static void midi_note_out(int note, float vel)
{
    if (!set.notes_out)
        return;
    if (sq.note_out >= 0)
        plat_midi_send(0x08u | 0x80u << 8 | (uint32_t)sq.note_out << 16, TO_USB);
    note = clamp(note, 0, 127);
    plat_midi_send(0x09u | 0x90u << 8 | (uint32_t)note << 16 | (uint32_t)clamp((int)(vel * 127.0f), 1, 127) << 24, TO_USB);
    sq.note_out = (int8_t)note;
}

static void eff_compute(void);
static lock_t hit_lk;                    /* the p-locks of the hit sounding: they last as long as it does */
static uint8_t hit_has;

static void hit(int semis, float vel, uint8_t fl, uint8_t step)
{
    if (step < NST) {
        hit_lk = pats(sq.cur).lk[step];
        hit_has = 1;
    } else {
        hit_has = 0;
    }
    eff_compute();
    kdsp_hit(&voice, &eff, (float)semis, vel, fl & FL_SLIDE, (fl & FL_REV) != 0);
    sq.hits++;
    midi_note_out(36 + semis, vel);
}

static void lane_step(int l)
{
    pat_t *p = &pats(sq.cur);
    uint32_t L = lane_len(p, l), D = lane_div(p, l), c = sq.lc[l];
    int pos;
    if (l == 0 && c % (L ? L : 1u) == 0u) {     /* the TRIG lane's bar starts: a queued pattern, the generator */
        if (sq.next != 0xFFu && c) {
            uint32_t k;
            sq.cur = sq.next;
            sq.next = 0xFFu;
            p = &pats(sq.cur);
            for (k = 0; k <= NMOD; k++) {
                sq.lc[k] = 0;
                sq.lpos[k] = -1;
            }
            c = 0;
            L = p->len;
            D = DIV_T[p->div];
            sq.gcycle = 0;
            sq.gen_dirty = 1;
        }
        if (p->algo != AL_MAN) {
            if (c)
                sq.gcycle++;
            gen_run(p, sq.gcycle);
            sq.gen_dirty = 0;
        }
    }
    if (l == 0 && sq.gen_dirty && p->algo != AL_MAN) {
        gen_run(p, sq.gcycle);
        sq.gen_dirty = 0;
    }
    pos = lane_pos(l, c, L, lane_dir(p, l));
    sq.lpos[l] = (int8_t)pos;
    sq.lc[l] = c + 1u;
    if (l == 0) {
        const step_t *s = &p->st[pos];
        int trig = p->algo == AL_MAN ? s->trig : gen_trig[pos];
        if (trig && (p->prob >= 100u || (int)(xr32(&rng_au) % 100u) < p->prob)) {
            uint32_t delay = (c & 1u) ? D * p->swing / 200u : 0u, r, nr = s->rat + 1u;
            float vel = trig == 2 ? 1.0f : 0.78f;
            if (p->human) {
                delay += xr32(&rng_au) % (D * p->human / 400u + 1u);
                vel += (float)(int32_t)xr32(&rng_au) * ((float)p->human / 100.0f * 0.18f / 2147483648.0f);
            }
            for (r = 0; r < nr; r++)
                ev_add(sq.tick + delay + r * D / nr, s->pit, r ? vel * 0.85f : vel, r ? (uint8_t)(s->fl & FL_REV) : s->fl,
                       (uint8_t)pos);
        }
    } else {
        const mod_t *m = &p->mod[l - 1];
        sq.mtgt[l - 1] = mod_value(m, l, pos);
    }
}

static void on_tick(uint32_t t)
{
    pat_t *p = &pats(sq.cur);
    uint32_t l, i;
    for (l = 0; l <= NMOD; l++)
        if (t % lane_div(p, (int)l) == 0u)
            lane_step((int)l);
    for (i = 0; i < sq.nev;) {
        if ((int32_t)(sq.ev[i].at - t) <= 0) {
            ev_t e = sq.ev[i];
            sq.ev[i] = sq.ev[--sq.nev];
            hit(e.semis, e.vel, e.fl, e.step);
        } else {
            i++;
        }
    }
    if (set.clkout && !set.clk && t % (PPQ / 24u) == 0u)
        plat_midi_send(0x0Fu | 0xF8u << 8, TO_USB);
}

static void seq_reset(void)
{
    uint32_t k;
    sq.tick = 0;
    sq.frac = 0.0f;
    sq.first = 1;
    sq.nev = 0;
    sq.gcycle = 0;
    sq.walk_e = 0.0f;
    sq.logi_x = 0.0f;
    sq.cell_row = 0;
    for (k = 0; k <= NMOD; k++) {
        sq.lc[k] = 0;
        sq.lpos[k] = -1;
        sq.pdir[k] = 1;
    }
}
static void seq_start(void)
{
    seq_reset();
    if (sq.next != 0xFFu) {
        sq.cur = sq.next;
        sq.next = 0xFFu;
    }
    sq.ext_c = -1;
    sq.running = 1;
    if (set.clkout && !set.clk)
        plat_midi_send(0x0Fu | 0xFAu << 8, TO_USB);
}
static void seq_stop(void)
{
    sq.running = 0;
    sq.nev = 0;
    if (set.clkout && !set.clk)
        plat_midi_send(0x0Fu | 0xFCu << 8, TO_USB);
    if (sq.note_out >= 0) {
        plat_midi_send(0x08u | 0x80u << 8 | (uint32_t)sq.note_out << 16, TO_USB);
        sq.note_out = -1;
    }
}

/* a step's sound: the pattern's, or the kick its lock names, with its parameter locks (no MOD lanes) */
static void step_sound(const pat_t *p, const lock_t *lk, float *out)
{
    const uint8_t *base = lk && lk->kit && lk->kit <= NPRE ? PRE[lk->kit - 1] : p->snd;
    uint32_t k;
    for (k = 0; k < KP_N; k++)
        out[k] = (float)base[k];
    if (lk)
        for (k = 0; k < NLOCK; k++)
            if (lk->id[k] && lk->id[k] <= KP_N)
                out[lk->id[k] - 1] = (float)lk->val[k];
}

/* the sound as played: the step's (the pattern's with the hit's p-locks), moved by the MOD lanes */
static void eff_compute(void)
{
    const pat_t *p = &pats(sq.cur);
    uint32_t k, m;
    step_sound(p, hit_has ? &hit_lk : 0, eff.p);
    for (m = 0; m < NMOD; m++) {
        const mod_t *md = &p->mod[m];
        int kp;
        float range;
        if (!sq.running || md->tgt == 0u || md->tgt >= NTGT || !md->depth)
            continue;
        kp = MOD_TGT_KP[md->tgt];
        range = kp == KP_TUNE ? 24.0f : 127.0f;
        eff.p[kp] += (float)md->depth / 100.0f * sq.mval[m] * range;
    }
    for (k = 0; k < KP_N; k++) {
        float lo = (float)kp_lo((int)k), hi = (float)kp_hi((int)k);
        eff.p[k] = eff.p[k] < lo ? lo : eff.p[k] > hi ? hi : eff.p[k];
    }
}
static void eff_update(void)             /* each block: the MOD lanes glide, then the sound */
{
    const pat_t *p = &pats(sq.cur);
    uint32_t m;
    for (m = 0; m < NMOD; m++) {
        const mod_t *md = &p->mod[m];
        float coef = 1.0f;
        if (md->smooth) {
            float tc = 1.0f * ((float)md->smooth / 127.0f) * ((float)md->smooth / 127.0f) + 0.002f;
            coef = 1.0f - fm_expf(-(float)APP_BLOCK / 44100.0f / tc);
        }
        sq.mval[m] += (sq.mtgt[m] - sq.mval[m]) * coef;
    }
    eff_compute();
}

/* MIDI in: notes play the kick, chromatically (36 is the pattern's own tune); the clock, when CLOCK is MIDI */
static void ext_tick_to(uint32_t target)
{
    while (sq.running && (int32_t)(sq.tick - target) < 0) {
        if (sq.first) {
            sq.first = 0;
            on_tick(0);
            continue;
        }
        sq.tick++;
        on_tick(sq.tick);
    }
}
static void midi_in(void)
{
    uint32_t pkt;
    int port;
    while (plat_midi_in(&pkt, &port)) {
        uint32_t b = (pkt >> 8) & 0xFFu, st = b & 0xF0u, d1 = (pkt >> 16) & 0x7Fu, d2 = pkt >> 24;
        if (b >= 0xF8u) {
            if (!set.clk)
                continue;
            if (b == 0xF8u) {
                uint32_t per = sq.smp - sq.ext_last;
                sq.ext_last = sq.smp;
                if (per > 100u && per < 20000u) {
                    sq.ext_per = sq.ext_per ? sq.ext_per + ((float)per - sq.ext_per) * 0.2f : (float)per;
                    sq.inc = (float)(PPQ / 24u) / sq.ext_per;
                    sq.ext_bpm10 = (uint16_t)(44100.0f * 60.0f / 24.0f / sq.ext_per * 10.0f + 0.5f);
                }
                if (sq.running) {
                    sq.ext_c++;
                    ext_tick_to((uint32_t)sq.ext_c * (PPQ / 24u));
                    if (sq.ext_c == 0 && sq.first) {
                        sq.first = 0;
                        on_tick(0);
                    }
                }
            } else if (b == 0xFAu) {
                seq_start();
            } else if (b == 0xFBu) {
                sq.running = 1;
            } else if (b == 0xFCu) {
                seq_stop();
            }
            continue;
        }
        midi_seen++;
        if (set.mch && st >= 0x80u && st <= 0xE0u && (b & 0x0Fu) != (uint32_t)set.mch - 1u)
            continue;
        if (st == 0x90u && d2) {
            hit_has = 0;
            eff_compute();
            kdsp_hit(&voice, &eff, (float)((int)d1 - 36), (float)d2 / 127.0f, 0, 0);
            sq.hits++;
        }
    }
}

/* ---------------------------------------------------------------- the render --- */
static float vbuf[APP_BLOCK];
static float gain_now;

static void render_to(uint32_t *from, uint32_t to)
{
    if (to > *from) {
        kdsp_render(&voice, &eff, vbuf + *from, to - *from);
        *from = to;
    }
}

void app_render(int32_t *lr, uint32_t n, uint32_t master_q12)
{
    uint32_t i, done = 0;
    float gain = (float)master_q12 / 4096.0f * 8300000.0f;
    if (n > APP_BLOCK)
        n = APP_BLOCK;
    tempo_set();
    eff_update();
    midi_in();
    if (sq.aud_req) {                    /* a black key or a key held: the kick now */
        sq.aud_req = 0;
        if (sq.aud_step < NST) {
            hit_lk = pats(sq.cur).lk[sq.aud_step];
            hit_has = 1;
        } else {
            hit_has = 0;
        }
        eff_compute();
        kdsp_hit(&voice, &eff, (float)sq.aud_semis, (float)sq.aud_vel / 127.0f, 0, 0);
        sq.hits++;
    }
    for (i = 0; i < n; i++) {
        sq.smp++;
        if (!sq.running)
            continue;
        if (sq.first && !set.clk) {      /* the first step, at once */
            sq.first = 0;
            render_to(&done, i);
            on_tick(0);
            continue;
        }
        if (set.clk) {                   /* following a clock: up to just before the next one's tick */
            uint32_t cap = sq.ext_c < 0 ? 0u : (uint32_t)sq.ext_c * (PPQ / 24u) + (PPQ / 24u) - 1u;
            if (sq.ext_c < 0 || sq.first || (int32_t)(sq.tick - cap) >= 0)
                continue;
        }
        sq.frac += sq.inc;
        while (sq.frac >= 1.0f) {
            sq.frac -= 1.0f;
            sq.tick++;
            render_to(&done, i);         /* what sounded up to here, then the tick's hits */
            on_tick(sq.tick);
        }
    }
    render_to(&done, n);
    for (i = 0; i < n; i++) {
        float s;
        gain_now += (gain - gain_now) * 0.004f;
        s = fm_tanhf(vbuf[i]) * gain_now;
        lr[2 * i] = (int32_t)s;
        lr[2 * i + 1] = (int32_t)s;
    }
}

void app_panic(void)
{
    sq.running = 0;
    voice.active = 0;
    voice.tail = 0.0f;
}

/* ---------------------------------------------------------------- patterns --- */
static void pat_sound(pat_t *p, int pre)
{
    uint32_t k;
    for (k = 0; k < KP_N; k++)
        p->snd[k] = PRE[pre][k];
    p->preset = (uint8_t)pre;
}
static void mod_init(mod_t *m)
{
    uint32_t i;
    m->tgt = 0;
    m->shape = SH_MAN;
    m->len = 16;
    m->div = 2;
    m->dir = 0;
    m->smooth = 0;
    m->depth = 0;
    m->seed = 1;
    for (i = 0; i < NST; i++)
        m->val[i] = 0;
}
static void lock_clear(lock_t *l)
{
    uint32_t k;
    l->kit = 0;
    for (k = 0; k < NLOCK; k++)
        l->id[k] = l->val[k] = 0;
}
static int lock_count(const lock_t *l)
{
    int n = l->kit ? 1 : 0, k;
    for (k = 0; k < NLOCK; k++)
        n += l->id[k] != 0;
    return n;
}
static int lock_find(const lock_t *l, int kp)
{
    int k;
    for (k = 0; k < NLOCK; k++)
        if (l->id[k] == kp + 1)
            return k;
    return -1;
}
static int lock_set(lock_t *l, int kp, int v)   /* 0: no room (NLOCK parameters already) */
{
    int k = lock_find(l, kp);
    if (k < 0)
        for (k = 0; k < NLOCK && l->id[k]; k++)
            ;
    if (k >= NLOCK)
        return 0;
    l->id[k] = (uint8_t)(kp + 1);
    l->val[k] = (uint8_t)v;
    return 1;
}
static void pat_clear(pat_t *p)
{
    uint32_t i;
    for (i = 0; i < NST; i++) {
        p->st[i].trig = 0;
        p->st[i].fl = 0;
        p->st[i].pit = 0;
        p->st[i].rat = 0;
    }
    p->len = 16;
    p->div = 2;
    p->dir = 0;
    p->swing = 0;
    p->prob = 100;
    p->human = 0;
    p->algo = AL_MAN;
    p->ga[0] = 34;
    p->ga[1] = 0;
    p->ga[2] = 40;
    p->ga[3] = 0;
    for (i = 0; i < NMOD; i++)
        mod_init(&p->mod[i]);
    for (i = 0; i < NST; i++)
        lock_clear(&p->lk[i]);
    for (i = 0; i < 4u; i++)
        p->rsv[i] = 0;
}
static void pat_floor(pat_t *p)
{
    uint32_t i;
    for (i = 0; i < NST; i += 4u)
        p->st[i].trig = 1;
    p->st[0].trig = 2;
}
/* the sixteen patterns of the first power-on: one of each generator, each with a sound of its own */
static void pats_defaults(void)
{
    uint32_t i;
    pk[0].magic = pk[1].magic = PATS_MAGIC;
    for (i = 0; i < NPAT; i++) {
        pat_t *p = &pats(i);
        pat_clear(p);
        pat_sound(p, (int)(i % NPRE));
        pat_floor(p);
    }
    /* P02 euclid 5 in 16, the decay ramps */
    pats(1).algo = AL_EUCL;
    pats(1).ga[0] = 34;
    pats(1).ga[2] = 40;
    pats(1).mod[0].tgt = 6;              /* DECAY */
    pats(1).mod[0].shape = SH_UP;
    pats(1).mod[0].len = 7;
    pats(1).mod[0].depth = -30;
    /* P03 a break, varied */
    pats(2).algo = AL_BREAK;
    pats(2).ga[0] = 0;
    pats(2).ga[1] = 20;
    pats(2).ga[2] = 40;
    /* P04 a clave */
    pats(3).algo = AL_CLAVE;
    pats(3).ga[0] = 14;
    pats(3).ga[2] = 40;
    /* P05 the floor with a slide and a reversed sweep */
    pats(4).st[6].trig = 1;
    pats(4).st[7].trig = 1;
    pats(4).st[7].fl = FL_SLIDE;
    pats(4).st[7].pit = 5;
    pats(4).st[14].trig = 1;
    pats(4).st[14].fl = FL_REV;
    /* P06 a random walk, toms in random pitches */
    pats(5).algo = AL_WALK;
    pats(5).ga[0] = 70;
    pats(5).ga[1] = 64;
    pats(5).ga[2] = 70;
    pats(5).ga[3] = 3;
    pats(5).mod[0].tgt = 1;              /* TUNE */
    pats(5).mod[0].shape = SH_RAND;
    pats(5).mod[0].depth = 30;
    /* P07 cellular */
    pats(6).algo = AL_CELL;
    pats(6).ga[0] = 45;                  /* rule 90 */
    pats(6).ga[1] = 9;
    pats(6).ga[2] = 16;
    pats(6).ga[3] = 30;
    /* P08 golden */
    pats(7).algo = AL_GOLD;
    pats(7).ga[0] = 50;
    pats(7).ga[2] = 40;
    /* P09 poly 3 against 4 */
    pats(8).algo = AL_POLY;
    pats(8).ga[0] = 36;
    pats(8).ga[1] = 54;
    pats(8).ga[3] = 64;
    /* P10 logistic */
    pats(9).algo = AL_LOGI;
    pats(9).ga[0] = 110;
    pats(9).ga[1] = 64;
    pats(9).ga[2] = 40;
    pats(9).ga[3] = 60;
    /* P11 wave */
    pats(10).algo = AL_WAVE;
    pats(10).ga[0] = 18;
    pats(10).ga[1] = 54;
    pats(10).ga[2] = 60;
    /* P12 the floor with a roll at the end and a colour lane */
    pats(11).st[15].trig = 1;
    pats(11).st[15].rat = 2;
    pats(11).mod[0].tgt = 8;             /* COLOUR */
    pats(11).mod[0].shape = SH_TRI;
    pats(11).mod[0].len = 5;
    pats(11).mod[0].depth = 40;
}

static void set_defaults(void)
{
    set.magic = SET_MAGIC;
    set.bpm10 = 1280;
    set.cur = 0;
    set.mch = 0;
    set.clk = 0;
    set.clkout = 0;
    set.notes_out = 1;
}

static int pats_valid(void)
{
    uint32_t i, k, m;
    if (pk[0].magic != PATS_MAGIC || pk[1].magic != PATS_MAGIC)
        return 0;
    for (i = 0; i < NPAT; i++) {
        pat_t *p = &pats(i);
        if (p->len < 1u || p->len > NST || p->div >= NDIV || p->dir >= NDIR || p->algo >= NALGO || p->swing > 100u ||
            p->prob > 100u || p->human > 100u)
            return 0;
        for (k = 0; k < KP_N; k++)
            if (p->snd[k] < kp_lo((int)k) || p->snd[k] > kp_hi((int)k))
                return 0;
        for (m = 0; m < NST; m++) {
            const lock_t *l = &p->lk[m];
            if (l->kit > NPRE)
                return 0;
            for (k = 0; k < NLOCK; k++)
                if (l->id[k] > KP_N || (l->id[k] && (l->val[k] < kp_lo(l->id[k] - 1) || l->val[k] > kp_hi(l->id[k] - 1))))
                    return 0;
        }
        for (m = 0; m < NMOD; m++)
            if (p->mod[m].len < 1u || p->mod[m].len > NST || p->mod[m].div >= NDIV || p->mod[m].dir >= NDIR ||
                p->mod[m].shape >= NSHAPE || p->mod[m].tgt >= NTGT)
                return 0;
    }
    return 1;
}

/* the patterns from the flash; 0.1.0's (one object, no p-locks) are moved to this layout, their steps unlocked */
static int pats_load(void)
{
    uint8_t *raw = (uint8_t *)pk;
    int n = plat_store_load(OBJ_PATS0, &pk[0], sizeof pk[0]);
    if (n == (int)sizeof pk[0] && plat_store_load(OBJ_PATS1, &pk[1], sizeof pk[1]) == (int)sizeof pk[1])
        return pats_valid();
    n = plat_store_load(OBJ_PATS0, raw, sizeof pk);
    if (n == 4 + NPAT * 176 && *(uint32_t *)raw == PATS_MAGIC1) {
        int i;
        for (i = NPAT - 1; i >= 0; i--) {   /* from the last: each one moves up, never over one still to move */
            uint8_t *from = raw + 4 + i * 176, *to = (uint8_t *)&pats(i);
            int b;
            for (b = 171; b >= 0; b--)
                to[b] = from[b];
            lock_t *l = pats(i).lk;
            for (b = 0; b < NST; b++)
                lock_clear(&l[b]);
            for (b = 0; b < 4; b++)
                pats(i).rsv[b] = 0;
        }
        pk[0].magic = pk[1].magic = PATS_MAGIC;
        if (pats_valid()) {
            ui_pat_dirty_boot = 1;          /* written in the new layout at the first autosave */
            return 1;
        }
    }
    return 0;
}

void app_init(void)
{
    if (plat_store_load(OBJ_SET, &set, sizeof set) != (int)sizeof set || set.magic != SET_MAGIC || set.cur >= NPAT ||
        set.bpm10 < 400u || set.bpm10 > 2400u || set.mch > 16u || set.clk > 1u)
        set_defaults();
    if (!pats_load())
        pats_defaults();
    kdsp_init(&voice, 0xC0FFEEu);
    sq.cur = set.cur;
    sq.next = 0xFFu;
    sq.note_out = -1;
    seq_reset();
    tempo_set();
    eff_update();
}

/* --------------------------------------------------------------- randomise --- */
static void rnd_param(pat_t *p, int k)
{
    uint32_t *s = &rng_ui;
    int v;
    switch (k) {
    case KP_TUNE: v = rr(s, 26, 40); break;
    case KP_BEND: v = rr(s, 20, 110); break;
    case KP_TIME: v = rr(s, 35, 85); break;
    case KP_CURVE: v = rr(s, 20, 120); break;
    case KP_TAIL: v = rr(s, 0, 1) ? 0 : rr(s, 2, 30); break;
    case KP_TTIME: v = rr(s, 40, 110); break;
    case KP_GLIDE: v = rr(s, 20, 80); break;
    case KP_DRIFT: v = rr(s, 0, 4) < 3 ? 0 : rr(s, 5, 40); break;
    case KP_ATK: v = rr(s, 0, 4) ? 0 : rr(s, 1, 12); break;
    case KP_DEC: v = rr(s, 50, 105); break;
    case KP_HOLD: v = rr(s, 0, 60); break;
    case KP_LEVEL: v = rr(s, 104, 114); break;
    case KP_COLOUR: v = rr(s, 40, 120); break;
    case KP_PULSE: v = rr(s, 10, 90); break;
    case KP_NOISE: v = rr(s, 0, 1) ? 0 : rr(s, 5, 40); break;
    case KP_SNAP: v = rr(s, 20, 70); break;
    case KP_BODY: v = rr(s, 90, 127); break;
    case KP_SUB: v = rr(s, 0, 2) ? rr(s, 0, 30) : rr(s, 30, 70); break;
    case KP_SUBT: v = rr(s, 0, 4) ? 1 : rr(s, 0, 1) ? 0 : 3; break;
    case KP_RESO: v = rr(s, 70, 125); break;
    case KP_DTYPE: { int r = rr(s, 0, 19); v = r < 10 ? DT_SOFT : r < 15 ? DT_HARD : r < 18 ? DT_FOLD : DT_CRUSH; break; }
    case KP_DRIVE: v = rr(s, 0, 100); break;
    case KP_SHAPE: v = rr(s, 0, 100); break;
    case KP_TONE: v = rr(s, 70, 127); break;
    default: v = 64; break;
    }
    p->snd[k] = (uint8_t)clamp(v, kp_lo(k), kp_hi(k));
}
static void rnd_sound(pat_t *p, int page)   /* page -1: the whole sound */
{
    int k;
    for (k = 0; k < KP_N; k++)
        if (page < 0 || k / 4 == page)
            rnd_param(p, k);
}
static void rnd_steps(pat_t *p)
{
    uint32_t i, *s = &rng_ui;
    int style = rr(s, 0, 3);
    for (i = 0; i < NST; i++) {
        step_t *t = &p->st[i];
        t->trig = 0;
        t->fl = 0;
        t->pit = 0;
        t->rat = 0;
        if (style < 2 ? i % 4u == 0u : (i == 0u || rr(s, 0, 3) == 0))
            t->trig = 1;
        if (style == 1 && i % 4u != 0u && rr(s, 0, 5) == 0)
            t->trig = 1;
        if (t->trig && rr(s, 0, 4) == 0)
            t->trig = 2;
        if (t->trig && i && rr(s, 0, 9) == 0)
            t->rat = (uint8_t)rr(s, 1, 2);
        if (t->trig && i && rr(s, 0, 9) == 0)
            t->fl |= FL_SLIDE;
        if (t->trig && rr(s, 0, 11) == 0)
            t->fl |= FL_REV;
        if (t->trig && rr(s, 0, 7) == 0)
            t->pit = (int8_t)(rr(s, 0, 1) ? rr(s, -5, 7) : 12 * rr(s, -1, 1));
    }
    p->st[0].trig = p->st[0].trig ? p->st[0].trig : 1;
}
static void rnd_gen(pat_t *p)
{
    uint32_t *s = &rng_ui, k;
    p->algo = (uint8_t)rr(s, 1, NALGO - 1);
    for (k = 0; k < 4u; k++)
        p->ga[k] = (uint8_t)rr(s, 0, 127);
    if (p->algo == AL_EUCL)
        p->ga[0] = (uint8_t)rr(s, 15, 70);
    if (p->ga[3] > 90u && p->algo != AL_WAVE && p->algo != AL_CELL)
        p->ga[3] = (uint8_t)rr(s, 0, 40);
}
static void rnd_feel(pat_t *p)
{
    uint32_t *s = &rng_ui;
    p->swing = (uint8_t)(rr(s, 0, 2) ? 0 : rr(s, 10, 45));
    p->human = (uint8_t)(rr(s, 0, 2) ? 0 : rr(s, 5, 30));
    p->prob = (uint8_t)(rr(s, 0, 2) ? 100 : rr(s, 70, 95));
}
static void rnd_mod(mod_t *m)
{
    uint32_t *s = &rng_ui, i;
    static const uint8_t TG[] = {1, 2, 3, 6, 8, 9, 13, 14, 15, 16};
    m->tgt = TG[rr(s, 0, (int)sizeof TG - 1)];
    m->shape = (uint8_t)rr(s, 0, NSHAPE - 1);
    m->len = (uint8_t)rr(s, 3, 16);
    m->div = (uint8_t)(rr(s, 0, 2) ? 2 : rr(s, 3, 6));
    m->dir = (uint8_t)(rr(s, 0, 3) ? 0 : rr(s, 1, NDIR - 1));
    m->smooth = (uint8_t)(rr(s, 0, 2) ? 0 : rr(s, 10, 60));
    m->depth = (int8_t)(rr(s, 10, 50) * (rr(s, 0, 1) ? 1 : -1));
    if (m->tgt == 1)
        m->depth = (int8_t)(m->depth / 3);
    m->seed = (uint8_t)rr(s, 0, 127);
    for (i = 0; i < NST; i++)
        m->val[i] = (int8_t)rr(s, -64, 63);
}
/* random p-locks: on about a third of the steps that play, a kick of another kind, one or two parameters moved, or both */
static void rnd_locks(pat_t *p)
{
    static const uint8_t KP_LK[] = {KP_TUNE, KP_BEND, KP_TIME, KP_DEC, KP_COLOUR, KP_PULSE, KP_NOISE, KP_TAIL, KP_RESO,
                                    KP_DRIVE, KP_SHAPE, KP_TONE, KP_HOLD, KP_SUB, KP_DTYPE};
    uint32_t *s = &rng_ui, i;
    int dens = rr(s, 20, 45);
    for (i = 0; i < NST; i++) {
        lock_t *l = &p->lk[i];
        int on = p->algo == AL_MAN ? p->st[i].trig : 1, n, j;
        lock_clear(l);
        if (i >= p->len || !on || rr(s, 0, 99) >= (p->algo == AL_MAN ? dens : dens / 2))
            continue;
        if (rr(s, 0, 3) == 0)            /* the kick of another kind */
            l->kit = (uint8_t)rr(s, 1, NPRE);
        n = l->kit && rr(s, 0, 1) ? 0 : rr(s, 1, 2);
        for (j = 0; j < n; j++) {
            int kp = KP_LK[rr(s, 0, (int)sizeof KP_LK - 1)], base = l->kit ? PRE[l->kit - 1][kp] : p->snd[kp], v;
            if (kp == KP_TUNE)
                v = base + (rr(s, 0, 1) ? rr(s, -7, 7) : 12 * rr(s, -1, 1));
            else if (kp == KP_DTYPE)
                v = rr(s, 0, DT_N - 1);
            else
                v = base + rr(s, -45, 45);
            lock_set(l, kp, clamp(v, kp_lo(kp), kp_hi(kp)));
        }
    }
}

static void rnd_seq(pat_t *p)
{
    uint32_t *s = &rng_ui, m;
    rnd_steps(p);
    if (rr(s, 0, 4))
        rnd_gen(p);
    else
        p->algo = AL_MAN;
    rnd_feel(p);
    for (m = 0; m < NMOD; m++) {
        mod_init(&p->mod[m]);
        if (rr(s, 0, 2) > (int)m)
            rnd_mod(&p->mod[m]);
    }
}

/* ---------------------------------------------------------------------- UI --- */
enum { M_HOME, M_SEQ, M_SND, M_MOD, M_PAT, NMODE };
static const uint8_t NPAGE[NMODE] = {1, 3, 6, 6, 2};
enum { A_COPY, A_PASTE, A_CLEAR, A_INIT, A_RAND, NACT };
static const char *const ACT_NAME[NACT] = {"COPY", "PASTE", "CLEAR", "INIT SND", "RANDOM"};

static struct {
    uint8_t mode, page[NMODE];
    char msg[2][24];
    uint32_t msg_until;
    uint32_t keys, btn;
    int8_t held;                         /* the white key held (0..15), -1 none */
    uint8_t held_turned;                 /* a knob turned while it was held: no toggle on release */
    uint8_t sel;                         /* the step last touched */
    uint8_t act;
    uint8_t redraw_h, redraw_c, redraw_f;
    uint32_t last_change;
    uint8_t set_dirty, pat_dirty;
    pat_t clip;
    uint8_t has_clip;
    uint32_t taps[4];
    uint8_t ntap;
    uint32_t rec_t;
    /* the scope */
    int8_t smin[240], smax[240];
    uint8_t spitch[240];
    float sdur;
    uint32_t shash;
    uint8_t szoom;
} ui;

static pat_t *P(void) { return &pats(sq.cur); }

static void text_cat(char *d, const char *s)
{
    while (*d)
        d++;
    while (*s)
        *d++ = *s++;
    *d = 0;
}
static void int_text(int32_t v, char *b)
{
    char t[12];
    int n = 0;
    uint32_t u = (uint32_t)(v < 0 ? -v : v);
    do
        t[n++] = (char)('0' + u % 10u);
    while ((u /= 10u) && n < 11);
    if (v < 0)
        *b++ = '-';
    while (n)
        *b++ = t[--n];
    *b = 0;
}
static void sint_text(int32_t v, char *b)   /* with its sign */
{
    if (v > 0) {
        *b++ = '+';
    }
    int_text(v, b);
}
static void dec1_text(int32_t v10, char *b)  /* 128.0 */
{
    char t[12];
    int_text(v10 / 10, b);
    t[0] = '.';
    t[1] = (char)('0' + (v10 < 0 ? -v10 : v10) % 10);
    t[2] = 0;
    text_cat(b, t);
}
static void note_text(int note, char *b)
{
    static const char *const N[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    char o[8];
    b[0] = 0;
    text_cat(b, N[note % 12]);
    int_text(note / 12 - 1, o);
    text_cat(b, o);
}
static void time_text(float s, char *b)
{
    char t[12];
    b[0] = 0;
    if (s < 0.01f) {
        int32_t v = (int32_t)(s * 10000.0f + 0.5f);
        int_text(v / 10, b);
        t[0] = '.';
        t[1] = (char)('0' + v % 10);
        t[2] = 0;
        text_cat(b, t);
        text_cat(b, "MS");
    } else if (s < 1.0f) {
        int_text((int32_t)(s * 1000.0f + 0.5f), b);
        text_cat(b, "MS");
    } else {
        int32_t v = (int32_t)(s * 100.0f + 0.5f);
        int_text(v / 100, b);
        t[0] = '.';
        t[1] = (char)('0' + v / 10 % 10);
        t[2] = (char)('0' + v % 10);
        t[3] = 'S';
        t[4] = 0;
        text_cat(b, t);
    }
}
static void hz_text(float hz, char *b)
{
    if (hz >= 1000.0f) {
        int32_t v = (int32_t)(hz / 100.0f + 0.5f);
        dec1_text(v, b);
        text_cat(b, "K");
    } else {
        int_text((int32_t)(hz + 0.5f), b);
        text_cat(b, "HZ");
    }
}

void app_say(const char *a, const char *b)
{
    uint32_t i;
    char t[48];
    for (i = 0; i < 23u && a && a[i]; i++)
        ui.msg[0][i] = a[i];
    ui.msg[0][i] = 0;
    for (i = 0; i < 23u && b && b[i]; i++)
        ui.msg[1][i] = b[i];
    ui.msg[1][i] = 0;
    ui.msg_until = plat_ms() + 1300u;
    ui.redraw_h = 1;
    if (ann_on()) {
        t[0] = 0;
        text_cat(t, ui.msg[0]);
        if (ui.msg[1][0]) {
            text_cat(t, " ");
            text_cat(t, ui.msg[1]);
        }
        ann_say(t, 0, ANN_NOW);
    }
}

static void changed(void)
{
    ui.pat_dirty |= (uint8_t)(1u << (sq.cur >> 3));
    ui.last_change = plat_ms();
    ui.redraw_c = ui.redraw_f = 1;
}
static void set_changed(void)
{
    ui.set_dirty = 1;
    ui.last_change = plat_ms();
    ui.redraw_h = ui.redraw_f = 1;
}

/* the sound's parameter text */
static void param_text(int k, int v, char *b)
{
    switch (k) {
    case KP_TUNE: note_text(v, b); break;
    case KP_BEND: sint_text(v * 60 / 127, b); text_cat(b, "ST"); break;
    case KP_TAIL: sint_text(-(v * 24 / 127), b); text_cat(b, "ST"); break;
    case KP_TIME: time_text(0.001f * fm_exp2f((float)v * (8.6439f / 127.0f)), b); break;
    case KP_TTIME: time_text(0.01f * fm_exp2f((float)v * (8.2288f / 127.0f)), b); break;
    case KP_DEC: time_text(0.02f * fm_exp2f((float)v * (8.2288f / 127.0f)), b); break;
    case KP_GLIDE: time_text(0.001f + 0.5f * ((float)v / 127.0f) * ((float)v / 127.0f), b); break;
    case KP_ATK: time_text(0.05f * ((float)v / 127.0f) * ((float)v / 127.0f), b); break;
    case KP_HOLD: time_text(0.3f * ((float)v / 127.0f) * ((float)v / 127.0f), b); break;
    case KP_SNAP: time_text(0.0002f * fm_exp2f((float)v * (6.6439f / 127.0f)), b); break;
    case KP_COLOUR: hz_text(200.0f * fm_exp2f((float)v * (6.3219f / 127.0f)), b); break;
    case KP_TONE:
        if (v >= 127) {
            b[0] = 0;
            text_cat(b, "OPEN");
        } else {
            hz_text(200.0f * fm_exp2f((float)v * (6.6439f / 127.0f)), b);
        }
        break;
    case KP_SUBT: b[0] = 0; text_cat(b, SUBT_NAME[clamp(v, 0, 3)]); break;
    case KP_DTYPE: b[0] = 0; text_cat(b, DT_NAME[clamp(v, 0, DT_N - 1)]); break;
    default: int_text(v, b); break;
    }
}

/* the footer's four knobs on each page: name, text, position (0..1000), bipolar */
typedef struct {
    char name[12], val[12];
    int pos;                             /* 0..1000, -1: unused */
    uint8_t bip, hot;
} knob_t;

static int home_kp(int i) { static const int8_t K[4] = {KP_TUNE, KP_BEND, KP_DEC, KP_DRIVE}; return K[i]; }

static void knob_snd(knob_t *k, int kp)
{
    int v = P()->snd[kp], lo = kp_lo(kp), hi = kp_hi(kp);
    k->name[0] = 0;
    text_cat(k->name, PNAME[kp]);
    param_text(kp, v, k->val);
    k->pos = (v - lo) * 1000 / (hi - lo);
}

static int held_step(void) { return ui.held >= 0 ? ui.held : -1; }

static void knob_info(int i, knob_t *k)
{
    pat_t *p = P();
    int pg = ui.page[ui.mode], hs = held_step();
    k->name[0] = k->val[0] = 0;
    k->pos = -1;
    k->bip = 0;
    k->hot = 0;
    if (hs >= 0 && ui.mode != M_PAT) {
        if (ui.mode == M_MOD) {          /* a MOD lane's step: its value */
            mod_t *m = &p->mod[pg / 2];
            if (i == 0) {
                text_cat(k->name, "VALUE");
                sint_text(m->val[hs], k->val);
                k->pos = (m->val[hs] + 64) * 1000 / 127;
                k->bip = 1;
                k->hot = 1;
            }
            return;
        }
        if (ui.mode == M_HOME || ui.mode == M_SND) {   /* the step's p-lock of this knob's parameter */
            const lock_t *l = &p->lk[hs];
            int kp = ui.mode == M_HOME ? home_kp(i) : pg * 4 + i, j = lock_find(l, kp), v, lo = kp_lo(kp), hi = kp_hi(kp);
            float snd[KP_N];
            step_sound(p, l, snd);
            v = (int)snd[kp];
            text_cat(k->name, j >= 0 ? "LOCK " : "");
            text_cat(k->name, PNAME[kp]);
            param_text(kp, v, k->val);
            k->pos = (v - lo) * 1000 / (hi - lo);
            k->hot = j >= 0;
            return;
        }
        {
            step_t *s = &p->st[hs];
            k->hot = 1;
            switch (i) {
            case 0: text_cat(k->name, "PITCH"); sint_text(s->pit, k->val); k->pos = (s->pit + 24) * 1000 / 48; k->bip = 1; break;
            case 1: text_cat(k->name, "RATCHET"); text_cat(k->val, "X"); int_text(s->rat + 1, k->val + 1); k->pos = s->rat * 1000 / 3; break;
            case 2: text_cat(k->name, "SLIDE"); text_cat(k->val, s->fl & FL_SLIDE ? "ON" : "OFF"); k->pos = s->fl & FL_SLIDE ? 1000 : 0; break;
            case 3: text_cat(k->name, "SWEEP"); text_cat(k->val, s->fl & FL_REV ? "REV" : "NORM"); k->pos = s->fl & FL_REV ? 1000 : 0; break;
            }
            return;
        }
    }
    switch (ui.mode) {
    case M_HOME:
        knob_snd(k, home_kp(i));
        break;
    case M_SND:
        knob_snd(k, pg * 4 + i);
        break;
    case M_SEQ:
        if (pg == 0) {
            switch (i) {
            case 0: text_cat(k->name, "LENGTH"); int_text(p->len, k->val); k->pos = (p->len - 1) * 1000 / 15; break;
            case 1: text_cat(k->name, "DIVISION"); text_cat(k->val, DIV_NAME[p->div]); k->pos = p->div * 1000 / (NDIV - 1); break;
            case 2: text_cat(k->name, "DIRECTION"); text_cat(k->val, DIR_NAME[p->dir]); k->pos = p->dir * 1000 / (NDIR - 1); break;
            case 3: text_cat(k->name, "SWING"); int_text(p->swing, k->val); text_cat(k->val, "%"); k->pos = p->swing * 10; break;
            }
        } else if (pg == 1) {
            text_cat(k->name, ALGO_P[p->algo][i]);
            if (p->algo == AL_MAN) {
                text_cat(k->val, "-");
                break;
            }
            k->pos = p->ga[i] * 1000 / 127;
            if (i == 0 && p->algo == AL_EUCL)
                int_text(1 + p->ga[0] * (p->len - 1) / 127, k->val);
            else if (i == 0 && p->algo == AL_CLAVE)
                text_cat(k->val, CLAVE_NAME[p->ga[0] * NCLAVE / 128]);
            else if (i == 0 && p->algo == AL_BREAK)
                text_cat(k->val, BREAK_NAME[p->ga[0] * NBREAK / 128]);
            else if (i == 0 && p->algo == AL_CELL) {
                text_cat(k->val, "R");
                int_text(p->ga[0] * 2, k->val + 1);
            } else if ((i == 0 || i == 1) && p->algo == AL_POLY)
                int_text(1 + p->ga[i] * 7 / 127, k->val);
            else if ((i == 0 || i == 1) && p->algo == AL_WAVE)
                int_text(1 + p->ga[i] * 7 / 127, k->val);
            else if (i == 1 && (p->algo == AL_EUCL || p->algo == AL_CLAVE))
                int_text(p->ga[1] * p->len / 128, k->val);
            else if (i == 2 && p->algo <= AL_GOLD && p->algo != AL_WALK && p->algo != AL_CELL) {
                static const char *const AM[4] = {"NONE", "FIRST", "EVERY 2", "OFFBEAT"};
                text_cat(k->val, AM[p->ga[2] / 32]);
            } else
                int_text(p->ga[i], k->val);
        } else {
            switch (i) {
            case 0: text_cat(k->name, "TEMPO"); dec1_text(set.bpm10, k->val); k->pos = (set.bpm10 - 400) * 1000 / 2000; break;
            case 1: text_cat(k->name, "PROB"); int_text(p->prob, k->val); text_cat(k->val, "%"); k->pos = p->prob * 10; break;
            case 2: text_cat(k->name, "HUMANIZE"); int_text(p->human, k->val); text_cat(k->val, "%"); k->pos = p->human * 10; break;
            case 3: text_cat(k->name, "FINE BPM"); dec1_text(set.bpm10, k->val); k->pos = (set.bpm10 % 10) * 100; break;
            }
        }
        break;
    case M_MOD: {
        mod_t *m = &p->mod[pg / 2];
        if (pg % 2 == 0) {
            switch (i) {
            case 0: {
                int kp = MOD_TGT_KP[m->tgt];
                text_cat(k->name, "TARGET");
                text_cat(k->val, kp < 0 ? "OFF" : PNAME[kp]);
                k->pos = m->tgt * 1000 / (NTGT - 1);
                break;
            }
            case 1: text_cat(k->name, "DEPTH"); sint_text(m->depth, k->val); text_cat(k->val, "%"); k->pos = (m->depth + 100) * 5; k->bip = 1; break;
            case 2: text_cat(k->name, "SHAPE"); text_cat(k->val, SHAPE_NAME[m->shape]); k->pos = m->shape * 1000 / (NSHAPE - 1); break;
            case 3: text_cat(k->name, "LENGTH"); int_text(m->len, k->val); k->pos = (m->len - 1) * 1000 / 15; break;
            }
        } else {
            switch (i) {
            case 0: text_cat(k->name, "DIVISION"); text_cat(k->val, DIV_NAME[m->div]); k->pos = m->div * 1000 / (NDIV - 1); break;
            case 1: text_cat(k->name, "DIRECTION"); text_cat(k->val, DIR_NAME[m->dir]); k->pos = m->dir * 1000 / (NDIR - 1); break;
            case 2: text_cat(k->name, "SMOOTH"); int_text(m->smooth, k->val); k->pos = m->smooth * 1000 / 127; break;
            case 3: text_cat(k->name, "SEED"); int_text(m->seed, k->val); k->pos = m->seed * 1000 / 127; break;
            }
        }
        break;
    }
    case M_PAT:
        if (pg == 0) {
            switch (i) {
            case 0: text_cat(k->name, "PATTERN"); text_cat(k->val, "P"); int_text(sq.cur + 1, k->val + 1); k->pos = sq.cur * 1000 / 15; break;
            case 1: text_cat(k->name, "ACTION"); text_cat(k->val, ACT_NAME[ui.act]); k->pos = ui.act * 1000 / (NACT - 1); break;
            case 2: text_cat(k->name, "PRESET"); text_cat(k->val, PRE_NAME[p->preset % NPRE]); k->pos = p->preset * 1000 / (NPRE - 1); break;
            case 3: text_cat(k->name, "DO IT"); text_cat(k->val, ">>"); k->pos = 0; break;
            }
        } else {
            switch (i) {
            case 0: text_cat(k->name, "IN CH"); if (set.mch) int_text(set.mch, k->val); else text_cat(k->val, "ALL"); k->pos = set.mch * 1000 / 16; break;
            case 1: text_cat(k->name, "CLOCK"); text_cat(k->val, set.clk ? "MIDI" : "INT"); k->pos = set.clk * 1000; break;
            case 2: text_cat(k->name, "CLOCK OUT"); text_cat(k->val, set.clkout ? "ON" : "OFF"); k->pos = set.clkout * 1000; break;
            case 3: text_cat(k->name, "NOTES OUT"); text_cat(k->val, set.notes_out ? "ON" : "OFF"); k->pos = set.notes_out * 1000; break;
            }
        }
        break;
    }
}

static const char *page_title(char *b)
{
    int pg = ui.page[ui.mode];
    b[0] = 0;
    switch (ui.mode) {
    case M_HOME: text_cat(b, "KICK"); break;
    case M_SEQ: text_cat(b, pg == 0 ? "TRIG" : pg == 1 ? "GENERATE" : "FEEL"); break;
    case M_SND: text_cat(b, SND_PAGE[pg]); break;
    case M_MOD: text_cat(b, "MOD "); int_text(pg / 2 + 1, b + 4); text_cat(b, pg % 2 ? " B" : " A"); break;
    case M_PAT: text_cat(b, pg == 0 ? "PATTERNS" : "MIDI"); break;
    }
    return b;
}

static int step_val(int v, int d, int lo, int hi) { return clamp(v + d, lo, hi); }

static void pat_pick(int n);
static void pat_action(void);

static int fast(void) { return (ui.btn >> B_OCTDN) & 1u; }

static void knob_turn(int i, int d)
{
    pat_t *p = P();
    int pg = ui.page[ui.mode], hs = held_step(), d8 = fast() ? d * 8 : d;
    if (hs >= 0 && ui.mode != M_PAT) {
        ui.held_turned = 1;
        if (ui.mode == M_MOD) {
            mod_t *m = &p->mod[pg / 2];
            m->val[hs] = (int8_t)step_val(m->val[hs], d8, -64, 63);
        } else if (ui.mode == M_HOME || ui.mode == M_SND) {   /* a p-lock: from what the step plays now */
            lock_t *l = &p->lk[hs];
            int kp = ui.mode == M_HOME ? home_kp(i) : pg * 4 + i, dd = (kp_hi(kp) - kp_lo(kp)) > 10 ? d8 : d;
            float snd[KP_N];
            step_sound(p, l, snd);
            if (!lock_set(l, kp, step_val((int)snd[kp], dd, kp_lo(kp), kp_hi(kp))))
                app_say("LOCKS FULL", "REC CLEARS");
            if (!p->st[hs].trig)
                p->st[hs].trig = 1;
        } else {
            step_t *s = &p->st[hs];
            switch (i) {
            case 0: s->pit = (int8_t)step_val(s->pit, fast() ? d * 12 : d, -24, 24); break;
            case 1: s->rat = (uint8_t)step_val(s->rat, d, 0, 3); break;
            case 2: s->fl = (uint8_t)(d > 0 ? s->fl | FL_SLIDE : s->fl & ~FL_SLIDE); break;
            case 3: s->fl = (uint8_t)(d > 0 ? s->fl | FL_REV : s->fl & ~FL_REV); break;
            }
            if (!s->trig)
                s->trig = 1;
        }
        changed();
        return;
    }
    switch (ui.mode) {
    case M_HOME:
    case M_SND: {
        int kp = ui.mode == M_HOME ? home_kp(i) : pg * 4 + i, dd = (kp_hi(kp) - kp_lo(kp)) > 10 ? d8 : d;
        p->snd[kp] = (uint8_t)step_val(p->snd[kp], dd, kp_lo(kp), kp_hi(kp));
        changed();
        break;
    }
    case M_SEQ:
        if (pg == 0) {
            switch (i) {
            case 0: p->len = (uint8_t)step_val(p->len, d, 1, NST); break;
            case 1: p->div = (uint8_t)step_val(p->div, d, 0, NDIV - 1); break;
            case 2: p->dir = (uint8_t)step_val(p->dir, d, 0, NDIR - 1); break;
            case 3: p->swing = (uint8_t)step_val(p->swing, d8, 0, 100); break;
            }
            sq.gen_dirty = 1;
        } else if (pg == 1) {
            if (p->algo == AL_MAN)
                return;
            p->ga[i] = (uint8_t)step_val(p->ga[i], d8, 0, 127);
            sq.gen_dirty = 1;
            if (!sq.running)
                gen_run(p, 0);
        } else {
            switch (i) {
            case 0: set.bpm10 = (uint16_t)step_val(set.bpm10, d8 * 10, 400, 2400); set_changed(); break;
            case 1: p->prob = (uint8_t)step_val(p->prob, d8, 0, 100); break;
            case 2: p->human = (uint8_t)step_val(p->human, d8, 0, 100); break;
            case 3: set.bpm10 = (uint16_t)step_val(set.bpm10, d, 400, 2400); set_changed(); break;
            }
        }
        changed();
        break;
    case M_MOD: {
        mod_t *m = &p->mod[pg / 2];
        if (pg % 2 == 0) {
            switch (i) {
            case 0: m->tgt = (uint8_t)step_val(m->tgt, d, 0, NTGT - 1); break;
            case 1: m->depth = (int8_t)step_val(m->depth, fast() ? d * 10 : d, -100, 100); break;
            case 2: m->shape = (uint8_t)step_val(m->shape, d, 0, NSHAPE - 1); break;
            case 3: m->len = (uint8_t)step_val(m->len, d, 1, NST); break;
            }
        } else {
            switch (i) {
            case 0: m->div = (uint8_t)step_val(m->div, d, 0, NDIV - 1); break;
            case 1: m->dir = (uint8_t)step_val(m->dir, d, 0, NDIR - 1); break;
            case 2: m->smooth = (uint8_t)step_val(m->smooth, d8, 0, 127); break;
            case 3: m->seed = (uint8_t)step_val(m->seed, d8, 0, 127); break;
            }
        }
        changed();
        break;
    }
    case M_PAT:
        if (pg == 0) {
            if (i == 0) {
                int n = step_val(sq.next != 0xFFu ? sq.next : sq.cur, d, 0, NPAT - 1);
                pat_pick(n);
            } else if (i == 1) {
                ui.act = (uint8_t)step_val(ui.act, d, 0, NACT - 1);
                ui.redraw_f = 1;
            } else if (i == 2) {
                int n = step_val(p->preset, d, 0, NPRE - 1);
                pat_sound(p, n);
                app_say("PRESET", PRE_NAME[n]);
                changed();
            } else if (i == 3 && d > 0) {
                pat_action();
            }
        } else {
            switch (i) {
            case 0: set.mch = (uint8_t)step_val(set.mch, d, 0, 16); break;
            case 1: set.clk = (uint8_t)step_val(set.clk, d, 0, 1); sq.ext_per = 0.0f; sq.ext_bpm10 = 0; break;
            case 2: set.clkout = (uint8_t)step_val(set.clkout, d, 0, 1); break;
            case 3: set.notes_out = (uint8_t)step_val(set.notes_out, d, 0, 1); break;
            }
            set_changed();
        }
        break;
    }
}

static void pat_pick(int n)
{
    if (sq.running) {
        sq.next = (uint8_t)(n == sq.cur ? 0xFF : n);
    } else {
        sq.cur = (uint8_t)n;
        sq.next = 0xFFu;
        seq_reset();
        sq.gen_dirty = 1;
        gen_run(P(), 0);
    }
    set.cur = (uint8_t)n;
    set_changed();
    ui.redraw_c = 1;
}

static void pat_action(void)
{
    pat_t *p = P();
    switch (ui.act) {
    case A_COPY:
        ui.clip = *p;
        ui.has_clip = 1;
        app_say("COPIED", "");
        return;
    case A_PASTE:
        if (!ui.has_clip) {
            app_say("NOTHING", "TO PASTE");
            return;
        }
        *p = ui.clip;
        app_say("PASTED", "");
        break;
    case A_CLEAR:
        {
            uint8_t snd[KP_N], pre = p->preset;
            uint32_t k;
            for (k = 0; k < KP_N; k++)
                snd[k] = p->snd[k];
            pat_clear(p);
            for (k = 0; k < KP_N; k++)
                p->snd[k] = snd[k];
            p->preset = pre;
        }
        app_say("CLEARED", "");
        break;
    case A_INIT:
        pat_sound(p, 0);
        app_say("SOUND", "INIT");
        break;
    case A_RAND:
        rnd_seq(p);
        rnd_sound(p, -1);
        if (!sq.running)
            gen_run(p, 0);
        rnd_locks(p);
        app_say("RANDOM", "PATTERN");
        break;
    }
    sq.gen_dirty = 1;
    if (!sq.running)
        gen_run(p, 0);
    changed();
}

static void random_page(void)
{
    pat_t *p = P();
    int pg = ui.page[ui.mode];
    switch (ui.mode) {
    case M_HOME: rnd_sound(p, -1); app_say("RANDOM", "SOUND"); break;
    case M_SND: rnd_sound(p, pg); app_say("RANDOM", SND_PAGE[pg]); break;
    case M_SEQ:
        if (pg == 0) {
            rnd_steps(p);
            p->algo = AL_MAN;
            app_say("RANDOM", "STEPS");
        } else if (pg == 1) {
            rnd_gen(p);
            app_say("RANDOM", ALGO_NAME[p->algo]);
        } else {
            rnd_feel(p);
            app_say("RANDOM", "FEEL");
        }
        break;
    case M_MOD: rnd_mod(&p->mod[pg / 2]); app_say("RANDOM", "MOD LANE"); break;
    case M_PAT:
        rnd_seq(p);
        rnd_sound(p, -1);
        if (!sq.running)
            gen_run(p, 0);
        rnd_locks(p);
        app_say("RANDOM", "PATTERN");
        break;
    }
    sq.gen_dirty = 1;
    if (!sq.running)
        gen_run(p, 0);
    changed();
}

static void go_mode(int m, int page)
{
    if (ui.mode == m && page < 0)
        ui.page[m] = (uint8_t)((ui.page[m] + 1u) % NPAGE[m]);
    else if (page >= 0)
        ui.page[m] = (uint8_t)page;
    ui.mode = (uint8_t)m;
    ui.redraw_h = ui.redraw_c = ui.redraw_f = 1;
}

static int white_index(uint32_t key)
{
    static const uint8_t WK[16] = PLAT_WHITE_KEYS;
    int i;
    for (i = 0; i < 16; i++)
        if (WK[i] == key)
            return i;
    return -1;
}
static int black_index(uint32_t key)
{
    static const uint8_t BK[11] = PLAT_BLACK_KEYS;
    int i;
    for (i = 0; i < 11; i++)
        if (BK[i] == key)
            return i;
    return -1;
}

static void step_tap(int s)
{
    pat_t *p = P();
    if (ui.mode == M_MOD) {
        mod_t *m = &p->mod[ui.page[M_MOD] / 2];
        m->val[s] = (int8_t)(m->val[s] ? 0 : 63);
    } else {
        step_t *t = &p->st[s];
        if (p->algo != AL_MAN) {         /* the steps of a generator: printed into the steps first */
            uint32_t i;
            for (i = 0; i < NST; i++)
                p->st[i].trig = gen_trig[i];
            p->algo = AL_MAN;
            app_say("GENERATOR", "TO STEPS");
        }
        t->trig = (uint8_t)((t->trig + 1u) % 3u);
    }
    changed();
}

static void tap_tempo(void)
{
    uint32_t now = plat_ms(), i, sum = 0;
    if (ui.ntap && now - ui.taps[(ui.ntap - 1u) % 4u] > 2000u)
        ui.ntap = 0;
    ui.taps[ui.ntap % 4u] = now;
    ui.ntap++;
    if (ui.ntap >= 2u) {
        uint32_t n = ui.ntap > 4u ? 3u : ui.ntap - 1u;
        for (i = 0; i < n; i++) {
            uint32_t a = (ui.ntap - 1u - i) % 4u, b = (ui.ntap - 2u - i) % 4u;
            sum += ui.taps[a] - ui.taps[b];
        }
        if (sum) {
            int bpm10 = (int)(600000u * n / sum);
            char t[12];
            set.bpm10 = (uint16_t)clamp(bpm10, 400, 2400);
            dec1_text(set.bpm10, t);
            app_say("TAP", t);
            set_changed();
        }
    } else {
        app_say("TAP", "...");
    }
}

static void save_now(void)
{
    int a = plat_store_save(OBJ_PATS0, &pk[0], sizeof pk[0]), a1 = plat_store_save(OBJ_PATS1, &pk[1], sizeof pk[1]);
    int b = plat_store_save(OBJ_SET, &set, sizeof set);
    if (a == 0 && a1 == 0 && b == 0) {
        ui.pat_dirty = ui.set_dirty = 0;
        app_say("SAVED", "");
    } else {
        app_say("SAVE", "FAILED");
    }
}

static void print_gen(void)
{
    pat_t *p = P();
    uint32_t i;
    if (p->algo == AL_MAN) {
        app_say("PRINT", "NO GENERATOR");
        return;
    }
    for (i = 0; i < NST; i++)
        p->st[i].trig = gen_trig[i];
    p->algo = AL_MAN;
    app_say("PRINTED", "TO STEPS");
    changed();
}

static void input(void)
{
    uint32_t keys = plat_keys(), btn = plat_buttons(), k, now = plat_ms();
    for (k = 0; k < NKEYS; k++) {
        uint32_t bit = 1u << k;
        int w = white_index(k), b = black_index(k);
        if ((keys & bit) && !(ui.keys & bit)) {
            if (w >= 0 && ui.mode == M_PAT && ui.page[M_PAT] == 0) {
                pat_pick(w);
            } else if (w >= 0) {
                ui.held = (int8_t)w;
                ui.held_turned = 0;
                ui.sel = (uint8_t)w;
                ui.redraw_c = ui.redraw_f = 1;
                if (!sq.running && ui.mode != M_MOD) {   /* stopped: the step sounds as it is touched */
                    pat_t *p = P();
                    sq.aud_semis = p->st[w].pit;
                    sq.aud_vel = 110;
                    sq.aud_step = (uint8_t)w;
                    sq.aud_req = 1;
                }
            } else if (b >= 0) {         /* black keys: the kick, a semitone apart (OCT+ held: accented) */
                sq.aud_semis = (int8_t)b;
                sq.aud_vel = (btn >> B_OCTUP & 1u) ? 127 : 100;
                sq.aud_step = 0xFFu;
                sq.aud_req = 1;
            }
        } else if (!(keys & bit) && (ui.keys & bit)) {
            if (w >= 0 && ui.held == w) {
                if (!ui.held_turned && !(ui.mode == M_PAT && ui.page[M_PAT] == 0))
                    step_tap(w);
                ui.held = -1;
                ui.redraw_c = ui.redraw_f = 1;
            }
        }
    }
    for (k = 0; k < NB; k++) {
        uint32_t bit = 1u << k;
        if (!(btn & bit) || (ui.btn & bit))
            continue;
        switch (k) {
        case B_HOME: go_mode(M_HOME, 0); break;
        case B_SEQ: go_mode(M_SEQ, ui.mode == M_SEQ ? -1 : (ui.page[M_SEQ] == 1 ? 0 : ui.page[M_SEQ])); break;
        case B_ARP: go_mode(M_SEQ, 1); break;
        case B_EDIT: go_mode(M_SND, ui.mode == M_SND ? -1 : ui.page[M_SND]); break;
        case B_ENV: go_mode(M_SND, 2); break;
        case B_FX: go_mode(M_SND, 5); break;
        case B_LFO: go_mode(M_MOD, ui.mode == M_MOD ? (ui.page[M_MOD] + 2) % 6 : ui.page[M_MOD]); break;
        case B_GLO: go_mode(M_PAT, ui.mode == M_PAT ? -1 : 0); break;
        case B_SEL: tap_tempo(); break;
        case B_PLAY:
            if (set.clk) {
                app_say("CLOCK", "IS MIDI");
            } else if (sq.running) {
                seq_stop();
            } else {
                seq_start();
            }
            ui.redraw_h = ui.redraw_c = 1;
            break;
        case B_REC:
            if (ui.held >= 0 && ui.mode != M_PAT && ui.mode != M_MOD) {   /* a step held: its p-locks cleared */
                char t[12] = "STEP ";
                lock_clear(&P()->lk[ui.held]);
                ui.held_turned = 1;
                int_text(ui.held + 1, t + 5);
                app_say(t, "LOCKS CLEARED");
                changed();
            } else if (btn >> B_OCTUP & 1u) {
                rnd_sound(P(), -1);
                rnd_gen(P());
                sq.gen_dirty = 1;
                if (!sq.running)
                    gen_run(P(), 0);
                app_say("RANDOM", "KICK + GEN");
                changed();
            } else if (btn >> B_OCTDN & 1u) {   /* the sequence and its p-locks (the kick of each step too) */
                rnd_seq(P());
                sq.gen_dirty = 1;
                if (!sq.running)
                    gen_run(P(), 0);
                rnd_locks(P());
                app_say("RANDOM", "SEQ + LOCKS");
                changed();
            } else {
                random_page();
            }
            break;
        case B_SAVE:
            if (btn >> B_OCTDN & 1u)
                print_gen();
            else
                save_now();
            break;
        default:
            break;
        }
    }
    for (k = 0; k < NE; k++) {
        int32_t e = plat_enc((int)k);
        if (!e)
            continue;
        if (k == EN_SELECT) {
            int pg = clamp(ui.page[ui.mode] + e, 0, NPAGE[ui.mode] - 1);
            go_mode(ui.mode, pg);
        } else if (k == EN_ALGO) {
            if (ui.mode == M_SEQ && ui.page[M_SEQ] == 1) {
                pat_t *p = P();
                p->algo = (uint8_t)clamp(p->algo + e, 0, NALGO - 1);
                sq.gen_dirty = 1;
                if (!sq.running)
                    gen_run(p, 0);
                app_say("GENERATOR", ALGO_NAME[p->algo]);
                changed();
            } else if (ui.mode == M_PAT && ui.page[M_PAT] == 0) {
                pat_pick(clamp((sq.next != 0xFFu ? sq.next : sq.cur) + e, 0, NPAT - 1));
            } else {
                char t[12];
                set.bpm10 = (uint16_t)clamp(set.bpm10 + e * (fast() ? 1 : 10), 400, 2400);
                dec1_text(set.bpm10, t);
                app_say("TEMPO", t);
                set_changed();
            }
        } else if (k == EN_PRESET && ui.held >= 0 && ui.mode != M_PAT && ui.mode != M_MOD) {
            pat_t *p = P();                  /* a step held: the kick it plays (a p-lock), or the pattern's */
            lock_t *l = &p->lk[ui.held];
            l->kit = (uint8_t)((l->kit + e % (NPRE + 1) + NPRE + 1) % (NPRE + 1));
            ui.held_turned = 1;
            if (!p->st[ui.held].trig)
                p->st[ui.held].trig = 1;
            app_say("STEP KICK", l->kit ? PRE_NAME[l->kit - 1] : "PATTERN'S");
            if (!sq.running) {
                sq.aud_semis = p->st[ui.held].pit;
                sq.aud_vel = 110;
                sq.aud_step = (uint8_t)ui.held;
                sq.aud_req = 1;
            }
            changed();
        } else if (k == EN_PRESET) {
            pat_t *p = P();
            int n = (p->preset + e % NPRE + NPRE) % NPRE;
            pat_sound(p, n);
            app_say("PRESET", PRE_NAME[n]);
            changed();
        } else {
            knob_turn((int)(k - EN_K1), e);
        }
    }
    if (keys != ui.keys || btn != ui.btn)
        ui.redraw_f = 1;
    ui.keys = keys;
    ui.btn = btn;
    (void)now;
}

void app_input_only(void) { input(); }

/* ------------------------------------------------------------------ drawing --- */
static void fill_circle(int32_t cx, int32_t cy, int32_t r, uint16_t c)
{
    int32_t dy;
    for (dy = -r; dy <= r; dy++) {
        int32_t w = (int32_t)fm_sqrtf((float)(r * r - dy * dy) + 0.25f);
        cv_rect(cx - w, cy + dy, 2 * w + 1, 1, c);
    }
}
static void circle_line(int32_t cx, int32_t cy, int32_t r, uint16_t c)
{
    int32_t i, n = r * 7;
    for (i = 0; i < n; i++) {
        float a = (float)i / (float)n;
        cv_pset(cx + (int32_t)(fm_cosf(a * 6.2831853f) * (float)r + 0.5f), cy + (int32_t)(fm_sinf(a * 6.2831853f) * (float)r + 0.5f), c);
    }
}
static void text_c(int32_t cx, int32_t y, const felucca_font_t *f, const char *s, uint16_t c)
{
    cv_text(cx - text_w(f, s) / 2, y, f, s, c);
}

static void draw_header(void)
{
    char t[24], b[24];
    uint32_t now = plat_ms();
    cv_begin(240, 24, K_BG);
    if (sq.running) {                    /* the play mark */
        int32_t i;
        for (i = 0; i < 9; i++)
            cv_rect(6 + i / 2, 7 + i / 2 + (i & 1) * 0, 1, 1, K_ACC);
        for (i = 0; i < 10; i++)
            cv_rect(6, 6 + i, 1 + (i < 5 ? i : 9 - i), 1, K_ACC);
    } else {
        cv_rect(6, 7, 8, 8, K_DIM);
    }
    t[0] = 'P';
    t[1] = (char)('0' + (sq.cur + 1) / 10);
    t[2] = (char)('0' + (sq.cur + 1) % 10);
    t[3] = 0;
    cv_text(20, 3, &FONT_B, t, K_INK);
    if (sq.next != 0xFFu) {
        b[0] = '>';
        b[1] = (char)('0' + (sq.next + 1) / 10);
        b[2] = (char)('0' + (sq.next + 1) % 10);
        b[3] = 0;
        cv_text(48, 3, &FONT_B, b, (now / 250u) & 1u ? K_ACC : K_MUTE);
    }
    if (now < ui.msg_until) {
        int32_t x = 78;
        x = cv_text(x, 3, &FONT_B, ui.msg[0], K_ACC);
        cv_text(x + 6, 3, &FONT_B, ui.msg[1], K_INK);
    } else {
        cv_text(78, 3, &FONT_B, page_title(t), K_ACC);
        {
            int32_t bpm = set.clk && sq.ext_bpm10 ? sq.ext_bpm10 : set.bpm10;
            dec1_text(bpm, b);
            cv_text(236 - text_w(&FONT_B, b), 3, &FONT_B, b, K_INK);
            if (set.clk)
                cv_text(236 - text_w(&FONT_B, b) - 6 - text_w(&FONT_XS, "MIDI"), 7, &FONT_XS, "MIDI", K_ACC);
        }
    }
    cv_rect(0, 23, 240, 1, K_GRID);
    cv_blit(0, 0);
}

/* the scope: one hit of the pattern's sound (its own, without the MOD lanes), drawn a part at a time */
static kick_t sk;
static kparm_t skp;
static struct {
    uint8_t busy;
    uint32_t done, total, per;
    int8_t mn[240], mx[240];
    uint32_t col;
    float cmin, cmax;
    uint32_t ccount;
} sj;

static uint32_t snd_hash(void)
{
    const pat_t *p = P();
    uint32_t h = 2166136261u, k;
    for (k = 0; k < KP_N; k++)
        h = (h ^ p->snd[k]) * 16777619u;
    if (ui.held >= 0 && (ui.mode == M_HOME || ui.mode == M_SND)) {   /* a step held: its sound, with its p-locks */
        const uint8_t *b = (const uint8_t *)&p->lk[ui.held];
        h = (h ^ (uint32_t)(ui.held + 1) * 0x9E37u) * 16777619u;
        for (k = 0; k < sizeof(lock_t); k++)
            h = (h ^ b[k]) * 16777619u;
    }
    return h ^ (uint32_t)(ui.mode == M_SND && ui.page[M_SND] == 3) * 0x55u;
}
static void scope_start(void)
{
    const pat_t *p = P();
    uint32_t k;
    float lo = 999.0f, hi = -999.0f, v[240];
    step_sound(p, ui.held >= 0 && (ui.mode == M_HOME || ui.mode == M_SND) ? &p->lk[ui.held] : 0, skp.p);
    ui.szoom = ui.mode == M_SND && ui.page[M_SND] == 3;
    ui.sdur = ui.szoom ? 0.04f : kdsp_length(&skp) * 1.15f;
    if (ui.sdur < 0.12f && !ui.szoom)
        ui.sdur = 0.12f;
    if (ui.sdur > 1.6f)
        ui.sdur = 1.6f;
    kdsp_init(&sk, 0xABCDu);
    kdsp_hit(&sk, &skp, 0.0f, 1.0f, 0, 0);
    sj.total = (uint32_t)(ui.sdur * 44100.0f);
    sj.per = sj.total / 240u;
    if (sj.per < 1u)
        sj.per = 1u;
    sj.total = sj.per * 240u;
    sj.done = 0;
    sj.col = 0;
    sj.cmin = 1e9f;
    sj.cmax = -1e9f;
    sj.ccount = 0;
    sj.busy = 1;
    for (k = 0; k < 240u; k++) {         /* the sweep, scaled to fit */
        v[k] = kdsp_pitch_at(&skp, ui.sdur * (float)k / 240.0f, 0);
        if (v[k] < lo)
            lo = v[k];
        if (v[k] > hi)
            hi = v[k];
    }
    if (hi - lo < 12.0f) {
        float m = (hi + lo) * 0.5f;
        lo = m - 6.0f;
        hi = m + 6.0f;
    }
    for (k = 0; k < 240u; k++)
        ui.spitch[k] = (uint8_t)(100.0f * (v[k] - lo) / (hi - lo));
    ui.shash = snd_hash();
}
static void scope_work(void)
{
    float buf[256];
    uint32_t budget = 6144u;
    while (sj.busy && budget) {
        uint32_t n = sj.total - sj.done, i;
        if (n > 256u)
            n = 256u;
        kdsp_render(&sk, &skp, buf, n);
        for (i = 0; i < n; i++) {
            if (buf[i] < sj.cmin)
                sj.cmin = buf[i];
            if (buf[i] > sj.cmax)
                sj.cmax = buf[i];
            if (++sj.ccount >= sj.per) {
                float a = sj.cmin * 52.0f, b = sj.cmax * 52.0f;
                sj.mn[sj.col] = (int8_t)(a < -60.0f ? -60 : a > 60.0f ? 60 : a);
                sj.mx[sj.col] = (int8_t)(b < -60.0f ? -60 : b > 60.0f ? 60 : b);
                sj.col++;
                sj.ccount = 0;
                sj.cmin = 1e9f;
                sj.cmax = -1e9f;
            }
        }
        sj.done += n;
        budget = budget > n ? budget - n : 0u;
        if (sj.done >= sj.total || sj.col >= 240u) {
            uint32_t k;
            for (k = 0; k < 240u; k++) {
                ui.smin[k] = k < sj.col ? sj.mn[k] : 0;
                ui.smax[k] = k < sj.col ? sj.mx[k] : 0;
            }
            sj.busy = 0;
            ui.redraw_c = 1;
        }
    }
}

static void draw_strip(int32_t y)        /* the TRIG lane, sixteen cells */
{
    const pat_t *p = P();
    int i;
    for (i = 0; i < NST; i++) {
        int32_t x = 8 + i * 14;
        int tr = p->algo == AL_MAN ? p->st[i].trig : gen_trig[i];
        uint16_t c = i >= p->len ? K_BG : tr == 2 ? K_ACC : tr ? K_INK : K_GRID;
        if (i >= p->len) {
            cv_rect(x + 5, y + 5, 2, 2, K_GRID);
            continue;
        }
        cv_rect(x, y, 12, 12, c);
        if (sq.running && sq.lpos[0] == i)
            cv_rect(x, y + 13, 12, 2, K_ACC);
        if (lock_count(&p->lk[i]))       /* p-locks: an amber bar over the cell */
            cv_rect(x, y - 3, 12, 2, K_LOCK);
        if (ui.held == i)
            cv_rect(x + 3, y + 3, 6, 6, K_BG);
    }
}

static void draw_scope(void)
{
    int32_t x, mid = 66;
    char t[24], b[16];
    float tt = voice.t;
    /* the grid: every 50 ms (5 ms zoomed) */
    {
        float step = ui.szoom ? 0.005f : ui.sdur > 0.8f ? 0.1f : 0.05f, s;
        for (s = step; s < ui.sdur; s += step) {
            x = (int32_t)(s / ui.sdur * 240.0f);
            cv_rect(x, 8, 1, 112, K_GRID);
        }
        cv_rect(0, mid, 240, 1, K_GRID);
    }
    for (x = 0; x < 240; x++) {          /* the hit: its shape filled, its edges bright */
        int32_t a = mid - ui.smax[x], b2 = mid - ui.smin[x];
        if (b2 > a + 2)
            cv_rect(x, a + 1, 1, b2 - a - 1, K_FILL);
        cv_pset(x, a, K_ACC);
        cv_pset(x, b2, K_ACC);
    }
    for (x = 1; x < 240; x++)            /* the sweep */
        cv_line(x - 1, 116 - ui.spitch[x - 1], x, 116 - ui.spitch[x], K_INK);
    if (voice.active && tt < ui.sdur) {  /* where the hit playing is */
        x = (int32_t)(tt / ui.sdur * 240.0f);
        cv_rect(x, 8, 1, 112, K_INK);
    }
    /* what it is (a step held: what that step plays) */
    {
        int tune = (int)skp.p[KP_TUNE];
        note_text(tune, t);
        text_cat(t, "  ");
        hz_text(kdsp_note_hz((float)tune), b);
    }
    text_cat(t, b);
    cv_text(4, 2, &FONT_XS, t, K_INK);
    time_text(ui.sdur, b);
    cv_text(236 - text_w(&FONT_XS, b), 2, &FONT_XS, b, K_MUTE);
    t[0] = 0;
    if (ui.held >= 0) {                  /* a step held: its number, its kick, its p-locks */
        const lock_t *l = &P()->lk[ui.held];
        int n = lock_count(l) - (l->kit ? 1 : 0);
        text_cat(t, "S");
        int_text(ui.held + 1, b);
        text_cat(t, b);
        text_cat(t, " ");
        text_cat(t, l->kit ? PRE_NAME[l->kit - 1] : "PAT");
        if (n) {
            text_cat(t, " +");
            int_text(n, b);
            text_cat(t, b);
        }
        cv_text(120 - text_w(&FONT_XS, t) / 2, 2, &FONT_XS, t, K_LOCK);
    } else {
        text_cat(t, PRE_NAME[P()->preset % NPRE]);
        {
            uint32_t k;
            for (k = 0; k < KP_N; k++)
                if (P()->snd[k] != PRE[P()->preset % NPRE][k]) {
                    text_cat(t, "*");
                    break;
                }
        }
        cv_text(120 - text_w(&FONT_XS, t) / 2, 2, &FONT_XS, t, K_MUTE);
    }
    draw_strip(126);
}

/* the ring: the TRIG lane round a circle */
static void step_xy(int i, int32_t r, int32_t *x, int32_t *y)
{
    float a = (float)i / 16.0f - 0.25f;
    *x = 120 + (int32_t)(fm_sin_turns(a + 0.25f) * (float)r);
    *y = 72 + (int32_t)(fm_sin_turns(a) * (float)r);
}

static void draw_ring(void)
{
    const pat_t *p = P();
    int i, pg = ui.page[M_SEQ];
    char t[24], b[16];
    int32_t x, y;
    circle_line(120, 72, 56, K_GRID);
    for (i = 0; i < NST; i++) {
        const step_t *s = &p->st[i];
        int tr = p->algo == AL_MAN ? s->trig : gen_trig[i];
        step_xy(i, 56, &x, &y);
        if (i >= p->len) {
            cv_rect(x, y, 1, 1, K_DIM);
            continue;
        }
        if ((s->fl & FL_SLIDE) && tr) {  /* a slide: joined to the step before */
            int32_t x0, y0, x1, y1;
            step_xy((i + NST - 1) % NST, 45, &x0, &y0);
            step_xy(i, 45, &x1, &y1);
            cv_line(x0, y0, x1, y1, K_ACC);
        }
        if (tr == 2)
            fill_circle(x, y, 6, K_ACC);
        else if (tr)
            fill_circle(x, y, 5, K_INK);
        else
            fill_circle(x, y, 2, K_GRID);
        if (tr && s->rat) {              /* a ratchet: a dot outside for each extra hit */
            int r;
            for (r = 1; r <= s->rat; r++) {
                int32_t rx, ry;
                step_xy(i, 62 + r * 4, &rx, &ry);
                cv_rect(rx - 1, ry - 1, 2, 2, K_INK);
            }
        }
        if (tr && (s->fl & FL_REV)) {    /* the sweep reversed: a mark inside */
            int32_t rx, ry;
            step_xy(i, 47, &rx, &ry);
            cv_rect(rx - 1, ry - 1, 3, 3, K_MUTE);
        }
        if (tr && s->pit) {              /* a pitch: a short tick in or out */
            int32_t a, b2, c, d;
            step_xy(i, 66, &a, &b2);
            step_xy(i, 66 + (s->pit > 0 ? 6 : -4), &c, &d);
            cv_line(a, b2, c, d, s->pit > 0 ? K_INK : K_MUTE);
        }
        if (lock_count(&p->lk[i])) {     /* p-locks: an amber dot inside; the kick locked: a ring round it */
            int32_t lx, ly;
            step_xy(i, 38, &lx, &ly);
            if (p->lk[i].kit)
                circle_line(lx, ly, 3, K_LOCK);
            cv_rect(lx - 1, ly - 1, 3, 3, K_LOCK);
        }
        if (sq.running && sq.lpos[0] == i)
            circle_line(x, y, 9, K_ACC);
        if (ui.held == i)
            circle_line(x, y, 8, K_INK);
    }
    /* the middle */
    if (ui.held >= 0 && pg != 1) {
        const step_t *s = &p->st[ui.held];
        t[0] = 0;
        text_cat(t, "STEP ");
        int_text(ui.held + 1, t + 5);
        text_c(120, 40, &FONT_XS, t, K_MUTE);
        sint_text(s->pit, t);
        text_c(120, 54, &FONT_L, t, K_INK);
        t[0] = 0;
        text_cat(t, "X");
        int_text(s->rat + 1, t + 1);
        if (s->fl & FL_SLIDE)
            text_cat(t, " SLIDE");
        if (s->fl & FL_REV)
            text_cat(t, " REV");
        text_c(120, 90, &FONT_XS, t, K_ACC);
        {
            const lock_t *l = &p->lk[ui.held];
            int n = lock_count(l) - (l->kit ? 1 : 0);
            t[0] = 0;
            if (l->kit)
                text_cat(t, PRE_NAME[l->kit - 1]);
            if (n) {
                text_cat(t, l->kit ? " +" : "LOCKS ");
                int_text(n, b);
                text_cat(t, b);
            }
            if (t[0])
                text_c(120, 102, &FONT_XS, t, K_LOCK);
        }
    } else if (pg == 0) {
        int_text(p->len, t);
        text_c(120, 48, &FONT_L, t, K_INK);
        t[0] = 0;
        text_cat(t, DIV_NAME[p->div]);
        text_cat(t, "  ");
        text_cat(t, DIR_NAME[p->dir]);
        text_c(120, 86, &FONT_XS, t, K_MUTE);
    } else if (pg == 1) {
        text_c(120, 52, &FONT_M, ALGO_NAME[p->algo], p->algo == AL_MAN ? K_MUTE : K_ACC);
        text_c(120, 80, &FONT_XS, p->algo == AL_MAN ? "TURN ALGO" : "GENERATOR", K_MUTE);
    } else {
        dec1_text(set.clk && sq.ext_bpm10 ? sq.ext_bpm10 : set.bpm10, t);
        text_c(120, 48, &FONT_L, t, K_INK);
        text_c(120, 86, &FONT_XS, set.clk ? "BPM  MIDI" : "BPM", K_MUTE);
    }
    if (p->swing) {
        t[0] = 0;
        text_cat(t, "SWING ");
        int_text(p->swing, b);
        text_cat(t, b);
        cv_text(4, 128, &FONT_XS, t, K_MUTE);
    }
    if (p->prob < 100u || p->human) {
        t[0] = 0;
        if (p->prob < 100u) {
            text_cat(t, "PROB ");
            int_text(p->prob, b);
            text_cat(t, b);
        }
        if (p->human) {
            text_cat(t, t[0] ? "  HUM " : "HUM ");
            int_text(p->human, b);
            text_cat(t, b);
        }
        cv_text(236 - text_w(&FONT_XS, t), 128, &FONT_XS, t, K_MUTE);
    }
}

static float mod_preview(const mod_t *m, int pos)
{
    float L = (float)(m->len > 1u ? m->len : 2u), x = (float)pos;
    switch (m->shape) {
    case SH_RAND:
    case SH_WALK:
        return (float)(int32_t)hash3(m->seed, 0u, (uint32_t)pos) * (1.0f / 2147483648.0f);
    case SH_UP: return x / (L - 1.0f) * 2.0f - 1.0f;
    case SH_DOWN: return 1.0f - x / (L - 1.0f) * 2.0f;
    case SH_TRI: { float u = x / L * 2.0f; return u < 1.0f ? u * 2.0f - 1.0f : 3.0f - u * 2.0f; }
    case SH_SINE: return fm_sin_turns(x / L);
    case SH_SQR: return x < L * 0.5f ? 1.0f : -1.0f;
    default: return (float)m->val[pos] / 63.0f;
    }
}

static void draw_mod(void)
{
    const pat_t *p = P();
    int f = ui.page[M_MOD] / 2, m, i;
    static const int32_t R[NMOD] = {60, 44, 28};
    char t[24];
    for (m = 0; m < NMOD; m++)
        circle_line(120, 72, R[m], K_GRID);
    for (m = 0; m < NMOD; m++) {
        const mod_t *md = &p->mod[m];
        int on = md->tgt && md->depth;
        uint16_t c = m == f ? MOD_COL[m] : on ? K_MUTE : K_DIM;
        for (i = 0; i < md->len; i++) {
            float a = (float)i / (float)md->len - 0.25f, v = mod_preview(md, i);
            float cs = fm_sin_turns(a + 0.25f), sn = fm_sin_turns(a);
            int32_t x0 = 120 + (int32_t)(cs * (float)R[m]), y0 = 72 + (int32_t)(sn * (float)R[m]);
            int32_t x1 = 120 + (int32_t)(cs * ((float)R[m] + v * 7.0f)), y1 = 72 + (int32_t)(sn * ((float)R[m] + v * 7.0f));
            cv_line(x0, y0, x1, y1, c);
            cv_line(x0 + 1, y0, x1 + 1, y1, c);
            cv_rect(x0, y0, 2, 2, m == f ? K_INK : K_GRID);
            if (m == f && ui.held == i)
                circle_line(x1, y1, 4, K_INK);
            if (sq.running && sq.lpos[m + 1] == i)
                fill_circle(x1, y1, m == f ? 3 : 2, c);
        }
    }
    {
        const mod_t *md = &p->mod[f];
        int kp = MOD_TGT_KP[md->tgt];
        text_c(120, 64, &FONT_XS, kp < 0 ? "OFF" : PNAME[kp], MOD_COL[f]);
        sint_text(md->depth, t);
        text_cat(t, "%");
        text_c(120, 76, &FONT_XS, t, K_INK);
    }
    t[0] = 0;
    text_cat(t, SHAPE_NAME[p->mod[f].shape]);
    cv_text(4, 128, &FONT_XS, t, K_MUTE);
    t[0] = 0;
    text_cat(t, DIV_NAME[p->mod[f].div]);
    text_cat(t, "  ");
    text_cat(t, DIR_NAME[p->mod[f].dir]);
    cv_text(236 - text_w(&FONT_XS, t), 128, &FONT_XS, t, K_MUTE);
}

static void draw_pat(void)
{
    uint32_t now = plat_ms();
    int n, i;
    for (n = 0; n < NPAT; n++) {
        const pat_t *p = &pats(n);
        int32_t x = 4 + (n % 4) * 59, y = 4 + (n / 4) * 35;
        char t[8];
        if (n == sq.cur)
            cv_rect(x, y, 56, 32, K_ACC2);
        cv_rect(x, y, 56, 1, n == sq.cur ? K_ACC : K_GRID);
        cv_rect(x, y + 31, 56, 1, n == sq.cur ? K_ACC : K_GRID);
        cv_rect(x, y, 1, 32, n == sq.cur ? K_ACC : K_GRID);
        cv_rect(x + 55, y, 1, 32, n == sq.cur ? K_ACC : K_GRID);
        if (n == sq.next && ((now / 250u) & 1u)) {
            cv_rect(x + 1, y + 1, 54, 1, K_INK);
            cv_rect(x + 1, y + 30, 54, 1, K_INK);
        }
        t[0] = 'P';
        t[1] = (char)('0' + (n + 1) / 10);
        t[2] = (char)('0' + (n + 1) % 10);
        t[3] = 0;
        cv_text(x + 4, y + 3, &FONT_XS, t, n == sq.cur ? K_INK : K_MUTE);
        if (p->algo != AL_MAN)
            cv_text(x + 52 - text_w(&FONT_XS, "G"), y + 3, &FONT_XS, "G", K_ACC);
        for (i = 0; i < p->len; i++) {
            int tr = p->algo == AL_MAN ? p->st[i].trig : (n == sq.cur ? gen_trig[i] : 0);
            cv_rect(x + 4 + i * 3, y + 22, 2, tr ? 5 : 1, tr == 2 ? K_ACC : tr ? K_INK : K_GRID);
        }
    }
}

static void draw_midi(void)
{
    char t[24], b[16];
    cv_text(12, 14, &FONT_XS, "IN CHANNEL", K_MUTE);
    if (set.mch)
        int_text(set.mch, t);
    else {
        t[0] = 0;
        text_cat(t, "ALL");
    }
    cv_text(130, 10, &FONT_B, t, K_INK);
    cv_text(12, 40, &FONT_XS, "CLOCK", K_MUTE);
    t[0] = 0;
    text_cat(t, set.clk ? "MIDI " : "INTERNAL");
    if (set.clk) {
        if (sq.ext_bpm10)
            dec1_text(sq.ext_bpm10, b);
        else {
            b[0] = 0;
            text_cat(b, "WAITING");
        }
        text_cat(t, b);
    }
    cv_text(130, 36, &FONT_B, t, set.clk ? K_ACC : K_INK);
    cv_text(12, 66, &FONT_XS, "CLOCK OUT", K_MUTE);
    cv_text(130, 62, &FONT_B, set.clkout ? "ON" : "OFF", K_INK);
    cv_text(12, 92, &FONT_XS, "NOTES OUT", K_MUTE);
    cv_text(130, 88, &FONT_B, set.notes_out ? "ON  C1+" : "OFF", K_INK);
    cv_text(12, 118, &FONT_XS, "NOTES IN", K_MUTE);
    int_text((int32_t)midi_seen, t);
    cv_text(130, 114, &FONT_B, t, K_INK);
}

static void draw_center(void)
{
    cv_begin(240, 144, K_BG);
    switch (ui.mode) {
    case M_HOME:
    case M_SND: draw_scope(); break;
    case M_SEQ: draw_ring(); break;
    case M_MOD: draw_mod(); break;
    case M_PAT: if (ui.page[M_PAT] == 0) draw_pat(); else draw_midi(); break;
    }
    cv_blit(0, 24);
}

static void draw_footer(void)
{
    int i, j;
    const int N = 27;
    cv_begin(240, 72, K_BG);
    cv_rect(0, 0, 240, 1, K_GRID);
    for (i = 0; i < 4; i++) {
        knob_t k;
        int32_t cx = 30 + 60 * i, cy = 22, r = 15;
        int lit_to, lit_from;
        uint16_t lc = ui.mode == M_MOD ? MOD_COL[ui.page[M_MOD] / 2] : K_ACC;
        knob_info(i, &k);
        if (k.hot)
            lc = K_INK;
        lit_to = k.pos < 0 ? -1 : k.pos * (N - 1) / 1000;
        lit_from = k.bip ? (N - 1) / 2 : 0;
        if (k.bip && lit_to < lit_from) {
            int t = lit_to;
            lit_to = lit_from;
            lit_from = t;
        }
        for (j = 0; j < N; j++) {        /* the arc: 270 degrees, from the bottom left round to the bottom right */
            float a = (135.0f + 270.0f * (float)j / (float)(N - 1)) / 360.0f;
            int32_t x = cx + (int32_t)(fm_sin_turns(a + 0.25f) * (float)r), y = cy + (int32_t)(fm_sin_turns(a) * (float)r);
            int on = k.pos >= 0 && j >= lit_from && j <= lit_to;
            cv_rect(x - 1, y - 1, 2, 2, on ? lc : K_GRID);
        }
        fill_circle(cx, cy, 9, RGB(30, 28, 26));
        if (k.pos >= 0) {
            float a = (135.0f + 270.0f * (float)k.pos / 1000.0f) / 360.0f;
            cv_line(cx, cy, cx + (int32_t)(fm_sin_turns(a + 0.25f) * 8.0f), cy + (int32_t)(fm_sin_turns(a) * 8.0f), K_INK);
        }
        text_c(cx, 40, &FONT_XS, k.name, k.hot ? K_ACC : K_MUTE);
        if (text_w(&FONT_B, k.val) > 58)
            text_c(cx, 54, &FONT_XS, k.val, k.pos < 0 ? K_DIM : K_INK);
        else
            text_c(cx, 52, &FONT_B, k.val, k.pos < 0 ? K_DIM : K_INK);
    }
    cv_blit(0, 168);
}

static void lights(void)
{
    static const uint8_t WK[16] = PLAT_WHITE_KEYS;
    const pat_t *p = P();
    uint32_t kb = 0, kd = 0, bb = 0, bd = 0;
    int i;
    if (ui.mode == M_PAT && ui.page[M_PAT] == 0) {
        for (i = 0; i < NPAT; i++)
            kd |= 1u << WK[i];
        kb |= 1u << WK[sq.cur];
        if (sq.next != 0xFFu && ((plat_ms() / 250u) & 1u))
            kb |= 1u << WK[sq.next];
    } else if (ui.mode == M_MOD) {
        const mod_t *m = &p->mod[ui.page[M_MOD] / 2];
        for (i = 0; i < m->len; i++)
            if (m->shape != SH_MAN || m->val[i])
                kd |= 1u << WK[i];
        if (sq.running && sq.lpos[ui.page[M_MOD] / 2 + 1] >= 0)
            kb |= 1u << WK[sq.lpos[ui.page[M_MOD] / 2 + 1]];
    } else {
        for (i = 0; i < p->len; i++) {
            int tr = p->algo == AL_MAN ? p->st[i].trig : gen_trig[i];
            if (tr == 2)
                kb |= 1u << WK[i];
            else if (tr)
                kd |= 1u << WK[i];
        }
        if (sq.running && sq.lpos[0] >= 0)
            kb ^= 1u << WK[sq.lpos[0]];
    }
    switch (ui.mode) {
    case M_HOME: bb |= 1u << B_HOME; break;
    case M_SEQ: bb |= 1u << (ui.page[M_SEQ] == 1 ? B_ARP : B_SEQ); break;
    case M_SND: bb |= 1u << (ui.page[M_SND] == 2 ? B_ENV : ui.page[M_SND] == 5 ? B_FX : B_EDIT); break;
    case M_MOD: bb |= 1u << B_LFO; break;
    case M_PAT: bb |= 1u << B_GLO; break;
    }
    if (sq.running)
        bb |= 1u << B_PLAY;
    else
        bd |= 1u << B_PLAY;
    if (ui.pat_dirty || ui.set_dirty)
        bd |= 1u << B_SAVE;
    bb |= ui.btn;
    plat_lights(bb, kb, bd, kd, 1);
}

void app_ui_init(void)
{
    lcd_fill(0, 0, 240, 240, K_BG);
    ui.held = -1;
    ui.mode = M_HOME;
    if (ui_pat_dirty_boot)
        ui.pat_dirty = 3;
    ui.redraw_h = ui.redraw_c = ui.redraw_f = 1;
    gen_run(P(), 0);
    scope_start();
}

static void autosave(uint32_t now)
{
    if (sq.running || now - ui.last_change < 1500u)
        return;
    if (ui.pat_dirty & 1u) {             /* one object a frame: the first eight patterns, the last eight, the settings */
        if (plat_store_save(OBJ_PATS0, &pk[0], sizeof pk[0]) == 0)
            ui.pat_dirty &= (uint8_t)~1u;
    } else if (ui.pat_dirty & 2u) {
        if (plat_store_save(OBJ_PATS1, &pk[1], sizeof pk[1]) == 0)
            ui.pat_dirty &= (uint8_t)~2u;
    } else if (ui.set_dirty) {
        if (plat_store_save(OBJ_SET, &set, sizeof set) == 0)
            ui.set_dirty = 0;
    }
}

void app_frame(void)
{
    static uint32_t frame, last_h;
    static int8_t last_pos[1 + NMOD];
    static uint8_t last_run, last_act;
    uint32_t now = plat_ms(), hstate, k;
    frame++;
    input();
    ann_tick(now);
    if (set.cur != sq.cur) {             /* the sequencer moved to a queued pattern */
        set.cur = sq.cur;
        set_changed();
        ui.redraw_c = ui.redraw_f = 1;
    }
    if (!sj.busy && snd_hash() != ui.shash)
        scope_start();
    scope_work();
    autosave(now);
    /* the header: on a change (the message, the state, the pattern, the tempo) */
    hstate = (uint32_t)sq.running | (uint32_t)sq.cur << 1 | (uint32_t)sq.next << 6 | (uint32_t)set.bpm10 << 14 |
             (uint32_t)(now < ui.msg_until) << 30 | (uint32_t)ui.mode << 26 | (uint32_t)ui.page[ui.mode] << 29;
    hstate ^= (uint32_t)sq.ext_bpm10 * 7919u ^ (sq.next != 0xFFu ? (now / 250u) & 1u : 0u) << 31;
    if (hstate != last_h || ui.redraw_h) {
        last_h = hstate;
        ui.redraw_h = 0;
        draw_header();
    }
    /* the middle: on a change, and while something moves */
    for (k = 0; k <= NMOD; k++)
        if (sq.lpos[k] != last_pos[k]) {
            last_pos[k] = sq.lpos[k];
            ui.redraw_c = 1;
        }
    if (sq.running != last_run) {
        last_run = sq.running;
        ui.redraw_c = ui.redraw_f = 1;
    }
    {
        uint8_t act = (uint8_t)(voice.active != 0);
        if ((act || act != last_act) && (ui.mode == M_HOME || ui.mode == M_SND) && (frame & 1u))
            ui.redraw_c = 1;
        last_act = act;
    }
    if (ui.mode == M_PAT && sq.next != 0xFFu && (frame % 8u) == 0u)
        ui.redraw_c = 1;
    if (ui.mode == M_PAT && ui.page[M_PAT] == 1 && (frame % 15u) == 0u)
        ui.redraw_c = 1;
    if (ui.redraw_c) {
        ui.redraw_c = 0;
        draw_center();
    }
    if (ui.redraw_f) {
        ui.redraw_f = 0;
        draw_footer();
    }
    lights();
}

#ifdef PLAT_HOST
int app_expect(const char *what, int *got)
{
    const pat_t *p = P();
    if (!strcmp(what, "running"))
        *got = sq.running;
    else if (!strcmp(what, "hits"))
        *got = (int)sq.hits;
    else if (!strcmp(what, "cur"))
        *got = sq.cur;
    else if (!strcmp(what, "next"))
        *got = sq.next == 0xFFu ? -1 : sq.next;
    else if (!strcmp(what, "algo"))
        *got = p->algo;
    else if (!strcmp(what, "len"))
        *got = p->len;
    else if (!strcmp(what, "bpm"))
        *got = set.bpm10;
    else if (!strcmp(what, "mode"))
        *got = ui.mode;
    else if (!strcmp(what, "page"))
        *got = ui.page[ui.mode];
    else if (!strcmp(what, "pos"))
        *got = sq.lpos[0];
    else if (!strcmp(what, "dirty"))
        *got = (ui.pat_dirty != 0) | ui.set_dirty << 1;
    else if (!strcmp(what, "preset"))
        *got = p->preset;
    else if (!strncmp(what, "snd", 3))
        *got = p->snd[atoi(what + 3) % KP_N];
    else if (!strncmp(what, "trig", 4))
        *got = p->st[atoi(what + 4) % NST].trig;
    else if (!strncmp(what, "pit", 3))
        *got = p->st[atoi(what + 3) % NST].pit;
    else if (!strncmp(what, "rat", 3))
        *got = p->st[atoi(what + 3) % NST].rat;
    else if (!strncmp(what, "gen", 3))
        *got = gen_trig[atoi(what + 3) % NST];
    else if (!strncmp(what, "mtgt", 4))
        *got = p->mod[atoi(what + 4) % NMOD].tgt;
    else if (!strncmp(what, "mdepth", 6))
        *got = p->mod[atoi(what + 6) % NMOD].depth;
    else if (!strcmp(what, "active"))
        *got = voice.active;
    else if (!strncmp(what, "lk", 2))    /* lkN: step N's p-locks (the kick counts as one) */
        *got = lock_count(&p->lk[atoi(what + 2) % NST]);
    else if (!strncmp(what, "kit", 3))
        *got = p->lk[atoi(what + 3) % NST].kit;
    else if (!strncmp(what, "lval", 4)) {   /* lvalN.K: step N's lock of parameter K, -1 none */
        int n = atoi(what + 4) % NST, kp = atoi(strchr(what, '.') ? strchr(what, '.') + 1 : "0") % KP_N, j;
        j = lock_find(&p->lk[n], kp);
        *got = j < 0 ? -1 : p->lk[n].val[j];
    } else if (!strcmp(what, "locks")) {   /* the pattern's p-locks, all steps */
        int n = 0, i;
        for (i = 0; i < NST; i++)
            n += lock_count(&p->lk[i]);
        *got = n;
    } else if (!strcmp(what, "tune_now"))
        *got = (int)(eff.p[KP_TUNE] + 0.5f);
    else
        return 0;
    return 1;
}
#endif
