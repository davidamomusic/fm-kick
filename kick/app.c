/* SPDX-License-Identifier: MPL-2.0 */
/* FM-KICK: a kick drum synthesizer and generative sequencer for the FM-1.
 *
 * The sound (dsp.c): six kinds of kick body (808, 909, 606, ANALOG, FM, WSHAPER) with a pitch sweep, an amplitude
 * envelope, click, sub and noise layers, distortion, filter, EQ and clip: 48 parameters on twelve pages.
 * The effects (fx.c): sixteen, one rack for all, on and off with the white keys.
 * The sequencer: up to 32 steps (trig, accent, pitch, ratchet, slide, reversed sweep, and p-locks of any parameter and
 * of the kick itself), played by hand or by one of nine generators, four MOD lanes, sixteen patterns each with its
 * sound, 32 user kicks. REC records from the keyboard or from MIDI.
 *
 * Part of the unity build (device/unity.c, host/plat_host.c), after the platform: gfx.c and announce.c are in scope. */
#include "fastmath.h"
#include "dsp.h"
#include "fx.h"

#define NPAT 16
#define NST 32
#define NMOD 4
#define NLOCK 4
#define NUSER 32
#define PPQ 960u                         /* clock ticks a quarter note */

/* ------------------------------------------------------------------ colours --- */
/* the themes (GLO, THEMES page): colours, and a design of their own (knobs as dials or bars, the scope filled or
 * drawn as a line, the steps round a ring or in a grid), which the page can change too */
typedef struct {
    const char *name;
    uint16_t bg, ink, acc, acc2, fill, lock, rec, grid, mute, dim, knob, mod[NMOD];
    uint8_t dial, scope, steps;          /* 0 dials, 1 bars; 0 filled, 1 line; 0 ring, 1 grid */
} theme_t;
#define NTHEME 6
static const theme_t THEMES[NTHEME] = {
    {"EMBER", 0x0000u, RGB(236, 230, 218), RGB(255, 72, 28), RGB(92, 24, 8), RGB(132, 36, 12), RGB(255, 176, 32),
     RGB(255, 36, 36), RGB(40, 37, 35), RGB(124, 116, 106), RGB(70, 64, 58), RGB(30, 28, 26),
     {RGB(255, 176, 32), RGB(255, 104, 160), RGB(110, 200, 255), RGB(150, 230, 120)}, 0, 0, 0},
    {"NIGHT", 0x0000u, RGB(150, 44, 30), RGB(200, 34, 20), RGB(48, 8, 4), RGB(70, 14, 6), RGB(170, 72, 20),
     RGB(220, 20, 20), RGB(26, 10, 8), RGB(100, 32, 24), RGB(52, 16, 12), RGB(18, 6, 4),
     {RGB(170, 72, 20), RGB(150, 40, 60), RGB(120, 40, 90), RGB(140, 90, 30)}, 0, 1, 0},
    {"ICE", RGB(4, 8, 16), RGB(220, 240, 255), RGB(40, 200, 255), RGB(8, 40, 64), RGB(16, 80, 120), RGB(255, 220, 80),
     RGB(255, 60, 90), RGB(24, 36, 52), RGB(110, 140, 170), RGB(50, 66, 86), RGB(14, 22, 34),
     {RGB(255, 220, 80), RGB(180, 140, 255), RGB(120, 255, 220), RGB(255, 140, 200)}, 1, 0, 1},
    {"ACID", 0x0000u, RGB(230, 255, 200), RGB(150, 255, 40), RGB(30, 64, 8), RGB(60, 120, 16), RGB(255, 240, 40),
     RGB(255, 60, 60), RGB(26, 40, 20), RGB(120, 150, 100), RGB(60, 80, 50), RGB(18, 28, 14),
     {RGB(255, 240, 40), RGB(40, 220, 255), RGB(255, 120, 220), RGB(255, 160, 40)}, 1, 1, 0},
    {"MONO", 0x0000u, RGB(240, 240, 240), RGB(255, 255, 255), RGB(60, 60, 60), RGB(110, 110, 110), RGB(200, 200, 200),
     RGB(255, 255, 255), RGB(40, 40, 40), RGB(130, 130, 130), RGB(70, 70, 70), RGB(28, 28, 28),
     {RGB(255, 255, 255), RGB(190, 190, 190), RGB(140, 140, 140), RGB(100, 100, 100)}, 0, 1, 1},
    {"PAPER", RGB(236, 228, 212), RGB(30, 26, 24), RGB(220, 60, 20), RGB(240, 190, 170), RGB(236, 150, 120),
     RGB(180, 110, 0), RGB(220, 20, 20), RGB(200, 190, 174), RGB(110, 100, 90), RGB(170, 160, 146), RGB(214, 204, 188),
     {RGB(180, 110, 0), RGB(190, 40, 110), RGB(20, 110, 170), RGB(40, 130, 40)}, 1, 0, 1},
};
static theme_t th;                       /* the theme in use, with the page's design */
#define K_BG (th.bg)
#define K_INK (th.ink)                   /* values, what is there */
#define K_ACC (th.acc)                   /* the kick, accents, the playhead */
#define K_ACC2 (th.acc2)                 /* its shadow: fills */
#define K_FILL (th.fill)                 /* the scope's body */
#define K_LOCK (th.lock)                 /* a p-lock */
#define K_REC (th.rec)                   /* recording */
#define K_GRID (th.grid)                 /* rules, empty cells */
#define K_MUTE (th.mute)                 /* labels */
#define K_DIM (th.dim)                   /* inactive */
#define MOD_COL (th.mod)

/* ------------------------------------------------------------------- data --- */
typedef struct {
    uint8_t trig;                        /* 0 off, 1 on, 2 accent */
    uint8_t fl;                          /* FL_* */
    uint8_t rat;                         /* 0..3: one to four hits in the step */
    int8_t pit;                          /* -24 .. +24 semitones */
} step_t;
enum { FL_SLIDE = 1, FL_REV = 2 };

typedef struct {
    uint8_t tgt;                         /* 0 off, 1 + KP_* */
    uint8_t shape, len, div, dir, smooth;
    int8_t depth;                        /* -100 .. +100 % */
    uint8_t seed;
    int8_t val[NST];                     /* -64 .. +63: the MANUAL shape's values */
} mod_t;

typedef struct {                         /* a step's p-locks: the kick (a preset), then up to four parameters */
    uint8_t kit;                         /* 0 none, 1 + preset index */
    uint8_t id[NLOCK];                   /* 0 none, 1 + KP_* */
    uint8_t val[NLOCK];
} lock_t;

typedef struct {
    uint8_t snd[KP_N];                   /* the pattern's kick */
    uint8_t preset;                      /* the preset it came from (factory, then user) */
    uint8_t len, div, dir, swing, prob, human, algo;
    uint8_t ga[4];                       /* the generator's four knobs */
    step_t st[NST];
    lock_t lk[NST];
    mod_t mod[NMOD];
    uint8_t rsv[4];
} pat_t;
_Static_assert(sizeof(pat_t) == 640, "pat_t: 640 bytes");

#define SET_MAGIC 0x4B434B33u            /* "KCK3": 0.3.0 (themes, metronome, LIVE and FINAL effects) */
#define PATS_MAGIC 0x4B505433u           /* "KPT3": 0.2.0 (older layouts are not read: the first power-on's patterns) */
#define USER_MAGIC 0x4B555331u           /* "KUS1" */
typedef struct {
    uint32_t magic;
    uint16_t bpm10;                      /* tempo x 10 */
    uint8_t cur;                         /* the pattern playing */
    uint8_t mch;                         /* MIDI in channel: 0 all, 1..16 */
    uint8_t clk;                         /* 0 internal, 1 MIDI clock in */
    uint8_t clkout, notes_out;
    uint8_t rsv;
    uint8_t theme, dial, scope, steps;   /* THEMES: the theme, and its design as the page left it */
    uint8_t metro;                       /* the metronome's click (OCT+ + REC) */
    uint8_t rsv2[3];
    fxset_t fx;                          /* the effects: one rack for every pattern */
} set_t;
typedef struct {
    uint32_t magic;
    pat_t p[4];
} pats_t;                                /* one flash object: four patterns (a sector holds 3840 bytes) */
_Static_assert(sizeof(pats_t) <= 3840u, "pats_t: one sector's payload");
typedef struct {
    uint8_t used;
    char name[8];
    uint8_t snd[KP_N];
} upre_t;
typedef struct {
    uint32_t magic;
    upre_t u[NUSER];
} users_t;
_Static_assert(sizeof(users_t) <= 3840u, "users_t: one sector's payload");

static set_t set;
static pats_t pk[4];
static users_t us;
#define pats(i) (pk[(i) >> 2].p[(i) & 3])
static const uint8_t PAT_OBJ[4] = {OBJ_P0, OBJ_P1, OBJ_P2, OBJ_P3};

/* ------------------------------------------------------------------ tables --- */
static const char *const PNAME[KP_N] = {
    "TYPE",    "TUNE",     "DECAY",    "LEVEL",   "CHAR A",  "CHAR B",   "CHAR C",  "CHAR D",
    "BEND",    "TIME",     "CURVE",    "TAIL",    "T.TIME",  "GLIDE",    "DRIFT",   "VEL>BEND",
    "ATTACK",  "HOLD",     "DROP",     "PUNCH",   "CLK TYPE", "CLK LVL", "CLK TONE", "CLK LEN",
    "SUB LVL", "SUB INT",  "SUB DEC",  "SUB DRV", "NSE LVL", "NSE TONE", "NSE DEC", "NSE WIDTH",
    "DST TYPE", "DRIVE",   "SHAPE",    "DST MIX", "FLT TYPE", "CUTOFF",  "RESO",    "FLT ENV",
    "LOW",     "MID",      "MID FREQ", "HIGH",    "BODY",    "VEL SENS", "ACCENT",  "CLIP"};
#define NSPAGE (KP_N / 4)
static const char *const SND_PAGE[NSPAGE] = {"KICK", "CHARACTER", "PITCH", "SWEEP", "AMP",    "CLICK",
                                             "SUB",  "NOISE",     "DIST",  "FILTER", "EQ",    "OUTPUT"};
static const char *const TYPE_NAME[KT_N] = {"808", "909", "606", "ANALOG", "FM", "WSHAPER"};
static const char *const CHAR_NAME[KT_N][4] = {{"TONE", "SNAP", "SAT", "BOOM"},      {"ATTACK", "SHAPE", "NOISE", "COMP"},
                                               {"TONE", "NOISE", "SNAP", "RING"},    {"COLOUR", "PULSE", "RESO", "SATUR"},
                                               {"RATIO", "INDEX", "MOD DEC", "FEEDBK"}, {"CURVE", "DRIVE", "BIAS", "SOURCE"}};
static const char *const CK_NAME[CK_N] = {"PULSE", "NOISE", "TICK", "ZAP", "SNAP"};
static const char *const DT_NAME[DT_N] = {"SOFT", "HARD", "FOLD", "CRUSH", "TUBE", "RECT"};
static const char *const FT_NAME[FT_N] = {"LP", "HP", "BP", "NOTCH"};
static const char *const SINT_NAME[4] = {"-12", "0", "+7", "+12"};
static const char *const WS_NAME[WS_N] = {"SOFT", "HARD", "S.FOLD", "T.FOLD", "CHEBY", "STAIRS", "RECT", "SQUARE"};
static const char *const FM_RATIO_NAME[12] = {"X0.5", "X1", "X1.5", "X2", "X2.5", "X3", "X3.5", "X4", "X5", "X6", "X7", "X8"};

#define NDIV 9
static const char *const DIV_NAME[NDIV] = {"1/32", "1/16T", "1/16", "1/8T", "1/8", "1/4T", "1/4", "1/2", "1 BAR"};
static const uint16_t DIV_T[NDIV] = {120, 160, 240, 320, 480, 640, 960, 1920, 3840};
#define NDIR 5
static const char *const DIR_NAME[NDIR] = {"FWD", "REV", "PING", "RAND", "WALK"};

enum { AL_MAN, AL_EUCL, AL_POLY, AL_METRIC, AL_SYNC, AL_BOUNCE, AL_MIRROR, AL_FRACT, AL_BITS, AL_ROLLS, NALGO };
static const char *const ALGO_NAME[NALGO] = {"MANUAL", "EUCLID",  "POLY",    "METRIC", "SYNCOPA",
                                             "BOUNCE", "MIRROR",  "FRACTAL", "BITS",   "ROLLS"};
static const char *const ALGO_P[NALGO][4] = {
    {"-", "-", "-", "-"},                    {"HITS", "ROTATE", "ACCENT", "FILL"},
    {"EVERY A", "EVERY B", "SHIFT B", "ACCENT"}, {"DENSITY", "WEIGHT", "ACCENT", "VARY"},
    {"PUSH", "OFFBEAT", "ACCENT", "VARY"},   {"START", "SPEED", "ACCENT", "ROTATE"},
    {"DENSITY", "UNIT", "ACCENT", "SEED"},   {"KIND", "ROTATE", "ACCENT", "FILL"},
    {"VALUE", "ROTATE", "ACCENT", "FILL"},   {"BASE", "ROLL AT", "ROLL LEN", "RATE"}};
static const char *const ACC_MODE[4] = {"NONE", "FIRST", "EVERY 2", "OFFBEAT"};
static const char *const FRACT_NAME[3] = {"THUE", "CANTOR", "FIBO"};
static const char *const ROLL_BASE[4] = {"FLOOR", "HALF", "OFFBEAT", "BOTH"};

enum { SH_MAN, SH_UP, SH_DOWN, SH_TRI, SH_SINE, SH_SQR, SH_RAND, SH_WALK, NSHAPE };
static const char *const SHAPE_NAME[NSHAPE] = {"MANUAL", "RAMP UP", "RAMP DN", "TRI", "SINE", "SQUARE", "RANDOM", "WALK"};

static const char *const FX_NAME[FX_N] = {"DISTORT", "CRUSH",  "FILTER",  "RING MOD", "CHORUS", "PHASER",
                                          "FLANGER", "DELAY",  "REVERB",  "STUTTER",  "GATE",   "WIDTH",
                                          "TRANSIENT", "COMP", "EQ",      "CLIPPER",  "LIMITER"};
static const char *const FX_SHORT[FX_N] = {"DIST", "CRUSH", "FILT", "RING", "CHOR", "PHAS", "FLNG", "DELAY", "VERB",
                                           "STUT", "GATE",  "WIDE", "TRANS", "COMP", "EQ",  "CLIP", "LIMIT"};
static const char *const FX_P[FX_N][4] = {
    {"DRIVE", "TONE", "MIX", "OUTPUT"},   {"BITS", "RATE", "MIX", "-"},          {"CUTOFF", "RESO", "-", "-"},
    {"FREQ", "MIX", "-", "-"},            {"RATE", "DEPTH", "MIX", "-"},         {"RATE", "DEPTH", "FEEDBACK", "MIX"},
    {"RATE", "DEPTH", "FEEDBACK", "MIX"}, {"TIME", "FEEDBACK", "MIX", "TONE"},   {"SIZE", "DAMP", "MIX", "WIDTH"},
    {"SLICE", "REPEAT", "MIX", "-"},      {"RATE", "DEPTH", "SMOOTH", "-"},      {"AMOUNT", "BASS MONO", "-", "-"},
    {"ATTACK", "SUSTAIN", "-", "-"},      {"THRESH", "RATIO", "ATTACK", "RELEASE"}, {"LOW", "MID", "MID FREQ", "HIGH"},
    {"DRIVE", "KNEE", "OUTPUT", "-"},     {"CEILING", "RELEASE", "-", "-"}};
static const char *const FX_DIV_NAME[FX_NDIV] = {"1/32", "1/16T", "1/16", "1/8T", "1/16D", "1/8", "1/8D", "1/4"};
static const char *const FX_GATE_NAME[FX_NGATE] = {"1/32", "1/16", "1/8", "1/4"};
static const char *const FX_SLICE_NAME[FX_NSLICE] = {"1/16", "1/32", "1/64", "1/128"};
static const char *const FX_REP_NAME[FX_NREP] = {"1 STEP", "2 STEPS", "4 STEPS", "8 STEPS"};

/* ------------------------------------------------------------------ presets --- */
#define NFAC 18
static const char *const FAC_NAME[NFAC] = {"808 LONG", "808 PUNCH", "808 DIST", "909 CLASS", "909 HARD", "909 SHORT",
                                           "606 TIGHT", "606 TOM",  "606 LOFI", "AN DEEP",  "AN RUMBLE", "AN HARD",
                                           "FM BOING",  "FM METAL", "FM SUB",   "WS RAW",   "WS FOLD",  "WS GABBA"};
static const uint8_t SND_BASE[KP_N] = {0,  31, 80, 110, 64, 0,  0,  0,   50, 60, 64, 0,  64, 40, 0,   0,
                                       0,  0,  64, 0,   0,  0,  80, 40,  0,  1,  64, 0,  0,  80, 40,  64,
                                       0,  0,  0,  127, 0,  127, 0, 64,  64, 64, 64, 64, 110, 64, 64, 0};
