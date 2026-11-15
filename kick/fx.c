/* SPDX-License-Identifier: MPL-2.0 */
/* FM-KICK: the effects (fx.h). Compiled on its own at -O2 (fm1.toml o2). */
#include "fx.h"
#include "fastmath.h"
#include "plat.h"

#define FS 44100.0f
#define INV_FS (1.0f / 44100.0f)
#define PI_F 3.14159265f
#define TWO_PI 6.2831853f

static inline float u01(uint8_t v) { return (float)v * (1.0f / 127.0f); }
static inline float bip(uint8_t v) { return ((float)v - 64.0f) / 64.0f; }
static inline float onepole(float fc) { float a = 1.0f - fm_expf(-TWO_PI * fc * INV_FS); return a > 1.0f ? 1.0f : a; }
static inline float coef_ms(float ms) { return 1.0f - fm_expf(-1.0f / (ms * 0.001f * FS)); }
static inline float db2lin(float db) { return fm_exp2f(db * 0.1660964f); }
static inline float lin2db(float x) { return 6.0205999f * fm_log2f(x > 1e-9f ? x : 1e-9f); }

static const float DELAY_BEATS[FX_NDIV] = {0.125f, 1.0f / 6.0f, 0.25f, 1.0f / 3.0f, 0.375f, 0.5f, 0.75f, 1.0f};
static const float GATE_BEATS[FX_NGATE] = {0.125f, 0.25f, 0.5f, 1.0f};
static const float SLICE_BEATS[FX_NSLICE] = {0.25f, 0.125f, 0.0625f, 0.03125f};
static const uint8_t REP_STEPS[FX_NREP] = {1, 2, 4, 8};


void fx_defaults(fxset_t *s)
{
    static const uint8_t D[FX_N][4] = {
        {60, 90, 127, 90}, {60, 50, 127, 0}, {30, 60, 0, 0},   {60, 70, 0, 0},   {30, 60, 60, 0},  {25, 80, 60, 70},
        {20, 70, 70, 60},  {80, 60, 50, 80}, {80, 50, 45, 40}, {40, 0, 127, 0},  {40, 100, 30, 0}, {80, 50, 0, 0},
        {80, 64, 0, 0},    {70, 50, 30, 50}, {64, 64, 64, 64},  {0, 50, 100, 127}, {110, 50, 0, 0}};
    int i, k;
    s->on = 0;
    for (i = 0; i < FX_N; i++)
        for (k = 0; k < 4; k++)
            s->p[i][k] = D[i][k];
}

/* ------------------------------------------------------------------ state --- */
#define DLEN 22050                       /* DELAY: half a second each side */
#define SLEN 16384                       /* STUTTER: what it listens to */
#define LLEN 11025                       /* STUTTER: the slice it loops */
#define CLEN 2048                        /* CHORUS, FLANGER */
#define HLEN 1024                        /* WIDTH */
#define BLK 256
/* the big lines live in the device's .pool RAM region (PLAT_POOL); chorus and flanger keep 16-bit lines */
static int16_t dl_l[DLEN] PLAT_POOL, dl_r[DLEN] PLAT_POOL;
static int16_t st_rec[2][SLEN] PLAT_POOL, st_loop[2][LLEN] PLAT_POOL;
static int16_t ch_l[CLEN], ch_r[CLEN], fl_l[CLEN], fl_r[CLEN];
static float hz_buf[HLEN];
static float tl[BLK], tr[BLK], ga[BLK];

/* REVERB, a hall: a pre-delay line whose taps are the early reflections, four allpasses that smear the hit, then
 * eight delay lines (30-71 ms) feeding each other through a Hadamard matrix, each with its own damping, two of them
 * slowly swaying so the tail does not ring. It runs at half the rate (22.05 kHz: its highs are damped anyway), a
 * block behind the rest (5.8 ms more pre-delay, unheard), so the second core can work it out while the first renders
 * the kick (fx_begin). Lines in 16 bits (2 = full scale), in the pool. */
