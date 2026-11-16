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
#define NLV 16                          /* the p-locks one step can have (the view of a step: lock_t) */
#define NLP 74                           /* the p-locks a pattern holds, all its steps together (pat_t's pool) */
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
/* a step's condition (SEQ's KICK 2, COND): fl's bits 2-5 */
#define FL_CSH 2u
#define STEP_COND(s) (((s)->fl >> FL_CSH) & 15u)
enum { CD_ALL, CD_1_2, CD_2_2, CD_1_3, CD_2_3, CD_3_3, CD_1_4, CD_2_4, CD_3_4, CD_4_4, CD_1ST, CD_N1ST, CD_PRE, CD_NPRE, NCOND };

typedef struct {
    uint8_t tgt;                         /* 0 off, 1 + KP_* */
    uint8_t shape, len, div, dir, smooth;
    int8_t depth;                        /* -100 .. +100 % */
    uint8_t seed;
    int8_t val[NST];                     /* -64 .. +63: the MANUAL shape's values */
} mod_t;

typedef struct {                         /* a step's p-locks, as one step sees them (made from the pattern's pool) */
    uint8_t kit;                         /* 0 none, 1 + preset index */
    uint8_t id[NLV];                     /* 0 none, 1 + KP_* */
    uint8_t val[NLV];
} lock_t;
typedef struct {                         /* one p-lock of the pattern's pool */
    uint8_t st;                          /* 0 free, 1 + the step */
    uint8_t id;                          /* 1 + KP_* */
    uint8_t val;
} lpe_t;
typedef struct {                         /* 0.2.0-0.4.6 (KPT3): four p-locks a step, in the step */
    uint8_t kit, id[4], val[4];
} lock3_t;

typedef struct {
    uint8_t snd[KP_N];                   /* the pattern's kick */
    uint8_t preset;                      /* the preset it came from (factory, then user) */
    uint8_t len, div, dir, swing, prob, human, algo;
    uint8_t ga[4];                       /* the generator's four knobs */
    step_t st[NST];
    uint8_t kit[NST];                    /* each step's kick: 0 the pattern's, 1 + preset index */
    uint8_t prb[NST];                    /* each step's probability: 100 - its % (0: always) */
    lpe_t lp[NLP];                       /* the p-locks of every step (0.4.7: KPT4; before, four in each step) */
    uint8_t lrsv[2];
    mod_t mod[NMOD];
    uint8_t shift;                       /* NUDGE: how far the generator's steps are turned round (0..len-1) */
    uint8_t rsv[3];
} pat_t;
_Static_assert(sizeof(pat_t) == 640, "pat_t: 640 bytes");