/* each factory kick: the base, then pairs (parameter, value) to 0xFF */
static const uint8_t FAC_OV[NFAC][40] = {
    {KP_TYPE, 0, KP_TUNE, 31, KP_DEC, 105, KP_CA, 70, KP_CB, 20, KP_CC, 15, KP_CD, 60, KP_BEND, 20, KP_TIME, 50, 0xFF},
    {KP_TYPE, 0, KP_TUNE, 34, KP_DEC, 80, KP_CA, 90, KP_CB, 50, KP_CC, 40, KP_BEND, 30, KP_TIME, 45, KP_CTYPE, 2,
     KP_CLVL, 40, KP_PUNCH, 50, 0xFF},
    {KP_TYPE, 0, KP_TUNE, 31, KP_DEC, 95, KP_CA, 60, KP_CB, 30, KP_CC, 90, KP_CD, 40, KP_DTYPE, 4, KP_DRIVE, 60,
     KP_SHAPE, 40, 0xFF},
    {KP_TYPE, 1, KP_TUNE, 35, KP_DEC, 70, KP_CA, 70, KP_CB, 80, KP_CC, 70, KP_CD, 30, KP_BEND, 60, KP_TIME, 48,
     KP_CURVE, 70, 0xFF},
    {KP_TYPE, 1, KP_TUNE, 36, KP_DEC, 78, KP_CA, 100, KP_CB, 60, KP_CC, 90, KP_CD, 60, KP_BEND, 75, KP_DTYPE, 1,
     KP_DRIVE, 70, KP_SHAPE, 60, KP_CLIP, 40, 0xFF},
    {KP_TYPE, 1, KP_TUNE, 38, KP_DEC, 55, KP_CA, 80, KP_CB, 100, KP_CC, 60, KP_BEND, 55, KP_TIME, 40, 0xFF},
    {KP_TYPE, 2, KP_TUNE, 40, KP_DEC, 50, KP_CA, 90, KP_CB, 40, KP_CC, 30, KP_CD, 20, KP_BEND, 40, KP_TIME, 40, 0xFF},
    {KP_TYPE, 2, KP_TUNE, 47, KP_DEC, 62, KP_CA, 80, KP_CB, 25, KP_CC, 20, KP_CD, 70, KP_BEND, 24, KP_TIME, 70, 0xFF},
    {KP_TYPE, 2, KP_TUNE, 38, KP_DEC, 58, KP_CA, 60, KP_CB, 60, KP_CC, 40, KP_CD, 30, KP_DTYPE, 3, KP_DRIVE, 30,
     KP_SHAPE, 80, 0xFF},
    {KP_TYPE, 3, KP_TUNE, 31, KP_DEC, 88, KP_CA, 70, KP_CB, 40, KP_CC, 100, KP_CD, 30, KP_BEND, 50, KP_TIME, 62, 0xFF},
    {KP_TYPE, 3, KP_TUNE, 29, KP_DEC, 105, KP_CA, 50, KP_CB, 25, KP_CC, 115, KP_CD, 50, KP_BEND, 40, KP_TAIL, 6,
     KP_TTIME, 100, KP_DRIFT, 30, KP_HOLD, 30, KP_DRIVE, 70, KP_SHAPE, 30, KP_CUT, 85, KP_RES, 20, KP_SLVL, 60, 0xFF},
    {KP_TYPE, 3, KP_TUNE, 36, KP_DEC, 80, KP_CA, 110, KP_CB, 80, KP_CC, 120, KP_CD, 60, KP_BEND, 90, KP_TIME, 56,
     KP_CURVE, 100, KP_TAIL, 20, KP_TTIME, 85, KP_HOLD, 50, KP_DTYPE, 1, KP_DRIVE, 95, KP_SHAPE, 80, KP_NLVL, 30,
     KP_NTONE, 90, KP_NDEC, 20, 0xFF},
    {KP_TYPE, 4, KP_TUNE, 33, KP_DEC, 75, KP_CA, 20, KP_CB, 60, KP_CC, 50, KP_CD, 10, KP_BEND, 50, 0xFF},
    {KP_TYPE, 4, KP_TUNE, 36, KP_DEC, 60, KP_CA, 80, KP_CB, 90, KP_CC, 40, KP_CD, 40, KP_BEND, 40, 0xFF},
    {KP_TYPE, 4, KP_TUNE, 29, KP_DEC, 95, KP_CA, 12, KP_CB, 40, KP_CC, 30, KP_BEND, 30, KP_SLVL, 50, KP_SINT, 0, 0xFF},
    {KP_TYPE, 5, KP_TUNE, 35, KP_DEC, 75, KP_CA, 18, KP_CB, 50, KP_CC, 76, KP_CD, 0, KP_BEND, 80, KP_TIME, 55,
     KP_CURVE, 90, KP_TAIL, 15, KP_DTYPE, 1, KP_DRIVE, 60, KP_SHAPE, 50, 0xFF},
    {KP_TYPE, 5, KP_TUNE, 30, KP_DEC, 90, KP_CA, 36, KP_CB, 70, KP_CC, 64, KP_CD, 40, KP_BEND, 60, 0xFF},
    {KP_TYPE, 5, KP_TUNE, 38, KP_DEC, 70, KP_CA, 18, KP_CB, 110, KP_CC, 84, KP_CD, 80, KP_BEND, 100, KP_TIME, 60,
     KP_CURVE, 90, KP_TAIL, 24, KP_TTIME, 70, KP_HOLD, 60, KP_DTYPE, 1, KP_DRIVE, 120, KP_SHAPE, 110, KP_CLIP, 60, 0xFF},
};
#define NPRE (NFAC + NUSER)              /* the preset list: the factory kicks, then the user's */

static void fac_sound(int i, uint8_t *out)
{
    const uint8_t *o = FAC_OV[i];
    int k;
    for (k = 0; k < KP_N; k++)
        out[k] = SND_BASE[k];
    for (k = 0; k < 40 && o[k] != 0xFFu; k += 2)
        out[o[k]] = o[k + 1];
}
static int pre_valid(int i) { return i >= 0 && (i < NFAC || (i < NPRE && us.u[i - NFAC].used)); }
static void pre_sound(int i, uint8_t *out)       /* a preset's kick (an empty user slot: the first factory kick) */
{
    int k;
    if (i >= NFAC && i < NPRE && us.u[i - NFAC].used) {
        for (k = 0; k < KP_N; k++)
            out[k] = us.u[i - NFAC].snd[k];
    } else {
        fac_sound(i < NFAC ? i : 0, out);
    }
}
static void pre_name(int i, char *b)
{
    int k;
    if (i >= NFAC && i < NPRE) {
        for (k = 0; k < 8; k++)
            b[k] = us.u[i - NFAC].name[k];
        b[8] = 0;
        for (k = 7; k >= 0 && (b[k] == ' ' || !b[k]); k--)
            b[k] = 0;
    } else {
        const char *s = FAC_NAME[i >= 0 && i < NFAC ? i : 0];
        for (k = 0; s[k] && k < 11; k++)
            b[k] = s[k];
        b[k] = 0;
    }
}
static int pre_step(int i, int d)                /* the next valid preset, d = +1 or -1, round */
{
    int n;
    for (n = 0; n < NPRE; n++) {
        i = (i + d + NPRE) % NPRE;
        if (pre_valid(i))
            return i;
    }
    return 0;
}

/* --------------------------------------------------------------- randomness --- */
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

/* ------------------------------------------------------------- the voice --- */
static kick_t voice;
static kparm_t eff;                      /* the sound as played (each block, and at each hit) */

/* ---------------------------------------------------------------- sequencer --- */
#define NEV 32
typedef struct {
    uint32_t at;                         /* the tick it sounds at */
    int8_t semis;
    uint8_t fl;                          /* KH_* */
    uint8_t step;                        /* its p-locks (0xFF: none) */
    float vel;
} ev_t;

static struct {
    volatile uint8_t running;
    uint32_t tick;
    float frac, inc;
    uint8_t first;
    uint32_t lc[1 + NMOD];               /* each lane's steps so far */
    int8_t lpos[1 + NMOD];               /* each lane's step now (-1: none yet) */
    float mval[NMOD], mtgt[NMOD];
    volatile uint8_t cur, next;          /* the pattern playing, the one queued (0xFF none) */
    ev_t ev[NEV];
    uint8_t nev;
    volatile uint32_t hits;
    volatile uint8_t gen_dirty;
    uint32_t gcycle;
    int8_t skip;                         /* a step recorded ahead of the playhead: not played this pass (-1 none) */
    uint8_t ext_wait;
    int32_t ext_c;
    uint32_t smp, ext_last;
    float ext_per;
    volatile uint16_t ext_bpm10;
    int8_t note_out;
    volatile int8_t aud_semis;
    volatile uint8_t aud_vel, aud_req, aud_step;
} sq;
static uint8_t gen_trig[NST], gen_rat[NST];   /* the generator's TRIG lane: 0, 1, 2; its rolls */
static volatile uint32_t midi_seen;
static volatile uint8_t rec_on;          /* REC: notes are written into the steps */
static volatile uint32_t fx_live;        /* the LIVE effects held */

static uint32_t lane_len(const pat_t *p, int l) { return l == 0 ? p->len : p->mod[l - 1].len; }
static uint32_t lane_div(const pat_t *p, int l) { return DIV_T[l == 0 ? p->div : p->mod[l - 1].div]; }
static uint32_t lane_dir(const pat_t *p, int l) { return l == 0 ? p->dir : p->mod[l - 1].dir; }

static float bpm_now(void) { return set.clk && sq.ext_bpm10 ? (float)sq.ext_bpm10 * 0.1f : (float)set.bpm10 * 0.1f; }
static void tempo_set(void)
{
    if (!set.clk || !sq.ext_per)
        sq.inc = bpm_now() * (float)PPQ / 60.0f / 44100.0f;
}

/* the generators: a pattern of L steps (0 off, 1 on, 2 accent) and its rolls */
static int acc_hit(int mode, int nth, uint32_t i)
{
    return (mode == 1 && nth == 0) || (mode == 2 && nth % 2 == 0) || (mode == 3 && (i & 1u));
}
static float hrand(uint32_t a, uint32_t b, uint32_t c) { return (float)(hash3(a, b, c) % 10000u) * 0.0001f; }

static void gen_run(const pat_t *p, uint32_t cycle)
{
    uint32_t L = p->len ? p->len : 1u, i, k, nh = 0;
    const uint8_t *g = p->ga;
    uint8_t out[NST] = {0}, rat[NST] = {0};
    int acc_mode = 0;
    uint32_t fill = 0, seed = (uint32_t)g[0] << 24 ^ (uint32_t)g[1] << 16 ^ (uint32_t)g[2] << 8 ^ g[3];
    switch (p->algo) {
    case AL_EUCL: {
        uint32_t hits = 1u + (uint32_t)g[0] * (L - 1u) / 127u, rot = (uint32_t)g[1] * L / 128u;
        for (i = 0; i < L; i++)
            if (((i + rot) % L * hits) % L < hits)
                out[i] = 1;
        acc_mode = g[2] / 32;
        fill = cycle % 4u == 3u ? g[3] : 0u;
        break;
    }
    case AL_POLY: {
        uint32_t a = 1u + (uint32_t)g[0] * 7u / 127u, b = 1u + (uint32_t)g[1] * 7u / 127u, sh = (uint32_t)g[2] * L / 128u;
        for (i = 0; i < L; i++) {
            int x = i % a == 0u, y = (i + L - sh) % L % b == 0u;
            if (x || y)
                out[i] = (x && y && g[3] > 40u) ? 2 : 1;
        }
        break;
    }
    case AL_METRIC: {                    /* the strong beats more often: DENSITY, how strongly (WEIGHT), VARY a bar */
        float d = (float)g[0] / 127.0f, kexp = 0.5f + (float)g[1] / 127.0f * 4.0f;
        uint32_t c = hrand(seed, cycle, 77u) * 127.0f < (float)g[3] ? cycle : 0u;
        for (i = 0; i < L; i++) {
            float w = i % 16u == 0u ? 1.0f : i % 8u == 0u ? 0.85f : i % 4u == 0u ? 0.7f : i % 2u == 0u ? 0.45f : 0.25f;
            float pr = d * 2.0f * fm_exp2f(kexp * fm_log2f(w));
            if (hrand(seed ^ 0x5Au, c, i) < pr)
                out[i] = 1;
        }
        if (d > 0.0f)
            out[0] = 1;
        acc_mode = g[2] / 32;
        break;
    }
    case AL_SYNC: {                      /* the beats, some pushed a step early (PUSH), some off-beats (OFFBEAT) */
        float push = (float)g[0] / 127.0f, off = (float)g[1] / 127.0f;
        uint32_t c = hrand(seed, cycle, 78u) * 127.0f < (float)g[3] ? cycle : 0u;
        for (i = 0; i < L; i += 4u) {
            if (i && hrand(seed, c, i) < push)
                out[i - 1u] = 1;
            else
                out[i] = 1;
            if (i + 2u < L && hrand(seed ^ 0xA5u, c, i) < off)
                out[i + 2u] = 1;
        }
        acc_mode = g[2] / 32;
        break;
    }
    case AL_BOUNCE: {                    /* a ball bouncing: the gaps shrink by SPEED from START, then again */
        float gap0 = 2.0f + (float)g[0] / 127.0f * 6.0f, f = 0.5f + (float)g[1] / 127.0f * 0.45f, pos = 0.0f, gap = gap0;
        uint32_t rot = (uint32_t)g[3] * L / 128u;
        while (pos < (float)L) {
            out[((uint32_t)(pos + 0.5f) + rot) % L] = 1;
            pos += gap < 1.0f ? 1.0f : gap;
            gap *= f;
            if (gap < 1.0f)
                gap = gap0;
        }
        acc_mode = g[2] / 32;
        break;
    }
    case AL_MIRROR: {                    /* a random half, then the same backwards, unit after unit */
        static const uint32_t U[4] = {2, 4, 8, 16};
        uint32_t u = U[g[1] / 32], h = u / 2u;
        for (i = 0; i < L; i++) {
            uint32_t j = i % u, m = j < h ? j : u - 1u - j;
            if (hrand(g[3], 91u, m) < (float)g[0] / 127.0f)
                out[i] = 1;
        }
        acc_mode = g[2] / 32;
        break;
    }
    case AL_FRACT: {
        uint32_t kind = g[0] * 3u / 128u, rot = (uint32_t)g[1] * L / 128u;
        for (i = 0; i < L; i++) {
            uint32_t j = (i + rot) % L, v = j, b = 0, on = 0;
            if (kind == 0) {             /* Thue-Morse: an odd number of ones */
                while (v) {
                    b ^= v & 1u;
                    v >>= 1;
                }
                on = b;
            } else if (kind == 1) {      /* Cantor: no 1 among its digits in base 3 */
                on = 1;
                while (v) {
                    if (v % 3u == 1u)
                        on = 0;
                    v /= 3u;
                }
            } else {                     /* the Fibonacci word's zeros */
                on = (uint32_t)((float)(j + 2u) * 0.618034f) - (uint32_t)((float)(j + 1u) * 0.618034f) == 0u;
            }
            if (on)
                out[i] = 1;
        }
        acc_mode = g[2] / 32;
        fill = cycle % 4u == 3u ? g[3] : 0u;
        break;
    }
    case AL_BITS: {                      /* the bits of VALUE, round the steps */
        uint32_t v = (uint32_t)g[0] * 2u + 1u, rot = (uint32_t)g[1] * L / 128u;
        for (i = 0; i < L; i++)
            if (v >> (((i + rot) % L) % 8u) & 1u)
                out[i] = 1;
        acc_mode = g[2] / 32;
        fill = cycle % 4u == 3u ? g[3] : 0u;
        break;
    }
    case AL_ROLLS: {                     /* a base and a roll: ROLL LEN steps from ROLL AT, RATE hits a step */
        uint32_t base = g[0] / 32u, at = (uint32_t)g[1] * L / 128u, len = 1u + (uint32_t)g[2] * 7u / 127u;
        uint8_t r = (uint8_t)(1u + (uint32_t)g[3] * 2u / 127u);
        for (i = 0; i < L; i++)
            if ((base == 0 && i % 4u == 0u) || (base == 1 && i % 8u == 0u) || (base == 2 && i % 4u == 2u) ||
                (base == 3 && i % 2u == 0u))
                out[i] = 1;
        for (k = 0; k < len && at + k < L; k++) {
            out[at + k] = 1;
            rat[at + k] = r;
        }
        if (at + len - 1u < L)
            out[at + len - 1u] = 2;
        break;
    }
    default:
        break;
    }
    if (fill)
        for (i = 0; i < L; i++)
            if (!out[i] && hash3(cycle, i, 99u) % 127u < fill)
                out[i] = 1;
    if (acc_mode)
        for (i = 0; i < L; i++)
            if (out[i]) {
                if (acc_hit(acc_mode, (int)nh, i))
                    out[i] = 2;
                nh++;
            }
    for (i = 0, k = 0; i < L; i++)
        k |= out[i];
    if (!k && p->algo != AL_MAN)         /* never a silent bar: the first step at least */
        out[0] = 1;
    for (i = 0; i < NST; i++) {
        gen_trig[i] = i < L ? out[i] : 0;
        gen_rat[i] = i < L ? rat[i] : 0;
    }
}

static int lane_pos(int l, uint32_t c, uint32_t L, uint32_t dir)
{
    if (L <= 1u)
        return 0;
    switch (dir) {
    default:
    case 0: return (int)(c % L);
    case 1: return (int)(L - 1u - c % L);
    case 2: {
        uint32_t per = 2u * L - 2u, m = c % per;
        return (int)(m < L ? m : per - m);
    }
    case 3: return (int)(xr32(&rng_au) % L);
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
        ev_t *e = &sq.ev[sq.nev++];
        e->at = at;
        e->semis = semis;
        e->vel = vel;
        e->fl = fl;
        e->step = step;
    }
}

static float mod_value(const mod_t *m, int l, int pos)
{
    float L = (float)(m->len > 1u ? m->len : 2u), x = (float)pos;
    switch (m->shape) {
    default:
    case SH_MAN: return (float)m->val[pos] / 63.0f;
    case SH_UP: return x / (L - 1.0f) * 2.0f - 1.0f;
    case SH_DOWN: return 1.0f - x / (L - 1.0f) * 2.0f;
    case SH_TRI: { float u = x / L * 2.0f; return u < 1.0f ? u * 2.0f - 1.0f : 3.0f - u * 2.0f; }
    case SH_SINE: return fm_sin_turns(x / L);
    case SH_SQR: return x < L * 0.5f ? 1.0f : -1.0f;
    case SH_RAND: return (float)(int32_t)hash3(m->seed, sq.lc[l], (uint32_t)pos) * (1.0f / 2147483648.0f);
    case SH_WALK: {
        float v = sq.mtgt[l - 1] + (float)(int32_t)hash3(m->seed, sq.lc[l], 5u) * (0.35f / 2147483648.0f);
        return v > 1.0f ? 2.0f - v : v < -1.0f ? -2.0f - v : v;
    }
    }
}

/* a step's sound: the pattern's, or the kick its lock names, with its parameter locks (no MOD lanes) */
static void step_sound(const pat_t *p, const lock_t *lk, float *out)
{
    uint8_t base[KP_N];
    uint32_t k;
    if (lk && lk->kit && pre_valid(lk->kit - 1))
        pre_sound(lk->kit - 1, base);
    else
        for (k = 0; k < KP_N; k++)
            base[k] = p->snd[k];
    for (k = 0; k < KP_N; k++)
        out[k] = (float)base[k];
    if (lk)
        for (k = 0; k < NLOCK; k++)
            if (lk->id[k] && lk->id[k] <= KP_N)
                out[lk->id[k] - 1] = (float)lk->val[k];
}