#define HN 8
#define HFS (FS * 0.5f)
#define PLEN 2646                        /* pre-delay up to 100 ms, and the reflections after it, 20 ms more */
static const int H_LEN[HN] = {661, 797, 919, 1039, 1163, 1297, 1427, 1559};
#define H_MOD 6                          /* samples of sway, either way */
#define H_SZ(j) (H_LEN[j] + H_MOD + 2)
#define H_TOT (661 + 797 + 919 + 1039 + 1163 + 1297 + 1427 + 1559 + HN * (H_MOD + 2))
static const int DF_LEN[4] = {73, 59, 181, 131};
static const float DF_G[4] = {0.72f, 0.72f, 0.62f, 0.62f};
static int16_t hl[H_TOT] PLAT_POOL;
static int16_t pre[PLEN] PLAT_POOL;
static float df[73 + 59 + 181 + 131];
/* the early reflections: when (half-rate samples after the pre-delay) and how loud, left and right */
static const int16_t ER_T[6] = {156, 234, 297, 351, 394, 432};
static const float ER_GL[6] = {0.42f, 0.0f, 0.30f, 0.21f, 0.0f, 0.15f}, ER_GR[6] = {0.0f, 0.38f, 0.24f, 0.0f, 0.18f, 0.13f};
/* the hall's work, handed from one block to the next: its input (the mono send), its output (wet, left and right) */
static float v_in[BLK], v_wl[BLK], v_wr[BLK];
static const uint8_t *v_p;
static volatile uint32_t v_n;            /* the input's frames waiting (0: none) */
static volatile uint8_t v_job;           /* 1: the second core has it; 2: done here already */
typedef struct { float b0, b1, b2, a1, a2, z1[2], z2[2]; } bq_t;
static struct {
    float g[FX_N];                       /* each effect's level now: it fades in and out over 5 ms, no clicks */
    uint32_t tail[FX_N];                 /* DELAY, REVERB: samples their tails still ring after the key */
    float comp_env;
    float sat_lp[2];
    float drv_lp[2], drv_dcx[2], drv_dcy[2];   /* FINAL DISTORTION: its tone, its DC blocker */
    float crush_hold[2], crush_n;
    float f1[2], f2[2];
    float tr_fast, tr_slow;
    uint32_t dl_w;
    float dl_lp[2];
    int h_w[HN], df_i[4], pre_w;
    float h_lp[HN], h_hp, h_c, h_s;      /* damping, the input's low cut, the sway (a turning phasor) */
    float h_ol, h_or;                    /* the last half-rate output (the line between two) */
    uint32_t ch_w, fl_w;
    float ch_ph, ph_ph, fl_ph, rg_ph;
    float ph_z[2][4], ph_fb[2];
    uint32_t st_w, st_len, st_r;
    uint8_t st_have;
    volatile uint8_t st_grab;
    float gate_g;
    uint32_t hz_w;
    float hz_lo;
    bq_t eq[3];
    float lim_env;
} z;

void fx_reset(void)
{
    uint8_t *b = (uint8_t *)&z;
    uint32_t i;
    for (i = 0; i < sizeof z; i++)
        b[i] = 0;
    z.gate_g = 1.0f;
    z.h_c = 1.0f;
    v_n = 0;
}

void fx_step(uint32_t sixteenth) { z.st_grab = (uint8_t)(1u + (sixteenth & 0x7Fu)); }

static inline float clampf(float x, float a, float b) { return x < a ? a : x > b ? b : x; }
static inline int16_t to16(float x) { x = clampf(x, -3.99f, 3.99f) * 8191.0f; return (int16_t)(x < 0.0f ? x - 0.5f : x + 0.5f); }
static inline float from16(int16_t v) { return (float)v * (1.0f / 8191.0f); }

static void bq_set(bq_t *q, int kind, float f, float db)    /* 0 low shelf, 1 peak, 2 high shelf (RBJ) */
{
    float A = fm_exp2f(db * 0.0830482f), w = TWO_PI * f * INV_FS, cs = fm_cosf(w), sn = fm_sinf(w), sa = fm_sqrtf(A);
    float b0, b1, b2, a0, a1, a2;
    if (db > -0.01f && db < 0.01f) {
        q->b0 = 1.0f;
        q->b1 = q->b2 = q->a1 = q->a2 = 0.0f;
        return;
    }
    if (kind == 1) {
        float al = sn * 0.5f;
        b0 = 1.0f + al * A;
        b1 = -2.0f * cs;
        b2 = 1.0f - al * A;
        a0 = 1.0f + al / A;
        a1 = -2.0f * cs;
        a2 = 1.0f - al / A;
    } else {
        float t = 2.0f * sa * sn * 0.5f * 1.41421356f;
        if (kind == 0) {
            b0 = A * ((A + 1.0f) - (A - 1.0f) * cs + t);
            b1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cs);
            b2 = A * ((A + 1.0f) - (A - 1.0f) * cs - t);
            a0 = (A + 1.0f) + (A - 1.0f) * cs + t;
            a1 = -2.0f * ((A - 1.0f) + (A + 1.0f) * cs);
            a2 = (A + 1.0f) + (A - 1.0f) * cs - t;
        } else {
            b0 = A * ((A + 1.0f) + (A - 1.0f) * cs + t);
            b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cs);
            b2 = A * ((A + 1.0f) + (A - 1.0f) * cs - t);
            a0 = (A + 1.0f) - (A - 1.0f) * cs + t;
            a1 = 2.0f * ((A - 1.0f) - (A + 1.0f) * cs);
            a2 = (A + 1.0f) - (A - 1.0f) * cs - t;
        }
    }
    q->b0 = b0 / a0;
    q->b1 = b1 / a0;
    q->b2 = b2 / a0;
    q->a1 = a1 / a0;
    q->a2 = a2 / a0;
}
static inline float bq_run(bq_t *q, int c, float x)
{
    float y = q->b0 * x + q->z1[c];
    q->z1[c] = fm_flush(q->b1 * x - q->a1 * y + q->z2[c]);
    q->z2[c] = fm_flush(q->b2 * x - q->a2 * y);
    return y;
}