#define SET_MAGIC 0x4B434B34u            /* "KCK4": 0.3.4 (FINAL's DISTORTION where the CLIPPER was) */
#define SET_MAGIC3 0x4B434B33u           /* "KCK3": 0.3.0..0.3.3, read: the CLIPPER's knobs become DISTORTION's */
#define PATS_MAGIC 0x4B505434u           /* "KPT4": 0.4.7, the p-locks in a pool, each step's condition and chance */
#define PATS_MAGIC3 0x4B505433u          /* "KPT3": 0.2.0-0.4.6, read (pat_from3); older layouts are not read */
#define USER_MAGIC 0x4B555331u           /* "KUS1" */
typedef struct {
    uint32_t magic;
    uint16_t bpm10;                      /* tempo x 10 */
    uint8_t cur;                         /* the pattern playing */
    uint8_t mch;                         /* MIDI in channel: 0 all, 1..16 */
    uint8_t clk;                         /* 0 internal, 1 MIDI clock in */
    uint8_t clkout, notes_out;
    uint8_t outs;                        /* AUDIO: the outputs switched off (OUT_USB, OUT_JACK, OUT_SPK); 0 all on */
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
/* a pattern of another bank picked while it plays: the bank comes into RAM at once, the pattern playing goes on from
 * this copy until its bar ends (sq.pcopy) */
static pat_t play_copy PLAT_POOL;
#define PCUR() (sq.pcopy ? &play_copy : &pats(sq.cur))
#define NBANK 3                          /* three banks of 16 patterns (A, B, C: the first three black keys on PATTERNS);
                                          * only the bank in use is in RAM */
static const uint8_t PAT_OBJ_ALL[NBANK * 4] = {OBJ_P0, OBJ_P1, OBJ_P2, OBJ_P3, OBJ_P4,  OBJ_P5,
                                               OBJ_P6, OBJ_P7, OBJ_P8, OBJ_P9, OBJ_P10, OBJ_P11};
#define PBANK (set.rsv2[0])              /* the bank in use (0..2), kept with the settings */
#define PJ_CUR (set.rsv2[2] & 7u)        /* the project last loaded or saved (1..4; 0 none) */
#define PJ_CLEAN 0x80u                   /* set.rsv2[2]: the projects' sectors checked once (another firmware's data dropped) */
#define POPS (set.rsv2[1])               /* POPUPS: which pages show the knob popup see-through (POP_*) */
enum { POP_SEQ = 1, POP_LFO = 2, POP_EDIT = 4, POP_ARP = 8 };
#define PAT_OBJ(i) (PAT_OBJ_ALL[PBANK * 4u + (i)])

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
static const char *const TYPE_NAME[KT_N] = {"808", "909", "606", "ANALOG", "FM", "WSHAPER", "THUNDER"};
static const char *const CHAR_NAME[KT_N][4] = {{"TONE", "SNAP", "SIGH", "BOOM"},     {"ATTACK", "SHAPE", "NOISE", "COMP"},
                                               {"TONE", "CLICK", "SNAP", "SIGH"},    {"COLOUR", "PULSE", "RESO", "SATUR"},
                                               {"RATIO", "INDEX", "MOD DEC", "FEEDBK"}, {"CURVE", "DRIVE", "BIAS", "SOURCE"},
                                               {"LENGTH", "GRIT", "TONE", "AMOUNT"}};
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
static const char *const DIR_NAME[NDIR] = {"FWD", "REV", "PING", "RAND", "WANDER"};

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
static const char *const SHAPE_NAME[NSHAPE] = {"MANUAL", "RAMP UP", "RAMP DN", "TRI", "SINE", "SQUARE", "RANDOM", "MEANDER"};

static const char *const FX_NAME[FX_N] = {"DISTORT", "CRUSH",  "FILTER",  "RING MOD", "CHORUS", "PHASER",
                                          "FLANGER", "DELAY",  "REVERB",  "STUTTER",  "GATE",   "WIDTH",
                                          "TRANSIENT", "COMP", "EQ",      "DISTORTION", "LIMITER"};
static const char *const FX_SHORT[FX_N] = {"DIST", "CRUSH", "FILT", "RING", "CHOR", "PHAS", "FLNG", "DELAY", "VERB",
                                           "STUT", "GATE",  "WIDE", "TRANS", "COMP", "EQ",  "DIST", "LIMIT"};
static const char *const FX_P[FX_N][4] = {
    {"DRIVE", "TONE", "MIX", "OUTPUT"},   {"BITS", "RATE", "MIX", "-"},          {"CUTOFF", "RESO", "-", "-"},
    {"FREQ", "MIX", "-", "-"},            {"RATE", "DEPTH", "MIX", "-"},         {"RATE", "DEPTH", "FEEDBACK", "MIX"},
    {"RATE", "DEPTH", "FEEDBACK", "MIX"}, {"TIME", "FEEDBACK", "MIX", "FILTER"},   {"DECAY", "DAMP", "MIX", "PRE"},
    {"SLICE", "REPEAT", "MIX", "-"},      {"RATE", "DEPTH", "SMOOTH", "-"},      {"AMOUNT", "BASS MONO", "-", "-"},
    {"ATTACK", "SUSTAIN", "-", "-"},      {"THRESH", "RATIO", "ATTACK", "RELEASE"}, {"LOW", "MID", "MID FREQ", "HIGH"},
    {"TYPE", "DRIVE", "TONE", "MIX"},     {"CEILING", "RELEASE", "-", "-"}};
static const char *const FX_DTYPE_NAME[FX_NDTYPE] = {"CLIP", "SOFT", "TUBE", "FUZZ", "FOLD", "RECT"};
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
    /* 808: ~49 Hz, its own jump (SNAP) and sigh (SIGH), no sweep added */
    {KP_TYPE, 0, KP_TUNE, 31, KP_DEC, 100, KP_CA, 55, KP_CB, 70, KP_CC, 52, KP_CD, 45, KP_BEND, 0, 0xFF},
    {KP_TYPE, 0, KP_TUNE, 33, KP_DEC, 78, KP_CA, 95, KP_CB, 90, KP_CC, 40, KP_BEND, 0, KP_CTYPE, 2, KP_CLVL, 30,
     KP_PUNCH, 40, 0xFF},
    {KP_TYPE, 0, KP_TUNE, 31, KP_DEC, 95, KP_CA, 60, KP_CB, 70, KP_CC, 64, KP_CD, 40, KP_BEND, 0, KP_DTYPE, 4,
     KP_DRIVE, 42, KP_SHAPE, 40, 0xFF},
    /* 909: settles ~55 Hz from ~230 Hz in tens of ms, the triangle through its shaper, noise in the attack */
    {KP_TYPE, 1, KP_TUNE, 33, KP_DEC, 70, KP_CA, 70, KP_CB, 70, KP_CC, 60, KP_CD, 30, KP_BEND, 53, KP_TIME, 66,
     KP_CURVE, 64, 0xFF},
    {KP_TYPE, 1, KP_TUNE, 34, KP_DEC, 78, KP_CA, 100, KP_CB, 100, KP_CC, 80, KP_CD, 60, KP_BEND, 58, KP_TIME, 64,
     KP_DTYPE, 1, KP_DRIVE, 12, KP_SHAPE, 30, KP_CLIP, 30, 0xFF},
    {KP_TYPE, 1, KP_TUNE, 36, KP_DEC, 55, KP_CA, 80, KP_CB, 80, KP_CC, 50, KP_BEND, 50, KP_TIME, 60, 0xFF},
    /* 606: higher, shorter, clickier */
    {KP_TYPE, 2, KP_TUNE, 38, KP_DEC, 52, KP_CA, 90, KP_CB, 60, KP_CC, 60, KP_CD, 20, KP_BEND, 0, 0xFF},
    {KP_TYPE, 2, KP_TUNE, 45, KP_DEC, 62, KP_CA, 80, KP_CB, 30, KP_CC, 40, KP_CD, 70, KP_BEND, 0, 0xFF},
    {KP_TYPE, 2, KP_TUNE, 38, KP_DEC, 58, KP_CA, 60, KP_CB, 60, KP_CC, 50, KP_CD, 30, KP_BEND, 0, KP_DTYPE, 3,
     KP_DRIVE, 30, KP_SHAPE, 80, 0xFF},
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
    volatile uint8_t rpt_len;            /* SEQ, OCT- + a white key held: the steps repeated (1..16; 0 none) */
    volatile uint8_t rpt_arm;            /* set by the key: the repeat starts from the next step */
    uint32_t rpt_c0;                     /* the TRIG lane's count where it started */
    volatile int8_t hit_step;            /* the step of the kick sounding now (-1: none) */
    volatile uint8_t pcopy, pcbank;      /* the pattern playing is play_copy, of bank pcbank (until its bar ends) */
    volatile uint8_t cpre;               /* COND's PRE: how the last conditional step went */
} sq;
static int8_t rpt_key = -1;              /* the white key holding the repeat */
static uint8_t gen_trig[NST], gen_rat[NST];   /* the generator's TRIG lane: 0, 1, 2; its rolls */
static volatile uint32_t midi_seen;
static volatile uint8_t rec_on;          /* REC: notes are written into the steps */
static volatile uint8_t listen_on;       /* OCT- + REC: the keyboard plays the kick chromatically, nothing recorded */
#define OUT_USB 1u
#define OUT_JACK 2u
#define OUT_SPK 4u
static volatile uint32_t fx_live;        /* the LIVE effects held */
static volatile uint32_t fx_latch;       /* the LIVE effects left on (OCT- + their key; again to let go) */

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
    for (i = 0; i < NST; i++) {          /* (turned round by NUDGE, with the steps' own data) */
        uint32_t j = i < L ? (i + L - p->shift % L) % L : i;
        gen_trig[i] = i < L ? out[j] : 0;
        gen_rat[i] = i < L ? rat[j] : 0;
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

/* a step's view of its p-locks (lock_t): its kick and the pool's entries for it */
static int lock_count(const lock_t *l)
{
    int n = l->kit ? 1 : 0, k;
    for (k = 0; k < NLV; k++)
        n += l->id[k] != 0;
    return n;
}
static int lock_find(const lock_t *l, int kp)
{
    int k;
    for (k = 0; k < NLV; k++)
        if (l->id[k] == kp + 1)
            return k;
    return -1;
}
static void lk_get(const pat_t *p, int s, lock_t *l)   /* step s's view */
{
    uint32_t i, n = 0;
    uint8_t *b = (uint8_t *)l;
    for (i = 0; i < sizeof *l; i++)
        b[i] = 0;
    if (s < 0 || s >= NST)
        return;
    l->kit = p->kit[s];
    for (i = 0; i < NLP && n < NLV; i++)
        if (p->lp[i].st == (uint8_t)(s + 1)) {
            l->id[n] = p->lp[i].id;
            l->val[n] = p->lp[i].val;
            n++;
        }
}
static const lock_t *LKV(const pat_t *p, int s)   /* the same, for reading at once (two at a time, the UI's) */
{
    static lock_t v[2];
    static uint8_t w;
    w ^= 1u;
    lk_get(p, s, &v[w]);
    return &v[w];
}
static int lk_find(const pat_t *p, int s, int kp)   /* the pool's entry for step s, parameter kp; -1 none */
{
    int i;
    for (i = 0; i < NLP; i++)
        if (p->lp[i].st == (uint8_t)(s + 1) && p->lp[i].id == (uint8_t)(kp + 1))
            return i;
    return -1;
}
static int lk_nstep(const pat_t *p, int s)       /* step s's parameters locked */
{
    int i, n = 0;
    for (i = 0; i < NLP; i++)
        n += p->lp[i].st == (uint8_t)(s + 1);
    return n;
}
static int lk_count(const pat_t *p, int s) { return lk_nstep(p, s) + (p->kit[s] ? 1 : 0); }
static int lk_set(pat_t *p, int s, int kp, int v)   /* 0: no room (the pattern's NLP, or the step's NLV) */
{
    int i = lk_find(p, s, kp);
    if (i < 0) {
        if (lk_nstep(p, s) >= NLV)
            return 0;
        for (i = 0; i < NLP && p->lp[i].st; i++)
            ;
        if (i >= NLP)
            return 0;
    }
    p->lp[i].id = (uint8_t)(kp + 1);
    p->lp[i].val = (uint8_t)v;
    p->lp[i].st = (uint8_t)(s + 1);      /* (last: the player reads the pool) */
    return 1;
}
static void lk_clear(pat_t *p, int s)    /* step s: no kick of its own, no p-locks */
{
    int i;
    p->kit[s] = 0;
    for (i = 0; i < NLP; i++)
        if (p->lp[i].st == (uint8_t)(s + 1))
            p->lp[i].st = p->lp[i].id = p->lp[i].val = 0;
}
static int lk_total(const pat_t *p)
{
    int i, n = 0;
    for (i = 0; i < NLP; i++)
        n += p->lp[i].st != 0;
    return n;
}
/* a pattern of 0.2.0-0.4.6 (KPT3: four p-locks in each step, where the kits, the chances and the pool are now): the
 * same, in this layout (each step's locks into the pool, in step order; past NLP, the rest are dropped) */
static void pat_from3(pat_t *p)
{
    lock3_t old[NST];
    uint8_t *src = (uint8_t *)p->kit;
    uint32_t i, k;
    _Static_assert(sizeof old == sizeof p->kit + sizeof p->prb + sizeof p->lp + sizeof p->lrsv, "the same bytes");
    for (i = 0; i < sizeof old; i++)
        ((uint8_t *)old)[i] = src[i];
    for (i = 0; i < sizeof old; i++)
        src[i] = 0;
    for (i = 0; i < NST; i++) {
        p->kit[i] = old[i].kit;
        p->st[i].fl &= (uint8_t)(FL_SLIDE | FL_REV);
        for (k = 0; k < 4u; k++)
            if (old[i].id[k] && old[i].id[k] <= KP_N)
                lk_set(p, (int)i, old[i].id[k] - 1, old[i].val[k]);
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
        for (k = 0; k < NLV; k++)
            if (lk->id[k] && lk->id[k] <= KP_N)
                out[lk->id[k] - 1] = (float)lk->val[k];
}

static lock_t hit_lk;                    /* the p-locks of the hit sounding: they last as long as it does */
static uint8_t hit_has;

static void eff_compute(void)
{
    const pat_t *p = PCUR();
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
    const pat_t *p = PCUR();
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

/* a new hit while the kick still sounds: the old one goes on a moment as a ghost, fading out over GHOST_N samples
 * (6 ms), under the new one: no cut in the low wave, no click. (kdsp_hit's own tail only hid the step) */
#define GHOST_N 265u
static kick_t ghost;
static kparm_t geff;
static uint32_t gleft;
static float gbuf[APP_BLOCK];
static void voice_hit(float semis, float vel, int fl)
{
    if (voice.active && !(fl & KH_SLIDE)) {
        ghost = voice;
        geff = eff;                      /* the params it was playing with */
        gleft = GHOST_N;
    }
    eff_compute();
    kdsp_hit(&voice, &eff, semis, vel, fl);
    if (gleft == GHOST_N)
        voice.tail = 0.0f;               /* the ghost carries the old wave on: no step to hide */
}

/* a hit: its p-locks (0xFF none) hold while it sounds */
static void hit(int semis, float vel, uint8_t fl, uint8_t step)
{
    if (step < NST) {
        lk_get(PCUR(), step, &hit_lk);
        hit_has = 1;
    } else {
        hit_has = 0;
    }
    voice_hit((float)semis, vel, fl);
    sq.hit_step = (int8_t)(step < NST ? step : -1);
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
    pat_t *p = PCUR();
    uint32_t L = lane_len(p, l), D = lane_div(p, l), c = sq.lc[l];
    int pos;
    if (l == 0 && c % (L ? L : 1u) == 0u) {     /* the TRIG lane's bar starts: a queued pattern, the generator */
        if (sq.next != 0xFFu && c) {
            uint32_t k;
            sq.cur = sq.next;
            sq.next = 0xFFu;
            sq.pcopy = 0;                /* (another bank's pattern: the copy of the old one is done with) */
            p = PCUR();
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
    if (l == 0 && sq.rpt_len) {          /* a repeat: the same few steps again, while the count goes on beneath;
                                          * let go, the steps are where they would have been */
        if (sq.rpt_arm || sq.rpt_c0 > c) {
            sq.rpt_c0 = c;
            sq.rpt_arm = 0;
        }
        pos = lane_pos(l, sq.rpt_c0 + (c - sq.rpt_c0) % sq.rpt_len, L, lane_dir(p, l));
    }
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
        if (trig) {                      /* KICK 2: the step's condition, then its chance */
            uint32_t cd = STEP_COND(s), loop = c / (L ? L : 1u);
            int ok = 1;
            switch (cd) {
            case CD_ALL: break;
            case CD_1ST: ok = loop == 0u; break;
            case CD_N1ST: ok = loop != 0u; break;
            case CD_PRE: ok = sq.cpre; break;
            case CD_NPRE: ok = !sq.cpre; break;
            default: {                   /* X:Y: the X-th pass of every Y */
                static const uint8_t CX[NCOND] = {0, 1, 2, 1, 2, 3, 1, 2, 3, 4}, CY[NCOND] = {1, 2, 2, 3, 3, 3, 4, 4, 4, 4};
                ok = loop % CY[cd] == (uint32_t)(CX[cd] - 1u);
                break;
            }
            }
            if (ok && p->prb[pos])
                ok = (int)(xr32(&rng_au) % 100u) < 100 - (int)p->prb[pos];
            if (cd != CD_PRE && cd != CD_NPRE && (cd != CD_ALL || p->prb[pos]))
                sq.cpre = (uint8_t)ok;
            if (!ok)
                trig = 0;
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

static int tick_hits(uint32_t t)         /* a tick where a hit may land: a TRIG step, or an event due */
{
    uint32_t i;
    if (t % lane_div(PCUR(), 0) == 0u)
        return 1;
    for (i = 0; i < sq.nev; i++)
        if ((int32_t)(sq.ev[i].at - t) <= 0)
            return 1;
    return 0;
}

#ifdef PLAT_WEB
static uint32_t web_end;                 /* Render Audio (kick/web.c, the browser pages only): the tick it stops at, 0 none */
static uint8_t web_dry;                  /* Render Audio: the kick without the effects */
#endif
static void on_tick(uint32_t t)
{
#ifdef PLAT_WEB
    if (web_end && t >= web_end) {       /* the render's 16ths are over: the sequencer stops, the tails ring on */
        sq.running = 0;
        sq.nev = 0;
        web_end = 0;
        return;
    }
#endif
    if (t % PPQ == 0u) {
        beat_now = t / PPQ;
        if (set.metro) {
            met.req = 1;
            met.hi = t % (PPQ * 4u) == 0u;
            met.at = blk_i;
        }
    }
    pat_t *p = PCUR();
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
    sq.hit_step = -1;
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
    if (sq.pcopy) {                      /* stopped before the bar's end: the pattern picked in the other bank now */
        sq.pcopy = 0;
        if (sq.next != 0xFFu)
            sq.cur = sq.next;
        sq.next = 0xFFu;
    }
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
static volatile int8_t rec_last = -1;    /* the step REC wrote last (-1 none yet): a stopped REC's knobs lock it */
static int16_t rec_val[KP_N];            /* REC while it plays: where each knob stands since REC went on (-1: not turned
                                          * yet), so a turn carries on from the last one whatever step sounds now */
static uint8_t rec_pat = 0xFFu;          /* the pattern those belong to */
static void rec_knobs_clear(void)
{
    int k;
    for (k = 0; k < KP_N; k++)
        rec_val[k] = -1;
    rec_pat = sq.cur;
}
static volatile uint32_t rec_count;
static void pat_changed_from_render(void);
static void rec_note(int semis, int vel)
{
    pat_t *p = PCUR();
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
    rec_last = (int8_t)s;
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
            voice_hit((float)((int)d1 - 36), (float)d2 / 127.0f, d2 >= 112u ? KH_ACC : 0);
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
        uint32_t n = to - *from;
        kdsp_render(&voice, &eff, vbuf + *from, n);
        if (gleft) {                     /* the ghost of the hit before, fading out (smoothstep) */
            uint32_t m = n < gleft ? n : gleft, j;
            kdsp_render(&ghost, &geff, gbuf, m);
            for (j = 0; j < m; j++) {
                float x = (float)(gleft - j) * (1.0f / (float)GHOST_N);
                vbuf[*from + j] += gbuf[j] * x * x * (3.0f - 2.0f * x);
            }
            gleft -= m;
        }
        *from = to;
    }
}

void app_render(int32_t *lr, uint32_t n, uint32_t master_q12)
{
    uint32_t i, done = 0;
    float gain = (float)master_q12 / 4096.0f * 8300000.0f, tick0;
    if (n > APP_BLOCK)
        n = APP_BLOCK;
    fx_begin();                          /* the hall on the second core while this one renders the kick */
    tempo_set();
    eff_update();
    blk_i = 0;
    midi_in();
    tick0 = (float)sq.tick + sq.frac;
    if (sq.aud_req) {                    /* the keyboard: the kick now (a step held: with its p-locks) */
        sq.aud_req = 0;
        if (sq.aud_step < NST) {
            lk_get(PCUR(), sq.aud_step, &hit_lk);
            hit_has = 1;
        } else {
            hit_has = 0;
        }
        voice_hit((float)sq.aud_semis, (float)sq.aud_vel / 127.0f, sq.aud_vel >= 112u ? KH_ACC : 0);
        sq.hits++;
    }
    i = 0;
    if (!sq.running) {                   /* stopped: nothing to count but the samples */
        sq.smp += n;
        i = n;
    } else if (!sq.first && !set.clk) {  /* its own clock, under way: the count kept in hand between ticks (the same
                                          * sums as the loop below, a sample at a time) */
        float frac = sq.frac, inc = sq.inc;
        uint32_t smp0 = sq.smp;
        for (; i < n; i++) {
            frac += inc;
            if (frac >= 1.0f) {
                blk_i = i;
                sq.smp = smp0 + i + 1u;
                do {
                    frac -= 1.0f;
                    sq.frac = frac;
                    sq.tick++;
                    if (tick_hits(sq.tick))      /* the kick so far, only where a hit may land (the rest: one go) */
                        render_to(&done, i);
                    on_tick(sq.tick);
                    frac = sq.frac;
                } while (frac >= 1.0f && sq.running);
                inc = sq.inc;
                if (!sq.running || sq.first) {   /* (stopped by the tick: the loop below takes the rest) */
                    i++;
                    break;
                }
            }
        }
        sq.frac = frac;
        sq.smp = smp0 + i;
    }
    for (; i < n; i++) {
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
            if (tick_hits(sq.tick))      /* the kick so far, only where a hit may land (the rest: one go) */
                render_to(&done, i);
            on_tick(sq.tick);
        }
    }
    render_to(&done, n);
#ifdef PLAT_WEB
    if (web_dry)
        for (i = 0; i < n; i++)
            lbuf[i] = rbuf[i] = vbuf[i];
    else
#endif
    fx_process(&set.fx, fx_live | fx_latch, vbuf, lbuf, rbuf, n, bpm_now(), tick0, sq.inc, sq.running);
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
    fx_out(lbuf, rbuf, lr, n, gain, &gain_now);   /* (in fx.c: an -O2 unit) */
}

void app_panic(void)
{
    sq.running = 0;
    voice.active = 0;
    voice.tail = 0.0f;
    fx_reset();
}

/* ---------------------------------------------------------------- patterns --- */
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
        p->prb[i] = 0;
        lk_clear(p, (int)i);
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
    for (i = 0; i < 3u; i++)
        p->rsv[i] = 0;
    p->shift = 0;
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
/* a bank never used: sixteen plain patterns, a kick on each beat, the factory kicks in turn */
static void pats_blank(void)
{
    uint32_t i;
    for (i = 0; i < 4u; i++)
        pk[i].magic = PATS_MAGIC;
    for (i = 0; i < NPAT; i++) {
        pat_t *p = &pats(i);
        pat_clear(p);
        pat_sound(p, (int)(i % NFAC));
        pat_floor(p);
    }
}
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
    pats(13).kit[6] = 1 + 3;
    pats(13).st[14].trig = 1;
    lk_set(&pats(13), 14, KP_TUNE, pats(13).snd[KP_TUNE] + 5);
    lk_set(&pats(13), 14, KP_DEC, 50);
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
    set.outs = 0;
    set.theme = 0;
    set.dial = THEMES[0].dial;
    set.scope = THEMES[0].scope;
    set.steps = THEMES[0].steps;
    set.metro = 0;
    set.rsv2[0] = set.rsv2[1] = set.rsv2[2] = 0;   /* (rsv2[0]: the pattern bank) */
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
    for (m = 0; m < NST; m++)
        if (p->kit[m] > NPRE || p->st[m].trig > 2u || p->st[m].rat > 3u || STEP_COND(&p->st[m]) >= NCOND ||
            p->prb[m] > 100u)
            return 0;
    for (k = 0; k < NLP; k++) {
        const lpe_t *e = &p->lp[k];
        if (e->st > NST || (e->st && (!e->id || e->id > KP_N || e->val < kp_lo(e->id - 1) || e->val > kp_hi(e->id - 1))))
            return 0;
    }
    for (m = 0; m < NMOD; m++)
        if (p->mod[m].len < 1u || p->mod[m].len > NST || p->mod[m].div >= NDIV || p->mod[m].dir >= NDIR ||
            p->mod[m].shape >= NSHAPE || p->mod[m].tgt > KP_N)
            return 0;
    return 1;
}
/* an object of four patterns as read: this layout (1), or 0.2.0-0.4.6's turned into it (1, the object marked to save
 * again: ui.pat_dirty by the caller), or neither (0) */
static int pats_take(pats_t *o)
{
    uint32_t k;
    if (o->magic == PATS_MAGIC)
        return 1;
    if (o->magic != PATS_MAGIC3)
        return 0;
    for (k = 0; k < 4u; k++)
        pat_from3(&o->p[k]);
    o->magic = PATS_MAGIC;
    return 2;
}
static uint8_t pk_conv;                  /* objects of the bank in RAM to write again (app_frame): read in the old layout,
                                          * or not on the flash (their defaults) */
static void pats_defaults(void);
static void pats_blank(void);
/* the bank in use (PBANK) from the flash into RAM: each of its four objects read, or its defaults where it is not
 * there (or damaged; that object marked to write). 0: none of the four was there */
static int pats_load(void)
{
    static pats_t t PLAT_POOL;
    uint32_t i, k;
    int got = 0;
    if (PBANK == 0)                      /* first the defaults, all of them: what is on the flash goes over them */
        pats_defaults();
    else
        pats_blank();
    pk_conv = 0;
    for (i = 0; i < 4u; i++) {
        int r = plat_store_load(PAT_OBJ(i), &t, sizeof t) == (int)sizeof t ? pats_take(&t) : 0;
        for (k = 0; r && k < 4u; k++)
            if (!pat_valid(&t.p[k]))
                r = 0;
        if (r) {
            pk[i] = t;
            got++;
        }
        if (r != 1)
            pk_conv |= (uint8_t)(1u << i);
    }
    if (!got)                            /* (nothing: the caller marks the whole bank) */
        pk_conv = 0;
    return got;
}

static void load_all(void)               /* everything from the flash: at power-on, after a restore */
{
    int got = plat_store_load(OBJ_SET, &set, sizeof set) == (int)sizeof set;
    if (got && set.magic == SET_MAGIC3) {
        fxset_t d;                       /* 0.3.x before DISTORTION: its knobs from the start, the rest kept */
        uint32_t k;
        fx_defaults(&d);
        for (k = 0; k < 4u; k++)
            set.fx.p[FX_DRIVE][k] = d.p[FX_DRIVE][k];
        set.magic = SET_MAGIC;
    }
    if (!got || set.magic != SET_MAGIC || set.cur >= NPAT ||
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
    if (PBANK >= NBANK)
        PBANK = 0;
    if (!pats_load()) {
        if (PBANK == 0)
            pats_defaults();
        else
            pats_blank();
    }
    sq.cur = set.cur;
    sq.next = 0xFFu;
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
        int on = p->algo == AL_MAN ? p->st[i].trig : 1, n, j;
        float base[KP_N];
        lk_clear(p, (int)i);
        if (i >= p->len || !on || rr(s, 0, 99) >= (p->algo == AL_MAN ? dens : dens / 2))
            continue;
        if (rr(s, 0, 3) == 0)
            p->kit[i] = (uint8_t)(1 + rr(s, 0, NFAC - 1));
        n = p->kit[i] && rr(s, 0, 1) ? 0 : rr(s, 1, 2);
        step_sound(p, LKV(p, (int)i), base);
        for (j = 0; j < n; j++) {
            int kp = KP_LK[rr(s, 0, (int)sizeof KP_LK - 1)], b = (int)base[kp], v;
            if (kp == KP_TUNE)
                v = b + (rr(s, 0, 1) ? rr(s, -7, 7) : 12 * rr(s, -1, 1));
            else if (kp_hi(kp) < 10)
                v = rr(s, kp_lo(kp), kp_hi(kp));
            else
                v = b + rr(s, -45, 45);
            lk_set(p, (int)i, kp, clamp(v, kp_lo(kp), kp_hi(kp)));
        }
    }
}

/* ---------------------------------------------------------------------- UI --- */
enum { M_HOME, M_SEQ, M_GEN, M_SND, M_MOD, M_FX, M_PAT, NMODE };
static const uint8_t NPAGE[NMODE] = {1, 2, 3, NSPAGE + 1, 2 * NMOD, 2, 7};
enum { AP_GEN, AP_TRIG, AP_FEEL };    /* ARP's pages (SEQ has one: KICK) */
enum { GP_PAT, GP_PROJ, GP_MIDI, GP_THEME, GP_POP, GP_AUDIO, GP_SYS };
#define PG_STORE NSPAGE
enum { A_PLAY, A_COPY, A_PASTE, A_CLEAR, A_INIT, A_RAND, NACT };
static const char *const ACT_NAME[NACT] = {"PLAY", "COPY", "PASTE", "CLEAR", "INIT KICK", "RANDOM"};

static struct {
    uint8_t mode, page[NMODE];
    char msg[2][24];
    uint32_t msg_until;
    uint32_t keys, btn;
    int8_t held;                         /* the step held (0..31), -1 none */
    int8_t sel;                          /* the step ALGORITHM picks on SEQ and EDIT (0..31) */
    int8_t nudge;                        /* FEEL's NUDGE: the steps moved since the page was opened (-99..+99) */
    uint8_t cpu_peak;                    /* SYSTEM: the highest audio load seen (%) */
    uint8_t held_turned;
    uint8_t octup_used;                  /* OCT+ held: something else was pressed or turned meanwhile */
    uint8_t bank;                        /* the white keys: steps 1-16 (0) or 17-32 (1) */
    uint8_t act;
    uint8_t pj, pj_act, pj_arm;          /* PROJECTS: the slot, the action (LOAD, SAVE, CLEAR), DO IT turned once */
    uint8_t vbank;                       /* PATTERNS: the bank shown (the black keys); the one playing goes on */
    uint32_t pj_arm_t;
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

static pat_t *P(void) { return PCUR(); }

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
        if (type == KT_THUNDER) {        /* the rumble's length: 80 ms .. 1.6 s */
            time_text(0.08f * ex2((float)v, 4.3f), b);
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
        if (type == KT_THUNDER) {        /* the rumble's low-pass */
            hz_text(60.0f * ex2((float)v, 4.0f), b);
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
    case FX_DRIVE * 4 + 0: text_cat(b, FX_DTYPE_NAME[fx_idx((uint8_t)v, FX_NDTYPE)]); break;
    case FX_DRIVE * 4 + 2: hz_text(500.0f * ex2((float)v, 5.3f), b); break;
    case FX_EQ * 4 + 0: case FX_EQ * 4 + 1: case FX_EQ * 4 + 3: sint_text((v - 64) * 12 / 64, b); text_cat(b, "DB"); break;
    case FX_EQ * 4 + 2: hz_text(60.0f * ex2((float)v, 6.7f), b); break;
    case FX_CRUSH * 4 + 0: cat_int(b, 1 + v * 14 / 127); text_cat(b, " BIT"); break;
    case FX_CRUSH * 4 + 1: text_cat(b, "/"); cat_int(b, 1 + 31 * v * v / (127 * 127)); break;
    case FX_FILT * 4 + 0:
    case FX_DELAY * 4 + 3:               /* (DELAY's FILTER: the same) */
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
    case FX_VERB * 4 + 0: time_text(0.6f + 7.4f * ((float)v / 127.0f) * ((float)v / 127.0f), b); break;
    case FX_VERB * 4 + 1: hz_text(14000.0f * fm_exp2f(-3.6f * (float)v / 127.0f), b); break;
    case FX_VERB * 4 + 3: time_text((1.0f + 4400.0f * (float)v / 127.0f) / 44100.0f, b); break;
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

static const int8_t SEQK_KP[4] = {KP_TYPE, KP_TUNE, KP_ATK, KP_DEC};   /* the kind, the pitch, the attack, the decay */
static int knob_kp(int i)                /* the sound parameter knob i turns on this page, -1 none */
{
    if (ui.mode == M_SEQ)
        return ui.page[M_SEQ] == 0 ? SEQK_KP[i] : -1;   /* (KICK 2: the step's condition and chance) */
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

/* OCT- held: every encoder moves faster (not while REC or LISTEN play the keyboard) */
/* REC on, on SEQ or any EDIT page: the knobs are recorded as p-locks. Playing: into the step of the kick sounding now,
 * each knob carrying on from where it stood (rec_val), heard at once. Stopped: into the step REC wrote last (none yet:
 * the one the next note goes to), from that step's own value */
static int edit_page(void);
static int motion_rec(void) { return rec_on && (ui.mode == M_SEQ || edit_page()); }
/* OCT- held: the knobs faster (not while REC or LISTEN play the keyboard; but with REC recording the knobs, yes) */
static int fast(void) { return !listen_on && ((ui.btn >> B_OCTDN) & 1u) && (!rec_on || motion_rec()); }
static int acc(int d, int lo, int hi) { return !fast() || hi - lo < 12 ? d : d * (hi - lo >= 100 ? 8 : 4); }
/* the step being edited: the one held, else the one ALGORITHM picked on a SEQ page */
static int edit_page(void) { return ui.mode == M_SND && ui.page[M_SND] != PG_STORE; }
/* the step ALGORITHM picks on SEQ and on the EDIT pages: SEQ's KICK edits it; on EDIT, OCT+ held + a knob locks it */
static int seq_cursor(void) { return ui.mode == M_SEQ || edit_page() ? ui.sel % P()->len : -1; }
static int ed_step(void)
{
    if (ui.held >= 0)
        return ui.held;
    if (motion_rec()) {                  /* recording the knobs: that step (playing, no kick yet: -2) */
        int st = sq.running ? sq.hit_step : rec_last >= 0 ? rec_last : (int)(rec_sel % P()->len);
        return st >= 0 && st < (int)P()->len ? st : -2;
    }
    if (ui.mode == M_SEQ || (edit_page() && (ui.btn >> B_OCTUP & 1u)))
        return ui.sel % P()->len;
    return -1;
}

static void pj_action(void);
static void pj_scan(void);
static const char *const PJ_ACT[3] = {"LOAD", "SAVE", "CLEAR"};
static const char *const COND_NAME[NCOND] = {"---", "1:2", "2:2", "1:3", "2:3", "3:3", "1:4", "2:4", "3:4", "4:4",
                                             "1ST", "!1ST", "PRE", "!PRE"};
static void knob_info(int i, knob_t *k)
{
    pat_t *p = P();
    int pg = ui.page[ui.mode], hs = ed_step();
    float snd[KP_N];
    k->name[0] = k->val[0] = 0;
    k->pos = -1;
    k->bip = 0;
    k->hot = 0;
    if (motion_rec() && sq.running && knob_kp(i) >= 0) {   /* REC, playing: where the knob stands */
        int kp = knob_kp(i);
        step_sound(p, hs >= 0 ? LKV(p, hs) : 0, snd);
        if (rec_val[kp] >= 0 && rec_pat == sq.cur)
            snd[kp] = (float)rec_val[kp];
        if (hs >= 0 && lk_find(p, hs, kp) >= 0) {
            text_cat(k->name, "*");
            k->hot = 1;
        }
        knob_snd(k, kp, snd);
        return;
    }
    if (hs >= 0 && ui.mode != M_HOME && ui.mode != M_PAT && ui.mode != M_FX && !(ui.mode == M_SND && pg == PG_STORE)) {
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
            const lock_t *l = LKV(p, hs);
            int kp = knob_kp(i);
            step_sound(p, l, snd);
            if (lock_find(l, kp) >= 0)
                text_cat(k->name, "*");
            knob_snd(k, kp, snd);
            k->hot = lock_find(l, kp) >= 0;
            return;
        }
        if (ui.mode == M_SEQ && pg == 1) {   /* KICK 2: the step's condition and chance */
            const step_t *s = &p->st[hs];
            switch (i) {
            case 0:
                text_cat(k->name, "COND");
                text_cat(k->val, COND_NAME[STEP_COND(s)]);
                k->pos = (int)STEP_COND(s) * 1000 / (NCOND - 1);
                k->hot = STEP_COND(s) != 0u;
                break;
            case 1:
                text_cat(k->name, "PROB");
                cat_int(k->val, 100 - p->prb[hs]);
                text_cat(k->val, "%");
                k->pos = (100 - p->prb[hs]) * 10;
                k->hot = p->prb[hs] != 0u;
                break;
            default: text_cat(k->name, "-"); text_cat(k->val, "-"); break;
            }
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
        if (ui.mode == M_HOME)           /* HOME: nothing to turn */
            break;
        step_sound(p, 0, snd);
        knob_snd(k, knob_kp(i), snd);
        break;
    case M_SEQ:                          /* (KICK: the step's own, above) */
        break;
    case M_GEN:
        if (pg == AP_TRIG) {
            switch (i) {
            case 0: text_cat(k->name, "LENGTH"); cat_int(k->val, p->len); k->pos = (p->len - 1) * 1000 / (NST - 1); break;
            case 1: text_cat(k->name, "DIVISION"); text_cat(k->val, DIV_NAME[p->div]); k->pos = p->div * 1000 / (NDIV - 1); break;
            case 2: text_cat(k->name, "DIRECTION"); text_cat(k->val, DIR_NAME[p->dir]); k->pos = p->dir * 1000 / (NDIR - 1); break;
            case 3:                      /* NUDGE: the whole sequence a step a click, round; the count since here */
                text_cat(k->name, "NUDGE");
                if (ui.nudge > 0)
                    text_cat(k->val, "+");
                cat_int(k->val, ui.nudge);
                k->pos = 500 + ui.nudge * 500 / 32;
                k->bip = 1;
                break;
            }
            break;
        } else if (pg == AP_FEEL) {
            switch (i) {
            case 0: text_cat(k->name, "TEMPO"); dec1_text(set.bpm10, k->val); k->pos = (set.bpm10 - 400) * 1000 / 2000; break;
            case 1: text_cat(k->name, "PROB"); cat_int(k->val, p->prob); text_cat(k->val, "%"); k->pos = p->prob * 10; break;
            case 2: text_cat(k->name, "HUMANIZE"); cat_int(k->val, p->human); text_cat(k->val, "%"); k->pos = p->human * 10; break;
            case 3: text_cat(k->name, "SWING"); cat_int(k->val, p->swing); text_cat(k->val, "%"); k->pos = p->swing * 10; break;
            }
            break;
        }
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
            case 0:
                text_cat(k->name, "PATTERN");
                k->val[0] = (char)('A' + PBANK);
                k->val[1] = 0;
                if (sq.cur < 9u)
                    text_cat(k->val, "0");
                cat_int(k->val, sq.cur + 1);
                k->pos = sq.cur * 1000 / 15;
                break;
            case 1: text_cat(k->name, "ACTION"); text_cat(k->val, ACT_NAME[ui.act]); k->pos = ui.act * 1000 / (NACT - 1); break;
            case 2: text_cat(k->name, "-"); text_cat(k->val, "-"); break;
            case 3: text_cat(k->name, "DO IT"); text_cat(k->val, ">>"); k->pos = 0; break;
            }
        } else if (pg == GP_PROJ) {
            switch (i) {
            case 0: text_cat(k->name, "PROJECT"); cat_int(k->val, ui.pj + 1); k->pos = ui.pj * 1000 / (NPROJ - 1); break;
            case 1: text_cat(k->name, "ACTION"); text_cat(k->val, PJ_ACT[ui.pj_act]); k->pos = ui.pj_act * 500; break;
            case 2: text_cat(k->name, "-"); text_cat(k->val, "-"); break;
            case 3: text_cat(k->name, "DO IT"); text_cat(k->val, ui.pj_arm ? "AGAIN" : ">>"); k->pos = 0; k->hot = ui.pj_arm; break;
            }
        } else if (pg == GP_MIDI) {
            switch (i) {
            case 0: text_cat(k->name, "IN CH"); if (set.mch) cat_int(k->val, set.mch); else text_cat(k->val, "ALL"); k->pos = set.mch * 1000 / 16; break;
            case 1: text_cat(k->name, "CLOCK"); text_cat(k->val, set.clk ? "MIDI" : "INT"); k->pos = set.clk * 1000; break;
            case 2: text_cat(k->name, "CLOCK OUT"); text_cat(k->val, set.clkout ? "ON" : "OFF"); k->pos = set.clkout * 1000; break;
            case 3: text_cat(k->name, "NOTES OUT"); text_cat(k->val, set.notes_out ? "ON" : "OFF"); k->pos = set.notes_out * 1000; break;
            }
        } else if (pg == GP_AUDIO) {
            static const char *const NM[3] = {"USB", "JACK", "SPEAKER"};
            static const uint8_t BIT[3] = {OUT_USB, OUT_JACK, OUT_SPK};
            if (i < 3) {
                int on = !(set.outs & BIT[i]);
                text_cat(k->name, NM[i]);
                text_cat(k->val, on ? "ON" : "OFF");
                k->pos = on * 1000;
                k->hot = !on;
            } else {
                text_cat(k->name, "-");
                text_cat(k->val, "-");
            }
        } else if (pg == GP_POP) {
            static const char *const NM[4] = {"SEQ", "LFO", "EDIT", "ARP"};
            int on = POPS >> i & 1u;
            text_cat(k->name, NM[i]);
            text_cat(k->val, on ? "CLEAR" : "SOLID");
            k->pos = on * 1000;
        } else if (pg == GP_SYS) {
            if (i == 0) {
                text_cat(k->name, "PEAK");
                cat_int(k->val, ui.cpu_peak);
                text_cat(k->val, "%");
                k->pos = ui.cpu_peak * 10;
            } else {
                text_cat(k->name, "-");
                text_cat(k->val, "-");
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
    case M_HOME: text_cat(b, "HOME"); break;
    case M_SEQ: text_cat(b, ui.page[M_SEQ] ? "KICK 2" : "KICK"); break;
    case M_GEN: text_cat(b, pg == AP_TRIG ? "TRIG" : pg == AP_FEEL ? "FEEL" : "GENERATE"); break;
    case M_SND: text_cat(b, pg == PG_STORE ? "STORE" : SND_PAGE[pg]); break;
    case M_MOD: text_cat(b, "LFO "); cat_int(b, pg / 2 + 1); text_cat(b, pg % 2 ? " B" : " A"); break;
    case M_FX: text_cat(b, pg ? "FINAL " : "LIVE "); text_cat(b, FX_SHORT[ui.fxsel[pg]]); break;
    case M_PAT: text_cat(b, pg == GP_PAT ? "PATTERNS" : pg == GP_PROJ ? "PROJECTS" : pg == GP_MIDI ? "MIDI" : pg == GP_THEME ? "THEMES" : pg == GP_AUDIO ? "AUDIO" : pg == GP_SYS ? "SYSTEM" : "POPUPS"); break;
    }
    return b;
}

static int step_val(int v, int d, int lo, int hi) { return clamp(v + d, lo, hi); }
static void pat_pick(int n);
static void bank_switch(int b);
static void pat_action(void);
static void vbank_show(int b);
static int play_bank(void);
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

/* FEEL's NUDGE: the whole sequence a step later (d > 0) or earlier a click, round within its length: each step's
 * trig, pitch, ratchet, slide, sweep and locks go with it, and a generator's steps turn with them (p->shift). The LFO
 * lanes stay */
static void nudge(pat_t *p, int d)
{
    uint32_t L = p->len, n = (uint32_t)(d < 0 ? -d : d), j, lk;
    if (L < 2u)
        return;
    lk = plat_lock();                    /* (the player reads the steps) */
    for (j = 0; j < n; j++) {
        step_t s0;
        uint8_t k0, b0;
        uint32_t i;
        if (d > 0) {
            s0 = p->st[L - 1u];
            k0 = p->kit[L - 1u];
            b0 = p->prb[L - 1u];
            for (i = L - 1u; i > 0u; i--) {
                p->st[i] = p->st[i - 1u];
                p->kit[i] = p->kit[i - 1u];
                p->prb[i] = p->prb[i - 1u];
            }
            p->st[0] = s0;
            p->kit[0] = k0;
            p->prb[0] = b0;
            p->shift = (uint8_t)((p->shift + 1u) % L);
        } else {
            s0 = p->st[0];
            k0 = p->kit[0];
            b0 = p->prb[0];
            for (i = 0; i + 1u < L; i++) {
                p->st[i] = p->st[i + 1u];
                p->kit[i] = p->kit[i + 1u];
                p->prb[i] = p->prb[i + 1u];
            }
            p->st[L - 1u] = s0;
            p->kit[L - 1u] = k0;
            p->prb[L - 1u] = b0;
            p->shift = (uint8_t)((p->shift % L + L - 1u) % L);
        }
        for (i = 0; i < NLP; i++)        /* the pool's steps, within the length, round with them */
            if (p->lp[i].st && p->lp[i].st <= L)
                p->lp[i].st = (uint8_t)((p->lp[i].st - 1u + (d > 0 ? 1u : L - 1u)) % L + 1u);
    }
    plat_unlock(lk);
    ui.nudge = (int8_t)clamp(ui.nudge + d, -99, 99);
    sq.gen_dirty = 1;
    if (!sq.running && p->algo != AL_MAN)
        gen_run(p, 0);
    changed();
}

/* OCT- + LFO: the four LFOs at random, gently (two or three lanes, smooth shapes, slow, shallow, on the tone);
 * OCT+ + LFO: wildly (all four, any shape, target, speed and depth) */
static void random_lfo(int wild)
{
    static const uint8_t SOFT[] = {KP_DEC, KP_CA, KP_CB, KP_CC, KP_CD, KP_PUNCH, KP_CLVL, KP_DRIVE, KP_CUT, KP_NLVL,
                                   KP_TIME, KP_EQHI};
    static const uint8_t SOFT_SH[] = {SH_SINE, SH_TRI, SH_UP, SH_DOWN, SH_WALK};
    uint32_t *r = &rng_ui;
    pat_t *p = P();
    int m, on = wild ? NMOD : rr(r, 2, 3);
    for (m = 0; m < NMOD; m++) {
        mod_t *x = &p->mod[m];
        uint32_t i;
        mod_init(x);
        if (m >= on)
            continue;
        if (wild) {
            x->tgt = (uint8_t)rr(r, 1, KP_N);
            x->shape = (uint8_t)rr(r, 0, NSHAPE - 1);
            x->len = (uint8_t)rr(r, 2, NST);
            x->div = (uint8_t)rr(r, 0, NDIV - 1);
            x->dir = (uint8_t)rr(r, 0, NDIR - 1);
            x->smooth = (uint8_t)(rr(r, 0, 3) ? 0 : rr(r, 0, 80));
            x->depth = (int8_t)(rr(r, 40, 100) * (rr(r, 0, 1) ? 1 : -1));
            for (i = 0; i < NST; i++)
                x->val[i] = (int8_t)rr(r, -64, 63);
        } else {
            x->tgt = (uint8_t)(1 + SOFT[rr(r, 0, (int)sizeof SOFT - 1)]);
            x->shape = SOFT_SH[rr(r, 0, (int)sizeof SOFT_SH - 1)];
            x->len = (uint8_t)(rr(r, 0, 1) ? 16 : rr(r, 0, 1) ? 8 : 32);
            x->div = (uint8_t)(rr(r, 0, 2) ? 2 : 3);
            x->dir = 0;
            x->smooth = (uint8_t)rr(r, 20, 60);
            x->depth = (int8_t)(rr(r, 8, 30) * (rr(r, 0, 1) ? 1 : -1));
            for (i = 0; i < NST; i++)
                x->val[i] = (int8_t)rr(r, -32, 32);
        }
        x->seed = (uint8_t)rr(r, 0, 127);
    }
    app_say("RANDOM LFO", wild ? "WILD" : "GENTLE");
    changed();
    ui.redraw_c = ui.redraw_f = 1;
}

/* every click shows a new value: a knob whose 0..127 shows fewer choices (a ratio, a curve, a division ...) goes
 * on, a unit at a time, until what it shows changes (or it stops at an end). Not for the knobs that do something
 * on a turn (STORE, PATTERNS, AUDIO, SYSTEM) */
static void knob_turn_raw(int i, int d);
static int text_same(const char *a, const char *b)   /* (no strcmp on the device) */
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}
static void knob_turn(int i, int d)
{
    int n = d < 0 ? -d : d, u = d < 0 ? -1 : 1, j, guard;
    knob_t a, b;
    if (ui.mode == M_PAT || (ui.mode == M_SND && ui.page[M_SND] == PG_STORE)) {
        knob_turn_raw(i, d);
        return;
    }
    for (j = 0; j < n; j++) {
        knob_info(i, &a);
        for (guard = 0; guard < 128; guard++) {
            knob_turn_raw(i, u);
            knob_info(i, &b);
            if (!text_same(a.val, b.val) || !text_same(a.name, b.name))
                break;
        }
    }
}

static void knob_turn_raw(int i, int d)
{
    pat_t *p = P();
    int pg = ui.page[ui.mode], hs = ed_step(), d8 = fast() ? d * 8 : d;
    if (ui.mode == M_HOME)               /* HOME: no knobs */
        return;
    ui.pop = (int8_t)i;
    ui.pop_until = plat_ms() + 550u;
    ui.redraw_c = ui.redraw_f = 1;       /* (the knobs below too, whatever the turn did) */
    if (motion_rec() && sq.running && knob_kp(i) >= 0) {   /* REC, playing: the knob moves on from where it stood */
        int kp = knob_kp(i), dd = kp_hi(kp) - kp_lo(kp) > 10 ? d8 : d, v;
        if (rec_pat != sq.cur)
            rec_knobs_clear();
        if (rec_val[kp] < 0) {
            float snd[KP_N];
            step_sound(p, hs >= 0 ? LKV(p, hs) : 0, snd);
            rec_val[kp] = (int16_t)snd[kp];
        }
        v = step_val(rec_val[kp], dd, kp_lo(kp), kp_hi(kp));
        rec_val[kp] = (int16_t)v;
        if (hs >= 0) {                   /* (no kick sounded yet: the knob stands there for the first one turned on) */
            if (lk_set(p, hs, kp, v)) {
                uint32_t lk = plat_lock();
                if (hit_has && sq.hit_step == hs)
                    lk_get(p, hs, &hit_lk);   /* heard in the kick sounding */
                plat_unlock(lk);
                changed();
            } else {
                app_say("LOCKS FULL", lk_total(p) >= NLP ? "THE PATTERN'S" : "THE STEP'S");
            }
        }
        return;
    }
    if (hs == -2)
        return;
    if (hs >= 0 && ui.mode != M_HOME && ui.mode != M_PAT && ui.mode != M_FX && !(ui.mode == M_SND && pg == PG_STORE)) {
        ui.held_turned = 1;
        if (ui.mode == M_MOD) {
            mod_t *m = &p->mod[pg / 2];
            m->val[hs] = (int8_t)step_val(m->val[hs], d8, -64, 63);
        } else if (knob_kp(i) >= 0) {     /* a p-lock, from what the step plays now */
            int kp = knob_kp(i), dd = kp_hi(kp) - kp_lo(kp) > 10 ? d8 : d;
            float snd[KP_N];
            step_sound(p, LKV(p, hs), snd);
            if (!lk_set(p, hs, kp, step_val((int)snd[kp], dd, kp_lo(kp), kp_hi(kp))))
                app_say("LOCKS FULL", lk_total(p) >= NLP ? "THE PATTERN'S" : "THE STEP'S");
            if (!p->st[hs].trig && !motion_rec())
                p->st[hs].trig = 1;
        } else if (ui.mode == M_SEQ && pg == 1) {   /* KICK 2: the step's condition, its chance (5 % a click) */
            step_t *s = &p->st[hs];
            if (i == 0)
                s->fl = (uint8_t)((s->fl & ~(15u << FL_CSH)) | (uint32_t)step_val((int)STEP_COND(s), d, 0, NCOND - 1) << FL_CSH);
            else if (i == 1)
                p->prb[hs] = (uint8_t)(100 - step_val(100 - p->prb[hs], fast() ? d : d * 5, 0, 100));
            else
                return;
        } else {
            step_t *s = &p->st[hs];
            switch (i) {
            case 0: s->pit = (int8_t)step_val(s->pit, fast() ? d * 12 : d, -24, 24); break;
            case 1: s->rat = (uint8_t)step_val(s->rat, d, 0, 3); break;   /* (four values: no faster) */
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
            case 0: ui.slot = (uint8_t)step_val(ui.slot, acc(d, 0, NUSER - 1), 0, NUSER - 1); name_from_slot(); break;
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
        if (kp < 0)                      /* HOME: nothing to turn */
            return;
        p->snd[kp] = (uint8_t)step_val(p->snd[kp], kp_hi(kp) - kp_lo(kp) > 10 ? d8 : d, kp_lo(kp), kp_hi(kp));
        changed();
        break;
    }
    case M_SEQ:                          /* (KICK: the step's own, above) */
        break;
    case M_GEN:
        if (pg == AP_TRIG) {
            switch (i) {
            case 0:
                p->len = (uint8_t)step_val(p->len, acc(d, 1, NST), 1, NST);
                if (p->len <= 16u)
                    ui.bank = 0;
                break;
            case 1: p->div = (uint8_t)step_val(p->div, d, 0, NDIV - 1); break;
            case 2: p->dir = (uint8_t)step_val(p->dir, d, 0, NDIR - 1); break;
            case 3: nudge(p, d); return;
            }
            sq.gen_dirty = 1;
            if (!sq.running && p->algo != AL_MAN)
                gen_run(p, 0);
            changed();
            break;
        }
        if (pg == AP_FEEL) {
            switch (i) {
            case 0: set.bpm10 = (uint16_t)step_val(set.bpm10, d8 * 10, 400, 2400); set_changed(); break;
            case 1: p->prob = (uint8_t)step_val(p->prob, d8, 0, 100); changed(); break;
            case 2: p->human = (uint8_t)step_val(p->human, d8, 0, 100); changed(); break;
            case 3: p->swing = (uint8_t)step_val(p->swing, d8, 0, 100); changed(); break;
            }
            break;
        }
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
            case 0: m->tgt = (uint8_t)step_val(m->tgt, acc(d, 0, KP_N), 0, KP_N); break;
            case 1: m->depth = (int8_t)step_val(m->depth, fast() ? d * 10 : d, -100, 100); break;
            case 2: m->shape = (uint8_t)step_val(m->shape, d, 0, NSHAPE - 1); break;
            case 3: m->len = (uint8_t)step_val(m->len, acc(d, 1, NST), 1, NST); break;
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
                pat_pick(step_val(sq.next != 0xFFu ? sq.next : sq.cur, acc(d, 0, NPAT - 1), 0, NPAT - 1));
                ui.vbank = PBANK;
            } else if (i == 1) {
                ui.act = (uint8_t)step_val(ui.act, d, 0, NACT - 1);
                ui.redraw_f = 1;
            } else if (i == 3 && d > 0) {
                pat_action();
            }
        } else if (pg == GP_PROJ) {
            if (i == 0) {
                ui.pj = (uint8_t)step_val(ui.pj, d, 0, NPROJ - 1);
                ui.pj_arm = 0;
            } else if (i == 1) {
                ui.pj_act = (uint8_t)step_val(ui.pj_act, d, 0, 2);
                ui.pj_arm = 0;
            } else if (i == 3 && d > 0) {
                pj_action();
            }
            ui.redraw_c = ui.redraw_f = 1;
        } else if (pg == GP_MIDI) {
            switch (i) {
            case 0: set.mch = (uint8_t)step_val(set.mch, acc(d, 0, 16), 0, 16); break;
            case 1: set.clk = (uint8_t)step_val(set.clk, d, 0, 1); sq.ext_per = 0.0f; sq.ext_bpm10 = 0; break;
            case 2: set.clkout = (uint8_t)step_val(set.clkout, d, 0, 1); break;
            case 3: set.notes_out = (uint8_t)step_val(set.notes_out, d, 0, 1); break;
            }
            set_changed();
        } else if (pg == GP_AUDIO) {     /* AUDIO: each output on or off */
            static const uint8_t BIT[3] = {OUT_USB, OUT_JACK, OUT_SPK};
            if (i < 3) {
                if (d > 0)
                    set.outs &= (uint8_t)~BIT[i];
                else
                    set.outs |= BIT[i];
                set_changed();
            }
        } else if (pg == GP_POP) {       /* POPUPS: right see-through, left solid */
            if (d > 0)
                POPS |= (uint8_t)(1u << i);
            else
                POPS &= (uint8_t)~(1u << i);
            set_changed();
        } else if (pg == GP_SYS) {       /* SYSTEM: K1 starts the peak again */
            if (i == 0)
                ui.cpu_peak = 0;
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

/* PATTERNS, a black key 1-3: another bank. What changed in this one is written first (a moment of silence if it
 * plays: the flash stops everything while it writes), then the other is read in; the pattern number stays */
static void bank_switch(int b)
{
    static const char *const BN[NBANK] = {"A", "B", "C"};
    uint32_t i, lk;
    int ok;
    if (b == (int)PBANK) {
        app_say("BANK", BN[b]);
        return;
    }
    for (i = 0; i < 4u; i++)             /* the bank left: what changed, and what was never written (so the grid can show
                                          * it from the flash) */
        if ((ui.pat_dirty & 1u << i) || plat_store_size(PAT_OBJ(i)) < 0)
            plat_store_save(PAT_OBJ(i), &pk[i], sizeof pk[i]);
    ui.pat_dirty = 0;
    lk = plat_lock();                    /* (the player reads the patterns) */
    PBANK = (uint8_t)b;
    ui.vbank = (uint8_t)b;
    ok = pats_load();
    if (!ok && b == 0)                   /* A never written: its factory patterns */
        pats_defaults();
    else if (!ok)
        pats_blank();
    sq.next = 0xFFu;
    sq.gen_dirty = 1;
    plat_unlock(lk);
    if (!ok)                             /* a new bank: it goes to the flash too */
        ui.pat_dirty = 0x0Fu;
    if (!sq.running)
        gen_run(P(), 0);
    if (P()->len <= 16u)
        ui.bank = 0;
    set_changed();
    app_say("BANK", BN[b]);
    ui.redraw_h = ui.redraw_c = ui.redraw_f = 1;
}

/* ------------------------------------------------------------------ projects --- */
/* A project: the three banks' 48 patterns, the tempo, the effects, the pattern and bank in use; nine pooled objects
 * (app.h PJ_OBJ): a head (prj_t), then the patterns six an object. Saved and loaded with the sequencer stopped (the
 * flash writes stop the sound). The user kicks stay apart: every project shares them. */
#define PRJ_MAGIC 0x314A504Bu            /* "KPJ1" */
typedef struct {
    uint32_t magic;
    uint16_t bpm10;
    uint8_t cur, pbank;                  /* the pattern and the bank in use */
    uint8_t npat;                        /* the patterns with steps on (or a generator), for the page */
    uint8_t fmt;                         /* the patterns' layout: 1 KPT4 (0.4.7); 0 KPT3 (0.4.4-0.4.6: pat_from3) */
    uint8_t rsv[2];
    fxset_t fx;
} prj_t;
static pat_t pj_six[6] PLAT_POOL;        /* one object of a project's patterns */
static pats_t pj_four PLAT_POOL;         /* one object of a bank's */
static struct { uint8_t used, npat, pbank; uint16_t bpm10; } pj_info[NPROJ];
static uint8_t bk_hold;                  /* a backup to the computer is streaming: nothing is saved meanwhile */
static int pj_head(int p, prj_t *h)      /* 1: project p is there */
{
    return plat_store_load(PJ_OBJ(p, 0), h, sizeof *h) == (int)sizeof *h && h->magic == PRJ_MAGIC && h->pbank < NBANK &&
           h->cur < NPAT && h->bpm10 >= 400u && h->bpm10 <= 2400u;
}
static void pj_scan(void)                /* the page's list */
{
    int p;
    for (p = 0; p < NPROJ; p++) {
        prj_t h;
        pj_info[p].used = (uint8_t)pj_head(p, &h);
        pj_info[p].npat = pj_info[p].used ? h.npat : 0;
        pj_info[p].pbank = pj_info[p].used ? h.pbank : 0;
        pj_info[p].bpm10 = pj_info[p].used ? h.bpm10 : 0;
    }
}
static int pat_used(const pat_t *p)      /* a pattern with something to play */
{
    uint32_t i;
    if (p->algo != AL_MAN)
        return 1;
    for (i = 0; i < NST; i++)
        if (p->st[i].trig)
            return 1;
    return 0;
}
static void pat_plain(pat_t *p, int i)   /* a pattern of a bank never saved: as a new bank's */
{
    pat_clear(p);
    pat_sound(p, i % NFAC);
    pat_floor(p);
}
static int pj_save(int p)
{
    prj_t h;
    int j, m, cached = -1, ok = 0, rc = 0, n = 0;
    for (j = 1; j < PJ_NOBJ; j++) {
        for (m = 0; m < 6; m++) {
            int g = (j - 1) * 6 + m, b = g / NPAT, w = g % NPAT;
            const pat_t *src = 0;
            if (b == (int)PBANK) {
                src = &pats(w);
            } else {
                int key = b * 4 + w / 4;
                if (cached != key) {
                    cached = key;
                    ok = plat_store_load(PAT_OBJ_ALL[key], &pj_four, sizeof pj_four) == (int)sizeof pj_four &&
                         pats_take(&pj_four);
                }
                if (ok && pat_valid(&pj_four.p[w % 4]))
                    src = &pj_four.p[w % 4];
            }
            if (src)
                pj_six[m] = *src;
            else
                pat_plain(&pj_six[m], w);
            n += pat_used(&pj_six[m]);
        }
        rc |= plat_store_save(PJ_OBJ(p, j), pj_six, sizeof pj_six);
    }
    h.magic = PRJ_MAGIC;
    h.bpm10 = set.bpm10;
    h.cur = sq.cur;
    h.pbank = PBANK;
    h.npat = (uint8_t)n;
    h.fmt = 1;
    h.rsv[0] = h.rsv[1] = 0;
    h.fx = set.fx;
    if (!rc)                             /* the head last: a cut save leaves the project as it was, or as before */
        rc |= plat_store_save(PJ_OBJ(p, 0), &h, sizeof h);
    if (!rc) {
        set.rsv2[2] = (uint8_t)((set.rsv2[2] & ~7u) | (uint32_t)(p + 1));
        set_changed();
    }
    return rc;
}
static int pj_load(int p)
{
    prj_t h;
    int b, k, m, cached = -1;
    uint32_t lk;
    if (!pj_head(p, &h))
        return -1;
    for (b = 0; b < NBANK; b++)          /* each bank's four objects from the project's */
        for (k = 0; k < 4; k++) {
            pj_four.magic = PATS_MAGIC;
            for (m = 0; m < 4; m++) {
                int g = b * NPAT + k * 4 + m, j = 1 + g / 6;
                if (cached != j) {
                    cached = j;
                    if (plat_store_load(PJ_OBJ(p, j), pj_six, sizeof pj_six) != (int)sizeof pj_six)
                        return -2;
                }
                pj_four.p[m] = pj_six[g % 6];
                if (h.fmt != 1)
                    pat_from3(&pj_four.p[m]);
                if (!pat_valid(&pj_four.p[m]))
                    pat_plain(&pj_four.p[m], g % NPAT);
            }
            if (plat_store_save(PAT_OBJ_ALL[b * 4 + k], &pj_four, sizeof pj_four))
                return -3;
        }
    lk = plat_lock();
    PBANK = h.pbank;
    if (!pats_load())
        pats_blank();
    sq.cur = h.cur;
    sq.next = 0xFFu;
    sq.gen_dirty = 1;
    set.cur = h.cur;
    set.bpm10 = h.bpm10;
    set.fx = h.fx;
    fx_latch = 0;
    plat_unlock(lk);
    ui.pat_dirty = 0;
    tempo_set();
    gen_run(P(), 0);
    if (P()->len <= 16u)
        ui.bank = 0;
    set.rsv2[2] = (uint8_t)((set.rsv2[2] & ~7u) | (uint32_t)(p + 1));
    set_changed();
    if (plat_store_save(OBJ_SET, &set, sizeof set) == 0)   /* now: the banks on the flash are the project's already */
        ui.set_dirty = 0;
    return 0;
}
static void pj_clear(int p)
{
    int j;
    for (j = 0; j < PJ_NOBJ; j++)
        plat_store_save(PJ_OBJ(p, j), &j, 0);
    if (PJ_CUR == (uint32_t)p + 1u) {
        set.rsv2[2] &= (uint8_t)~7u;
        set_changed();
    }
}
static void pj_clean(void)               /* once: what another firmware left where the projects go, dropped */
{
    int p, j;
    if (set.rsv2[2] & PJ_CLEAN)
        return;
    for (p = 0; p < NPROJ; p++) {
        prj_t h;
        if (pj_head(p, &h))
            continue;
        for (j = 0; j < PJ_NOBJ; j++)
            if (plat_store_size(PJ_OBJ(p, j)) >= 0)
                plat_store_save(PJ_OBJ(p, j), &j, 0);
    }
    set.rsv2[2] |= PJ_CLEAN;
    set_changed();
}
void app_init(void)
{
    plat_cpu2_set(1);                    /* the second core: REVERB's hall (fx_begin); one core if it cannot */
    load_all();
    pj_clean();
    kdsp_init(&voice, 0xC0FFEEu);
    fx_reset();
    sq.note_out = -1;
    seq_reset();
    tempo_set();
    eff_update();
}

/* backup and restore over USB (device/backup.c: the mirror page) */
int app_backup_playing(void) { return sq.running; }
void app_backup_flush(void)              /* a backup starts: what is not saved yet, saved */
{
    uint32_t i;
    for (i = 0; i < 4u; i++)
        if (ui.pat_dirty & 1u << i)
            plat_store_save(PAT_OBJ(i), &pk[i], sizeof pk[i]);
    if (ui.user_dirty)
        plat_store_save(OBJ_USER, &us, sizeof us);
    if (ui.set_dirty)
        plat_store_save(OBJ_SET, &set, sizeof set);
    ui.pat_dirty = 0;
    ui.set_dirty = ui.user_dirty = 0;
}
void app_backup_hold(int on) { bk_hold = (uint8_t)(on != 0); }
void app_backup_restored(void)           /* the flash holds the restored set: loaded as at power-on */
{
    uint32_t lk = plat_lock();
    sq.running = 0;
    sq.nev = 0;
    load_all();
    fx_latch = 0;
    plat_unlock(lk);
    ui.pat_dirty = 0;
    ui.set_dirty = ui.user_dirty = 0;
    theme_apply();
    tempo_set();
    gen_run(P(), 0);
    pj_scan();
}

static void pj_action(void)              /* DO IT on PROJECTS: turned once to arm, again within 3 s to do it */
{
    static const char *const PN[NPROJ] = {"PROJECT 1", "PROJECT 2", "PROJECT 3", "PROJECT 4"};
    int p = ui.pj, rc;
    uint32_t now = plat_ms();
    if (sq.running) {
        app_say("STOP FIRST", "PLAY STOPS IT");
        return;
    }
    if (bk_hold) {
        app_say("BACKUP RUNNING", "WAIT");
        return;
    }
    if (ui.pj_act != 1 && !pj_info[p].used) {
        app_say(PN[p], "IS EMPTY");
        return;
    }
    if (!ui.pj_arm || now - ui.pj_arm_t > 3000u) {
        ui.pj_arm = 1;
        ui.pj_arm_t = now;
        app_say(ui.pj_act == 0 ? "LOAD: TURN AGAIN" : ui.pj_act == 1 ? "SAVE: TURN AGAIN" : "CLEAR: TURN AGAIN",
                ui.pj_act == 0 ? "UNSAVED WORK IS LOST" : ui.pj_act == 1 && pj_info[p].used ? "IT IS REPLACED" : PN[p]);
        return;
    }
    ui.pj_arm = 0;
    switch (ui.pj_act) {
    case 0:
        rc = pj_load(p);
        app_say(PN[p], rc == 0 ? "LOADED" : rc == -1 ? "IS EMPTY" : "LOAD FAILED");
        break;
    case 1:
        rc = pj_save(p);
        app_say(PN[p], rc == 0 ? "SAVED" : "SAVE FAILED");
        break;
    default:
        pj_clear(p);
        app_say(PN[p], "CLEARED");
        break;
    }
    pj_scan();
    ui.redraw_h = ui.redraw_c = ui.redraw_f = 1;
}

static void pat_action(void)
{
    pat_t *p = P();
    switch (ui.act) {
    case A_PLAY:                         /* the pattern picked: play it, or stop */
        if (set.clk) {
            app_say("CLOCK", "IS MIDI");
        } else if (sq.running) {
            seq_stop();
            app_say("STOP", "");
        } else {
            seq_start();
            app_say("PLAY", "");
        }
        ui.redraw_h = ui.redraw_c = 1;
        return;
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
/* on EDIT, OCT- + GLO: the kick moved a little, each knob round where it is (the kind, the choices and the level
 * stay); OCT+ + GLO: a new kick at random, everything. The steps, their locks and the LFOs stay */
static void audition(int semis, int vel, int step);
static void random_edit(int wild)
{
    pat_t *p = P();
    int k;
    if (wild) {
        rnd_sound(p, -1);
    } else {
        for (k = 0; k < KP_N; k++) {
            int lo = kp_lo(k), hi = kp_hi(k), span = hi - lo, v;
            if (span <= 12 || k == KP_LEVEL)
                continue;
            v = p->snd[k] + rr(&rng_ui, -span / 9, span / 9);
            p->snd[k] = (uint8_t)clamp(v, lo, hi);
        }
    }
    app_say("RANDOM KICK", wild ? "WILD" : "GENTLE");
    if (!sq.running)
        audition(0, 110, -1);
    changed();
    ui.redraw_c = ui.redraw_f = 1;
}

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
    ui.nudge = 0;                        /* (NUDGE's count starts again on each page) */
    ui.pj_arm = 0;
    if (m == M_PAT && ui.page[M_PAT] == GP_PROJ)
        pj_scan();
    if (m == M_PAT && ui.page[M_PAT] == GP_PAT)
        ui.vbank = PBANK;
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
    if (bk_hold) {
        app_say("BACKUP RUNNING", "WAIT");
        return;
    }
    for (i = 0; i < 4u; i++)
        ok &= plat_store_save(PAT_OBJ(i), &pk[i], sizeof pk[i]) == 0;
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

static void init_all(void);
/* OCT- + HOME: the page you are on, back to how a new pattern has it; on SEQ the sequence is cleared too */
static void seq_wipe(pat_t *p)
{
    steps_clear(p);
    p->algo = AL_MAN;                    /* (a generator's steps would go on playing) */
    p->shift = 0;
    sq.gen_dirty = 1;
}
static void init_page(void)
{
    pat_t *p = P();
    int pg = ui.page[ui.mode];
    uint32_t k;
    switch (ui.mode) {
    case M_HOME:
        app_say("HOME", "NOTHING TO INIT");
        return;
    case M_SEQ:
        seq_wipe(p);
        ui.sel = 0;
        app_say("SEQUENCE", "CLEARED");
        break;
    case M_GEN:
        if (pg == AP_TRIG) {
            p->len = 16;
            p->div = 2;
            p->dir = 0;
            ui.bank = 0;
            sq.gen_dirty = 1;
            app_say("TRIG", "INIT");
            break;
        }
        if (pg == AP_FEEL) {
            p->prob = 100;
            p->human = 0;
            p->swing = 0;
            set.bpm10 = 1280;
            set_changed();
            app_say("FEEL", "INIT");
            break;
        }
        p->algo = AL_MAN;
        p->ga[0] = 34;
        p->ga[1] = 0;
        p->ga[2] = 40;
        p->ga[3] = 0;
        sq.gen_dirty = 1;
        app_say("GENERATE", "INIT");
        break;
    case M_SND: {
        uint8_t d[KP_N];
        if (pg >= PG_STORE) {
            app_say("STORE", "NOTHING TO INIT");
            return;
        }
        pre_sound(0, d);                 /* the first factory kick's values for this page's four */
        for (k = 0; k < 4u; k++)
            p->snd[pg * 4 + (int)k] = d[pg * 4 + (int)k];
        app_say(SND_PAGE[pg], "INIT");
        break;
    }
    case M_MOD:
        mod_init(&p->mod[pg / 2]);
        app_say("LFO", "INIT");
        break;
    case M_FX: {
        fxset_t d;
        int f, a = pg == 0 ? 0 : FX_NLIVE, b = pg == 0 ? FX_NLIVE : FX_N;
        fx_defaults(&d);
        for (f = a; f < b; f++) {
            for (k = 0; k < 4u; k++)
                set.fx.p[f][k] = d.p[f][k];
            set.fx.on = (set.fx.on & ~(1u << f)) | (d.on & (1u << f));
        }
        if (pg == 0)
            fx_latch = 0;
        set_changed();
        app_say(pg == 0 ? "LIVE FX" : "FINAL FX", "INIT");
        ui.redraw_c = ui.redraw_f = 1;
        return;
    }
    case M_PAT:
        switch (pg) {
        case GP_PAT: init_all(); return;
        case GP_MIDI: set.mch = 0; set.clk = 0; set.clkout = 0; set.notes_out = 1; sq.ext_per = 0.0f; sq.ext_bpm10 = 0; break;
        case GP_THEME:
            set.theme = 0;
            set.dial = THEMES[0].dial;
            set.scope = THEMES[0].scope;
            set.steps = THEMES[0].steps;
            theme_apply();
            lcd_fill(0, 0, 240, 240, K_BG);
            ui.redraw_h = 1;
            break;
        case GP_AUDIO: set.outs = 0; break;
        case GP_POP: POPS = 0; break;
        case GP_PROJ: ui.pj = 0; ui.pj_act = 0; ui.pj_arm = 0; break;
        default: ui.cpu_peak = 0; break;
        }
        set_changed();
        {
            static const char *const GN[7] = {"PATTERNS", "PROJECTS", "MIDI", "THEMES", "POPUPS", "AUDIO", "SYSTEM"};
            app_say(GN[pg], "INIT");
        }
        ui.redraw_c = ui.redraw_f = 1;
        return;
    }
    if (!sq.running)
        gen_run(p, 0);
    changed();
    ui.redraw_c = ui.redraw_f = 1;
}

/* OCT+ + HOME: the whole pattern as new (the first factory kick, no steps, no generator, the LFOs off); off GLO the
 * effects too. Not the other patterns, the user kicks, MIDI, the theme or the outputs */
static void init_all(void)
{
    pat_t *p = P();
    int glo = ui.mode == M_PAT;
    pat_clear(p);
    pat_sound(p, 0);
    sq.gen_dirty = 1;
    ui.sel = 0;
    ui.bank = 0;
    if (!glo) {
        fx_defaults(&set.fx);
        fx_live = fx_latch = 0;
        set_changed();
    }
    if (!sq.running)
        gen_run(p, 0);
    changed();
    {
        char t[16] = "PATTERN ";
        cat_int(t, sq.cur + 1);
        app_say(t, glo ? "INIT" : "INIT + FX");
    }
    ui.redraw_c = ui.redraw_f = 1;
}

static void input(void)
{
    uint32_t keys = plat_keys(), btn = plat_buttons(), k;
    for (k = 0; k < NKEYS; k++) {
        uint32_t bit = 1u << k;
        int w = white_index(k), b = black_index(k);
        if ((keys & bit) && !(ui.keys & bit)) {
            if (rec_on || listen_on) {   /* REC / LISTEN: the whole keyboard, chromatic (C4 is the kick's own note) */
                int vel = (btn >> B_OCTUP & 1u) ? 127 : 100;
                audition((int)k - 7, vel, -1);
                if (rec_on)
                    rec_note((int)k - 7, vel);
            } else if (w >= 0 && ui.mode == M_SEQ && (btn >> B_OCTDN & 1u)) {   /* SEQ, OCT- + a white key: repeat */
                char t[12] = "";
                rpt_key = (int8_t)w;
                sq.rpt_arm = 1;
                sq.rpt_len = (uint8_t)(w + 1);
                cat_int(t, w + 1);
                text_cat(t, w ? " STEPS" : " STEP");
                app_say("REPEAT", t);
                ui.redraw_h = ui.redraw_c = 1;
            } else if (w >= 0 && ui.mode == M_PAT && ui.page[M_PAT] == 0) {
                if (ui.vbank == PBANK) {
                    pat_pick(w);
                } else if (sq.running) {  /* another bank's: the one playing goes on (a copy) to its bar's end */
                    uint32_t lk = plat_lock();
                    play_copy = pats(sq.cur);
                    sq.pcbank = PBANK;
                    sq.pcopy = 1;
                    plat_unlock(lk);
                    bank_switch(ui.vbank);
                    sq.next = (uint8_t)w;
                    ui.redraw_c = 1;
                } else {
                    bank_switch(ui.vbank);
                    pat_pick(w);
                }
            } else if (b >= 0 && b < NBANK && ui.mode == M_PAT && ui.page[M_PAT] == 0) {   /* the banks: shown only */
                static const char *const BN[NBANK] = {"BANK A", "BANK B", "BANK C"};
                vbank_show(b);
                app_say(BN[b], b == play_bank() ? "PLAYING" : "PICK A PATTERN");
                ui.redraw_c = ui.redraw_f = 1;
            } else if (w >= 0 && ui.mode == M_FX && (btn >> B_OCTUP & 1u)) {   /* OCT+ + a key: just pick it */
                int f = ui.page[M_FX] == 0 ? w : FX_NLIVE + w;
                if (ui.page[M_FX] == 0 ? w < FX_NLIVE : w < FX_NFINAL) {
                    ui.fxsel[ui.page[M_FX] ? 1 : 0] = (uint8_t)f;
                    app_say(FX_NAME[f], "EDIT");
                    ui.redraw_c = ui.redraw_f = 1;
                }
            } else if (w >= 0 && ui.mode == M_FX && ui.page[M_FX] == 0 && (btn >> B_OCTDN & 1u)) {   /* OCT-: left on */
                if (w < FX_NLIVE) {
                    fx_latch ^= 1u << w;
                    ui.fxsel[0] = (uint8_t)w;
                    app_say(FX_NAME[w], fx_latch >> w & 1u ? "LATCHED" : "OFF");
                    ui.redraw_c = ui.redraw_f = 1;
                }
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
            if (w >= 0 && w == rpt_key) {   /* the repeat let go: the steps carry on where they would be */
                sq.rpt_len = 0;
                rpt_key = -1;
                ui.redraw_h = ui.redraw_c = 1;
                continue;
            }
            if (w >= 0 && w < FX_NLIVE && (fx_live & 1u << w)) {   /* a LIVE effect let go */
                fx_live &= ~(1u << w);
                ui.redraw_c = 1;
            }
            if (w >= 0 && ui.held == step_of_key(w)) {
                if (!ui.held_turned && !rec_on && !listen_on)
                    step_tap(ui.held);
                ui.held = -1;
                ui.redraw_c = ui.redraw_f = 1;
            }
        }
    }
    if ((btn >> B_OCTUP & 1u) && !(ui.btn >> B_OCTUP & 1u))
        ui.octup_used = 0;
    else if ((btn >> B_OCTUP & 1u) && ((keys & ~ui.keys) || (btn & ~ui.btn)))
        ui.octup_used = 1;
    if (!(btn >> B_OCTUP & 1u) && (ui.btn >> B_OCTUP & 1u) && !ui.octup_used && ui.mode == M_FX) {
        fx_latch = 0;                    /* FX, OCT+ alone (pressed and let go): every effect off */
        fx_live = 0;
        set.fx.on = 0;
        set_changed();
        app_say("ALL EFFECTS", "OFF");
        ui.redraw_h = ui.redraw_c = ui.redraw_f = 1;
    }
    for (k = 0; k < NB; k++) {
        uint32_t bit = 1u << k;
        if (!(btn & bit) || (ui.btn & bit))
            continue;
        switch (k) {
        case B_HOME:
            if (btn >> B_OCTDN & 1u)      /* OCT- + HOME: this page back to its start (SEQ: the sequence too) */
                init_page();
            else if (btn >> B_OCTUP & 1u) /* OCT+ + HOME: the whole pattern (and, off GLO, the effects) */
                init_all();
            else
                go_mode(M_HOME, 0);
            break;
        case B_SEQ: go_mode(M_SEQ, ui.mode == M_SEQ ? -1 : ui.page[M_SEQ]); break;
        case B_ARP: go_mode(M_GEN, ui.mode == M_GEN ? -1 : ui.page[M_GEN]); break;
        case B_EDIT: go_mode(M_SND, ui.mode == M_SND ? -1 : ui.page[M_SND]); break;
        case B_ENV: break;               /* (nothing for now) */
        case B_FX: go_mode(M_FX, ui.mode == M_FX ? -1 : ui.page[M_FX]); break;
        case B_LFO:
            if (btn >> B_OCTDN & 1u)      /* OCT- + LFO: the four LFOs at random, gently */
                random_lfo(0);
            else if (btn >> B_OCTUP & 1u) /* OCT+ + LFO: wildly */
                random_lfo(1);
            else
                go_mode(M_MOD, ui.mode == M_MOD ? (ui.page[M_MOD] / 2 * 2 + 2) % (2 * NMOD) : ui.page[M_MOD]);
            break;
        case B_GLO:
            if ((btn >> B_OCTDN & 1u) && ui.mode == M_SND)   /* on EDIT: the kick only, gently */
                random_edit(0);
            else if ((btn >> B_OCTUP & 1u) && ui.mode == M_SND)   /* on EDIT: the kick only, wildly */
                random_edit(1);
            else if (btn >> B_OCTDN & 1u) /* OCT- + GLO: the kick and the trigs at random */
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
            } else if (btn >> B_OCTDN & 1u) {   /* OCT- + REC: LISTEN, the keyboard plays the kick, nothing recorded */
                listen_on = (uint8_t)!listen_on;
                rec_on = 0;
                ui.held = -1;
                app_say("LISTEN", listen_on ? "ON" : "OFF");
            } else if (ui.held >= 0 && ui.mode != M_PAT && ui.mode != M_MOD && ui.mode != M_FX) {
                char t[12] = "STEP ";
                lk_clear(P(), ui.held);
                ui.held_turned = 1;
                cat_int(t, ui.held + 1);
                app_say(t, "LOCKS CLEARED");
                changed();
            } else {                     /* REC on or off: notes from the keyboard or MIDI go into the steps */
                rec_on = (uint8_t)!rec_on;
                listen_on = 0;
                ui.held = -1;
                rec_last = -1;
                rec_knobs_clear();
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
        ui.octup_used = 1;
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
                pat_pick(clamp((sq.next != 0xFFu ? sq.next : sq.cur) + acc((int)e, 0, NPAT - 1), 0, NPAT - 1));
                ui.vbank = PBANK;
            } else if (ui.mode == M_FX) {    /* FX: the effect the knobs edit (round and round), on or off as it was */
                int pg = ui.page[M_FX], lo = pg ? FX_NLIVE : 0, nn = pg ? FX_NFINAL : FX_NLIVE;
                int f = lo + ((ui.fxsel[pg] - lo + (int)e) % nn + nn) % nn;
                ui.fxsel[pg] = (uint8_t)f;
                app_say(FX_NAME[f], pg ? (set.fx.on >> f & 1u ? "ON" : "OFF") : "");
                ui.redraw_c = ui.redraw_f = 1;
            } else if (ui.mode == M_SEQ || edit_page()) {   /* SEQ, EDIT: the step to edit; round, both ways */
                pat_t *p = P();
                int len = p->len, n = ((ui.sel % len + acc((int)e, 0, NST)) % len + len) % len;
                ui.sel = (int8_t)n;
                ui.bank = (uint8_t)(n / 16);
                if (!sq.running)
                    audition(p->st[n].pit, 110, n);
                ui.redraw_h = ui.redraw_c = ui.redraw_f = 1;
            }                            /* (elsewhere ALGORITHM does nothing: the tempo is on SEQ's FEEL page) */
        } else if (k == EN_PRESET) {
            pat_t *p = P();
            char t[12];
            int d = e > 0 ? 1 : -1, n, j, cnt = e > 0 ? (int)e : -(int)e, hs = ed_step();
            if (fast())
                cnt *= 4;
            if (hs >= 0 && ui.mode != M_PAT && ui.mode != M_MOD && ui.mode != M_FX) {
                uint8_t *kt = &p->kit[hs];   /* a step held: the kick it plays (a p-lock), or the pattern's */
                for (j = 0; j < cnt; j++) {     /* the pattern's kick sits between the last preset and the first */
                    n = *kt ? *kt - 1 : (d > 0 ? NPRE - 1 : 0);
                    n = *kt && ((d > 0 && pre_step(n, 1) <= n) || (d < 0 && pre_step(n, -1) >= n)) ? -1 : pre_step(n, d);
                    *kt = (uint8_t)(n < 0 ? 0 : n + 1);
                }
                ui.held_turned = 1;
                if (!p->st[hs].trig)
                    p->st[hs].trig = 1;
                if (*kt)
                    pre_name(*kt - 1, t);
                app_say("STEP KICK", *kt ? t : "PATTERN'S");
                if (!sq.running)
                    audition(p->st[hs].pit, 110, hs);
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

/* the screen mirror lost a part of the screen (a slow link): all of it again at the next frame */
void app_redraw(void)
{
    ui.redraw_h = ui.redraw_c = ui.redraw_f = 1;
}

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

static void text_ring(int32_t cx, int32_t y, const felucca_font_t *f, const char *s, uint16_t c)
{                                        /* text that stays readable over anything: ringed in the background colour */
    int32_t x = cx - text_w(f, s) / 2, dx, dy;
    for (dy = -1; dy <= 1; dy++)
        for (dx = -1; dx <= 1; dx++)
            if (dx || dy)
                cv_text(x + dx, y + dy, f, s, K_BG);
    cv_text(x, y, f, s, c);
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
    t[0] = (char)('A' + play_bank());
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
    } else if (listen_on) {              /* LISTEN: a ring */
        circle_line(84, 11, 5, K_ACC);
        circle_line(84, 11, 4, K_ACC);
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
        if (seq_cursor() >= 0) {         /* SEQ: the step ALGORITHM picks */
            char st[8] = "S";
            cat_int(st, seq_cursor() + 1);
            cv_text(236 - text_w(&FONT_B, b) - (set.metro ? 22 : 6) - text_w(&FONT_B, st), 3, &FONT_B, st, K_LOCK);
        }
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

/* the step the scope draws: one held (or picked with OCT+); on EDIT, while the sequencer runs, the one playing */
static int scope_step(void)
{
    if (ed_step() >= 0 && ui.mode == M_SND)
        return ed_step();
    if (edit_page() && sq.running && sq.lpos[0] >= 0 && sq.lpos[0] < (int)P()->len)
        return sq.lpos[0];
    return -1;
}
static uint32_t snd_hash(void)
{
    const pat_t *p = P();
    uint32_t h = 2166136261u, k;
    int s = scope_step();
    for (k = 0; k < KP_N; k++)
        h = (h ^ p->snd[k]) * 16777619u;
    if (s >= 0) {
        const uint8_t *b = (const uint8_t *)LKV(p, s);
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
    step_sound(p, s >= 0 ? LKV(p, s) : 0, skp.p);
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
        if (sq.running && sq.lpos[0] == s)  /* playing: a frame round it */
            cv_rect(x - 1, y - 1, 14, 14, K_ACC);
        cv_rect(x, y, 12, 12, tr == 2 ? K_ACC : tr ? K_INK : K_GRID);
        if (seq_cursor() == s)           /* the step ALGORITHM picks: a line under it (OCT+ + a knob locks it) */
            cv_rect(x, y + 14, 12, 3, (ui.btn >> B_OCTUP & 1u) ? K_LOCK : K_INK);
        if (lk_count(p, s))
            cv_rect(x, y - 3, 12, 2, K_LOCK);
        if (ed_step() == s)
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
        x = (int32_t)(tt / ui.sdur * 240.0f);   /* the playhead: a short mark at the top, not a line down the
                                                  * wave (each frame of a line was ~2 KB for the screen mirror) */
        cv_rect(x - 1, 8, 3, 8, K_INK);
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
    if (ed_step() >= 0) {
        const lock_t *l = LKV(P(), ed_step());
        int n = lock_count(l) - (l->kit ? 1 : 0);
        text_cat(t, "S");
        cat_int(t, ed_step() + 1);
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
        int tr = trig_at(p, i), lk = lk_count(p, (int)i);
        uint16_t edge = sq.running && sq.lpos[0] == i ? K_ACC : ed_step() == i || (ui.held < 0 && seq_cursor() == i) ? (ui.held < 0 && ui.mode == M_SEQ ? K_LOCK : K_INK) : K_GRID;
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
    if (ui.mode == M_SEQ && ui.page[M_SEQ] == 1) {   /* KICK 2: the step's condition and chance */
        int st = ed_step();
        text_cat(t, "STEP ");
        cat_int(t, st + 1);
        text_cat(t, "  ");
        text_cat(t, COND_NAME[STEP_COND(&p->st[st])]);
        text_cat(t, "  ");
        cat_int(t, 100 - p->prb[st]);
        text_cat(t, "%");
        text_c(120, 82, &FONT_B, t, STEP_COND(&p->st[st]) || p->prb[st] ? K_LOCK : K_INK);
    } else if (ui.mode == M_SEQ) {       /* KICK: the step's kind of kick */
        int st = ed_step();
        const lock_t *l = LKV(p, st);
        float snd[KP_N];
        step_sound(p, l, snd);
        text_cat(t, "STEP ");
        cat_int(t, st + 1);
        text_cat(t, "  ");
        text_cat(t, TYPE_NAME[(int)(snd[KP_TYPE] + 0.5f)]);
        text_c(120, 82, &FONT_B, t, lock_count(l) ? K_LOCK : K_INK);
    } else if (ed_step() >= 0 && ui.mode == M_GEN && ui.page[M_GEN] != AP_GEN) {
        const step_t *st = &p->st[ed_step()];
        const lock_t *l = LKV(p, ed_step());
        int nl = lock_count(l) - (l->kit ? 1 : 0);
        text_cat(t, "STEP ");
        cat_int(t, ed_step() + 1);
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
    } else if (ui.mode == M_GEN && ui.page[M_GEN] == AP_GEN) {
        text_c(120, 82, &FONT_M, ALGO_NAME[p->algo], p->algo == AL_MAN ? K_MUTE : K_ACC);
    } else if (ui.page[M_GEN] == AP_TRIG) {
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
        if (lk_count(p, (int)i)) {
            int32_t lx, ly;
            step_xy(i, n, R - 18, &lx, &ly);
            if (p->kit[i])
                circle_line(lx, ly, 3, K_LOCK);
            cv_rect(lx - 1, ly - 1, 3, 3, K_LOCK);
        }
        if (sq.running && sq.lpos[0] == i)
            circle_line(x, y, big ? 9 : 6, K_ACC);
        if (ed_step() == i || (ui.held < 0 && seq_cursor() == i))
            circle_line(x, y, big ? 8 : 6, ui.held < 0 && ui.mode == M_SEQ ? K_LOCK : K_INK);
    }
    if (ui.mode == M_SEQ && ui.page[M_SEQ] == 1) {   /* KICK 2: the step's condition and chance */
        int st = ed_step();
        t[0] = 0;
        text_cat(t, "STEP ");
        cat_int(t, st + 1);
        if (!trig_at(p, st))
            text_cat(t, " (OFF)");
        text_c(120, 40, &FONT_XS, t, K_MUTE);
        text_c(120, 56, &FONT_M, COND_NAME[STEP_COND(&p->st[st])], STEP_COND(&p->st[st]) ? K_LOCK : K_INK);
        t[0] = 0;
        cat_int(t, 100 - p->prb[st]);
        text_cat(t, "% CHANCE");
        text_c(120, 90, &FONT_XS, t, p->prb[st] ? K_LOCK : K_MUTE);
    } else if (ui.mode == M_SEQ) {       /* KICK: the step's kind of kick and its locks */
        int st = ed_step(), nl;
        const lock_t *l = LKV(p, st);
        float snd[KP_N];
        step_sound(p, l, snd);
        nl = lock_count(l) - (l->kit ? 1 : 0);
        t[0] = 0;
        text_cat(t, "STEP ");
        cat_int(t, st + 1);
        if (!trig_at(p, st))
            text_cat(t, " (OFF)");
        text_c(120, 40, &FONT_XS, t, K_MUTE);
        text_c(120, 56, &FONT_M, TYPE_NAME[(int)(snd[KP_TYPE] + 0.5f)], lock_find(l, KP_TYPE) >= 0 ? K_LOCK : K_INK);
        t[0] = 0;
        if (l->kit) {
            pre_name(l->kit - 1, t);
            text_cat(t, " ");
        }
        if (nl) {
            text_cat(t, "LOCKS ");
            cat_int(t, nl);
        } else if (!l->kit) {
            text_cat(t, "PATTERN'S KICK");
        }
        text_c(120, 90, &FONT_XS, t, nl || l->kit ? K_LOCK : K_MUTE);
    } else if (ed_step() >= 0 && ui.mode == M_GEN && ui.page[M_GEN] != AP_GEN) {
        const step_t *s = &p->st[ed_step()];
        const lock_t *l = LKV(p, ed_step());
        int nl = lock_count(l) - (l->kit ? 1 : 0);
        t[0] = 0;
        text_cat(t, "STEP ");
        cat_int(t, ed_step() + 1);
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
    } else if (ui.mode == M_GEN && ui.page[M_GEN] == AP_GEN) {
        text_c(120, 52, &FONT_M, ALGO_NAME[p->algo], p->algo == AL_MAN ? K_MUTE : K_ACC);
        text_c(120, 80, &FONT_XS, p->algo == AL_MAN ? "TURN ALGO" : "GENERATOR", K_MUTE);
    } else if (ui.page[M_GEN] == AP_TRIG) {
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
            if (m == f && ed_step() == i)
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
            int lt = fx_latch >> n & 1u, on = (fx_live >> n & 1u) | lt, sel = n == ui.fxsel[0];
            cell(x, y, 56, 34, on ? K_ACC : K_BG, sel ? K_INK : on ? K_ACC : K_GRID, sel);
            if (lt)                      /* left on: a bar at its foot */
                cv_rect(x + 6, y + 29, 44, 2, K_BG);
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
        text_c(120, 122, &FONT_XS, "HOLD A KEY  OCT- + KEY: LATCH", K_MUTE);
        text_c(120, 132, &FONT_XS, "OCT+ + KEY / ALGORITHM: PICK", K_MUTE);
    } else {
        for (n = 0; n < FX_NFINAL; n++) {
            int f = FX_NLIVE + n, on = set.fx.on >> f & 1u, sel = f == ui.fxsel[1];
            int32_t y = 6 + n * 25;
            cell(20, y, 200, 22, on ? K_ACC2 : K_BG, sel ? K_INK : on ? K_ACC : K_GRID, sel);
            cv_text(30, y + 6, &FONT_XS, FX_NAME[f], on ? K_INK : K_MUTE);
            cv_text(210 - text_w(&FONT_XS, on ? "ON" : "OFF"), y + 6, &FONT_XS, on ? "ON" : "OFF", on ? K_ACC : K_DIM);
        }
        text_c(120, 132, &FONT_XS, "KEYS 1-5: ON / OFF   ALGORITHM: PICK", K_MUTE);
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

static pats_t vbk[4] PLAT_POOL;          /* PATTERNS: another bank than the one in RAM, read to be shown */
static int play_bank(void) { return sq.pcopy ? sq.pcbank : (int)PBANK; }
static void vbank_show(int b)            /* PATTERNS: bank b on the grid (from the flash if not the one in RAM) */
{
    uint32_t i, k;
    ui.vbank = (uint8_t)b;
    if (b == (int)PBANK)
        return;
    for (i = 0; i < 4u; i++) {
        int ok = plat_store_load(PAT_OBJ_ALL[b * 4 + (int)i], &vbk[i], sizeof vbk[i]) == (int)sizeof vbk[i] &&
                 pats_take(&vbk[i]);
        for (k = 0; k < 4u; k++)
            if (!ok || !pat_valid(&vbk[i].p[k]))
                pat_plain(&vbk[i].p[k], (int)(i * 4u + k));
    }
}
static const pat_t *vpat(int n) { return ui.vbank == PBANK ? &pats(n) : &vbk[n >> 2].p[n & 3]; }
static void draw_pat(void)
{
    uint32_t now = plat_ms();
    int n, i, here = ui.vbank == play_bank(), nxt = ui.vbank == PBANK;
    uint8_t prev[NST];
    for (n = 0; n < NPAT; n++) {
        const pat_t *p = vpat(n);
        int32_t x = 4 + (n % 4) * 59, y = 4 + (n / 4) * 35;
        int cur = here && n == sq.cur;
        uint16_t bc = cur ? K_ACC : K_GRID;
        char t[12];
        if (cur)
            cv_rect(x, y, 56, 32, K_ACC2);
        cv_rect(x, y, 56, 1, bc);
        cv_rect(x, y + 31, 56, 1, bc);
        cv_rect(x, y, 1, 32, bc);
        cv_rect(x + 55, y, 1, 32, bc);
        if (nxt && n == sq.next && ((now / 250u) & 1u)) {
            cv_rect(x + 1, y + 1, 54, 1, K_INK);
            cv_rect(x + 1, y + 30, 54, 1, K_INK);
        }
        t[0] = (char)('A' + ui.vbank);
        t[1] = (char)('0' + (n + 1) / 10);
        t[2] = (char)('0' + (n + 1) % 10);
        t[3] = 0;
        cv_text(x + 4, y + 1, &FONT_XS, t, cur ? K_INK : K_MUTE);
        cv_text(x + 4, y + 11, &FONT_XS, TYPE_NAME[p->snd[KP_TYPE] % KT_N], K_MUTE);   /* its kind of kick */
        if (p->algo != AL_MAN)           /* a generator plays it */
            cv_text(x + 52 - text_w(&FONT_XS, "G"), y + 1, &FONT_XS, "G", K_ACC);
        if (p->len > 16u)                /* 32 steps */
            cv_text(x + 26, y + 1, &FONT_XS, "32", K_MUTE);
        if (p->algo != AL_MAN && !cur) {   /* a generator's steps, worked out for the grid */
            uint8_t keep_t[NST], keep_r[NST];
            uint32_t lk = plat_lock(), j;
            for (j = 0; j < NST; j++) {
                keep_t[j] = gen_trig[j];
                keep_r[j] = gen_rat[j];
            }
            gen_run(p, 0);
            for (j = 0; j < NST; j++) {
                prev[j] = gen_trig[j];
                gen_trig[j] = keep_t[j];
                gen_rat[j] = keep_r[j];
            }
            plat_unlock(lk);
        }
        for (i = 0; i < p->len; i++) {
            int tr = p->algo == AL_MAN ? p->st[i].trig : (cur ? gen_trig[i] : prev[i]);
            int32_t w = p->len > 16u ? 1 : 2, px = x + 4 + (p->len > 16u ? i * 3 / 2 : i * 3);
            cv_rect(px, y + (tr ? 27 : 30), w, tr ? 4 : 1, tr == 2 ? K_ACC : tr ? K_INK : K_GRID);
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

#ifndef FELUCCA_ID
#define FELUCCA_ID "HOST"                /* (the device's build passes the package identity) */
#endif
#ifndef FM1B_BUILD
#define FM1B_BUILD 0u
#endif

static void draw_audio(void)             /* AUDIO: where the sound goes */
{
    static const char *const NM[3] = {"USB AUDIO", "HEADPHONE JACK", "SPEAKER"};
    static const uint8_t BIT[3] = {OUT_USB, OUT_JACK, OUT_SPK};
    int hp = plat_headphones(), i;
    for (i = 0; i < 3; i++) {
        int on = !(set.outs & BIT[i]);
        int32_t y = 8 + i * 30;
        cv_text(12, y + 4, &FONT_XS, NM[i], K_MUTE);
        cell(150, y, 76, 24, on ? K_ACC : K_BG, on ? K_ACC : K_GRID, 1);
        text_c(188, y + 4, &FONT_B, on ? "ON" : "OFF", on ? K_BG : K_DIM);
    }
    cv_text(12, 102, &FONT_XS, "JACK", K_MUTE);
    cv_text(80, 98, &FONT_B, hp > 0 ? "HEADPHONES IN" : hp == 0 ? "NOTHING IN" : "-", K_INK);
    cv_text(12, 124, &FONT_XS, hp > 0 ? "THE SPEAKER IS OFF WITH HEADPHONES IN" : "KNOBS 1-3: ON / OFF", K_DIM);
}

static void draw_proj(void)              /* PROJECTS: the four slots */
{
    int i;
    char t[24];
    for (i = 0; i < NPROJ; i++) {
        int sel = i == ui.pj, cur = PJ_CUR == (uint32_t)i + 1u;
        int32_t y = 4 + i * 26;
        cell(8, y, 224, 23, K_BG, sel ? K_ACC : K_GRID, sel);
        t[0] = 'P';
        t[1] = (char)('1' + i);
        t[2] = 0;
        cv_text(16, y + 3, &FONT_B, t, sel ? K_ACC : K_INK);
        t[0] = 0;
        if (pj_info[i].used) {
            cat_int(t, pj_info[i].bpm10 / 10);
            text_cat(t, ".");
            cat_int(t, pj_info[i].bpm10 % 10);
            text_cat(t, " BPM  ");
            cat_int(t, pj_info[i].npat);
            text_cat(t, " PATTERNS");
        } else {
            text_cat(t, "EMPTY");
        }
        cv_text(46, y + 6, &FONT_XS, t, pj_info[i].used ? K_INK : K_DIM);
        if (cur)
            cv_text(188, y + 6, &FONT_XS, "IN USE", K_LOCK);
    }
    cv_text(12, 112, &FONT_XS, "THE 3 BANKS, TEMPO AND EFFECTS.", K_DIM);
    cv_text(12, 126, &FONT_XS, "K1 SLOT  K2 LOAD/SAVE/CLEAR  K4 DO IT", K_DIM);
}

static void draw_pops(void)              /* POPUPS: the knob popup on SEQ, LFO, EDIT and ARP, solid or see-through */
{
    static const char *const NM[4] = {"SEQ", "LFO", "EDIT", "ARP"};
    int i;
    for (i = 0; i < 4; i++) {
        int on = POPS >> i & 1u;
        int32_t y = 6 + i * 26;
        cv_text(12, y + 4, &FONT_XS, NM[i], K_MUTE);
        cell(150, y, 76, 22, on ? K_BG : K_ACC, K_ACC, 1);
        text_c(188, y + 3, &FONT_B, on ? "CLEAR" : "SOLID", on ? K_ACC : K_BG);
    }
    cv_text(12, 112, &FONT_XS, "THE KNOB POPUP: SOLID, OR CLEAR", K_DIM);
    cv_text(12, 126, &FONT_XS, "TO SEE THE PAGE UNDER IT. KNOBS 1-4", K_DIM);
}

static void draw_sys(void)               /* SYSTEM: the load and the firmware */
{
    char t[24];
    uint32_t cpu = plat_cpu_pct(), khz = plat_cpu_khz();
    int32_t w = (int32_t)(cpu > 100u ? 100u : cpu) * 136 / 100;
    uint16_t c = cpu > 85u ? K_REC : cpu > 65u ? K_LOCK : K_ACC;
    cv_text(12, 10, &FONT_XS, "CPU", K_MUTE);
    cv_rect(60, 10, 138, 14, K_GRID);
    cv_rect(61, 11, w, 12, c);
    int_text((int32_t)cpu, t);
    text_cat(t, "%");
    cv_text(204, 8, &FONT_B, t, c);
    cv_text(12, 34, &FONT_XS, "PEAK", K_MUTE);
    int_text(ui.cpu_peak, t);
    text_cat(t, "%");
    cv_text(60, 30, &FONT_B, t, ui.cpu_peak > 85u ? K_REC : K_INK);
    cv_text(120, 34, &FONT_XS, "XRUNS", K_MUTE);
    int_text((int32_t)plat_xruns(), t);
    cv_text(172, 30, &FONT_B, t, plat_xruns() ? K_REC : K_INK);
    cv_text(12, 58, &FONT_XS, "CLOCK", K_MUTE);
    t[0] = 0;
    if (khz) {
        cat_int(t, (int)(khz / 1000u));
        text_cat(t, " MHZ");
    } else {
        text_cat(t, "-");
    }
    cv_text(60, 54, &FONT_B, t, K_INK);
    /* the flash: either of the FM-1's two parts (the Puya P25Q80, the UC25HQ80 of the newer units) is known to
     * fm1-base and saves go there; an unknown one keeps everything in RAM until power-off */
    cv_text(146, 58, &FONT_XS, "FLASH", K_MUTE);
    cv_text(190, 54, &FONT_B, plat_data(PLAT_DATA_LO) ? "OK" : "RAM", plat_data(PLAT_DATA_LO) ? K_INK : K_REC);
    cv_text(12, 82, &FONT_XS, "FIRMWARE", K_MUTE);
    t[0] = 0;
    text_cat(t, APP_NAME " " APP_VERSION);
    cv_text(80, 78, &FONT_B, t, K_INK);
    cv_text(12, 106, &FONT_XS, "IDENTITY", K_MUTE);
    cv_text(80, 102, &FONT_B, FELUCCA_ID, K_INK);
    cv_text(166, 106, &FONT_XS, "CORES", K_MUTE);   /* 2: the hall on the second core; 1: it could not run there */
    cv_text(214, 102, &FONT_B, plat_cpu2_on() ? "2" : "1", plat_cpu2_on() ? K_INK : K_LOCK);
    cv_text(12, 126, &FONT_XS, "BUILD", K_MUTE);
    {
        static const char HX[] = "0123456789ABCDEF";
        uint32_t b = FM1B_BUILD, j;
        for (j = 0; j < 8u; j++)
            t[j] = HX[(b >> (28u - 4u * j)) & 15u];
        t[8] = 0;
    }
    cv_text(80, 122, &FONT_B, t, K_DIM);
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
    int clear = POPS & (ui.mode == M_SEQ ? POP_SEQ : ui.mode == M_MOD ? POP_LFO : ui.mode == M_SND ? POP_EDIT :
                        ui.mode == M_GEN ? POP_ARP : 0);
    char title[20];
    knob_info(ui.pop, &k);
    if (!k.name[0] || k.name[0] == '-')
        return;
    title[0] = 0;
    if (motion_rec())                    /* REC: the move goes into a step */
        text_cat(title, "REC ");
    text_cat(title, k.name);
    if (clear) {                         /* see-through: the frame alone, the text ringed in the background colour */
        cv_rect(x0, 18, w0, 1, K_ACC);
        cv_rect(x0, 109, w0, 1, K_ACC);
        cv_rect(x0, 18, 1, 92, K_ACC);
        cv_rect(x0 + w0 - 1, 18, 1, 92, K_ACC);
    } else {
        cell(x0, 18, w0, 92, K_BG, K_ACC, 0);
    }
    if (text_w(f, k.val) > w0 - 12)
        f = &FONT_M;
    if (text_w(f, k.val) > w0 - 12)
        f = &FONT_B;
    if (clear) {
        text_ring(cx, 26, &FONT_B, title, k.hot ? K_LOCK : K_MUTE);
        text_ring(cx, 50, f, k.val, K_INK);
    } else {
        text_c(cx, 26, &FONT_B, title, k.hot ? K_LOCK : K_MUTE);
        text_c(cx, 50, f, k.val, K_INK);
    }
    if (k.pos >= 0) {
        int32_t w = bw * k.pos / 1000, bx = x0 + 16;
        if (clear)
            cv_rect(bx - 1, 97, bw + 2, 6, K_BG);
        cv_rect(bx, 98, bw, 4, K_GRID);
        if (k.bip) {
            int32_t c = bx + bw / 2, e = bx + w;
            cv_rect(c < e ? c : e, 98, (c < e ? e - c : c - e) + 1, 4, K_ACC);
        } else {
            cv_rect(bx, 98, w, 4, K_ACC);
        }
    }
}

#include "glove.h"
static void draw_logo(void)              /* HOME: FM-KICK's glove; each hit throws it up a touch, the punch lines round it */
{
    int32_t gx = 120 - GLOVE_W / 2, gy = 89 - GLOVE_H, k;
    int hit = voice.active && voice.t < 0.12f;
    if (hit) {
        gy -= 3;
        for (k = 0; k < GLOVE_NHIT; k++)
            cv_rect(gx + GLOVE_HIT[k][1], gy + GLOVE_HIT[k][0], GLOVE_HIT[k][2], 1, GLOVE_PAL[GLOVE_HIT[k][3]]);
    }
    for (k = 0; k < GLOVE_NRUN; k++)
        cv_rect(gx + GLOVE_RUN[k][1], gy + GLOVE_RUN[k][0], GLOVE_RUN[k][2], 1, GLOVE_PAL[GLOVE_RUN[k][3]]);
    text_c(120, 92, &FONT_L, APP_NAME, K_INK);
    text_c(120, 126, &FONT_XS, "KICK SYNTH + SEQUENCER  " APP_VERSION, K_MUTE);
}

static void draw_center(void)
{
    cv_begin(240, 144, K_BG);
    switch (ui.mode) {
    case M_HOME: draw_logo(); break;
    case M_SND: if (ui.page[M_SND] == PG_STORE) draw_store(); else draw_scope(); break;
    case M_SEQ:
    case M_GEN: draw_ring(); break;
    case M_MOD: draw_mod(); break;
    case M_FX: draw_fx(); break;
    case M_PAT:
        switch (ui.page[M_PAT]) {
        case GP_PAT: draw_pat(); break;
        case GP_PROJ: draw_proj(); break;
        case GP_MIDI: draw_midi(); break;
        case GP_THEME: draw_themes(); break;
        case GP_AUDIO: draw_audio(); break;
        case GP_POP: draw_pops(); break;
        default: draw_sys(); break;
        }
        break;
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
    if (ui.mode == M_HOME) {             /* HOME: no knobs; the two inits */
        text_c(120, 18, &FONT_XS, "OCT- + HOME  INIT THE PAGE", K_DIM);
        text_c(120, 38, &FONT_XS, "OCT+ + HOME  INIT THE PATTERN", K_DIM);
        cv_blit(0, 168);
        return;
    }
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
    if (rec_on || listen_on) {           /* REC, LISTEN: a keyboard */
        kd = (1u << NKEYS) - 1u;
        kb = ui.keys;
    } else if (ui.mode == M_PAT && ui.page[M_PAT] == 0) {
        for (i = 0; i < NPAT; i++)
            kd |= 1u << WK[i];
        if (ui.vbank == play_bank())
            kb |= 1u << WK[sq.cur];
        {                                /* the banks: the first three black keys, the one shown lit */
            static const uint8_t BK3[11] = PLAT_BLACK_KEYS;
            for (i = 0; i < NBANK; i++)
                if (i == (int)ui.vbank)
                    kb |= 1u << BK3[i];
                else
                    kd |= 1u << BK3[i];
        }
        if (sq.next != 0xFFu && ui.vbank == PBANK && ((plat_ms() / 250u) & 1u))
            kb |= 1u << WK[sq.next];
    } else if (ui.mode == M_FX) {
        static const uint8_t WK2[16] = PLAT_WHITE_KEYS;
        if (ui.page[M_FX] == 0) {
            for (i = 0; i < FX_NLIVE; i++)
                if ((fx_live | fx_latch) >> i & 1u)
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
        if (ui.held < 0 && seq_cursor() >= 0 && seq_cursor() / 16 == ui.bank && ((plat_ms() / 200u) & 1u))
            kb ^= 1u << WK[seq_cursor() % 16];   /* SEQ: the step ALGORITHM picks blinks */
    }
    switch (ui.mode) {
    case M_HOME: bb |= 1u << B_HOME; break;
    case M_SEQ: bb |= 1u << B_SEQ; break;
    case M_GEN: bb |= 1u << B_ARP; break;
    case M_SND: bb |= 1u << B_EDIT; break;
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
    else if (listen_on)
        bd |= 1u << B_REC;
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
    ui.sel = 0;
    ui.fxsel[1] = FX_NLIVE;               /* FINAL: its first effect (not a LIVE one) */
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
    if (sq.running || bk_hold || now - ui.last_change < 1500u)
        return;
    for (i = 0; i < 4u; i++)             /* one object a frame */
        if (ui.pat_dirty & 1u << i) {
            if (plat_store_save(PAT_OBJ(i), &pk[i], sizeof pk[i]) == 0)
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
    if (pk_conv) {                       /* a bank read in the old layout: saved again in this one */
        ui.pat_dirty |= pk_conv;
        pk_conv = 0;
        ui.last_change = now;
    }
    if (edit_page()) {                   /* the waveform: only where it shows (it renders the whole kick) */
        if (!sj.busy && snd_hash() != ui.shash)
            scope_start();
        scope_work();
    }
    autosave(now);
    hstate = (uint32_t)sq.running | (uint32_t)sq.cur << 1 | (uint32_t)sq.next << 6 | (uint32_t)set.bpm10 << 14 |
             (uint32_t)(now < ui.msg_until) << 30 | (uint32_t)ui.mode << 26 | (uint32_t)ui.page[ui.mode] << 29;
    hstate ^= (uint32_t)set.metro * 0x1F00u ^ (set.metro && sq.running ? (beat_now & 1u) << 12 : 0u) ^ fx_live * 31u ^ fx_latch * 77u ^ (uint32_t)PBANK * 0x5151u;
    {                                    /* AUDIO: the speaker and the jack share one output: off when the one in use is */
        static uint32_t last_out = 0xFFu;
        int hp = plat_headphones();
        uint32_t m = ((set.outs & OUT_USB) ? PLAT_OUT_USB : 0u) |
                     ((set.outs & (hp > 0 ? OUT_JACK : OUT_SPK)) ? PLAT_OUT_LINE : 0u);
        uint32_t cpu = plat_cpu_pct();
        if (m != last_out) {
            last_out = m;
            plat_out_off(m);
        }
        if (cpu > ui.cpu_peak && cpu <= 255u)
            ui.cpu_peak = (uint8_t)cpu;
    }
    hstate ^= (uint32_t)listen_on * 0x5A5u ^ (uint32_t)(seq_cursor() + 1) * 65537u;
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
            if (ui.mode == M_SEQ || ui.mode == M_GEN || ui.mode == M_MOD || (ui.mode == M_PAT && !ui.page[M_PAT]))
                ui.redraw_c = 1;         /* (only where the steps are drawn: the mirror sends every change) */
        }
    if (sq.running != last_run) {
        last_run = sq.running;
        ui.redraw_c = ui.redraw_f = 1;
    }
    {
        uint8_t act = (uint8_t)(voice.active != 0), pop = (uint8_t)(ui.pop >= 0 && now < ui.pop_until);
        static uint32_t anim_t;          /* the playhead over the kick: 20 frames a second at most (the screen mirror
                                          * sends every change over USB: more is lost there) */
        if ((act || act != last_act) && (ui.mode == M_HOME || (ui.mode == M_SND && ui.page[M_SND] != PG_STORE)) &&
            (act != last_act || now - anim_t >= 50u)) {
            anim_t = now;
            ui.redraw_c = 1;
        }
        if (pop != last_pop)
            ui.redraw_c = 1;
        last_act = act;
        last_pop = pop;
    }
    if (ui.mode == M_PAT && sq.next != 0xFFu && (frame % 8u) == 0u)
        ui.redraw_c = 1;
    if (ui.mode == M_PAT && ui.page[M_PAT] != GP_PAT && ui.page[M_PAT] != GP_THEME && (frame % 15u) == 0u)
        ui.redraw_c = 1;                 /* MIDI, AUDIO, SYSTEM: their readings */
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
    else if (!strcmp(what, "swing")) *got = p->swing;
    else if (!strcmp(what, "bpm")) *got = set.bpm10;
    else if (!strcmp(what, "mode")) *got = ui.mode;
    else if (!strcmp(what, "page")) *got = ui.page[ui.mode];
    else if (!strcmp(what, "pos")) *got = sq.lpos[0];
    else if (!strcmp(what, "rpt")) *got = sq.rpt_len;
    else if (!strncmp(what, "pjused", 6)) {
        prj_t h;
        *got = pj_head(atoi(what + 6) % NPROJ, &h);
    }
    else if (!strcmp(what, "pjcur")) *got = (int)PJ_CUR;
    else if (!strcmp(what, "hitstep")) *got = sq.hit_step;
    else if (!strcmp(what, "pcopy")) *got = sq.pcopy;
    else if (!strcmp(what, "vbank")) *got = ui.vbank;
    else if (!strncmp(what, "cond", 4)) *got = (int)STEP_COND(&p->st[atoi(what + 4) % NST]);
    else if (!strncmp(what, "prob", 4)) *got = 100 - p->prb[atoi(what + 4) % NST];
    else if (!strcmp(what, "lktotal")) *got = lk_total(p);
    else if (!strncmp(what, "hitplk", 6)) {   /* hitplkK: the sounding kick's step's p-lock of parameter K (-1: none) */
        int kp = atoi(what + 6), j;
        *got = -1;
        if (sq.hit_step >= 0)
            for (j = 0; j < NLV; j++)
                if (LKV(P(), sq.hit_step)->id[j] == kp + 1)
                    *got = LKV(P(), sq.hit_step)->val[j];
    }
    else if (!strcmp(what, "pops")) *got = POPS;
    else if (!strncmp(what, "plk", 3)) {    /* plkS.K: step S's p-lock of parameter K (-1: none) */
        const lock_t *l = LKV(P(), atoi(what + 3) % NST);
        const char *dot = what + 3;
        int kp, j;
        while (*dot && *dot != '.')
            dot++;
        kp = *dot ? atoi(dot + 1) : 0;
        *got = -1;
        for (j = 0; j < NLV; j++)
            if (l->id[j] == kp + 1)
                *got = l->val[j];
    }
    else if (!strcmp(what, "dirty")) *got = (ui.pat_dirty != 0) | ui.set_dirty << 1 | ui.user_dirty << 2;
    else if (!strcmp(what, "preset")) *got = p->preset;
    else if (!strcmp(what, "rec")) *got = rec_on;
    else if (!strcmp(what, "reclast")) *got = rec_last;
    else if (!strcmp(what, "listen")) *got = listen_on;
    else if (!strcmp(what, "scopestep")) *got = scope_step() + 1;
    else if (!strcmp(what, "sel")) *got = seq_cursor() + 1;
    else if (!strcmp(what, "outs")) *got = set.outs;
    else if (!strcmp(what, "recs")) *got = (int)rec_count;
    else if (!strcmp(what, "bank")) *got = ui.bank;
    else if (!strcmp(what, "fxon")) *got = (int)set.fx.on;
    else if (!strcmp(what, "fxsel")) *got = ui.fxsel[ui.page[M_FX] ? 1 : 0];
    else if (!strncmp(what, "fxp", 3)) *got = set.fx.p[atoi(what + 3) / 4 % FX_N][atoi(what + 3) % 4];
    else if (!strcmp(what, "fxlive")) *got = (int)fx_live;
    else if (!strcmp(what, "fxlatch")) *got = (int)fx_latch;
    else if (!strcmp(what, "pbank")) *got = PBANK;
    else if (!strcmp(what, "metro")) *got = set.metro;
    else if (!strcmp(what, "theme")) *got = set.theme | set.dial << 4 | set.scope << 5 | set.steps << 6;
    else if (!strcmp(what, "active")) *got = voice.active;
    else if (!strcmp(what, "pop")) *got = ui.pop >= 0 && plat_ms() < ui.pop_until;
    else if (!strcmp(what, "tune_now")) *got = (int)(eff.p[KP_TUNE] + 0.5f);
    else if (!strcmp(what, "type_now")) *got = (int)(eff.p[KP_TYPE] + 0.5f);
    else if (!strcmp(what, "locks")) {
        int n = 0, i;
        for (i = 0; i < NST; i++)
            n += lk_count(p, (int)i);
        *got = n;
    } else if (!strncmp(what, "user", 4)) *got = us.u[atoi(what + 4) % NUSER].used;
    else if (!strncmp(what, "snd", 3)) *got = p->snd[atoi(what + 3) % KP_N];
    else if (!strncmp(what, "trig", 4)) *got = p->st[atoi(what + 4) % NST].trig;
    else if (!strncmp(what, "pit", 3)) *got = p->st[atoi(what + 3) % NST].pit;
    else if (!strncmp(what, "rat", 3)) *got = p->st[atoi(what + 3) % NST].rat;
    else if (!strncmp(what, "gen", 3)) *got = gen_trig[atoi(what + 3) % NST];
    else if (!strncmp(what, "mtgt", 4)) *got = p->mod[atoi(what + 4) % NMOD].tgt;
    else if (!strncmp(what, "mdepth", 6)) *got = p->mod[atoi(what + 6) % NMOD].depth;
    else if (!strncmp(what, "lk", 2)) *got = lk_count(p, atoi(what + 2) % NST);
    else if (!strncmp(what, "kit", 3)) *got = p->kit[atoi(what + 3) % NST];
    else if (!strncmp(what, "lval", 4)) {
        int n = atoi(what + 4) % NST, kp = atoi(strchr(what, '.') ? strchr(what, '.') + 1 : "0") % KP_N, j;
        j = lk_find(p, n, kp);
        *got = j < 0 ? -1 : p->lp[j].val;
    } else
        return 0;
    return 1;
}
#endif