static lock_t hit_lk;                    /* the p-locks of the hit sounding: they last as long as it does */
static uint8_t hit_has;

static void eff_compute(void)
{
    const pat_t *p = &pats(sq.cur);
    uint32_t k, m;
    step_sound(p, hit_has ? &hit_lk : 0, eff.p);
    for (m = 0; m < NMOD; m++) {
        const mod_t *md = &p->mod[m];
        int kp;
        if (!sq.running || md->tgt == 0u || md->tgt > KP_N || !md->depth)
            continue;
        kp = md->tgt - 1;
        eff.p[kp] += (float)md->depth / 100.0f * sq.mval[m] * (kp == KP_TUNE ? 24.0f : (float)(kp_hi(kp) - kp_lo(kp)));
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
            float tc = ((float)md->smooth / 127.0f) * ((float)md->smooth / 127.0f) + 0.002f;
            coef = 1.0f - fm_expf(-(float)APP_BLOCK / 44100.0f / tc);
        }
        sq.mval[m] += (sq.mtgt[m] - sq.mval[m]) * coef;
    }
    eff_compute();
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

/* a hit: its p-locks (0xFF none) hold while it sounds */
static void hit(int semis, float vel, uint8_t fl, uint8_t step)
{
    if (step < NST) {
        hit_lk = pats(sq.cur).lk[step];
        hit_has = 1;
    } else {
        hit_has = 0;
    }
    eff_compute();
    kdsp_hit(&voice, &eff, (float)semis, vel, fl);
    sq.hits++;
    midi_note_out(36 + semis, vel);
}

static void gen_print(pat_t *p)          /* the generator's steps into the TRIG lane; MANUAL from then on */
{
    uint32_t i;
    for (i = 0; i < NST; i++) {
        p->st[i].trig = gen_trig[i];
        if (gen_rat[i])
            p->st[i].rat = gen_rat[i];
    }
    p->algo = AL_MAN;
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
        uint32_t nr = 1u + (p->algo == AL_MAN ? s->rat : (gen_rat[pos] > s->rat ? gen_rat[pos] : s->rat));
        if (sq.skip == pos) {            /* recorded just ahead: it sounded when it was played */
            sq.skip = -1;
            return;
        }
        if (trig && (p->prob >= 100u || (int)(xr32(&rng_au) % 100u) < p->prob)) {
            uint32_t delay = (c & 1u) ? D * p->swing / 200u : 0u, r;
            float vel = 1.0f;
            uint8_t fl = (uint8_t)((s->fl & FL_SLIDE ? KH_SLIDE : 0) | (s->fl & FL_REV ? KH_REV : 0) | (trig == 2 ? KH_ACC : 0));
            if (p->human) {
                delay += xr32(&rng_au) % (D * p->human / 400u + 1u);
                vel -= (float)(xr32(&rng_au) % 1000u) * 0.001f * (float)p->human / 100.0f * 0.25f;
            }
            for (r = 0; r < nr; r++)
                ev_add(sq.tick + delay + r * D / nr, s->pit, r ? vel * 0.85f : vel, r ? (uint8_t)(fl & ~KH_SLIDE) : fl,
                       (uint8_t)pos);
        }
    } else {
        sq.mtgt[l - 1] = mod_value(&p->mod[l - 1], l, pos);
    }
}

static struct {
    volatile uint8_t req, hi;
    uint32_t at, from;
    float env, ph, f;
} met;                                   /* the metronome: a click on each beat, a higher one on the bar */
static uint32_t blk_i;
static volatile uint32_t beat_now;

static void on_tick(uint32_t t)
{
    if (t % PPQ == 0u) {
        beat_now = t / PPQ;
        if (set.metro) {
            met.req = 1;
            met.hi = t % (PPQ * 4u) == 0u;
            met.at = blk_i;
        }
    }
    pat_t *p = &pats(sq.cur);
    uint32_t l, i;
    for (l = 0; l <= NMOD; l++)
        if (t % lane_div(p, (int)l) == 0u)
            lane_step((int)l);
    if (t % 240u == 0u)
        fx_step(t / 240u);
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
    sq.skip = -1;
    for (k = 0; k <= NMOD; k++) {
        sq.lc[k] = 0;
        sq.lpos[k] = -1;
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

/* REC: a note into the steps (from the keyboard or from MIDI). Playing: the nearest step (the next one past half
 * of this one, and that one does not sound again this pass); stopped: the selected step, then the next one. */
static volatile uint8_t rec_sel;         /* the step a stopped recording writes next */
static volatile uint32_t rec_count;
static void pat_changed_from_render(void);
static void rec_note(int semis, int vel)
{
    pat_t *p = &pats(sq.cur);
    int s;
    if (p->algo != AL_MAN)
        gen_print(p);
    if (sq.running && sq.lpos[0] >= 0) {
        uint32_t D = DIV_T[p->div], ph = sq.tick % D;
        s = sq.lpos[0];
        if (ph * 2u >= D) {
            s = (s + 1) % p->len;
            sq.skip = (int8_t)s;
        }
    } else {
        s = rec_sel % p->len;
        rec_sel = (uint8_t)((s + 1) % p->len);
    }
    p->st[s].trig = vel >= 112 ? 2 : 1;
    p->st[s].pit = (int8_t)clamp(semis, -24, 24);
    rec_count++;
    pat_changed_from_render();
}

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
        (void)port;
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
        if (set.mch && st >= 0x80u && st <= 0xE0u && (b & 0x0Fu) != (uint32_t)set.mch - 1u)
            continue;
        if (st == 0x90u && d2) {         /* a note plays the kick, chromatic from 36 (C1); with REC, it is recorded */
            midi_seen++;
            hit_has = 0;
            eff_compute();
            kdsp_hit(&voice, &eff, (float)((int)d1 - 36), (float)d2 / 127.0f, d2 >= 112u ? KH_ACC : 0);
            sq.hits++;
            if (rec_on)
                rec_note((int)d1 - 36, (int)d2);
        }
    }
}

/* ---------------------------------------------------------------- the render --- */
static float vbuf[APP_BLOCK], lbuf[APP_BLOCK], rbuf[APP_BLOCK];
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
    float gain = (float)master_q12 / 4096.0f * 8300000.0f, tick0;
    if (n > APP_BLOCK)
        n = APP_BLOCK;
    tempo_set();
    eff_update();
    blk_i = 0;
    midi_in();
    tick0 = (float)sq.tick + sq.frac;
    if (sq.aud_req) {                    /* the keyboard: the kick now (a step held: with its p-locks) */
        sq.aud_req = 0;
        if (sq.aud_step < NST) {
            hit_lk = pats(sq.cur).lk[sq.aud_step];
            hit_has = 1;
        } else {
            hit_has = 0;
        }
        eff_compute();
        kdsp_hit(&voice, &eff, (float)sq.aud_semis, (float)sq.aud_vel / 127.0f, sq.aud_vel >= 112u ? KH_ACC : 0);
        sq.hits++;
    }
    for (i = 0; i < n; i++) {
        sq.smp++;
        if (!sq.running)
            continue;
        blk_i = i;
        if (sq.first && !set.clk) {
            sq.first = 0;
            render_to(&done, i);
            on_tick(0);
            continue;
        }
        if (set.clk) {
            uint32_t cap = sq.ext_c < 0 ? 0u : (uint32_t)sq.ext_c * (PPQ / 24u) + (PPQ / 24u) - 1u;
            if (sq.ext_c < 0 || sq.first || (int32_t)(sq.tick - cap) >= 0)
                continue;
        }
        sq.frac += sq.inc;
        while (sq.frac >= 1.0f) {
            sq.frac -= 1.0f;
            sq.tick++;
            render_to(&done, i);
            on_tick(sq.tick);
        }
    }
    render_to(&done, n);
    fx_process(&set.fx, fx_live, vbuf, lbuf, rbuf, n, bpm_now(), tick0, sq.inc, sq.running);
    if (met.req && met.at < n) {         /* the metronome's click, where its beat fell */
        met.req = 0;
        met.env = 1.0f;
        met.ph = 0.0f;
        met.f = met.hi ? 2400.0f : 1500.0f;
        met.from = met.at;
    } else {
        met.from = 0;
    }
    if (met.env > 1e-4f)
        for (i = met.from; i < n; i++) {
            float c;
            met.ph += met.f * (1.0f / 44100.0f);
            if (met.ph >= 1.0f)
                met.ph -= 1.0f;
            c = fm_sin_turns(met.ph) * met.env * 0.3f;
            met.env *= 0.9982f;
            lbuf[i] += c;
            rbuf[i] += c;
        }
    for (i = 0; i < n; i++) {
        gain_now += (gain - gain_now) * 0.004f;
        lr[2 * i] = (int32_t)(fm_tanhf(lbuf[i]) * gain_now);
        lr[2 * i + 1] = (int32_t)(fm_tanhf(rbuf[i]) * gain_now);
    }
}

void app_panic(void)
{
    sq.running = 0;
    voice.active = 0;
    voice.tail = 0.0f;
    fx_reset();
}

