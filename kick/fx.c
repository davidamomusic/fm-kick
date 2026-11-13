/* SPDX-License-Identifier: MPL-2.0 */
/* FM-KICK: the effects (fx.h). Compiled on its own at -O2 (fm1.toml o2). */
#include "fx.h"
#include "fastmath.h"

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
        {20, 70, 70, 60},  {80, 60, 50, 80}, {80, 50, 45, 100}, {40, 0, 127, 0},  {40, 100, 30, 0}, {80, 50, 0, 0},
        {80, 64, 0, 0},    {70, 50, 30, 50}, {64, 64, 64, 64},  {40, 64, 100, 0}, {110, 50, 0, 0}};
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
static int16_t dl_l[DLEN], dl_r[DLEN];
static int16_t st_rec[2][SLEN], st_loop[2][LLEN];
static float ch_l[CLEN], ch_r[CLEN], fl_l[CLEN], fl_r[CLEN], hz_buf[HLEN];
static float tl[BLK], tr[BLK], ga[BLK];

static const int COMB_LEN[4] = {1116, 1188, 1277, 1356}, AP_LEN[2] = {556, 441};
#define SPREAD 23
static float cb[2][4][1356 + SPREAD], ap[2][2][556 + SPREAD];
typedef struct { float b0, b1, b2, a1, a2, z1[2], z2[2]; } bq_t;
static struct {
    float g[FX_N];                       /* each effect's level now: it fades in and out over 5 ms, no clicks */
    uint32_t tail[FX_N];                 /* DELAY, REVERB: samples their tails still ring after the key */
    float comp_env;
    float sat_lp[2];
    float crush_hold[2], crush_n;
    float f1[2], f2[2];
    float tr_fast, tr_slow;
    uint32_t dl_w;
    float dl_lp[2];
    int cb_i[2][4], ap_i[2][2];
    float cb_lp[2][4];
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
        ch_l[z.ch_w % CLEN] = l[i];
        ch_r[z.ch_w % CLEN] = r[i];
        z.ch_ph += rate * INV_FS;
        if (z.ch_ph >= 1.0f)
            z.ch_ph -= 1.0f;
        dl = 0.015f * FS + dep * fm_sin_turns(z.ch_ph);
        dr = 0.015f * FS + dep * fm_sin_turns(z.ch_ph + 0.25f);
        a = (uint32_t)dl;
        fr = dl - (float)a;
        yl = ch_l[(z.ch_w + CLEN - a) % CLEN] * (1.0f - fr) + ch_l[(z.ch_w + CLEN - a - 1u) % CLEN] * fr;
        a = (uint32_t)dr;
        fr = dr - (float)a;
        yr = ch_r[(z.ch_w + CLEN - a) % CLEN] * (1.0f - fr) + ch_r[(z.ch_w + CLEN - a - 1u) % CLEN] * fr;
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
            float *buf = c ? fl_r : fl_l, *x = c ? &r[i] : &l[i];
            float d = (0.001f + 0.008f * dep * (0.5f + 0.5f * fm_sin_turns(z.fl_ph + (c ? 0.5f : 0.0f)))) * FS;
            uint32_t a = (uint32_t)d;
            float fr = d - (float)a;
            float y = buf[(z.fl_w + CLEN - a) % CLEN] * (1.0f - fr) + buf[(z.fl_w + CLEN - a - 1u) % CLEN] * fr;
            buf[z.fl_w % CLEN] = fm_flush(*x + y * fb);
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
static void do_clip(const uint8_t *p, float *l, float *r, uint32_t n)
{
    float g = 1.0f + 10.0f * u01(p[0]) * u01(p[0]), kn = u01(p[1]), out = u01(p[2]) * 1.27f;
    uint32_t i;
    for (i = 0; i < n; i++) {
        float a = l[i] * g, b = r[i] * g, ha = clampf(a, -1.0f, 1.0f), hb = clampf(b, -1.0f, 1.0f);
        l[i] = (ha + (fm_tanhf(a) - ha) * kn) * out;
        r[i] = (hb + (fm_tanhf(b) - hb) * kn) * out;
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
static void do_verb(const uint8_t *p, float *l, float *r, uint32_t n)
{
    float fb = 0.7f + 0.27f * u01(p[0]), dmp = 0.05f + 0.6f * u01(p[1]), mx = u01(p[2]) * 1.5f, w = u01(p[3]);
    uint32_t i;
    for (i = 0; i < n; i++) {
        float inp = (l[i] + r[i]) * 0.5f * 0.12f * ga[i], o[2], wl, wr;
        int c, j;
        for (c = 0; c < 2; c++) {
            float acc = 0.0f;
            for (j = 0; j < 4; j++) {
                int len = COMB_LEN[j] + (c ? SPREAD : 0), ix = z.cb_i[c][j];
                float y = cb[c][j][ix];
                z.cb_lp[c][j] = fm_flush(y + (z.cb_lp[c][j] - y) * dmp);
                cb[c][j][ix] = fm_flush(inp + z.cb_lp[c][j] * fb);
                z.cb_i[c][j] = ix + 1 >= len ? 0 : ix + 1;
                acc += y;
            }
            for (j = 0; j < 2; j++) {
                int len = AP_LEN[j] + (c ? SPREAD : 0), ix = z.ap_i[c][j];
                float b = ap[c][j][ix], y = b - acc;
                ap[c][j][ix] = fm_flush(acc + b * 0.5f);
                z.ap_i[c][j] = ix + 1 >= len ? 0 : ix + 1;
                acc = y;
            }
            o[c] = acc;
        }
        wl = 2.5f * (o[0] * (0.5f + 0.5f * w) + o[1] * (0.5f - 0.5f * w));
        wr = 2.5f * (o[1] * (0.5f + 0.5f * w) + o[0] * (0.5f - 0.5f * w));
        l[i] += wl * mx;
        r[i] += wr * mx;
    }
}
static void tail_clear(int fx)           /* a tail that has died: its lines emptied, so the next press starts clean */
{
    uint32_t i;
    if (fx == FX_DELAY) {
        for (i = 0; i < DLEN; i++)
            dl_l[i] = dl_r[i] = 0;
        z.dl_lp[0] = z.dl_lp[1] = 0.0f;
    } else {
        float *f = &cb[0][0][0];
        for (i = 0; i < sizeof cb / sizeof(float); i++)
            f[i] = 0.0f;
        f = &ap[0][0][0];
        for (i = 0; i < sizeof ap / sizeof(float); i++)
            f[i] = 0.0f;
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
        for (i = 0; i < n; i++)
            ga[i] = g0 + (g1 - g0) * (float)(i + 1u) / (float)n;
        if (f == FX_DELAY || f == FX_VERB) {
            if (g0 > 0.0f || g1 > 0.0f)
                z.tail[f] = (uint32_t)(4.0f * FS);
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
        case FX_CLIP: do_clip(p, tl, tr, n); break;
        case FX_LIMIT: do_limit(p, tl, tr, n); break;
        default: break;
        }
        for (i = 0; i < n; i++) {
            l[i] += (tl[i] - l[i]) * ga[i];
            r[i] += (tr[i] - r[i]) * ga[i];
        }
    }
}