/* ------------------------------------------------------- the insert effects --- */
/* each works on l, r in place; the caller fades between what came in and what comes out */
static void do_dist(const uint8_t *p, float *l, float *r, uint32_t n)
{
    float g = 1.0f + 30.0f * u01(p[0]) * u01(p[0]), a = onepole(500.0f * fm_exp2f(u01(p[1]) * 5.3f));
    float mx = u01(p[2]), out = 1.6f * u01(p[3]);
    uint32_t i;
    for (i = 0; i < n; i++) {
        float wl = fm_tanhf(l[i] * g), wr = fm_tanhf(r[i] * g);
        z.sat_lp[0] += a * (wl - z.sat_lp[0]);
        z.sat_lp[1] += a * (wr - z.sat_lp[1]);
        l[i] += (z.sat_lp[0] * out * 0.6f - l[i]) * mx;
        r[i] += (z.sat_lp[1] * out * 0.6f - r[i]) * mx;
    }
}
static void do_crush(const uint8_t *p, float *l, float *r, uint32_t n)
{
    float q = fm_exp2f(1.0f + 14.0f * u01(p[0])), step = 1.0f + 31.0f * u01(p[1]) * u01(p[1]), mx = u01(p[2]);
    uint32_t i;
    for (i = 0; i < n; i++) {
        z.crush_n += 1.0f;
        if (z.crush_n >= step) {
            z.crush_n -= step;
            z.crush_hold[0] = fm_floorf(clampf(l[i], -1.0f, 1.0f) * q + 0.5f) / q;
            z.crush_hold[1] = fm_floorf(clampf(r[i], -1.0f, 1.0f) * q + 0.5f) / q;
        }
        l[i] += (z.crush_hold[0] - l[i]) * mx;
        r[i] += (z.crush_hold[1] - r[i]) * mx;
    }
}
static void do_filt(const uint8_t *p, float *l, float *r, uint32_t n)
{
    float v = bip(p[0]), k = 2.0f - 1.9f * u01(p[1]), fc, g, h;
    int hp = v > 0.0f;
    uint32_t i;
    if (v > -0.02f && v < 0.02f)
        return;
    fc = hp ? 20.0f * fm_exp2f(v * 9.5f) : 20000.0f * fm_exp2f(v * 9.5f);
    g = fm_tanf(PI_F * clampf(fc, 20.0f, 18000.0f) * INV_FS);
    h = 1.0f / (1.0f + g * (g + k));
    for (i = 0; i < n; i++) {
        int c;
        for (c = 0; c < 2; c++) {
            float *x = c ? &r[i] : &l[i], hpv = (*x - (g + k) * z.f1[c] - z.f2[c]) * h, bp = g * hpv + z.f1[c], lp;
            z.f1[c] = fm_flush(bp + g * hpv);
            lp = g * bp + z.f2[c];
            z.f2[c] = fm_flush(lp + g * bp);
            *x = hp ? hpv : lp;
        }
    }
}
static void do_ring(const uint8_t *p, float *l, float *r, uint32_t n)
{
    float f = 20.0f * fm_exp2f(u01(p[0]) * 7.0f), mx = u01(p[1]);
    uint32_t i;
    for (i = 0; i < n; i++) {
        float c;
        z.rg_ph += f * INV_FS;
        if (z.rg_ph >= 1.0f)
            z.rg_ph -= 1.0f;
        c = fm_sin_turns(z.rg_ph);
        l[i] += (l[i] * c - l[i]) * mx;
        r[i] += (r[i] * c - r[i]) * mx;
    }
}
static void do_chorus(const uint8_t *p, float *l, float *r, uint32_t n)
{
    float rate = 0.05f + 4.0f * u01(p[0]) * u01(p[0]), dep = 0.007f * FS * u01(p[1]), mx = u01(p[2]);
    uint32_t i;
    for (i = 0; i < n; i++) {
        float dl, dr, yl, yr, fr;
        uint32_t a;
        ch_l[z.ch_w % CLEN] = to16(l[i]);
        ch_r[z.ch_w % CLEN] = to16(r[i]);
        z.ch_ph += rate * INV_FS;
        if (z.ch_ph >= 1.0f)
            z.ch_ph -= 1.0f;
        dl = 0.015f * FS + dep * fm_sin_turns(z.ch_ph);
        dr = 0.015f * FS + dep * fm_sin_turns(z.ch_ph + 0.25f);
        a = (uint32_t)dl;
        fr = dl - (float)a;
        yl = from16(ch_l[(z.ch_w + CLEN - a) % CLEN]) * (1.0f - fr) + from16(ch_l[(z.ch_w + CLEN - a - 1u) % CLEN]) * fr;
        a = (uint32_t)dr;
        fr = dr - (float)a;
        yr = from16(ch_r[(z.ch_w + CLEN - a) % CLEN]) * (1.0f - fr) + from16(ch_r[(z.ch_w + CLEN - a - 1u) % CLEN]) * fr;
        z.ch_w++;
        l[i] += (yl - l[i]) * mx * 0.5f;
        r[i] += (yr - r[i]) * mx * 0.5f;
    }
}
static void do_phaser(const uint8_t *p, float *l, float *r, uint32_t n)
{
    float rate = 0.05f + 3.0f * u01(p[0]) * u01(p[0]), dep = u01(p[1]), fb = 0.85f * u01(p[2]), mx = u01(p[3]);
    uint32_t i;
    for (i = 0; i < n; i++) {
        int c, j;
        z.ph_ph += rate * INV_FS;
        if (z.ph_ph >= 1.0f)
            z.ph_ph -= 1.0f;
        for (c = 0; c < 2; c++) {
            float lfo = 0.5f + 0.5f * fm_sin_turns(z.ph_ph + (c ? 0.25f : 0.0f));
            float fc = 200.0f * fm_exp2f(lfo * dep * 4.5f), t = fm_tanf(PI_F * fc * INV_FS), a = (t - 1.0f) / (t + 1.0f);
            float *x = c ? &r[i] : &l[i], y = *x + z.ph_fb[c] * fb;
            for (j = 0; j < 4; j++) {
                float o = a * y + z.ph_z[c][j];
                z.ph_z[c][j] = fm_flush(y - a * o);
                y = o;
            }
            z.ph_fb[c] = fm_flush(y);
            *x += (y - *x) * mx * 0.5f;
        }
    }
}
static void do_flanger(const uint8_t *p, float *l, float *r, uint32_t n)
{
    float rate = 0.03f + 2.0f * u01(p[0]) * u01(p[0]), dep = u01(p[1]), fb = 0.9f * u01(p[2]), mx = u01(p[3]);
    uint32_t i;
    for (i = 0; i < n; i++) {
        int c;
        z.fl_ph += rate * INV_FS;
        if (z.fl_ph >= 1.0f)
            z.fl_ph -= 1.0f;
        for (c = 0; c < 2; c++) {
            int16_t *buf = c ? fl_r : fl_l;
            float *x = c ? &r[i] : &l[i];
            float d = (0.001f + 0.008f * dep * (0.5f + 0.5f * fm_sin_turns(z.fl_ph + (c ? 0.5f : 0.0f)))) * FS;
            uint32_t a = (uint32_t)d;
            float fr = d - (float)a;
            float y = from16(buf[(z.fl_w + CLEN - a) % CLEN]) * (1.0f - fr) + from16(buf[(z.fl_w + CLEN - a - 1u) % CLEN]) * fr;
            buf[z.fl_w % CLEN] = to16(*x + y * fb);
            *x += y * mx * 0.7f;
        }
        z.fl_w++;
    }
}
static void do_stutter(const uint8_t *p, float *l, float *r, uint32_t n, float spb, int start)
{
    uint32_t len = (uint32_t)(SLICE_BEATS[fx_idx(p[0], FX_NSLICE)] * spb), rep = REP_STEPS[fx_idx(p[1], FX_NREP)], i;
    float mx = u01(p[2]);
    if (len > LLEN)
        len = LLEN;
    if (len < 32u)
        len = 32u;
    if (start || (z.st_grab && ((uint32_t)(z.st_grab - 1u) % rep) == 0u) || !z.st_have) {   /* a new slice */
        uint32_t j, from = (z.st_w + SLEN - len) % SLEN;
        for (j = 0; j < len; j++) {
            st_loop[0][j] = st_rec[0][(from + j) % SLEN];
            st_loop[1][j] = st_rec[1][(from + j) % SLEN];
        }
        z.st_len = len;
        z.st_r = 0;
        z.st_have = 1;
    }
    z.st_grab = 0;
    for (i = 0; i < n; i++) {
        float yl = from16(st_loop[0][z.st_r]), yr = from16(st_loop[1][z.st_r]);
        z.st_r = z.st_r + 1u >= z.st_len ? 0u : z.st_r + 1u;
        l[i] += (yl - l[i]) * mx;
        r[i] += (yr - r[i]) * mx;
    }
}
static void do_gate(const uint8_t *p, float *l, float *r, uint32_t n, float tick, float inc, int running)
{
    float per = GATE_BEATS[fx_idx(p[0], FX_NGATE)] * 960.0f, dep = u01(p[1]), sm = coef_ms(0.5f + 30.0f * u01(p[2]));
    uint32_t i;
    for (i = 0; i < n; i++) {
        float ph = (tick + inc * (float)i) / per, target;
        ph -= fm_floorf(ph);
        target = !running || ph < 0.5f ? 1.0f : 1.0f - dep;
        z.gate_g += sm * (target - z.gate_g);
        l[i] *= z.gate_g;
        r[i] *= z.gate_g;
    }
}
static void do_width(const uint8_t *p, float *l, float *r, uint32_t n)
{
    uint32_t d = (uint32_t)(u01(p[0]) * 0.018f * FS), i;
    float a = onepole(40.0f + 260.0f * u01(p[1]));
    for (i = 0; i < n; i++) {
        float m = (l[i] + r[i]) * 0.5f, hl, hr;
        z.hz_lo += a * (m - z.hz_lo);
        hl = l[i] - z.hz_lo;
        hz_buf[z.hz_w % HLEN] = r[i] - z.hz_lo;
        hr = hz_buf[(z.hz_w + HLEN - d) % HLEN];
        z.hz_w++;
        l[i] = z.hz_lo + hl;
        r[i] = z.hz_lo + hr;
    }
}
static void do_trans(const uint8_t *p, float *l, float *r, uint32_t n)
{
    float at = bip(p[0]) * 2.0f, su = bip(p[1]) * 2.0f, cf = coef_ms(1.0f), cs = coef_ms(60.0f);
    uint32_t i;
    for (i = 0; i < n; i++) {
        float a = fm_fabsf(l[i] + r[i]) * 0.5f, g;
        z.tr_fast += cf * (a - z.tr_fast);
        z.tr_slow += cs * (a - z.tr_slow);
        g = 1.0f + at * clampf((z.tr_fast - z.tr_slow) / (z.tr_slow + 1e-4f), 0.0f, 2.0f) +
            su * clampf((z.tr_slow - z.tr_fast) / (z.tr_fast + 1e-4f), 0.0f, 1.0f);
        g = clampf(g, 0.0f, 4.0f);
        l[i] *= g;
        r[i] *= g;
    }
}
static void do_comp(const uint8_t *p, float *l, float *r, uint32_t n)
{
    float th = -40.0f + 40.0f * u01(p[0]), ra = 1.0f + 19.0f * u01(p[1]) * u01(p[1]);
    float ca = coef_ms(0.1f + 30.0f * u01(p[2]) * u01(p[2])), cr = coef_ms(20.0f + 780.0f * u01(p[3]));
    float mk = db2lin(-th * (1.0f - 1.0f / ra) * 0.5f);
    uint32_t i;
    for (i = 0; i < n; i++) {
        float a = fm_fabsf(l[i]) > fm_fabsf(r[i]) ? fm_fabsf(l[i]) : fm_fabsf(r[i]), g, over;
        z.comp_env += (a > z.comp_env ? ca : cr) * (a - z.comp_env);
        over = lin2db(z.comp_env) - th;
        g = over > 0.0f ? db2lin(-over * (1.0f - 1.0f / ra)) * mk : mk;
        l[i] *= g;
        r[i] *= g;
    }
}
static void do_eq(const uint8_t *p, float *l, float *r, uint32_t n)
{
    uint32_t i;
    bq_set(&z.eq[0], 0, 90.0f, bip(p[0]) * 12.0f);
    bq_set(&z.eq[1], 1, 60.0f * fm_exp2f(u01(p[2]) * 6.7f), bip(p[1]) * 12.0f);
    bq_set(&z.eq[2], 2, 6000.0f, bip(p[3]) * 12.0f);
    for (i = 0; i < n; i++) {
        l[i] = bq_run(&z.eq[2], 0, bq_run(&z.eq[1], 0, bq_run(&z.eq[0], 0, l[i])));
        r[i] = bq_run(&z.eq[2], 1, bq_run(&z.eq[1], 1, bq_run(&z.eq[0], 1, r[i])));
    }
}
/* FINAL DISTORTION: TYPE (CLIP, SOFT, TUBE, FUZZ, FOLD, RECT), DRIVE, TONE (a low-pass on what it makes), MIX */
static inline float drv_shape(int t, float x)
{
    switch (t) {
    case 0: return clampf(x, -1.0f, 1.0f);                                /* CLIP: hard */
    case 1: return fm_tanhf(x);                                           /* SOFT */
    case 2: return x > 0.0f ? fm_tanhf(x) : fm_tanhf(0.6f * x) * 1.25f;  /* TUBE: lopsided, even harmonics */
    case 3: {                                                             /* FUZZ: squared off fast */
        float a = fm_fabsf(x) * 3.0f, y = 1.0f - fm_expf(-a);
        return x < 0.0f ? -y : y;
    }
    case 4: return fm_sin_turns(x * 0.25f);                               /* FOLD: folds back past the top */
    default: return fm_tanhf(fm_fabsf(x)) * 1.4f;                     /* RECT: both halves up, an octave */
    }
}
static void do_drive(const uint8_t *p, float *l, float *r, uint32_t n)
{
    int t = fx_idx(p[0], FX_NDTYPE);
    float g = 1.0f + 24.0f * u01(p[1]) * u01(p[1]), a = onepole(500.0f * fm_exp2f(u01(p[2]) * 5.3f));
    float mx = u01(p[3]), out = 0.8f / (1.0f + 0.04f * g);
    float *ch[2] = {l, r};
    uint32_t i, c;
    if (t == 4)                                  /* FOLD: louder in, so it folds */
        out = 0.7f;
    for (c = 0; c < 2; c++) {
        float *x = ch[c];
        for (i = 0; i < n; i++) {
            float w = drv_shape(t, x[i] * g), y;
            z.drv_lp[c] += a * (w - z.drv_lp[c]);
            y = z.drv_lp[c] - z.drv_dcx[c] + 0.9995f * z.drv_dcy[c];   /* DC out (TUBE and RECT lean) */
            z.drv_dcx[c] = z.drv_lp[c];
            z.drv_dcy[c] = fm_flush(y);
            x[i] += (y * out * (t == 0 || t == 1 ? 1.0f + 0.04f * g : 1.0f) - x[i]) * mx;
        }
    }
}
static void do_limit(const uint8_t *p, float *l, float *r, uint32_t n)
{
    float ceil = db2lin(-12.0f + 12.0f * u01(p[0])), cr = coef_ms(20.0f + 480.0f * u01(p[1]));
    uint32_t i;
    for (i = 0; i < n; i++) {
        float a = fm_fabsf(l[i]) > fm_fabsf(r[i]) ? fm_fabsf(l[i]) : fm_fabsf(r[i]), g;
        if (a > z.lim_env)
            z.lim_env = a;
        else
            z.lim_env += cr * (a - z.lim_env);
        g = z.lim_env > ceil ? ceil / z.lim_env : 1.0f;
        l[i] *= g;
        r[i] *= g;
    }
}