/* ---------------------------------------------------------------- patterns --- */
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
static void pat_sound(pat_t *p, int pre)
{
    pre_sound(pre, p->snd);
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
static void steps_clear(pat_t *p)
{
    uint32_t i;
    for (i = 0; i < NST; i++) {
        p->st[i].trig = p->st[i].fl = p->st[i].rat = 0;
        p->st[i].pit = 0;
        lock_clear(&p->lk[i]);
    }
}
static void pat_clear(pat_t *p)
{
    uint32_t i;
    steps_clear(p);
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
    for (i = 0; i < 4u; i++)
        p->rsv[i] = 0;
}
static void pat_floor(pat_t *p)
{
    uint32_t i;
    for (i = 0; i < p->len; i += 4u)
        p->st[i].trig = 1;
    p->st[0].trig = 2;
}
static void pat_gen(pat_t *p, int algo, int a, int b, int c, int d)
{
    p->algo = (uint8_t)algo;
    p->ga[0] = (uint8_t)a;
    p->ga[1] = (uint8_t)b;
    p->ga[2] = (uint8_t)c;
    p->ga[3] = (uint8_t)d;
}
/* the sixteen patterns of the first power-on: every kind of kick, every generator */
static void pats_defaults(void)
{
    static const uint8_t PRE_OF[NPAT] = {9, 3, 0, 15, 12, 7, 4, 10, 17, 1, 13, 6, 11, 16, 2, 14};
    uint32_t i;
    for (i = 0; i < 4u; i++)
        pk[i].magic = PATS_MAGIC;
    for (i = 0; i < NPAT; i++) {
        pat_t *p = &pats(i);
        pat_clear(p);
        pat_sound(p, PRE_OF[i]);
        pat_floor(p);
    }
    pat_gen(&pats(1), AL_EUCL, 34, 0, 40, 0);
    pats(1).mod[0].tgt = 1 + KP_DEC;
    pats(1).mod[0].shape = SH_UP;
    pats(1).mod[0].len = 7;
    pats(1).mod[0].depth = -30;
    pats(2).st[6].trig = 1;
    pats(2).st[7].trig = 1;
    pats(2).st[7].fl = FL_SLIDE;
    pats(2).st[7].pit = 5;
    pats(2).st[14].trig = 1;
    pats(2).st[14].fl = FL_REV;
    pat_gen(&pats(3), AL_SYNC, 60, 40, 40, 30);
    pat_gen(&pats(4), AL_POLY, 36, 54, 0, 64);
    pat_gen(&pats(5), AL_METRIC, 70, 60, 40, 50);
    pats(5).mod[0].tgt = 1 + KP_TUNE;
    pats(5).mod[0].shape = SH_RAND;
    pats(5).mod[0].depth = 30;
    pat_gen(&pats(6), AL_BOUNCE, 64, 80, 40, 0);
    pats(7).len = 32;
    pat_gen(&pats(7), AL_MIRROR, 60, 64, 40, 7);
    pats(8).st[15].trig = 1;
    pats(8).st[15].rat = 2;
    pats(8).mod[0].tgt = 1 + KP_CUT;
    pats(8).mod[0].shape = SH_TRI;
    pats(8).mod[0].len = 5;
    pats(8).mod[0].depth = -25;
    pat_gen(&pats(9), AL_FRACT, 0, 0, 40, 0);
    pat_gen(&pats(10), AL_BITS, 77, 0, 40, 0);
    pat_gen(&pats(11), AL_ROLLS, 0, 96, 40, 64);
    pats(12).len = 32;
    pat_gen(&pats(12), AL_EUCL, 33, 0, 40, 30);
    pats(13).st[6].trig = 1;                       /* p-locks: another kick on step 7, a higher one on step 15 */
    pats(13).lk[6].kit = 1 + 3;
    pats(13).st[14].trig = 1;
    lock_set(&pats(13).lk[14], KP_TUNE, pats(13).snd[KP_TUNE] + 5);
    lock_set(&pats(13).lk[14], KP_DEC, 50);
    pat_gen(&pats(14), AL_METRIC, 50, 100, 40, 90);
    for (i = 0; i < NST; i++)
        pats(15).st[i].trig = 0;
    pats(15).st[0].trig = 2;
    pats(15).st[10].trig = 1;
}

static void theme_apply(void)
{
    th = THEMES[set.theme < NTHEME ? set.theme : 0];
    th.dial = set.dial & 1u;
    th.scope = set.scope & 1u;
    th.steps = set.steps & 1u;
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
    set.rsv = 0;
    set.theme = 0;
    set.dial = THEMES[0].dial;
    set.scope = THEMES[0].scope;
    set.steps = THEMES[0].steps;
    set.metro = 0;
    set.rsv2[0] = set.rsv2[1] = set.rsv2[2] = 0;
    fx_defaults(&set.fx);
}
static void users_defaults(void)
{
    uint32_t i, k;
    us.magic = USER_MAGIC;
    for (i = 0; i < NUSER; i++) {
        us.u[i].used = 0;
        for (k = 0; k < 8u; k++)
            us.u[i].name[k] = ' ';
        for (k = 0; k < KP_N; k++)
            us.u[i].snd[k] = SND_BASE[k];
    }
}

static int snd_valid(const uint8_t *s)
{
    int k;
    for (k = 0; k < KP_N; k++)
        if (s[k] < kp_lo(k) || s[k] > kp_hi(k))
            return 0;
    return 1;
}
static int pat_valid(const pat_t *p)
{
    uint32_t k, m;
    if (p->len < 1u || p->len > NST || p->div >= NDIV || p->dir >= NDIR || p->algo >= NALGO || p->swing > 100u ||
        p->prob > 100u || p->human > 100u || p->preset >= NPRE || !snd_valid(p->snd))
        return 0;
    for (m = 0; m < NST; m++) {
        const lock_t *l = &p->lk[m];
        if (l->kit > NPRE || p->st[m].trig > 2u || p->st[m].rat > 3u)
            return 0;
        for (k = 0; k < NLOCK; k++)
            if (l->id[k] > KP_N || (l->id[k] && (l->val[k] < kp_lo(l->id[k] - 1) || l->val[k] > kp_hi(l->id[k] - 1))))
                return 0;
    }
    for (m = 0; m < NMOD; m++)
        if (p->mod[m].len < 1u || p->mod[m].len > NST || p->mod[m].div >= NDIV || p->mod[m].dir >= NDIR ||
            p->mod[m].shape >= NSHAPE || p->mod[m].tgt > KP_N)
            return 0;
    return 1;
}
static int pats_load(void)
{
    uint32_t i, k;
    for (i = 0; i < 4u; i++)
        if (plat_store_load(PAT_OBJ[i], &pk[i], sizeof pk[i]) != (int)sizeof pk[i] || pk[i].magic != PATS_MAGIC)
            return 0;
    for (i = 0; i < 4u; i++)
        for (k = 0; k < 4u; k++)
            if (!pat_valid(&pk[i].p[k]))
                return 0;
    return 1;
}

void app_init(void)
{
    if (plat_store_load(OBJ_SET, &set, sizeof set) != (int)sizeof set || set.magic != SET_MAGIC || set.cur >= NPAT ||
        set.bpm10 < 400u || set.bpm10 > 2400u || set.mch > 16u || set.clk > 1u || set.theme >= NTHEME)
        set_defaults();
    theme_apply();
    if (plat_store_load(OBJ_USER, &us, sizeof us) != (int)sizeof us || us.magic != USER_MAGIC)
        users_defaults();
    else {
        uint32_t i;
        for (i = 0; i < NUSER; i++)
            if (us.u[i].used && !snd_valid(us.u[i].snd))
                us.u[i].used = 0;
    }
    if (!pats_load())
        pats_defaults();
    kdsp_init(&voice, 0xC0FFEEu);
    fx_reset();
    sq.cur = set.cur;
    sq.next = 0xFFu;
    sq.note_out = -1;
    seq_reset();
    tempo_set();
    eff_update();
}

/* --------------------------------------------------------------- randomise --- */
static void rnd_param(uint8_t *snd, int k)
{
    uint32_t *s = &rng_ui;
    int v;
    switch (k) {
    case KP_TYPE: v = rr(s, 0, KT_N - 1); break;
    case KP_TUNE: v = rr(s, 26, 42); break;
    case KP_DEC: v = rr(s, 50, 105); break;
    case KP_LEVEL: v = rr(s, 104, 114); break;
    case KP_BEND: v = rr(s, 15, 110); break;
    case KP_TIME: v = rr(s, 35, 85); break;
    case KP_CURVE: v = rr(s, 20, 120); break;
    case KP_TAIL: v = rr(s, 0, 1) ? 0 : rr(s, 2, 30); break;
    case KP_TTIME: v = rr(s, 40, 110); break;
    case KP_GLIDE: v = rr(s, 20, 80); break;
    case KP_DRIFT: v = rr(s, 0, 9) < 7 ? 0 : rr(s, 5, 40); break;
    case KP_VBEND: v = rr(s, 0, 60); break;
    case KP_ATK: v = rr(s, 0, 4) ? 0 : rr(s, 1, 12); break;
    case KP_HOLD: v = rr(s, 0, 60); break;
    case KP_DROP: v = rr(s, 40, 90); break;
    case KP_PUNCH: v = rr(s, 0, 80); break;
    case KP_CTYPE: v = rr(s, 0, CK_N - 1); break;
    case KP_CLVL: v = rr(s, 0, 1) ? 0 : rr(s, 10, 80); break;
    case KP_CTONE: v = rr(s, 30, 120); break;
    case KP_CLEN: v = rr(s, 10, 80); break;
    case KP_SLVL: v = rr(s, 0, 4) < 3 ? 0 : rr(s, 20, 70); break;
    case KP_SINT: v = rr(s, 0, 4) ? 1 : rr(s, 0, 1) ? 0 : 3; break;
    case KP_SDEC: v = rr(s, 30, 100); break;
    case KP_SDRV: v = rr(s, 0, 60); break;
    case KP_NLVL: v = rr(s, 0, 4) < 3 ? 0 : rr(s, 5, 50); break;
    case KP_NTONE: v = rr(s, 30, 120); break;
    case KP_NDEC: v = rr(s, 5, 60); break;
    case KP_NBW: v = rr(s, 20, 110); break;
    case KP_DTYPE: { int r = rr(s, 0, 19); v = r < 9 ? DT_SOFT : r < 13 ? DT_HARD : r < 15 ? DT_TUBE : r < 17 ? DT_FOLD : r < 19 ? DT_CRUSH : DT_RECT; break; }
    case KP_DRIVE: v = rr(s, 0, 100); break;
    case KP_SHAPE: v = rr(s, 0, 100); break;
    case KP_DMIX: v = rr(s, 70, 127); break;
    case KP_FTYPE: v = rr(s, 0, 4) ? FT_LP : rr(s, 0, FT_N - 1); break;
    case KP_CUT: v = snd[KP_FTYPE] == FT_LP ? (rr(s, 0, 1) ? 127 : rr(s, 60, 126)) : rr(s, 20, 100); break;
    case KP_RES: v = rr(s, 0, 70); break;
    case KP_FENV: v = rr(s, 50, 100); break;
    case KP_EQLO: v = rr(s, 54, 84); break;
    case KP_EQMID: v = rr(s, 50, 78); break;
    case KP_EQFRQ: v = rr(s, 20, 100); break;
    case KP_EQHI: v = rr(s, 50, 80); break;
    case KP_BODY: v = rr(s, 95, 127); break;
    case KP_VSENS: v = 64; break;
    case KP_ACC: v = rr(s, 40, 90); break;
    case KP_CLIP: v = rr(s, 0, 60); break;
    default: v = rr(s, 0, 127); break;   /* the CHARACTER knobs */
    }
    snd[k] = (uint8_t)clamp(v, kp_lo(k), kp_hi(k));
}
static void rnd_sound(pat_t *p, int page)   /* page -1: the whole kick */
{
    int k;
    for (k = 0; k < KP_N; k++)
        if (page < 0 || k / 4 == page)
            rnd_param(p->snd, k);
}
static void rnd_steps(pat_t *p)
{
    uint32_t i, *s = &rng_ui;
    int style = rr(s, 0, 3);
    for (i = 0; i < NST; i++) {
        step_t *t = &p->st[i];
        t->trig = t->fl = t->rat = 0;
        t->pit = 0;
        if (i >= p->len)
            continue;
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
    if (!p->st[0].trig)
        p->st[0].trig = 1;
}
static void rnd_gen(pat_t *p)
{
    uint32_t *s = &rng_ui, k;
    p->algo = (uint8_t)rr(s, 1, NALGO - 1);
    for (k = 0; k < 4u; k++)
        p->ga[k] = (uint8_t)rr(s, 0, 127);
    if (p->algo == AL_EUCL)
        p->ga[0] = (uint8_t)rr(s, 15, 70);
    if (p->algo == AL_EUCL || p->algo == AL_FRACT || p->algo == AL_BITS)
        p->ga[3] = (uint8_t)rr(s, 0, 40);
}
static void rnd_feel(pat_t *p)           /* never the tempo */
{
    uint32_t *s = &rng_ui;
    p->swing = (uint8_t)(rr(s, 0, 2) ? 0 : rr(s, 10, 45));
    p->human = (uint8_t)(rr(s, 0, 2) ? 0 : rr(s, 5, 30));
    p->prob = (uint8_t)(rr(s, 0, 2) ? 100 : rr(s, 70, 95));
}
static void rnd_mod(mod_t *m)
{
    uint32_t *s = &rng_ui, i;
    static const uint8_t TG[] = {KP_TUNE, KP_BEND, KP_TIME, KP_DEC, KP_CA, KP_CB, KP_CC, KP_CD, KP_PUNCH,
                                 KP_CLVL, KP_DRIVE, KP_SHAPE, KP_CUT, KP_RES, KP_NLVL, KP_DROP};
    m->tgt = (uint8_t)(1 + TG[rr(s, 0, (int)sizeof TG - 1)]);
    m->shape = (uint8_t)rr(s, 0, NSHAPE - 1);
    m->len = (uint8_t)rr(s, 3, 16);
    m->div = (uint8_t)(rr(s, 0, 2) ? 2 : rr(s, 3, 6));
    m->dir = (uint8_t)(rr(s, 0, 3) ? 0 : rr(s, 1, NDIR - 1));
    m->smooth = (uint8_t)(rr(s, 0, 2) ? 0 : rr(s, 10, 60));
    m->depth = (int8_t)(rr(s, 10, 50) * (rr(s, 0, 1) ? 1 : -1));
    if (m->tgt == 1 + KP_TUNE)
        m->depth = (int8_t)(m->depth / 3);
    m->seed = (uint8_t)rr(s, 0, 127);
    for (i = 0; i < NST; i++)
        m->val[i] = (int8_t)rr(s, -64, 63);
}
/* p-locks: on some of the steps that play, a kick of another kind, one or two parameters moved, or both */
static void rnd_locks(pat_t *p)
{
    static const uint8_t KP_LK[] = {KP_TYPE, KP_TUNE, KP_BEND, KP_TIME, KP_DEC, KP_CA, KP_CB, KP_CC, KP_CD, KP_PUNCH,
                                    KP_CLVL, KP_NLVL, KP_DRIVE, KP_SHAPE, KP_CUT, KP_TAIL, KP_DTYPE};
    uint32_t *s = &rng_ui, i;
    int dens = rr(s, 20, 45);
    for (i = 0; i < NST; i++) {
        lock_t *l = &p->lk[i];
        int on = p->algo == AL_MAN ? p->st[i].trig : 1, n, j;
        float base[KP_N];
        lock_clear(l);
        if (i >= p->len || !on || rr(s, 0, 99) >= (p->algo == AL_MAN ? dens : dens / 2))
            continue;
        if (rr(s, 0, 3) == 0)
            l->kit = (uint8_t)(1 + rr(s, 0, NFAC - 1));
        n = l->kit && rr(s, 0, 1) ? 0 : rr(s, 1, 2);
        step_sound(p, l, base);
        for (j = 0; j < n; j++) {
            int kp = KP_LK[rr(s, 0, (int)sizeof KP_LK - 1)], b = (int)base[kp], v;
            if (kp == KP_TUNE)
                v = b + (rr(s, 0, 1) ? rr(s, -7, 7) : 12 * rr(s, -1, 1));
            else if (kp_hi(kp) < 10)
                v = rr(s, kp_lo(kp), kp_hi(kp));
            else
                v = b + rr(s, -45, 45);
            lock_set(l, kp, clamp(v, kp_lo(kp), kp_hi(kp)));
        }
    }
}

/* ---------------------------------------------------------------------- UI --- */
enum { M_HOME, M_SEQ, M_GEN, M_SND, M_MOD, M_FX, M_PAT, NMODE };
static const uint8_t NPAGE[NMODE] = {1, 2, 1, NSPAGE + 1, 2 * NMOD, 2, 3};
#define PG_STORE NSPAGE
enum { A_COPY, A_PASTE, A_CLEAR, A_INIT, A_RAND, NACT };
static const char *const ACT_NAME[NACT] = {"COPY", "PASTE", "CLEAR", "INIT KICK", "RANDOM"};

static struct {
    uint8_t mode, page[NMODE];
    char msg[2][24];
    uint32_t msg_until;
    uint32_t keys, btn;
    int8_t held;                         /* the step held (0..31), -1 none */
    uint8_t held_turned;
    uint8_t bank;                        /* the white keys: steps 1-16 (0) or 17-32 (1) */
    uint8_t act;
    uint8_t redraw_h, redraw_c, redraw_f;
    uint32_t last_change;
    volatile uint8_t pat_dirty;          /* bit i: patterns 4i..4i+3 */
    uint8_t set_dirty, user_dirty;
    pat_t clip;
    uint8_t has_clip;
    uint8_t fxsel[2];                    /* FX: the effect the knobs edit, on LIVE and on FINAL */
    int8_t pop;                          /* the knob just turned (shown big), -1 none */
    uint32_t pop_until;
    uint8_t slot, cursor;                /* STORE */
    char name[9];
    int8_t smin[240], smax[240];
    uint8_t spitch[240];
    float sdur;
    uint32_t shash;
    uint8_t szoom;
} ui;

static pat_t *P(void) { return &pats(sq.cur); }

static void pat_changed_from_render(void)
{
    ui.pat_dirty |= (uint8_t)(1u << (sq.cur >> 2));
    ui.last_change = plat_ms();
    ui.redraw_c = ui.redraw_f = 1;
}

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
static void sint_text(int32_t v, char *b)
{
    if (v > 0)
        *b++ = '+';
    int_text(v, b);
}
static void cat_int(char *b, int32_t v)
{
    char t[12];
    int_text(v, t);
    text_cat(b, t);
}
static void dec1_text(int32_t v10, char *b)
{
    char t[4];
    int_text(v10 / 10, b);
    t[0] = '.';
    t[1] = (char)('0' + (v10 < 0 ? -v10 : v10) % 10);
    t[2] = 0;
    text_cat(b, t);
}
static void note_text(int note, char *b)
{
    static const char *const N[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    b[0] = 0;
    text_cat(b, N[note % 12]);
    cat_int(b, note / 12 - 1);
}
static void time_text(float s, char *b)
{
    char t[8];
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
    b[0] = 0;
    if (hz >= 1000.0f) {
        dec1_text((int32_t)(hz / 100.0f + 0.5f), b);
        text_cat(b, "K");
    } else {
        int_text((int32_t)(hz + 0.5f), b);
        text_cat(b, "HZ");
    }
}
static void pct_text(int v127, char *b)
{
    int_text(v127 * 100 / 127, b);
    text_cat(b, "%");
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
    ui.pat_dirty |= (uint8_t)(1u << (sq.cur >> 2));
    ui.last_change = plat_ms();
    ui.redraw_c = ui.redraw_f = 1;
}
static void set_changed(void)
{
    ui.set_dirty = 1;
    ui.last_change = plat_ms();
    ui.redraw_h = ui.redraw_c = ui.redraw_f = 1;
}

/* a parameter's name and text: the CHARACTER knobs are named after the kind of kick */
static const char *pname(int kp, int type)
{
    return kp >= KP_CA && kp <= KP_CD ? CHAR_NAME[clamp(type, 0, KT_N - 1)][kp - KP_CA] : PNAME[kp];
}
static float ex2(float v, float oct) { return fm_exp2f(v / 127.0f * oct); }
static void param_text(int kp, int v, int type, char *b)
{
    b[0] = 0;
    switch (kp) {
    case KP_TYPE: text_cat(b, TYPE_NAME[clamp(v, 0, KT_N - 1)]); break;
    case KP_TUNE: note_text(v, b); break;
    case KP_DEC: time_text(0.02f * ex2((float)v, 8.2288f), b); break;
    case KP_BEND: sint_text(v * 60 / 127, b); text_cat(b, "ST"); break;
    case KP_TAIL: sint_text(-(v * 24 / 127), b); text_cat(b, "ST"); break;
    case KP_TIME: time_text(0.001f * ex2((float)v, 8.6439f), b); break;
    case KP_TTIME: time_text(0.01f * ex2((float)v, 8.2288f), b); break;
    case KP_GLIDE: time_text(0.001f + 0.5f * ((float)v / 127.0f) * ((float)v / 127.0f), b); break;
    case KP_ATK: time_text(0.05f * ((float)v / 127.0f) * ((float)v / 127.0f), b); break;
    case KP_HOLD: time_text(0.3f * ((float)v / 127.0f) * ((float)v / 127.0f), b); break;
    case KP_DROP: case KP_FENV: sint_text(v - 64, b); break;
    case KP_CTYPE: text_cat(b, CK_NAME[clamp(v, 0, CK_N - 1)]); break;
    case KP_CTONE: hz_text(300.0f * ex2((float)v, 5.64f), b); break;
    case KP_CLEN: time_text(0.0002f * ex2((float)v, 7.23f), b); break;
    case KP_SINT: text_cat(b, SINT_NAME[clamp(v, 0, 3)]); break;
    case KP_SDEC: {
        int x = (int)(25.0f * ex2((float)v, 4.0f) + 0.5f);   /* x0.25 .. x4 of the body's decay, in hundredths */
        text_cat(b, "X");
        cat_int(b, x / 100);
        text_cat(b, ".");
        cat_int(b, x / 10 % 10);
        cat_int(b, x % 10);
        break;
    }
    case KP_NTONE: hz_text(100.0f * ex2((float)v, 7.0f), b); break;
    case KP_NDEC: time_text(0.005f * ex2((float)v, 7.64f), b); break;
    case KP_DTYPE: text_cat(b, DT_NAME[clamp(v, 0, DT_N - 1)]); break;
    case KP_FTYPE: text_cat(b, FT_NAME[clamp(v, 0, FT_N - 1)]); break;
    case KP_CUT: hz_text(20.0f * ex2((float)v, 10.0f), b); break;
    case KP_EQLO: case KP_EQMID: case KP_EQHI: sint_text((v - 64) * 12 / 64, b); text_cat(b, "DB"); break;
    case KP_EQFRQ: hz_text(60.0f * ex2((float)v, 6.7f), b); break;
    case KP_CA:
        if (type == KT_FM) {
            text_cat(b, FM_RATIO_NAME[clamp((int)((float)v / 127.0f * 11.0f + 0.5f), 0, 11)]);
            break;
        }
        if (type == KT_WSHAPER) {        /* the curve nearest */
            text_cat(b, WS_NAME[clamp((int)((float)v / 127.0f * (float)(WS_N - 1) + 0.5f), 0, WS_N - 1)]);
            break;
        }
        int_text(v, b);
        break;
    case KP_CC:
        if (type == KT_FM) {
            time_text(0.001f * ex2((float)v, 8.97f), b);
            break;
        }
        if (type == KT_WSHAPER) {
            sint_text(v - 64, b);
            break;
        }
        int_text(v, b);
        break;
    case KP_CD:
        if (type == KT_WSHAPER) {        /* the source: a sine, a triangle, between */
            if (v == 0)
                text_cat(b, "SINE");
            else if (v == 127)
                text_cat(b, "TRI");
            else {
                text_cat(b, "TRI ");
                cat_int(b, v * 100 / 127);
                text_cat(b, "%");
            }
            break;
        }
        int_text(v, b);
        break;
    default: int_text(v, b); break;
    }
}

static void fx_text(int fx, int k, int v, char *b)
{
    b[0] = 0;
    if (FX_P[fx][k][0] == '-') {
        text_cat(b, "-");
        return;
    }
    switch (fx * 4 + k) {
    case FX_COMP * 4 + 0: sint_text(-40 + v * 40 / 127, b); text_cat(b, "DB"); break;
    case FX_COMP * 4 + 1: text_cat(b, "X"); cat_int(b, 1 + 19 * v * v / (127 * 127)); break;
    case FX_COMP * 4 + 2: time_text((0.1f + 30.0f * ((float)v / 127.0f) * ((float)v / 127.0f)) * 0.001f, b); break;
    case FX_COMP * 4 + 3: time_text((20.0f + 780.0f * (float)v / 127.0f) * 0.001f, b); break;
    case FX_DIST * 4 + 1: hz_text(500.0f * ex2((float)v, 5.3f), b); break;
    case FX_EQ * 4 + 0: case FX_EQ * 4 + 1: case FX_EQ * 4 + 3: sint_text((v - 64) * 12 / 64, b); text_cat(b, "DB"); break;
    case FX_EQ * 4 + 2: hz_text(60.0f * ex2((float)v, 6.7f), b); break;
    case FX_CRUSH * 4 + 0: cat_int(b, 1 + v * 14 / 127); text_cat(b, " BIT"); break;
    case FX_CRUSH * 4 + 1: text_cat(b, "/"); cat_int(b, 1 + 31 * v * v / (127 * 127)); break;
    case FX_FILT * 4 + 0:
        if (v >= 63 && v <= 65)
            text_cat(b, "OPEN");
        else if (v > 64) {
            text_cat(b, "HP ");
            hz_text(20.0f * fm_exp2f((float)(v - 64) / 64.0f * 9.5f), b + 3);
        } else {
            text_cat(b, "LP ");
            hz_text(20000.0f * fm_exp2f((float)(v - 64) / 64.0f * 9.5f), b + 3);
        }
        break;
    case FX_TRANS * 4 + 0: case FX_TRANS * 4 + 1: sint_text(v - 64, b); break;
    case FX_DELAY * 4 + 0: text_cat(b, FX_DIV_NAME[fx_idx((uint8_t)v, FX_NDIV)]); break;
    case FX_RING * 4 + 0: hz_text(20.0f * ex2((float)v, 7.0f), b); break;
    case FX_STUTTER * 4 + 0: text_cat(b, FX_SLICE_NAME[fx_idx((uint8_t)v, FX_NSLICE)]); break;
    case FX_STUTTER * 4 + 1: text_cat(b, FX_REP_NAME[fx_idx((uint8_t)v, FX_NREP)]); break;
    case FX_GATE * 4 + 0: text_cat(b, FX_GATE_NAME[fx_idx((uint8_t)v, FX_NGATE)]); break;
    case FX_WIDTH * 4 + 1: hz_text(40.0f + 260.0f * (float)v / 127.0f, b); break;
    case FX_LIMIT * 4 + 0: sint_text(-12 + v * 12 / 127, b); text_cat(b, "DB"); break;
    case FX_LIMIT * 4 + 1: time_text((20.0f + 480.0f * (float)v / 127.0f) * 0.001f, b); break;
    default: pct_text(v, b); break;
    }
}

/* the footer's knobs on each page: name, text, position (0..1000, -1 unused), bipolar, a step's (hot), a lock */
typedef struct {
    char name[12], val[16];
    int pos;
    uint8_t bip, hot;
} knob_t;

static const int8_t HOME_KP[4] = {KP_TYPE, KP_TUNE, KP_BEND, KP_DEC};
static int knob_kp(int i)                /* the sound parameter knob i turns on this page, -1 none */
{
    if (ui.mode == M_HOME)
        return HOME_KP[i];
    if (ui.mode == M_SND && ui.page[M_SND] < PG_STORE)
        return ui.page[M_SND] * 4 + i;
    return -1;
}
static int step_of_key(int w) { return ui.bank * 16 + w; }

static void knob_snd(knob_t *k, int kp, const float *snd)
{
    int v = (int)(snd[kp] + 0.5f), lo = kp_lo(kp), hi = kp_hi(kp), type = (int)(snd[KP_TYPE] + 0.5f);
    text_cat(k->name, pname(kp, type));
    param_text(kp, v, type, k->val);
    k->pos = (v - lo) * 1000 / (hi - lo);
    k->bip = kp == KP_DROP || kp == KP_FENV || kp == KP_EQLO || kp == KP_EQMID || kp == KP_EQHI ||
             (kp == KP_CC && type == KT_WSHAPER);
}

static void algo_text(const pat_t *p, int i, char *b)
{
    int v = p->ga[i];
    b[0] = 0;
    switch (p->algo * 4 + i) {
    case AL_EUCL * 4 + 0: cat_int(b, 1 + v * (p->len - 1) / 127); break;
    case AL_EUCL * 4 + 1: case AL_FRACT * 4 + 1: case AL_BITS * 4 + 1: case AL_BOUNCE * 4 + 3:
        cat_int(b, v * p->len / 128);
        break;
    case AL_POLY * 4 + 0: case AL_POLY * 4 + 1: cat_int(b, 1 + v * 7 / 127); break;
    case AL_POLY * 4 + 2: cat_int(b, v * p->len / 128); break;
    case AL_BOUNCE * 4 + 0: cat_int(b, 2 + v * 6 / 127); break;
    case AL_MIRROR * 4 + 1: { static const char *const U[4] = {"2", "4", "8", "16"}; text_cat(b, U[v / 32]); break; }
    case AL_FRACT * 4 + 0: text_cat(b, FRACT_NAME[v * 3 / 128]); break;
    case AL_BITS * 4 + 0: {
        uint32_t x = (uint32_t)v * 2u + 1u, j;
        for (j = 0; j < 8u; j++)
            b[j] = (x >> j & 1u) ? '1' : '0';
        b[8] = 0;
        break;
    }
    case AL_ROLLS * 4 + 0: text_cat(b, ROLL_BASE[v / 32]); break;
    case AL_ROLLS * 4 + 1: cat_int(b, 1 + v * p->len / 128); break;
    case AL_ROLLS * 4 + 2: cat_int(b, 1 + v * 7 / 127); break;
    case AL_ROLLS * 4 + 3: text_cat(b, "X"); cat_int(b, 2 + v * 2 / 127); break;
    case AL_METRIC * 4 + 0: case AL_METRIC * 4 + 3: case AL_SYNC * 4 + 0: case AL_SYNC * 4 + 1: case AL_SYNC * 4 + 3:
    case AL_MIRROR * 4 + 0:
        pct_text(v, b);
        break;
    default:
        if (i == 2 && p->algo != AL_POLY && p->algo != AL_ROLLS)
            text_cat(b, ACC_MODE[v / 32]);
        else
            cat_int(b, v);
        break;
    }
}

static void knob_info(int i, knob_t *k)
{
    pat_t *p = P();
    int pg = ui.page[ui.mode], hs = ui.held;
    float snd[KP_N];
    k->name[0] = k->val[0] = 0;
    k->pos = -1;
    k->bip = 0;
    k->hot = 0;
    if (hs >= 0 && ui.mode != M_PAT && ui.mode != M_FX && !(ui.mode == M_SND && pg == PG_STORE)) {
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
        if (knob_kp(i) >= 0) {           /* the step's p-lock of this knob's parameter */
            const lock_t *l = &p->lk[hs];
            int kp = knob_kp(i);
            step_sound(p, l, snd);
            if (lock_find(l, kp) >= 0)
                text_cat(k->name, "*");
            knob_snd(k, kp, snd);
            k->hot = lock_find(l, kp) >= 0;
            return;
        }
        {
            step_t *s = &p->st[hs];
            k->hot = 1;
            switch (i) {
            case 0: text_cat(k->name, "PITCH"); sint_text(s->pit, k->val); k->pos = (s->pit + 24) * 1000 / 48; k->bip = 1; break;
            case 1: text_cat(k->name, "RATCHET"); text_cat(k->val, "X"); cat_int(k->val, s->rat + 1); k->pos = s->rat * 1000 / 3; break;
            case 2: text_cat(k->name, "SLIDE"); text_cat(k->val, s->fl & FL_SLIDE ? "ON" : "OFF"); k->pos = s->fl & FL_SLIDE ? 1000 : 0; break;
            case 3: text_cat(k->name, "SWEEP"); text_cat(k->val, s->fl & FL_REV ? "REV" : "NORM"); k->pos = s->fl & FL_REV ? 1000 : 0; break;
            }
            return;
        }
    }
    switch (ui.mode) {
    case M_HOME:
    case M_SND:
        if (ui.mode == M_SND && pg == PG_STORE) {
            switch (i) {
            case 0: text_cat(k->name, "SLOT"); text_cat(k->val, "U"); cat_int(k->val, ui.slot + 1); k->pos = ui.slot * 1000 / (NUSER - 1); break;
            case 1: text_cat(k->name, "CURSOR"); cat_int(k->val, ui.cursor + 1); k->pos = ui.cursor * 1000 / 7; break;
            case 2: text_cat(k->name, "LETTER"); k->val[0] = ui.name[ui.cursor] == ' ' ? '_' : ui.name[ui.cursor]; k->val[1] = 0; k->pos = 0; break;
            case 3: text_cat(k->name, "LOAD<>SAVE"); text_cat(k->val, "<  >"); k->pos = 500; k->bip = 1; break;
            }
            break;
        }
        step_sound(p, 0, snd);
        knob_snd(k, knob_kp(i), snd);
        break;
    case M_SEQ:
        if (pg == 0) {
            switch (i) {
            case 0: text_cat(k->name, "LENGTH"); cat_int(k->val, p->len); k->pos = (p->len - 1) * 1000 / (NST - 1); break;
            case 1: text_cat(k->name, "DIVISION"); text_cat(k->val, DIV_NAME[p->div]); k->pos = p->div * 1000 / (NDIV - 1); break;
            case 2: text_cat(k->name, "DIRECTION"); text_cat(k->val, DIR_NAME[p->dir]); k->pos = p->dir * 1000 / (NDIR - 1); break;
            case 3: text_cat(k->name, "SWING"); cat_int(k->val, p->swing); text_cat(k->val, "%"); k->pos = p->swing * 10; break;
            }
        } else {
            switch (i) {
            case 0: text_cat(k->name, "TEMPO"); dec1_text(set.bpm10, k->val); k->pos = (set.bpm10 - 400) * 1000 / 2000; break;
            case 1: text_cat(k->name, "PROB"); cat_int(k->val, p->prob); text_cat(k->val, "%"); k->pos = p->prob * 10; break;
            case 2: text_cat(k->name, "HUMANIZE"); cat_int(k->val, p->human); text_cat(k->val, "%"); k->pos = p->human * 10; break;
            case 3: text_cat(k->name, "FINE BPM"); dec1_text(set.bpm10, k->val); k->pos = (set.bpm10 % 10) * 100; break;
            }
        }
        break;
    case M_GEN:
        text_cat(k->name, ALGO_P[p->algo][i]);
        if (p->algo == AL_MAN) {
            text_cat(k->val, "-");
            break;
        }
        k->pos = p->ga[i] * 1000 / 127;
        algo_text(p, i, k->val);
        break;
    case M_MOD: {
        mod_t *m = &p->mod[pg / 2];
        if (pg % 2 == 0) {
            switch (i) {
            case 0:
                text_cat(k->name, "TARGET");
                text_cat(k->val, m->tgt ? pname(m->tgt - 1, p->snd[KP_TYPE]) : "OFF");
                k->pos = m->tgt * 1000 / KP_N;
                break;
            case 1: text_cat(k->name, "DEPTH"); sint_text(m->depth, k->val); text_cat(k->val, "%"); k->pos = (m->depth + 100) * 5; k->bip = 1; break;
            case 2: text_cat(k->name, "SHAPE"); text_cat(k->val, SHAPE_NAME[m->shape]); k->pos = m->shape * 1000 / (NSHAPE - 1); break;
            case 3: text_cat(k->name, "LENGTH"); cat_int(k->val, m->len); k->pos = (m->len - 1) * 1000 / (NST - 1); break;
            }
        } else {
            switch (i) {
            case 0: text_cat(k->name, "DIVISION"); text_cat(k->val, DIV_NAME[m->div]); k->pos = m->div * 1000 / (NDIV - 1); break;
            case 1: text_cat(k->name, "DIRECTION"); text_cat(k->val, DIR_NAME[m->dir]); k->pos = m->dir * 1000 / (NDIR - 1); break;
            case 2: text_cat(k->name, "SMOOTH"); cat_int(k->val, m->smooth); k->pos = m->smooth * 1000 / 127; break;
            case 3: text_cat(k->name, "SEED"); cat_int(k->val, m->seed); k->pos = m->seed * 1000 / 127; break;
            }
        }
        break;
    }
    case M_FX: {
        int f = ui.fxsel[pg];
        text_cat(k->name, FX_P[f][i]);
        fx_text(f, i, set.fx.p[f][i], k->val);
        if (FX_P[f][i][0] != '-') {
            k->pos = set.fx.p[f][i] * 1000 / 127;
            k->bip = (f == FX_FILT && i == 0) || (f == FX_TRANS && i < 2) || (f == FX_EQ && i != 2);
        }
        break;
    }
    case M_PAT:
        if (pg == 0) {
            switch (i) {
            case 0: text_cat(k->name, "PATTERN"); text_cat(k->val, "P"); cat_int(k->val, sq.cur + 1); k->pos = sq.cur * 1000 / 15; break;
            case 1: text_cat(k->name, "ACTION"); text_cat(k->val, ACT_NAME[ui.act]); k->pos = ui.act * 1000 / (NACT - 1); break;
            case 2: text_cat(k->name, "-"); text_cat(k->val, "-"); break;
            case 3: text_cat(k->name, "DO IT"); text_cat(k->val, ">>"); k->pos = 0; break;
            }
        } else if (pg == 1) {
            switch (i) {
            case 0: text_cat(k->name, "IN CH"); if (set.mch) cat_int(k->val, set.mch); else text_cat(k->val, "ALL"); k->pos = set.mch * 1000 / 16; break;
            case 1: text_cat(k->name, "CLOCK"); text_cat(k->val, set.clk ? "MIDI" : "INT"); k->pos = set.clk * 1000; break;
            case 2: text_cat(k->name, "CLOCK OUT"); text_cat(k->val, set.clkout ? "ON" : "OFF"); k->pos = set.clkout * 1000; break;
            case 3: text_cat(k->name, "NOTES OUT"); text_cat(k->val, set.notes_out ? "ON" : "OFF"); k->pos = set.notes_out * 1000; break;
            }
        } else {                         /* THEMES */
            switch (i) {
            case 0: text_cat(k->name, "THEME"); text_cat(k->val, THEMES[set.theme].name); k->pos = set.theme * 1000 / (NTHEME - 1); break;
            case 1: text_cat(k->name, "KNOBS"); text_cat(k->val, set.dial ? "BARS" : "DIALS"); k->pos = set.dial * 1000; break;
            case 2: text_cat(k->name, "SCOPE"); text_cat(k->val, set.scope ? "LINE" : "FILLED"); k->pos = set.scope * 1000; break;
            case 3: text_cat(k->name, "STEPS"); text_cat(k->val, set.steps ? "GRID" : "RING"); k->pos = set.steps * 1000; break;
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
    case M_SEQ: text_cat(b, pg == 0 ? "TRIG" : "FEEL"); break;
    case M_GEN: text_cat(b, "GENERATE"); break;
    case M_SND: text_cat(b, pg == PG_STORE ? "STORE" : SND_PAGE[pg]); break;
    case M_MOD: text_cat(b, "LFO "); cat_int(b, pg / 2 + 1); text_cat(b, pg % 2 ? " B" : " A"); break;
    case M_FX: text_cat(b, pg ? "FINAL " : "LIVE "); text_cat(b, FX_SHORT[ui.fxsel[pg]]); break;
    case M_PAT: text_cat(b, pg == 0 ? "PATTERNS" : pg == 1 ? "MIDI" : "THEMES"); break;
    }
    return b;
}

static int step_val(int v, int d, int lo, int hi) { return clamp(v + d, lo, hi); }
static int fast(void) { return (ui.btn >> B_OCTDN) & 1u; }
static void pat_pick(int n);
static void pat_action(void);
static void store_action(int d);

static void name_from_slot(void)
{
    uint32_t k;
    if (us.u[ui.slot].used) {
        for (k = 0; k < 8u; k++)
            ui.name[k] = us.u[ui.slot].name[k] ? us.u[ui.slot].name[k] : ' ';
    } else {
        const char *d = "USER    ";
        for (k = 0; k < 8u; k++)
            ui.name[k] = d[k];
        ui.name[5] = (char)('0' + (ui.slot + 1) / 10);
        ui.name[6] = (char)('0' + (ui.slot + 1) % 10);
    }
    ui.name[8] = 0;
}

static void knob_turn(int i, int d)
{
    pat_t *p = P();
    int pg = ui.page[ui.mode], hs = ui.held, d8 = fast() ? d * 8 : d;
    ui.pop = (int8_t)i;
    ui.pop_until = plat_ms() + 1100u;
    ui.redraw_c = 1;
    if (hs >= 0 && ui.mode != M_PAT && ui.mode != M_FX && !(ui.mode == M_SND && pg == PG_STORE)) {
        ui.held_turned = 1;
        if (ui.mode == M_MOD) {
            mod_t *m = &p->mod[pg / 2];
            m->val[hs] = (int8_t)step_val(m->val[hs], d8, -64, 63);
        } else if (knob_kp(i) >= 0) {     /* a p-lock, from what the step plays now */
            lock_t *l = &p->lk[hs];
            int kp = knob_kp(i), dd = kp_hi(kp) - kp_lo(kp) > 10 ? d8 : d;
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
        int kp;
        if (ui.mode == M_SND && pg == PG_STORE) {
            switch (i) {
            case 0: ui.slot = (uint8_t)step_val(ui.slot, d, 0, NUSER - 1); name_from_slot(); break;
            case 1: ui.cursor = (uint8_t)step_val(ui.cursor, d, 0, 7); break;
            case 2: {
                static const char CH[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-";
                int n = (int)sizeof CH - 1, c = 0, j;
                for (j = 0; j < n; j++)
                    if (CH[j] == ui.name[ui.cursor])
                        c = j;
                ui.name[ui.cursor] = CH[(c + d % n + n) % n];
                break;
            }
            case 3: store_action(d); break;
            }
            ui.redraw_c = ui.redraw_f = 1;
            return;
        }
        kp = knob_kp(i);
        p->snd[kp] = (uint8_t)step_val(p->snd[kp], kp_hi(kp) - kp_lo(kp) > 10 ? d8 : d, kp_lo(kp), kp_hi(kp));
        changed();
        break;
    }
    case M_SEQ:
        if (pg == 0) {
            switch (i) {
            case 0:
                p->len = (uint8_t)step_val(p->len, d, 1, NST);
                if (p->len <= 16u)
                    ui.bank = 0;
                break;
            case 1: p->div = (uint8_t)step_val(p->div, d, 0, NDIV - 1); break;
            case 2: p->dir = (uint8_t)step_val(p->dir, d, 0, NDIR - 1); break;
            case 3: p->swing = (uint8_t)step_val(p->swing, d8, 0, 100); break;
            }
            sq.gen_dirty = 1;
            if (!sq.running && p->algo != AL_MAN)
                gen_run(p, 0);
            changed();
        } else {
            switch (i) {
            case 0: set.bpm10 = (uint16_t)step_val(set.bpm10, d8 * 10, 400, 2400); set_changed(); break;
            case 1: p->prob = (uint8_t)step_val(p->prob, d8, 0, 100); changed(); break;
            case 2: p->human = (uint8_t)step_val(p->human, d8, 0, 100); changed(); break;
            case 3: set.bpm10 = (uint16_t)step_val(set.bpm10, d, 400, 2400); set_changed(); break;
            }
        }
        break;
    case M_GEN:
        if (p->algo == AL_MAN)
            return;
        p->ga[i] = (uint8_t)step_val(p->ga[i], d8, 0, 127);
        sq.gen_dirty = 1;
        if (!sq.running)
            gen_run(p, 0);
        changed();
        break;
    case M_MOD: {
        mod_t *m = &p->mod[pg / 2];
        if (pg % 2 == 0) {
            switch (i) {
            case 0: m->tgt = (uint8_t)step_val(m->tgt, d, 0, KP_N); break;
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
    case M_FX: {
        int f = ui.fxsel[pg];
        if (FX_P[f][i][0] == '-')
            return;
        set.fx.p[f][i] = (uint8_t)step_val(set.fx.p[f][i], d8, 0, 127);
        set_changed();
        break;
    }
    case M_PAT:
        if (pg == 0) {
            if (i == 0) {
                pat_pick(step_val(sq.next != 0xFFu ? sq.next : sq.cur, d, 0, NPAT - 1));
            } else if (i == 1) {
                ui.act = (uint8_t)step_val(ui.act, d, 0, NACT - 1);
                ui.redraw_f = 1;
            } else if (i == 3 && d > 0) {
                pat_action();
            }
        } else if (pg == 1) {
            switch (i) {
            case 0: set.mch = (uint8_t)step_val(set.mch, d, 0, 16); break;
            case 1: set.clk = (uint8_t)step_val(set.clk, d, 0, 1); sq.ext_per = 0.0f; sq.ext_bpm10 = 0; break;
            case 2: set.clkout = (uint8_t)step_val(set.clkout, d, 0, 1); break;
            case 3: set.notes_out = (uint8_t)step_val(set.notes_out, d, 0, 1); break;
            }
            set_changed();
        } else {                         /* THEMES: a theme brings its own design; K2..K4 change it */
            switch (i) {
            case 0:
                set.theme = (uint8_t)step_val(set.theme, d, 0, NTHEME - 1);
                set.dial = THEMES[set.theme].dial;
                set.scope = THEMES[set.theme].scope;
                set.steps = THEMES[set.theme].steps;
                break;
            case 1: set.dial = (uint8_t)step_val(set.dial, d, 0, 1); break;
            case 2: set.scope = (uint8_t)step_val(set.scope, d, 0, 1); break;
            case 3: set.steps = (uint8_t)step_val(set.steps, d, 0, 1); break;
            }
            theme_apply();
            lcd_fill(0, 0, 240, 240, K_BG);
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
        gen_run(P(), 0);
    }
    if (pats(n).len <= 16u)
        ui.bank = 0;
    set.cur = (uint8_t)n;
    set_changed();
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
    case A_CLEAR: {
        uint8_t snd[KP_N], pre = p->preset;
        uint32_t k;
        for (k = 0; k < KP_N; k++)
            snd[k] = p->snd[k];
        pat_clear(p);
        for (k = 0; k < KP_N; k++)
            p->snd[k] = snd[k];
        p->preset = pre;
        app_say("CLEARED", "");
        break;
    }
    case A_INIT:
        pat_sound(p, 0);
        app_say("KICK", "INIT");
        break;
    case A_RAND:
        rnd_sound(p, -1);
        rnd_steps(p);
        if (rr(&rng_ui, 0, 4))
            rnd_gen(p);
        else
            p->algo = AL_MAN;
        rnd_feel(p);
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

/* STORE: K4 to the right saves the pattern's kick in the slot, under the name; to the left loads the slot */
static void store_action(int d)
{
    pat_t *p = P();
    upre_t *u = &us.u[ui.slot];
    uint32_t k;
    char t[12];
    if (d > 0) {
        u->used = 1;
        for (k = 0; k < 8u; k++)
            u->name[k] = ui.name[k];
        for (k = 0; k < KP_N; k++)
            u->snd[k] = p->snd[k];
        p->preset = (uint8_t)(NFAC + ui.slot);
        ui.user_dirty = 1;
        ui.last_change = plat_ms();
        pre_name(NFAC + ui.slot, t);
        app_say("SAVED", t);
        changed();
    } else {
        if (!u->used) {
            app_say("SLOT", "EMPTY");
            return;
        }
        pat_sound(p, NFAC + ui.slot);
        pre_name(NFAC + ui.slot, t);
        app_say("LOADED", t);
        changed();
    }
}

/* OCT- + GLO: the kick's parameters and the trigs at random (with p-locks); never the tempo, the LFOs or the FX.
 * OCT+ + GLO: the same, and the four LFOs too. */
static void random_all(int lfos)
{
    pat_t *p = P();
    rnd_sound(p, -1);
    rnd_steps(p);
    if (rr(&rng_ui, 0, 9) < 6)
        rnd_gen(p);
    else
        p->algo = AL_MAN;
    rnd_feel(p);
    gen_run(p, 0);
    rnd_locks(p);
    if (lfos) {
        int m;
        for (m = 0; m < NMOD; m++) {
            mod_init(&p->mod[m]);
            if (rr(&rng_ui, 0, 3))
                rnd_mod(&p->mod[m]);
        }
    }
    sq.gen_dirty = 1;
    app_say("RANDOM", lfos ? "ALL + LFO" : "KICK + TRIGS");
    changed();
}

static void go_mode(int m, int page)
{
    if (ui.mode == m && page < 0)
        ui.page[m] = (uint8_t)((ui.page[m] + 1u) % NPAGE[m]);
    else if (page >= 0)
        ui.page[m] = (uint8_t)page;
    if (m != M_FX)
        fx_live = 0;
    if (m != ui.mode)
        ui.pop = -1;                     /* the big value belongs to the page it was turned on */
    ui.mode = (uint8_t)m;
    ui.held = -1;
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
        if (p->algo != AL_MAN) {         /* the steps of a generator: printed into the steps first */
            gen_print(p);
            app_say("GENERATOR", "TO STEPS");
        }
        p->st[s].trig = (uint8_t)((p->st[s].trig + 1u) % 3u);
    }
    changed();
}

static void save_now(void)
{
    int ok = 1;
    uint32_t i;
    for (i = 0; i < 4u; i++)
        ok &= plat_store_save(PAT_OBJ[i], &pk[i], sizeof pk[i]) == 0;
    ok &= plat_store_save(OBJ_USER, &us, sizeof us) == 0;
    ok &= plat_store_save(OBJ_SET, &set, sizeof set) == 0;
    if (ok) {
        ui.pat_dirty = 0;
        ui.set_dirty = ui.user_dirty = 0;
        app_say("SAVED", "");
    } else {
        app_say("SAVE", "FAILED");
    }
}

static void audition(int semis, int vel, int step)
{
    sq.aud_semis = (int8_t)semis;
    sq.aud_vel = (uint8_t)vel;
    sq.aud_step = (uint8_t)(step < 0 ? 0xFF : step);
    sq.aud_req = 1;
}

static void input(void)
{
    uint32_t keys = plat_keys(), btn = plat_buttons(), k;
    for (k = 0; k < NKEYS; k++) {
        uint32_t bit = 1u << k;
        int w = white_index(k), b = black_index(k);
        if ((keys & bit) && !(ui.keys & bit)) {
            if (rec_on) {                /* REC: the whole keyboard, chromatic (C4 is the kick's own note) */
                int vel = (btn >> B_OCTUP & 1u) ? 127 : 100;
                audition((int)k - 7, vel, -1);
                rec_note((int)k - 7, vel);
            } else if (w >= 0 && ui.mode == M_PAT && ui.page[M_PAT] == 0) {
                pat_pick(w);
            } else if (w >= 0 && ui.mode == M_FX && ui.page[M_FX] == 0) {   /* LIVE: on while the key is held */
                if (w < FX_NLIVE) {
                    fx_live |= 1u << w;
                    ui.fxsel[0] = (uint8_t)w;
                    ui.redraw_h = ui.redraw_c = ui.redraw_f = 1;
                }
            } else if (w >= 0 && ui.mode == M_FX) {   /* FINAL: a white key turns its effect on or off */
                if (w < FX_NFINAL) {
                    int f = FX_NLIVE + w;
                    set.fx.on ^= 1u << f;
                    ui.fxsel[1] = (uint8_t)f;
                    app_say(FX_NAME[f], set.fx.on & 1u << f ? "ON" : "OFF");
                    set_changed();
                }
            } else if (w >= 0 && !(ui.mode == M_SND && ui.page[M_SND] == PG_STORE)) {
                int s = step_of_key(w);
                ui.held = (int8_t)s;
                ui.held_turned = 0;
                rec_sel = (uint8_t)s;
                ui.redraw_c = ui.redraw_f = 1;
                if (!sq.running && ui.mode != M_MOD)   /* stopped: the step sounds as it is touched */
                    audition(P()->st[s].pit, 110, s);
            } else if (b >= 0) {         /* black keys: the kick, a semitone apart (OCT+ held: accented) */
                audition(b, (btn >> B_OCTUP & 1u) ? 127 : 100, -1);
            }
        } else if (!(keys & bit) && (ui.keys & bit)) {
            if (w >= 0 && w < FX_NLIVE && (fx_live & 1u << w)) {   /* a LIVE effect let go */
                fx_live &= ~(1u << w);
                ui.redraw_c = 1;
            }
            if (w >= 0 && ui.held == step_of_key(w)) {
                if (!ui.held_turned && !rec_on)
                    step_tap(ui.held);
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
        case B_SEQ: go_mode(M_SEQ, ui.mode == M_SEQ ? -1 : ui.page[M_SEQ]); break;
        case B_ARP: go_mode(M_GEN, 0); break;
        case B_EDIT: go_mode(M_SND, ui.mode == M_SND ? -1 : ui.page[M_SND]); break;
        case B_ENV: go_mode(M_SND, 4); break;
        case B_FX: go_mode(M_FX, ui.mode == M_FX ? -1 : ui.page[M_FX]); break;
        case B_LFO: go_mode(M_MOD, ui.mode == M_MOD ? (ui.page[M_MOD] / 2 * 2 + 2) % (2 * NMOD) : ui.page[M_MOD]); break;
        case B_GLO:
            if (btn >> B_OCTDN & 1u)      /* OCT- + GLO: the kick and the trigs at random */
                random_all(0);
            else if (btn >> B_OCTUP & 1u) /* OCT+ + GLO: and the LFOs */
                random_all(1);
            else
                go_mode(M_PAT, ui.mode == M_PAT ? -1 : ui.page[M_PAT]);
            break;
        case B_SEL:                      /* the white keys: steps 1-16 or 17-32 */
            if (P()->len > 16u) {
                ui.bank ^= 1u;
                app_say("STEPS", ui.bank ? "17-32" : "1-16");
            } else {
                ui.bank = 0;
                app_say("STEPS", "1-16 ONLY");
            }
            ui.redraw_c = 1;
            break;
        case B_PLAY:
            if (set.clk)
                app_say("CLOCK", "IS MIDI");
            else if (sq.running)
                seq_stop();
            else
                seq_start();
            ui.redraw_h = ui.redraw_c = 1;
            break;
        case B_REC:
            if (btn >> B_OCTUP & 1u) {    /* OCT+ + REC: the metronome */
                set.metro = (uint8_t)!set.metro;
                app_say("METRONOME", set.metro ? "ON" : "OFF");
                set_changed();
            } else if (ui.held >= 0 && ui.mode != M_PAT && ui.mode != M_MOD && ui.mode != M_FX) {
                char t[12] = "STEP ";
                lock_clear(&P()->lk[ui.held]);
                ui.held_turned = 1;
                cat_int(t, ui.held + 1);
                app_say(t, "LOCKS CLEARED");
                changed();
            } else {                     /* REC on or off: notes from the keyboard or MIDI go into the steps */
                rec_on = (uint8_t)!rec_on;
                ui.held = -1;
                if (rec_on && !sq.running)
                    rec_sel = 0;
                app_say("REC", rec_on ? "ON: PLAY NOTES" : "OFF");
            }
            ui.redraw_h = ui.redraw_c = ui.redraw_f = 1;
            break;
        case B_SAVE:
            if (btn >> B_OCTDN & 1u) {
                if (P()->algo == AL_MAN) {
                    app_say("PRINT", "NO GENERATOR");
                } else {
                    gen_print(P());
                    app_say("PRINTED", "TO STEPS");
                    changed();
                }
            } else {
                save_now();
            }
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
            go_mode(ui.mode, clamp(ui.page[ui.mode] + e, 0, NPAGE[ui.mode] - 1));
            if (ui.mode == M_SND && ui.page[M_SND] == PG_STORE)
                name_from_slot();
        } else if (k == EN_ALGO) {
            if (ui.mode == M_GEN) {
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
        } else if (k == EN_PRESET) {
            pat_t *p = P();
            char t[12];
            int d = e > 0 ? 1 : -1, n, j, cnt = e > 0 ? (int)e : -(int)e;
            if (ui.held >= 0 && ui.mode != M_PAT && ui.mode != M_MOD && ui.mode != M_FX) {
                lock_t *l = &p->lk[ui.held];   /* a step held: the kick it plays (a p-lock), or the pattern's */
                for (j = 0; j < cnt; j++) {     /* the pattern's kick sits between the last preset and the first */
                    n = l->kit ? l->kit - 1 : (d > 0 ? NPRE - 1 : 0);
                    n = l->kit && ((d > 0 && pre_step(n, 1) <= n) || (d < 0 && pre_step(n, -1) >= n)) ? -1 : pre_step(n, d);
                    l->kit = (uint8_t)(n < 0 ? 0 : n + 1);
                }
                ui.held_turned = 1;
                if (!p->st[ui.held].trig)
                    p->st[ui.held].trig = 1;
                if (l->kit)
                    pre_name(l->kit - 1, t);
                app_say("STEP KICK", l->kit ? t : "PATTERN'S");
                if (!sq.running)
                    audition(p->st[ui.held].pit, 110, ui.held);
            } else {
                n = p->preset;
                for (j = 0; j < cnt; j++)
                    n = pre_step(n, d);
                pat_sound(p, n);
                pre_name(n, t);
                app_say("KICK", t);
                if (!sq.running)
                    audition(0, 110, -1);
            }
            changed();
        } else {
            knob_turn((int)(k - EN_K1), e);
        }
    }
    if (keys != ui.keys || btn != ui.btn)
        ui.redraw_f = 1;
    ui.keys = keys;
    ui.btn = btn;
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
        cv_pset(cx + (int32_t)(fm_sin_turns(a + 0.25f) * (float)r + 0.5f), cy + (int32_t)(fm_sin_turns(a) * (float)r + 0.5f), c);
    }
}
static void text_c(int32_t cx, int32_t y, const felucca_font_t *f, const char *s, uint16_t c)
{
    cv_text(cx - text_w(f, s) / 2, y, f, s, c);
}

static void cell(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t fill, uint16_t edge, int thick)
{
    cv_rect(x, y, w, h, fill);
    cv_rect(x, y, w, 1, edge);
    cv_rect(x, y + h - 1, w, 1, edge);
    cv_rect(x, y, 1, h, edge);
    cv_rect(x + w - 1, y, 1, h, edge);
    if (thick) {
        cv_rect(x + 1, y + 1, w - 2, 1, edge);
        cv_rect(x + 1, y + h - 2, w - 2, 1, edge);
    }
}
static void draw_header(void)
{
    char t[24], b[24];
    uint32_t now = plat_ms();
    int32_t x0 = 78;
    cv_begin(240, 24, K_BG);
    if (sq.running) {
        int32_t i;
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
    if (rec_on) {                        /* REC: a red dot */
        fill_circle(84, 11, 5, K_REC);
        x0 = 94;
    }
    if (now < ui.msg_until) {
        int32_t x = cv_text(x0, 3, &FONT_B, ui.msg[0], rec_on ? K_REC : K_ACC);
        cv_text(x + 6, 3, &FONT_B, ui.msg[1], K_INK);
    } else {
        int32_t bpm = set.clk && sq.ext_bpm10 ? sq.ext_bpm10 : set.bpm10;
        cv_text(x0, 3, &FONT_B, page_title(t), K_ACC);
        dec1_text(bpm, b);
        cv_text(236 - text_w(&FONT_B, b), 3, &FONT_B, b, K_INK);
        if (set.metro) {                 /* the metronome: a pyramid, its arm swinging with the beat */
            int32_t mx = 236 - text_w(&FONT_B, b) - 16, j, sw = sq.running ? ((beat_now & 1u) ? 4 : -4) : 0;
            (void)j;
            cv_line(mx + 6, 4, mx, 17, K_LOCK);
            cv_line(mx + 6, 4, mx + 12, 17, K_LOCK);
            cv_rect(mx, 17, 13, 2, K_LOCK);
            cv_line(mx + 6, 15, mx + 6 + sw, 5, K_INK);
            cv_rect(mx + 5 + sw / 2, 9, 3, 2, K_INK);
        }
        if (set.clk)
            cv_text(236 - text_w(&FONT_B, b) - 6 - text_w(&FONT_XS, "MIDI"), 7, &FONT_XS, "MIDI", K_ACC);
    }
    cv_rect(0, 23, 240, 1, K_GRID);
    cv_blit(0, 0);
}

/* the scope: one hit of the kick (a step held: that step's), without the MOD lanes, drawn a part at a time */
static kick_t sk;
static kparm_t skp;
static struct {
    uint8_t busy;
    uint32_t done, total, per, col, ccount;
    int8_t mn[240], mx[240];
    float cmin, cmax;
} sj;

static int scope_step(void) { return ui.held >= 0 && (ui.mode == M_HOME || ui.mode == M_SND) ? ui.held : -1; }
static uint32_t snd_hash(void)
{
    const pat_t *p = P();
    uint32_t h = 2166136261u, k;
    int s = scope_step();
    for (k = 0; k < KP_N; k++)
        h = (h ^ p->snd[k]) * 16777619u;
    if (s >= 0) {
        const uint8_t *b = (const uint8_t *)&p->lk[s];
        h = (h ^ (uint32_t)(s + 1) * 0x9E37u) * 16777619u;
        for (k = 0; k < sizeof(lock_t); k++)
            h = (h ^ b[k]) * 16777619u;
    }
    return h ^ (uint32_t)(ui.mode == M_SND && ui.page[M_SND] == 5) * 0x55u;
}
static void scope_start(void)
{
    const pat_t *p = P();
    uint32_t k;
    int s = scope_step();
    float lo = 999.0f, hi = -999.0f, v[240];
    step_sound(p, s >= 0 ? &p->lk[s] : 0, skp.p);
    ui.szoom = ui.mode == M_SND && ui.page[M_SND] == 5;      /* CLICK: the first 40 ms */
    ui.sdur = ui.szoom ? 0.04f : kdsp_length(&skp) * 1.15f;
    if (ui.sdur < 0.12f && !ui.szoom)
        ui.sdur = 0.12f;
    if (ui.sdur > 1.6f)
        ui.sdur = 1.6f;
    kdsp_init(&sk, 0xABCDu);
    kdsp_hit(&sk, &skp, 0.0f, 1.0f, KH_ACC);
    sj.total = (uint32_t)(ui.sdur * 44100.0f);
    sj.per = sj.total / 240u;
    if (sj.per < 1u)
        sj.per = 1u;
    sj.total = sj.per * 240u;
    sj.done = sj.col = sj.ccount = 0;
    sj.cmin = 1e9f;
    sj.cmax = -1e9f;
    sj.busy = 1;
    for (k = 0; k < 240u; k++) {
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

static int trig_at(const pat_t *p, int i) { return p->algo == AL_MAN ? p->st[i].trig : gen_trig[i]; }

static void draw_strip(int32_t y)        /* the TRIG lane: the sixteen steps of the keys' bank */
{
    const pat_t *p = P();
    int i;
    if (p->len > 16u) {                  /* which half: a mark at the left */
        cv_rect(1, y + (ui.bank ? 7 : 0), 4, 5, K_INK);
        cv_rect(1, y + (ui.bank ? 0 : 7), 4, 5, K_GRID);
    }
    for (i = 0; i < 16; i++) {
        int s = ui.bank * 16 + i, tr = trig_at(p, s);
        int32_t x = 8 + i * 14;
        if (s >= p->len) {
            cv_rect(x + 5, y + 5, 2, 2, K_GRID);
            continue;
        }
        cv_rect(x, y, 12, 12, tr == 2 ? K_ACC : tr ? K_INK : K_GRID);
        if (sq.running && sq.lpos[0] == s)
            cv_rect(x, y + 13, 12, 2, K_ACC);
        if (lock_count(&p->lk[s]))
            cv_rect(x, y - 3, 12, 2, K_LOCK);
        if (ui.held == s)
            cv_rect(x + 3, y + 3, 6, 6, K_BG);
    }
}

static void draw_scope(void)
{
    int32_t x, mid = 66;
    char t[24], b[16];
    float tt = voice.t;
    {
        float step = ui.szoom ? 0.005f : ui.sdur > 0.8f ? 0.1f : 0.05f, s;
        for (s = step; s < ui.sdur; s += step)
            cv_rect((int32_t)(s / ui.sdur * 240.0f), 8, 1, 112, K_GRID);
        cv_rect(0, mid, 240, 1, K_GRID);
    }
    for (x = 0; x < 240; x++) {
        int32_t a = mid - ui.smax[x], b2 = mid - ui.smin[x];
        if (th.scope) {                  /* LINE: the edges only, joined */
            if (x) {
                cv_line(x - 1, mid - ui.smax[x - 1], x, a, K_ACC);
                cv_line(x - 1, mid - ui.smin[x - 1], x, b2, K_FILL);
            }
            continue;
        }
        if (b2 > a + 2)
            cv_rect(x, a + 1, 1, b2 - a - 1, K_FILL);
        cv_pset(x, a, K_ACC);
        cv_pset(x, b2, K_ACC);
    }
    for (x = 1; x < 240; x++)
        cv_line(x - 1, 116 - ui.spitch[x - 1], x, 116 - ui.spitch[x], K_INK);
    if (voice.active && tt < ui.sdur) {
        x = (int32_t)(tt / ui.sdur * 240.0f);
        cv_rect(x, 8, 1, 112, K_INK);
    }
    {
        int tune = (int)skp.p[KP_TUNE];
        note_text(tune, t);
        text_cat(t, " ");
        text_cat(t, TYPE_NAME[clamp((int)skp.p[KP_TYPE], 0, KT_N - 1)]);
        cv_text(4, 2, &FONT_XS, t, K_INK);
    }
    time_text(ui.sdur, b);
    cv_text(236 - text_w(&FONT_XS, b), 2, &FONT_XS, b, K_MUTE);
    t[0] = 0;
    if (ui.held >= 0) {
        const lock_t *l = &P()->lk[ui.held];
        int n = lock_count(l) - (l->kit ? 1 : 0);
        text_cat(t, "S");
        cat_int(t, ui.held + 1);
        text_cat(t, " ");
        if (l->kit) {
            pre_name(l->kit - 1, b);
            text_cat(t, b);
        } else {
            text_cat(t, "PAT");
        }
        if (n) {
            text_cat(t, " +");
            cat_int(t, n);
        }
        cv_text(120 - text_w(&FONT_XS, t) / 2, 2, &FONT_XS, t, K_LOCK);
    } else {
        uint8_t ps[KP_N];
        uint32_t k;
        pre_name(P()->preset, t);
        pre_sound(P()->preset, ps);
        for (k = 0; k < KP_N; k++)
            if (P()->snd[k] != ps[k]) {
                text_cat(t, "*");
                break;
            }
        cv_text(120 - text_w(&FONT_XS, t) / 2, 2, &FONT_XS, t, K_MUTE);
    }
    if ((int)(skp.p[KP_TYPE] + 0.5f) == KT_WSHAPER && ui.mode == M_SND && ui.page[M_SND] == 1) {
        int32_t bx = 146, by = 14, bs = 88, px = 0, py = 0;   /* WSHAPER: its curve, in to out */
        cell(bx - 2, by - 2, bs + 4, bs + 4, K_BG, K_GRID, 0);
        cv_rect(bx, by + bs / 2, bs, 1, K_GRID);
        cv_rect(bx + bs / 2, by, 1, bs, K_GRID);
        for (x = 0; x < bs; x++) {
            float in = (float)x / (float)(bs - 1) * 2.0f - 1.0f, y = kdsp_ws(&skp, in);
            int32_t yy = by + bs / 2 - (int32_t)(fm_clampf(y, -1.2f, 1.2f) * (float)(bs / 2) / 1.2f);
            if (x)
                cv_line(bx + px, py, bx + x, yy, K_LOCK);
            px = x;
            py = yy;
        }
        cv_text(bx, by + bs + 4, &FONT_XS, "CURVE", K_LOCK);
    }
    draw_strip(126);
}

/* the ring: the TRIG lane round a circle, 16 or 32 places */
static void step_xy(int i, int n, int32_t r, int32_t *x, int32_t *y)
{
    float a = (float)i / (float)n - 0.25f;
    *x = 120 + (int32_t)(fm_sin_turns(a + 0.25f) * (float)r);
    *y = 72 + (int32_t)(fm_sin_turns(a) * (float)r);
}

/* GRID design: the steps in rows of eight, the step held or the page's main value under them */
static void draw_grid(void)
{
    const pat_t *p = P();
    int i, n = p->len > 16u ? 32 : 16, rows = n / 8;
    int32_t ch = n == 16 ? 30 : 16, cw = 27;
    char t[32];
    for (i = 0; i < n; i++) {
        int32_t x = 12 + (i % 8) * (cw + 1), y = 4 + (i / 8) * (ch + 2);
        int tr = trig_at(p, i), lk = lock_count(&p->lk[i]);
        uint16_t edge = sq.running && sq.lpos[0] == i ? K_ACC : ui.held == i ? K_INK : K_GRID;
        if (i >= p->len) {
            cv_rect(x + cw / 2, y + ch / 2, 2, 2, K_DIM);
            continue;
        }
        cell(x, y, cw, ch, tr == 2 ? K_ACC : tr ? K_INK : K_BG, edge, edge != K_GRID);
        if (lk)
            cv_rect(x + 3, y + 3, 5, 3, K_LOCK);
        if (tr && (p->st[i].rat || (p->algo != AL_MAN && gen_rat[i])))
            cv_rect(x + cw - 8, y + 3, 5, 3, K_BG);
        if (tr && p->st[i].fl & FL_SLIDE)
            cv_rect(x + 3, y + ch - 5, cw - 6, 2, K_ACC2);
        if (n == 32 && i / 16 == ui.bank)
            cv_rect(x, y + ch, cw, 1, K_MUTE);
    }
    (void)rows;
    t[0] = 0;
    if (ui.held >= 0 && ui.mode == M_SEQ) {
        const step_t *st = &p->st[ui.held];
        const lock_t *l = &p->lk[ui.held];
        int nl = lock_count(l) - (l->kit ? 1 : 0);
        text_cat(t, "STEP ");
        cat_int(t, ui.held + 1);
        text_cat(t, "  ");
        {
            char b[8];
            sint_text(st->pit, b);
            text_cat(t, b);
        }
        text_cat(t, "  X");
        cat_int(t, st->rat + 1);
        if (st->fl & FL_SLIDE)
            text_cat(t, " SL");
        if (st->fl & FL_REV)
            text_cat(t, " REV");
        text_c(120, 82, &FONT_B, t, K_INK);
        t[0] = 0;
        if (l->kit)
            pre_name(l->kit - 1, t);
        if (nl) {
            text_cat(t, l->kit ? " +" : "LOCKS ");
            cat_int(t, nl);
        }
        if (t[0])
            text_c(120, 104, &FONT_XS, t, K_LOCK);
    } else if (ui.mode == M_GEN) {
        text_c(120, 82, &FONT_M, ALGO_NAME[p->algo], p->algo == AL_MAN ? K_MUTE : K_ACC);
    } else if (ui.page[M_SEQ] == 0) {
        cat_int(t, p->len);
        text_cat(t, " STEPS  ");
        text_cat(t, DIV_NAME[p->div]);
        text_cat(t, "  ");
        text_cat(t, DIR_NAME[p->dir]);
        text_c(120, 84, &FONT_B, t, K_INK);
    } else {
        dec1_text(set.clk && sq.ext_bpm10 ? sq.ext_bpm10 : set.bpm10, t);
        text_cat(t, " BPM");
        text_c(120, 82, &FONT_M, t, K_INK);
    }
    if (n == 32)
        cv_text(4, 130, &FONT_XS, ui.bank ? "KEYS 17-32" : "KEYS 1-16", K_MUTE);
}

static void draw_ring(void)
{
    const pat_t *p = P();
    int i, n = p->len > 16u ? 32 : 16, big = n == 16;
    char t[24];
    int32_t x, y, R = 56;
    if (th.steps) {
        draw_grid();
        return;
    }
    circle_line(120, 72, R, K_GRID);
    for (i = 0; i < n; i++) {
        const step_t *s = &p->st[i];
        int tr = trig_at(p, i);
        step_xy(i, n, R, &x, &y);
        if (i >= p->len) {
            cv_rect(x, y, 1, 1, K_DIM);
            continue;
        }
        if (n == 32 && (i / 16) == ui.bank) {    /* the keys' half: a tick outside */
            int32_t bx, by;
            step_xy(i, n, R + 11, &bx, &by);
            cv_rect(bx, by, 2, 2, K_MUTE);
        }
        if ((s->fl & FL_SLIDE) && tr) {
            int32_t x0, y0, x1, y1;
            step_xy((i + n - 1) % n, n, R - 11, &x0, &y0);
            step_xy(i, n, R - 11, &x1, &y1);
            cv_line(x0, y0, x1, y1, K_ACC);
        }
        if (tr == 2)
            fill_circle(x, y, big ? 6 : 4, K_ACC);
        else if (tr)
            fill_circle(x, y, big ? 5 : 3, K_INK);
        else
            fill_circle(x, y, big ? 2 : 1, K_GRID);
        if (tr && (s->rat || (p->algo != AL_MAN && gen_rat[i]))) {
            int r, nr = s->rat > gen_rat[i] || p->algo == AL_MAN ? s->rat : gen_rat[i];
            for (r = 1; r <= nr; r++) {
                int32_t rx, ry;
                step_xy(i, n, R + 5 + r * 4, &rx, &ry);
                cv_rect(rx - 1, ry - 1, 2, 2, K_INK);
            }
        }
        if (tr && (s->fl & FL_REV)) {
            int32_t rx, ry;
            step_xy(i, n, R - 9, &rx, &ry);
            cv_rect(rx - 1, ry - 1, 3, 3, K_MUTE);
        }
        if (lock_count(&p->lk[i])) {
            int32_t lx, ly;
            step_xy(i, n, R - 18, &lx, &ly);
            if (p->lk[i].kit)
                circle_line(lx, ly, 3, K_LOCK);
            cv_rect(lx - 1, ly - 1, 3, 3, K_LOCK);
        }
        if (sq.running && sq.lpos[0] == i)
            circle_line(x, y, big ? 9 : 6, K_ACC);
        if (ui.held == i)
            circle_line(x, y, big ? 8 : 6, K_INK);
    }
    if (ui.held >= 0 && ui.mode == M_SEQ) {
        const step_t *s = &p->st[ui.held];
        const lock_t *l = &p->lk[ui.held];
        int nl = lock_count(l) - (l->kit ? 1 : 0);
        t[0] = 0;
        text_cat(t, "STEP ");
        cat_int(t, ui.held + 1);
        text_c(120, 40, &FONT_XS, t, K_MUTE);
        sint_text(s->pit, t);
        text_c(120, 54, &FONT_L, t, K_INK);
        t[0] = 0;
        text_cat(t, "X");
        cat_int(t, s->rat + 1);
        if (s->fl & FL_SLIDE)
            text_cat(t, " SLIDE");
        if (s->fl & FL_REV)
            text_cat(t, " REV");
        text_c(120, 90, &FONT_XS, t, K_ACC);
        t[0] = 0;
        if (l->kit)
            pre_name(l->kit - 1, t);
        if (nl) {
            text_cat(t, l->kit ? " +" : "LOCKS ");
            cat_int(t, nl);
        }
        if (t[0])
            text_c(120, 102, &FONT_XS, t, K_LOCK);
    } else if (ui.mode == M_GEN) {
        text_c(120, 52, &FONT_M, ALGO_NAME[p->algo], p->algo == AL_MAN ? K_MUTE : K_ACC);
        text_c(120, 80, &FONT_XS, p->algo == AL_MAN ? "TURN ALGO" : "GENERATOR", K_MUTE);
    } else if (ui.page[M_SEQ] == 0) {
        int_text(p->len, t);
        text_c(120, 48, &FONT_L, t, K_INK);
        t[0] = 0;
        text_cat(t, DIV_NAME[p->div]);
        text_cat(t, "  ");
        text_cat(t, DIR_NAME[p->dir]);
        text_c(120, 86, &FONT_XS, t, K_MUTE);
    } else {
        dec1_text(set.clk && sq.ext_bpm10 ? sq.ext_bpm10 : set.bpm10, t);
        text_c(120, 48, &FONT_L, t, K_INK);
        text_c(120, 86, &FONT_XS, set.clk ? "BPM  MIDI" : "BPM", K_MUTE);
    }
    if (p->swing) {
        t[0] = 0;
        text_cat(t, "SWING ");
        cat_int(t, p->swing);
        cv_text(4, 130, &FONT_XS, t, K_MUTE);
    }
    if (p->prob < 100u || p->human) {
        t[0] = 0;
        if (p->prob < 100u) {
            text_cat(t, "PROB ");
            cat_int(t, p->prob);
        }
        if (p->human) {
            text_cat(t, t[0] ? "  HUM " : "HUM ");
            cat_int(t, p->human);
        }
        cv_text(236 - text_w(&FONT_XS, t), 130, &FONT_XS, t, K_MUTE);
    }
    if (n == 32)
        cv_text(4, 2, &FONT_XS, ui.bank ? "KEYS 17-32" : "KEYS 1-16", K_MUTE);
}

static float mod_preview(const mod_t *m, int pos)
{
    float L = (float)(m->len > 1u ? m->len : 2u), x = (float)pos;
    switch (m->shape) {
    case SH_RAND:
    case SH_WALK: return (float)(int32_t)hash3(m->seed, 0u, (uint32_t)pos) * (1.0f / 2147483648.0f);
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
    static const int32_t R[NMOD] = {62, 49, 36, 23};
    char t[24];
    for (m = 0; m < NMOD; m++)
        circle_line(120, 72, R[m], K_GRID);
    for (m = 0; m < NMOD; m++) {
        const mod_t *md = &p->mod[m];
        int on = md->tgt && md->depth;
        uint16_t c = m == f ? MOD_COL[m] : on ? K_MUTE : K_DIM;
        float amp = m == 3 ? 5.0f : 6.0f;
        for (i = 0; i < md->len; i++) {
            float a = (float)i / (float)md->len - 0.25f, v = mod_preview(md, i);
            float cs = fm_sin_turns(a + 0.25f), sn = fm_sin_turns(a);
            int32_t x0 = 120 + (int32_t)(cs * (float)R[m]), y0 = 72 + (int32_t)(sn * (float)R[m]);
            int32_t x1 = 120 + (int32_t)(cs * ((float)R[m] + v * amp)), y1 = 72 + (int32_t)(sn * ((float)R[m] + v * amp));
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
        cv_text(4, 2, &FONT_XS, md->tgt ? pname(md->tgt - 1, p->snd[KP_TYPE]) : "OFF", MOD_COL[f]);
        sint_text(md->depth, t);
        text_cat(t, "%");
        cv_text(236 - text_w(&FONT_XS, t), 2, &FONT_XS, t, K_INK);
    }
    t[0] = 0;
    text_cat(t, SHAPE_NAME[p->mod[f].shape]);
    cv_text(4, 130, &FONT_XS, t, K_MUTE);
    t[0] = 0;
    text_cat(t, DIV_NAME[p->mod[f].div]);
    text_cat(t, "  ");
    text_cat(t, DIR_NAME[p->mod[f].dir]);
    cv_text(236 - text_w(&FONT_XS, t), 130, &FONT_XS, t, K_MUTE);
}

static void draw_fx(void)                /* LIVE: twelve, lit while held; FINAL: five, on or off */
{
    int n;
    if (ui.page[M_FX] == 0) {
        for (n = 0; n < FX_NLIVE; n++) {
            int32_t x = 4 + (n % 4) * 59, y = 4 + (n / 4) * 38;
            int on = fx_live >> n & 1u, sel = n == ui.fxsel[0];
            cell(x, y, 56, 34, on ? K_ACC : K_BG, sel ? K_INK : on ? K_ACC : K_GRID, sel);
            text_c(x + 28, y + 6, &FONT_XS, FX_SHORT[n], on ? K_BG : K_INK);
            {
                char t[4] = {(char)('1' + n % 9), 0, 0, 0};
                if (n >= 9) {
                    t[0] = '1';
                    t[1] = (char)('0' + (n + 1) % 10);
                }
                text_c(x + 28, y + 19, &FONT_XS, t, on ? K_BG : K_DIM);
            }
        }
        text_c(120, 124, &FONT_XS, "HOLD A WHITE KEY", K_MUTE);
    } else {
        for (n = 0; n < FX_NFINAL; n++) {
            int f = FX_NLIVE + n, on = set.fx.on >> f & 1u, sel = f == ui.fxsel[1];
            int32_t y = 6 + n * 25;
            cell(20, y, 200, 22, on ? K_ACC2 : K_BG, sel ? K_INK : on ? K_ACC : K_GRID, sel);
            cv_text(30, y + 6, &FONT_XS, FX_NAME[f], on ? K_INK : K_MUTE);
            cv_text(210 - text_w(&FONT_XS, on ? "ON" : "OFF"), y + 6, &FONT_XS, on ? "ON" : "OFF", on ? K_ACC : K_DIM);
        }
        text_c(120, 132, &FONT_XS, "KEYS 1-5: ON / OFF", K_MUTE);
    }
}

static void draw_themes(void)            /* the six themes, each with its colours */
{
    int n;
    for (n = 0; n < NTHEME; n++) {
        const theme_t *t = &THEMES[n];
        int32_t y = 6 + n * 21;
        uint16_t sw[5] = {t->bg, t->acc, t->ink, t->lock, t->mod[2]};
        int j;
        if (n == set.theme)
            cell(10, y - 2, 220, 20, K_ACC2, K_ACC, 0);
        cv_text(20, y + 3, &FONT_XS, t->name, n == set.theme ? K_INK : K_MUTE);
        for (j = 0; j < 5; j++) {
            cv_rect(110 + j * 22, y + 1, 18, 13, sw[j]);
            cv_rect(110 + j * 22, y + 1, 18, 1, K_GRID);
        }
    }
    text_c(120, 132, &FONT_XS, set.dial ? (set.steps ? "BARS  GRID" : "BARS  RING") : (set.steps ? "DIALS  GRID" : "DIALS  RING"), K_MUTE);
}

static void draw_pat(void)
{
    uint32_t now = plat_ms();
    int n, i;
    for (n = 0; n < NPAT; n++) {
        const pat_t *p = &pats(n);
        int32_t x = 4 + (n % 4) * 59, y = 4 + (n / 4) * 35;
        uint16_t bc = n == sq.cur ? K_ACC : K_GRID;
        char t[12];
        if (n == sq.cur)
            cv_rect(x, y, 56, 32, K_ACC2);
        cv_rect(x, y, 56, 1, bc);
        cv_rect(x, y + 31, 56, 1, bc);
        cv_rect(x, y, 1, 32, bc);
        cv_rect(x + 55, y, 1, 32, bc);
        if (n == sq.next && ((now / 250u) & 1u)) {
            cv_rect(x + 1, y + 1, 54, 1, K_INK);
            cv_rect(x + 1, y + 30, 54, 1, K_INK);
        }
        t[0] = 'P';
        t[1] = (char)('0' + (n + 1) / 10);
        t[2] = (char)('0' + (n + 1) % 10);
        t[3] = 0;
        cv_text(x + 4, y + 3, &FONT_XS, t, n == sq.cur ? K_INK : K_MUTE);
        cv_text(x + 4, y + 12, &FONT_XS, TYPE_NAME[p->snd[KP_TYPE] % KT_N], K_MUTE);   /* its kind of kick */
        if (p->algo != AL_MAN)           /* a generator plays it */
            cv_text(x + 52 - text_w(&FONT_XS, "G"), y + 3, &FONT_XS, "G", K_ACC);
        if (p->len > 16u)                /* 32 steps */
            cv_text(x + 26, y + 3, &FONT_XS, "32", K_MUTE);
        for (i = 0; i < p->len; i++) {
            int tr = p->algo == AL_MAN ? p->st[i].trig : (n == sq.cur ? gen_trig[i] : 0);
            int32_t w = p->len > 16u ? 1 : 2, px = x + 4 + (p->len > 16u ? i * 3 / 2 : i * 3);
            cv_rect(px, y + 24, w, tr ? 5 : 1, tr == 2 ? K_ACC : tr ? K_INK : K_GRID);
        }
    }
}

static void draw_midi(void)
{
    char t[24], b[16];
    cv_text(12, 14, &FONT_XS, "IN CHANNEL", K_MUTE);
    t[0] = 0;
    if (set.mch)
        cat_int(t, set.mch);
    else
        text_cat(t, "ALL");
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

static void draw_store(void)             /* the user kicks: the name being written, the slots round it */
{
    int j;
    char t[16];
    for (j = 0; j < 8; j++) {
        char c[2] = {ui.name[j], 0};
        int32_t x = 64 + j * 14;
        cv_text(x, 6, &FONT_B, c[0] == ' ' ? "" : c, K_INK);
        cv_rect(x, 26, 11, j == ui.cursor ? 2 : 1, j == ui.cursor ? K_ACC : K_GRID);
    }
    for (j = -3; j <= 3; j++) {
        int s = ui.slot + j;
        int32_t y = 64 + j * 11;
        if (s < 0 || s >= NUSER)
            continue;
        t[0] = 'U';
        t[1] = (char)('0' + (s + 1) / 10);
        t[2] = (char)('0' + (s + 1) % 10);
        t[3] = 0;
        if (j == 0)
            cv_rect(40, y - 1, 160, 11, K_ACC2);
        cv_text(48, y, &FONT_XS, t, j == 0 ? K_INK : K_MUTE);
        if (us.u[s].used) {
            pre_name(NFAC + s, t);
            cv_text(84, y, &FONT_XS, t, j == 0 ? K_INK : K_MUTE);
        } else {
            cv_text(84, y, &FONT_XS, "--", K_DIM);
        }
    }
    text_c(120, 132, &FONT_XS, "K4: LEFT LOAD  RIGHT SAVE", K_MUTE);
}

static void draw_pop(void)               /* the knob just turned, big (WSHAPER's curve showing: a narrow one at the left) */
{
    knob_t k;
    const felucca_font_t *f = &FONT_L;
    int narrow = ui.mode == M_SND && ui.page[M_SND] == 1 && (int)(skp.p[KP_TYPE] + 0.5f) == KT_WSHAPER;
    int32_t x0 = narrow ? 4 : 14, w0 = narrow ? 136 : 212, cx = x0 + w0 / 2, bw = w0 - 32;
    knob_info(ui.pop, &k);
    if (!k.name[0] || k.name[0] == '-')
        return;
    cell(x0, 18, w0, 92, K_BG, K_ACC, 0);
    text_c(cx, 26, &FONT_B, k.name, k.hot ? K_LOCK : K_MUTE);
    if (text_w(f, k.val) > w0 - 12)
        f = &FONT_M;
    if (text_w(f, k.val) > w0 - 12)
        f = &FONT_B;
    text_c(cx, 50, f, k.val, K_INK);
    if (k.pos >= 0) {
        int32_t w = bw * k.pos / 1000, bx = x0 + 16;
        cv_rect(bx, 98, bw, 4, K_GRID);
        if (k.bip) {
            int32_t c = bx + bw / 2, e = bx + w;
            cv_rect(c < e ? c : e, 98, (c < e ? e - c : c - e) + 1, 4, K_ACC);
        } else {
            cv_rect(bx, 98, w, 4, K_ACC);
        }
    }
}

static void draw_center(void)
{
    cv_begin(240, 144, K_BG);
    switch (ui.mode) {
    case M_HOME: draw_scope(); break;
    case M_SND: if (ui.page[M_SND] == PG_STORE) draw_store(); else draw_scope(); break;
    case M_SEQ:
    case M_GEN: draw_ring(); break;
    case M_MOD: draw_mod(); break;
    case M_FX: draw_fx(); break;
    case M_PAT: if (ui.page[M_PAT] == 0) draw_pat(); else if (ui.page[M_PAT] == 1) draw_midi(); else draw_themes(); break;
    }
    if (ui.pop >= 0 && plat_ms() < ui.pop_until)
        draw_pop();
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
            lc = K_LOCK;
        lit_to = k.pos < 0 ? -1 : k.pos * (N - 1) / 1000;
        lit_from = k.bip ? (N - 1) / 2 : 0;
        if (k.bip && lit_to < lit_from) {
            int t = lit_to;
            lit_to = lit_from;
            lit_from = t;
        }
        if (th.dial) {                   /* BARS: a bar, filled to the value (from the middle when bipolar) */
            int32_t bx = cx - 25, bw = 50, w = k.pos < 0 ? 0 : bw * k.pos / 1000;
            cv_rect(bx, 14, bw, 10, K_GRID);
            if (k.pos >= 0) {
                if (k.bip) {
                    int32_t c = bx + bw / 2, e = bx + w;
                    cv_rect(c < e ? c : e, 14, (c < e ? e - c : c - e) + 1, 10, lc);
                } else {
                    cv_rect(bx, 14, w, 10, lc);
                }
                cv_rect(bx + w - (w > 0), 12, 2, 14, K_INK);
            }
            goto knob_text;
        }
        for (j = 0; j < N; j++) {
            float a = (135.0f + 270.0f * (float)j / (float)(N - 1)) / 360.0f;
            int32_t x = cx + (int32_t)(fm_sin_turns(a + 0.25f) * (float)r), y = cy + (int32_t)(fm_sin_turns(a) * (float)r);
            int on = k.pos >= 0 && j >= lit_from && j <= lit_to;
            cv_rect(x - 1, y - 1, 2, 2, on ? lc : K_GRID);
        }
        fill_circle(cx, cy, 9, th.knob);
        if (k.pos >= 0) {
            float a = (135.0f + 270.0f * (float)k.pos / 1000.0f) / 360.0f;
            cv_line(cx, cy, cx + (int32_t)(fm_sin_turns(a + 0.25f) * 8.0f), cy + (int32_t)(fm_sin_turns(a) * 8.0f), K_INK);
        }
    knob_text:
        text_c(cx, 40, &FONT_XS, k.name, k.hot ? K_LOCK : K_MUTE);
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
    if (rec_on) {                        /* REC: a keyboard */
        kd = (1u << NKEYS) - 1u;
        kb = ui.keys;
    } else if (ui.mode == M_PAT && ui.page[M_PAT] == 0) {
        for (i = 0; i < NPAT; i++)
            kd |= 1u << WK[i];
        kb |= 1u << WK[sq.cur];
        if (sq.next != 0xFFu && ((plat_ms() / 250u) & 1u))
            kb |= 1u << WK[sq.next];
    } else if (ui.mode == M_FX) {
        static const uint8_t WK2[16] = PLAT_WHITE_KEYS;
        if (ui.page[M_FX] == 0) {
            for (i = 0; i < FX_NLIVE; i++)
                if (fx_live >> i & 1u)
                    kb |= 1u << WK2[i];
                else
                    kd |= 1u << WK2[i];
        } else {
            for (i = 0; i < FX_NFINAL; i++)
                if (set.fx.on >> (FX_NLIVE + i) & 1u)
                    kb |= 1u << WK2[i];
                else
                    kd |= 1u << WK2[i];
        }
    } else if (ui.mode == M_MOD) {
        const mod_t *m = &p->mod[ui.page[M_MOD] / 2];
        int l = ui.page[M_MOD] / 2 + 1;
        for (i = 0; i < 16; i++) {
            int s = ui.bank * 16 + i;
            if (s < m->len && (m->shape != SH_MAN || m->val[s]))
                kd |= 1u << WK[i];
        }
        if (sq.running && sq.lpos[l] >= 0 && sq.lpos[l] / 16 == ui.bank)
            kb |= 1u << WK[sq.lpos[l] % 16];
    } else {
        for (i = 0; i < 16; i++) {
            int s = ui.bank * 16 + i, tr;
            if (s >= p->len)
                continue;
            tr = trig_at(p, s);
            if (tr == 2)
                kb |= 1u << WK[i];
            else if (tr)
                kd |= 1u << WK[i];
        }
        if (sq.running && sq.lpos[0] >= 0 && sq.lpos[0] / 16 == ui.bank)
            kb ^= 1u << WK[sq.lpos[0] % 16];
    }
    switch (ui.mode) {
    case M_HOME: bb |= 1u << B_HOME; break;
    case M_SEQ: bb |= 1u << B_SEQ; break;
    case M_GEN: bb |= 1u << B_ARP; break;
    case M_SND: bb |= 1u << (ui.page[M_SND] == 4 ? B_ENV : B_EDIT); break;
    case M_MOD: bb |= 1u << B_LFO; break;
    case M_FX: bb |= 1u << B_FX; break;
    case M_PAT: bb |= 1u << B_GLO; break;
    }
    if (sq.running)
        bb |= 1u << B_PLAY;
    else
        bd |= 1u << B_PLAY;
    if (rec_on)
        bb |= 1u << B_REC;
    if (P()->len > 16u)
        bd |= 1u << B_SEL;
    if (ui.pat_dirty || ui.set_dirty || ui.user_dirty)
        bd |= 1u << B_SAVE;
    bb |= ui.btn;
    plat_lights(bb, kb, bd, kd, 1);
}

void app_ui_init(void)
{
    lcd_fill(0, 0, 240, 240, K_BG);
    ui.held = -1;
    ui.pop = -1;
    ui.mode = M_HOME;
    ui.redraw_h = ui.redraw_c = ui.redraw_f = 1;
    name_from_slot();
    gen_run(P(), 0);
    scope_start();
}

static void autosave(uint32_t now)
{
    uint32_t i;
    if (sq.running || now - ui.last_change < 1500u)
        return;
    for (i = 0; i < 4u; i++)             /* one object a frame */
        if (ui.pat_dirty & 1u << i) {
            if (plat_store_save(PAT_OBJ[i], &pk[i], sizeof pk[i]) == 0)
                ui.pat_dirty &= (uint8_t)~(1u << i);
            return;
        }
    if (ui.user_dirty) {
        if (plat_store_save(OBJ_USER, &us, sizeof us) == 0)
            ui.user_dirty = 0;
    } else if (ui.set_dirty) {
        if (plat_store_save(OBJ_SET, &set, sizeof set) == 0)
            ui.set_dirty = 0;
    }
}

void app_frame(void)
{
    static uint32_t frame, last_h, last_rec;
    static int8_t last_pos[1 + NMOD];
    static uint8_t last_run, last_act, last_pop;
    uint32_t now = plat_ms(), hstate, k;
    frame++;
    input();
    ann_tick(now);
    if (set.cur != sq.cur) {
        set.cur = sq.cur;
        set_changed();
    }
    if (rec_count != last_rec) {         /* a note recorded (perhaps from MIDI, in the render) */
        last_rec = rec_count;
        ui.redraw_c = ui.redraw_f = 1;
    }
    if (!sj.busy && snd_hash() != ui.shash)
        scope_start();
    scope_work();
    autosave(now);
    hstate = (uint32_t)sq.running | (uint32_t)sq.cur << 1 | (uint32_t)sq.next << 6 | (uint32_t)set.bpm10 << 14 |
             (uint32_t)(now < ui.msg_until) << 30 | (uint32_t)ui.mode << 26 | (uint32_t)ui.page[ui.mode] << 29;
    hstate ^= (uint32_t)set.metro * 0x1F00u ^ (set.metro && sq.running ? (beat_now & 1u) << 12 : 0u) ^ fx_live * 31u;
    hstate ^= (uint32_t)sq.ext_bpm10 * 7919u ^ (uint32_t)rec_on * 0x3779u ^
              (sq.next != 0xFFu ? (now / 250u) & 1u : 0u) << 31;
    if (hstate != last_h || ui.redraw_h) {
        last_h = hstate;
        ui.redraw_h = 0;
        draw_header();
    }
    for (k = 0; k <= NMOD; k++)
        if (sq.lpos[k] != last_pos[k]) {
            last_pos[k] = sq.lpos[k];
            if (ui.mode != M_FX && !(ui.mode == M_PAT && ui.page[M_PAT]))
                ui.redraw_c = 1;
        }
    if (sq.running != last_run) {
        last_run = sq.running;
        ui.redraw_c = ui.redraw_f = 1;
    }
    {
        uint8_t act = (uint8_t)(voice.active != 0), pop = (uint8_t)(ui.pop >= 0 && now < ui.pop_until);
        if ((act || act != last_act) && (ui.mode == M_HOME || (ui.mode == M_SND && ui.page[M_SND] != PG_STORE)) &&
            (frame & 1u))
            ui.redraw_c = 1;
        if (pop != last_pop)
            ui.redraw_c = 1;
        last_act = act;
        last_pop = pop;
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
    if (!strcmp(what, "running")) *got = sq.running;
    else if (!strcmp(what, "hits")) *got = (int)sq.hits;
    else if (!strcmp(what, "cur")) *got = sq.cur;
    else if (!strcmp(what, "next")) *got = sq.next == 0xFFu ? -1 : sq.next;
    else if (!strcmp(what, "algo")) *got = p->algo;
    else if (!strcmp(what, "len")) *got = p->len;
    else if (!strcmp(what, "bpm")) *got = set.bpm10;
    else if (!strcmp(what, "mode")) *got = ui.mode;
    else if (!strcmp(what, "page")) *got = ui.page[ui.mode];
    else if (!strcmp(what, "pos")) *got = sq.lpos[0];
    else if (!strcmp(what, "dirty")) *got = (ui.pat_dirty != 0) | ui.set_dirty << 1 | ui.user_dirty << 2;
    else if (!strcmp(what, "preset")) *got = p->preset;
    else if (!strcmp(what, "rec")) *got = rec_on;
    else if (!strcmp(what, "recs")) *got = (int)rec_count;
    else if (!strcmp(what, "bank")) *got = ui.bank;
    else if (!strcmp(what, "fxon")) *got = (int)set.fx.on;
    else if (!strcmp(what, "fxlive")) *got = (int)fx_live;
    else if (!strcmp(what, "metro")) *got = set.metro;
    else if (!strcmp(what, "theme")) *got = set.theme | set.dial << 4 | set.scope << 5 | set.steps << 6;
    else if (!strcmp(what, "active")) *got = voice.active;
    else if (!strcmp(what, "pop")) *got = ui.pop >= 0 && plat_ms() < ui.pop_until;
    else if (!strcmp(what, "tune_now")) *got = (int)(eff.p[KP_TUNE] + 0.5f);
    else if (!strcmp(what, "type_now")) *got = (int)(eff.p[KP_TYPE] + 0.5f);
    else if (!strcmp(what, "locks")) {
        int n = 0, i;
        for (i = 0; i < NST; i++)
            n += lock_count(&p->lk[i]);
        *got = n;
    } else if (!strncmp(what, "user", 4)) *got = us.u[atoi(what + 4) % NUSER].used;
    else if (!strncmp(what, "snd", 3)) *got = p->snd[atoi(what + 3) % KP_N];
    else if (!strncmp(what, "trig", 4)) *got = p->st[atoi(what + 4) % NST].trig;
    else if (!strncmp(what, "pit", 3)) *got = p->st[atoi(what + 3) % NST].pit;
    else if (!strncmp(what, "rat", 3)) *got = p->st[atoi(what + 3) % NST].rat;
    else if (!strncmp(what, "gen", 3)) *got = gen_trig[atoi(what + 3) % NST];
    else if (!strncmp(what, "mtgt", 4)) *got = p->mod[atoi(what + 4) % NMOD].tgt;
    else if (!strncmp(what, "mdepth", 6)) *got = p->mod[atoi(what + 6) % NMOD].depth;
    else if (!strncmp(what, "lk", 2)) *got = lock_count(&p->lk[atoi(what + 2) % NST]);
    else if (!strncmp(what, "kit", 3)) *got = p->lk[atoi(what + 3) % NST].kit;
    else if (!strncmp(what, "lval", 4)) {
        int n = atoi(what + 4) % NST, kp = atoi(strchr(what, '.') ? strchr(what, '.') + 1 : "0") % KP_N, j;
        j = lock_find(&p->lk[n], kp);
        *got = j < 0 ? -1 : p->lk[n].val[j];
    } else
        return 0;
    return 1;
}
#endif