/* ----------------------------------------------------- the send effects --- */
/* DELAY and REVERB take what comes in times the fade (ga), add their echoes, and ring on after the key */
static void do_delay(const uint8_t *p, float *l, float *r, uint32_t n, float spb)
{
    uint32_t d = (uint32_t)(DELAY_BEATS[fx_idx(p[0], FX_NDIV)] * spb), i;
    float fb = 0.9f * u01(p[1]), mx = u01(p[2]) * 1.4f, a = onepole(400.0f * fm_exp2f(u01(p[3]) * 5.6f));
    if (d >= DLEN)
        d = DLEN - 1u;
    if (d < 1u)
        d = 1u;
    for (i = 0; i < n; i++) {
        uint32_t rd = (z.dl_w + DLEN - d) % DLEN;
        float yl = from16(dl_l[rd]), yr = from16(dl_r[rd]), m = (l[i] + r[i]) * 0.5f * ga[i];
        z.dl_lp[0] += a * (yl - z.dl_lp[0]);
        z.dl_lp[1] += a * (yr - z.dl_lp[1]);
        dl_l[z.dl_w] = to16(m + z.dl_lp[1] * fb);   /* ping-pong: each side feeds the other */
        dl_r[z.dl_w] = to16(z.dl_lp[0] * fb);
        z.dl_w = (z.dl_w + 1u) % DLEN;
        l[i] += yl * mx;
        r[i] += yr * mx;
    }
}
static inline float h16(int16_t v) { return (float)v * (1.0f / 16384.0f); }
static inline int16_t h16w(float x)
{
    x = x > 1.999f ? 1.999f : x < -1.999f ? -1.999f : x;
    return (int16_t)(x * 16384.0f);
}
static float verb_rt(const uint8_t *p) { float d = u01(p[0]); return 0.6f + 7.4f * d * d; }   /* DECAY: 0.6-8 s */
static inline float h_onepole(float fc) { float a = 1.0f - fm_expf(-TWO_PI * fc / HFS); return a > 1.0f ? 1.0f : a; }
/* the hall on v_in's v_n frames into v_wl, v_wr (wet only, MIX applied): on either core */
static void hall_run(uint32_t arg)
{
    const uint8_t *p = v_p;
    float rt = verb_rt(p), mx = u01(p[2]) * 1.6f * 0.55f, fb[HN], dmp, hp = h_onepole(90.0f), erm = 0.9f / 0.55f;
    float fc = 14000.0f * fm_exp2f(-3.6f * u01(p[1]));   /* DAMP: the highs die first, 14 kHz down to 1.2 kHz */
    float sw = TWO_PI * 0.37f / HFS;                      /* the sway, 0.37 Hz */
    int pd = 1 + (int)(u01(p[3]) * 2200.0f), j, base[HN];
    uint32_t i, n = v_n;
    (void)arg;
    dmp = h_onepole(fc > 10000.0f ? 10000.0f : fc);
    for (j = 0; j < HN; j++) {
        fb[j] = fm_exp2f(-9.965784f * (float)H_LEN[j] / (rt * HFS));    /* -60 dB in rt seconds */
        base[j] = j ? base[j - 1] + H_SZ(j - 1) : 0;
    }
    for (i = 0; i + 1u < n + 1u; i += 2u) {
        float m = (v_in[i] + (i + 1u < n ? v_in[i + 1u] : v_in[i])) * 0.5f, x, y[HN], el, er, ol, orr, t, v, w;
        float a0, a1, a2, a3, a4, a5, a6, a7;
        int k, o = 0, ix0, ix;
        /* the sway: a phasor turned a little each sample, renormalised now and then */
        t = z.h_c - sw * z.h_s;
        z.h_s += sw * z.h_c;
        z.h_c = t;
        if ((i & 63u) == 0u) {
            t = 1.5f - 0.5f * (z.h_c * z.h_c + z.h_s * z.h_s);
            z.h_c *= t;
            z.h_s *= t;
        }
        z.h_hp += hp * (m - z.h_hp);                       /* the lows below 90 Hz stay out: the kick stays clear */
        pre[z.pre_w] = h16w(m - z.h_hp);
        ix0 = z.pre_w - pd;
        if (ix0 < 0)
            ix0 += PLEN;
        x = h16(pre[ix0]);
        el = er = 0.0f;
        for (k = 0; k < 6; k++) {                          /* the reflections */
            ix = ix0 - ER_T[k];
            if (ix < 0)
                ix += PLEN;
            v = h16(pre[ix]);
            el += v * ER_GL[k];
            er += v * ER_GR[k];
        }
        z.pre_w = z.pre_w + 1 >= PLEN ? 0 : z.pre_w + 1;
        for (k = 0; k < 4; k++) {                          /* diffusion: four allpasses in a row */
            float *d = df + o;
            int di = z.df_i[k];
            v = d[di];
            w = x + v * DF_G[k];
            d[di] = w;
            x = v - w * DF_G[k];
            z.df_i[k] = di + 1 >= DF_LEN[k] ? 0 : di + 1;
            o += DF_LEN[k];
        }
        for (j = 0; j < HN; j++) {                         /* each line's end, damped */
            int len = H_LEN[j], wi = z.h_w[j], sz = H_SZ(j), ra;
            if (j == 1 || j == 6) {                        /* these two sway, opposite ways: a fractional read */
                float dl = (float)(len - H_MOD) + (float)H_MOD * (j == 1 ? z.h_s : -z.h_s), fr;
                int di = (int)dl, rb;
                fr = dl - (float)di;
                ra = wi - di;
                if (ra < 0)
                    ra += sz;
                rb = ra - 1 < 0 ? sz - 1 : ra - 1;
                v = h16(hl[base[j] + ra]) * (1.0f - fr) + h16(hl[base[j] + rb]) * fr;
            } else {
                ra = wi - len;
                if (ra < 0)
                    ra += sz;
                v = h16(hl[base[j] + ra]);
            }
            z.h_lp[j] += dmp * (v - z.h_lp[j]);
            y[j] = z.h_lp[j] * fb[j];
        }
        ol = y[0] - y[2] + y[4] - y[6] + 0.5f * (y[1] - y[5]) + el * erm;   /* out: two different blends */
        orr = y[1] - y[3] + y[5] - y[7] + 0.5f * (y[2] - y[6]) + er * erm;
        /* Hadamard 8, by hand: sums and differences in three rounds, then 1/sqrt(8) */
        a0 = y[0] + y[1]; a1 = y[0] - y[1]; a2 = y[2] + y[3]; a3 = y[2] - y[3];
        a4 = y[4] + y[5]; a5 = y[4] - y[5]; a6 = y[6] + y[7]; a7 = y[6] - y[7];
        y[0] = a0 + a2; y[2] = a0 - a2; y[1] = a1 + a3; y[3] = a1 - a3;
        y[4] = a4 + a6; y[6] = a4 - a6; y[5] = a5 + a7; y[7] = a5 - a7;
        x *= 0.5f;
        hl[base[0] + z.h_w[0]] = h16w((y[0] + y[4]) * 0.35355339f + x);
        hl[base[1] + z.h_w[1]] = h16w((y[1] + y[5]) * 0.35355339f - x);
        hl[base[2] + z.h_w[2]] = h16w((y[2] + y[6]) * 0.35355339f + x);
        hl[base[3] + z.h_w[3]] = h16w((y[3] + y[7]) * 0.35355339f - x);
        hl[base[4] + z.h_w[4]] = h16w((y[0] - y[4]) * 0.35355339f + x);
        hl[base[5] + z.h_w[5]] = h16w((y[1] - y[5]) * 0.35355339f - x);
        hl[base[6] + z.h_w[6]] = h16w((y[2] - y[6]) * 0.35355339f + x);
        hl[base[7] + z.h_w[7]] = h16w((y[3] - y[7]) * 0.35355339f - x);
        for (j = 0; j < HN; j++)
            z.h_w[j] = z.h_w[j] + 1 >= H_SZ(j) ? 0 : z.h_w[j] + 1;
        ol *= mx;
        orr *= mx;
        v_wl[i] = (z.h_ol + ol) * 0.5f;                    /* back to the full rate: a line between two */
        v_wr[i] = (z.h_or + orr) * 0.5f;
        if (i + 1u < n) {
            v_wl[i + 1u] = ol;
            v_wr[i + 1u] = orr;
        }
        z.h_ol = ol;
        z.h_or = orr;
    }
    for (j = 0; j < HN; j++)                               /* (no denormals: once a block is enough) */
        z.h_lp[j] = fm_flush(z.h_lp[j]);
    for (j = 0; j < (int)(sizeof df / sizeof df[0]); j++)
        df[j] = fm_flush(df[j]);
    z.h_hp = fm_flush(z.h_hp);
}
/* the block's start: last block's send handed to the second core, if it takes it */
void fx_begin(void)
{
    v_job = 0;
    if (v_n && plat_cpu2_run(hall_run, 0))
        v_job = 1;
}
static void hall_done(void)              /* the hall's output for the send waiting: from the second core, or now */
{
    if (v_job == 1 && plat_cpu2_wait() == 0) {
        v_job = 2;
        return;
    }
    if (v_job != 2 && v_n)
        hall_run(0);
    v_job = 2;
}
static void do_verb(const uint8_t *p, float *l, float *r, uint32_t n)
{
    uint32_t i, m = v_n;
    hall_done();
    for (i = 0; i < m && i < n; i++) {   /* last block's hall */
        l[i] += v_wl[i];
        r[i] += v_wr[i];
    }
    v_p = p;                             /* this block's send, for the next */
    for (i = 0; i < n; i++)
        v_in[i] = (l[i] + r[i]) * 0.5f * ga[i];
    v_n = n;
}
static void tail_clear(int fx)           /* a tail that has died: its lines emptied, so the next press starts clean */
{
    uint32_t i;
    if (fx == FX_DELAY) {
        for (i = 0; i < DLEN; i++)
            dl_l[i] = dl_r[i] = 0;
        z.dl_lp[0] = z.dl_lp[1] = 0.0f;
    } else {
        hall_done();
        v_n = 0;
        z.h_ol = z.h_or = 0.0f;
        for (i = 0; i < sizeof hl / sizeof hl[0]; i++)
            hl[i] = 0;
        for (i = 0; i < PLEN; i++)
            pre[i] = 0;
        for (i = 0; i < sizeof df / sizeof df[0]; i++)
            df[i] = 0.0f;
        for (i = 0; i < HN; i++)
            z.h_lp[i] = 0.0f;
        z.h_hp = 0.0f;
    }
}

void fx_process(const fxset_t *s, uint32_t live, const float *in, float *l, float *r, uint32_t n, float bpm, float tick,
                float inc, int running)
{
    uint32_t i;
    int f;
    float spb = 60.0f / (bpm > 20.0f ? bpm : 20.0f) * FS;   /* samples a beat */
    if (n > BLK)
        n = BLK;
    for (i = 0; i < n; i++)
        l[i] = r[i] = in[i];
    for (i = 0; i < n; i++) {            /* STUTTER listens all the time */
        st_rec[0][z.st_w] = to16(in[i]);
        st_rec[1][z.st_w] = to16(in[i]);
        z.st_w = (z.st_w + 1u) % SLEN;
    }
    for (f = 0; f < FX_N; f++) {
        const uint8_t *p = s->p[f];
        int target = f < FX_NLIVE ? (int)(live >> f & 1u) : (int)(s->on >> f & 1u);
        float g0 = z.g[f], g1 = g0 + ((float)target - g0 > 0.0f ? 1.0f : -1.0f) * (float)n / 220.0f;
        int start = g0 == 0.0f && target;
        if (target ? g1 > 1.0f : g1 < 0.0f)
            g1 = target ? 1.0f : 0.0f;
        if (g0 == (float)target)
            g1 = g0;
        z.g[f] = g1;
        if (g0 != 0.0f || g1 != 0.0f) { /* (an effect off: no ramp to work out) */
            float st = (g1 - g0) / (float)n;
            for (i = 0; i < n; i++)
                ga[i] = g0 + st * (float)(i + 1u);
        } else if (f == FX_DELAY || f == FX_VERB) {
            for (i = 0; i < n; i++)
                ga[i] = 0.0f;
        }
        if (f == FX_DELAY || f == FX_VERB) {
            if (g0 > 0.0f || g1 > 0.0f)
                z.tail[f] = (uint32_t)((f == FX_VERB ? verb_rt(p) + 0.3f : 4.0f) * FS);
            if (!z.tail[f])
                continue;
            if (f == FX_DELAY)
                do_delay(p, l, r, n, spb);
            else
                do_verb(p, l, r, n);
            if (g1 == 0.0f) {
                z.tail[f] = z.tail[f] > n ? z.tail[f] - n : 0u;
                if (!z.tail[f])
                    tail_clear(f);
            }
            continue;
        }
        if (g0 == 0.0f && g1 == 0.0f) {
            if (f == FX_STUTTER)
                z.st_have = 0;
            continue;
        }
        for (i = 0; i < n; i++) {
            tl[i] = l[i];
            tr[i] = r[i];
        }
        switch (f) {
        case FX_DIST: do_dist(p, tl, tr, n); break;
        case FX_CRUSH: do_crush(p, tl, tr, n); break;
        case FX_FILT: do_filt(p, tl, tr, n); break;
        case FX_RING: do_ring(p, tl, tr, n); break;
        case FX_CHORUS: do_chorus(p, tl, tr, n); break;
        case FX_PHASER: do_phaser(p, tl, tr, n); break;
        case FX_FLANGER: do_flanger(p, tl, tr, n); break;
        case FX_STUTTER: do_stutter(p, tl, tr, n, spb, start); break;
        case FX_GATE: do_gate(p, tl, tr, n, tick, inc, running); break;
        case FX_WIDTH: do_width(p, tl, tr, n); break;
        case FX_TRANS: do_trans(p, tl, tr, n); break;
        case FX_COMP: do_comp(p, tl, tr, n); break;
        case FX_EQ: do_eq(p, tl, tr, n); break;
        case FX_DRIVE: do_drive(p, tl, tr, n); break;
        case FX_LIMIT: do_limit(p, tl, tr, n); break;
        default: break;
        }
        for (i = 0; i < n; i++) {
            l[i] += (tl[i] - l[i]) * ga[i];
            r[i] += (tr[i] - r[i]) * ga[i];
        }
    }
    if (v_job == 1)                      /* (never left outstanding past the block) */
        hall_done();
}
