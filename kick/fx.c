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
        {70, 50, 30, 50},  {40, 90, 127, 100}, {80, 30, 127, 0}, {64, 30, 0, 0},  {90, 64, 0, 0},
        {40, 64, 100, 0},  {80, 50, 35, 80},   {60, 60, 25, 100}, {30, 60, 50, 0}, {25, 80, 60, 60},
        {20, 70, 70, 50},  {50, 40, 0, 0},     {40, 40, 127, 0}, {40, 100, 30, 0}, {60, 50, 0, 0},
        {110, 50, 0, 0}};
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
static int16_t dl_l[DLEN], dl_r[DLEN];
static int16_t st_rec[2][SLEN], st_loop[2][LLEN];
static float ch_l[CLEN], ch_r[CLEN], fl_l[CLEN], fl_r[CLEN], hz_buf[HLEN];

static const int COMB_LEN[4] = {1116, 1188, 1277, 1356}, AP_LEN[2] = {556, 441};
#define SPREAD 23
static float cb[2][4][1356 + SPREAD], ap[2][2][556 + SPREAD];
static struct {
    float comp_env;
    float sat_lp[2];
    float crush_hold[2], crush_n;
    float f1[2], f2[2];
    float tr_fast, tr_slow;
    uint32_t dl_w;
    float dl_lp[2];
    int cb_i[2][4], ap_i[2][2];
    float cb_lp[2][4];
    uint32_t ch_w;
    float ch_ph, ph_ph, fl_ph, rg_ph;
    float ph_z[2][4], ph_fb[2];
    uint32_t st_w, st_len, st_r;
    uint8_t st_have;
    volatile uint8_t st_grab;
    float gate_g;
    uint32_t hz_w;
    float hz_lo[2];
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

void fx_process(const fxset_t *s, const float *in, float *l, float *r, uint32_t n, float bpm, float tick, float inc,
                int running)
{
    uint32_t i, on = s->on;
    float spb = 60.0f / (bpm > 20.0f ? bpm : 20.0f) * FS;   /* samples a beat */
    for (i = 0; i < n; i++)
        l[i] = r[i] = in[i];
    if (!on)
        return;
    /* 1 COMP: stereo-linked; THRESH -40..0 dB, RATIO 1..20, ATTACK 0.1..30 ms, RELEASE 20..800 ms, made up */
    if (on & 1u << FX_COMP) {
        const uint8_t *p = s->p[FX_COMP];
        float th = -40.0f + 40.0f * u01(p[0]), ra = 1.0f + 19.0f * u01(p[1]) * u01(p[1]);
        float ca = coef_ms(0.1f + 30.0f * u01(p[2]) * u01(p[2])), cr = coef_ms(20.0f + 780.0f * u01(p[3]));
        float mk = db2lin(-th * (1.0f - 1.0f / ra) * 0.5f);
        for (i = 0; i < n; i++) {
            float a = fm_fabsf(l[i]) > fm_fabsf(r[i]) ? fm_fabsf(l[i]) : fm_fabsf(r[i]), g, over;
            z.comp_env += (a > z.comp_env ? ca : cr) * (a - z.comp_env);
            over = lin2db(z.comp_env) - th;
            g = over > 0.0f ? db2lin(-over * (1.0f - 1.0f / ra)) * mk : mk;
            l[i] *= g;
            r[i] *= g;
        }
    }
    /* 2 SATURATE: DRIVE, TONE, MIX, OUTPUT */
    if (on & 1u << FX_SAT) {
        const uint8_t *p = s->p[FX_SAT];
        float g = 1.0f + 15.0f * u01(p[0]) * u01(p[0]), a = onepole(500.0f * fm_exp2f(u01(p[1]) * 5.3f));
        float mx = u01(p[2]), out = 2.0f * u01(p[3]) / fm_tanhf(g > 1.5f ? 1.5f : g);
        for (i = 0; i < n; i++) {
            float wl = fm_tanhf(l[i] * g), wr = fm_tanhf(r[i] * g);
            z.sat_lp[0] += a * (wl - z.sat_lp[0]);
            z.sat_lp[1] += a * (wr - z.sat_lp[1]);
            l[i] = (l[i] + (z.sat_lp[0] * out * 0.5f - l[i]) * mx);
            r[i] = (r[i] + (z.sat_lp[1] * out * 0.5f - r[i]) * mx);
        }
    }
    /* 3 CRUSH: BITS 2..16, RATE (hold 1..32 samples), MIX */
    if (on & 1u << FX_CRUSH) {
        const uint8_t *p = s->p[FX_CRUSH];
        float q = fm_exp2f(2.0f + 14.0f * u01(p[0]) - 1.0f), step = 1.0f + 31.0f * u01(p[1]) * u01(p[1]), mx = u01(p[2]);
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
    /* 4 FILTER: one knob, low-pass to the left, high-pass to the right; RESO */
    if (on & 1u << FX_FILT) {
        const uint8_t *p = s->p[FX_FILT];
        float v = bip(p[0]), k = 2.0f - 1.9f * u01(p[1]), fc, g, h;
        int hp = v > 0.0f;
        if (v > -0.02f && v < 0.02f)
            goto filt_done;
        fc = hp ? 20.0f * fm_exp2f(v * 9.5f) : 20000.0f * fm_exp2f(v * 9.5f);
        g = fm_tanf(PI_F * clampf(fc, 20.0f, 18000.0f) * INV_FS);
        h = 1.0f / (1.0f + g * (g + k));
        for (i = 0; i < n; i++) {
            int c;
            for (c = 0; c < 2; c++) {
                float x = c ? r[i] : l[i], hpv = (x - (g + k) * z.f1[c] - z.f2[c]) * h, bp = g * hpv + z.f1[c], lp;
                z.f1[c] = fm_flush(bp + g * hpv);
                lp = g * bp + z.f2[c];
                z.f2[c] = fm_flush(lp + g * bp);
                if (c)
                    r[i] = hp ? hpv : lp;
                else
                    l[i] = hp ? hpv : lp;
            }
        }
    filt_done:;
    }
    /* 5 TRANSIENT: ATTACK and SUSTAIN, more or less (64 none) */
    if (on & 1u << FX_TRANS) {
        const uint8_t *p = s->p[FX_TRANS];
        float at = bip(p[0]) * 2.0f, su = bip(p[1]) * 2.0f, cf = coef_ms(1.0f), cs = coef_ms(60.0f);
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
    /* 6 CLIPPER: DRIVE, KNEE (hard .. soft), OUTPUT */
    if (on & 1u << FX_CLIP) {
        const uint8_t *p = s->p[FX_CLIP];
        float g = 1.0f + 10.0f * u01(p[0]) * u01(p[0]), kn = u01(p[1]), out = u01(p[2]) * 1.27f;
        for (i = 0; i < n; i++) {
            float a = l[i] * g, b = r[i] * g;
            float ha = clampf(a, -1.0f, 1.0f), hb = clampf(b, -1.0f, 1.0f);
            l[i] = (ha + (fm_tanhf(a) - ha) * kn) * out;
            r[i] = (hb + (fm_tanhf(b) - hb) * kn) * out;
        }
    }
    /* 7 DELAY: ping-pong, TIME in beats (to the tempo), FEEDBACK, MIX, TONE */
    if (on & 1u << FX_DELAY) {
        const uint8_t *p = s->p[FX_DELAY];
        uint32_t d = (uint32_t)(DELAY_BEATS[fx_idx(p[0], FX_NDIV)] * spb);
        float fb = 0.9f * u01(p[1]), mx = u01(p[2]), a = onepole(400.0f * fm_exp2f(u01(p[3]) * 5.6f));
        if (d >= DLEN)
            d = DLEN - 1u;
        if (d < 1u)
            d = 1u;
        for (i = 0; i < n; i++) {
            uint32_t rd = (z.dl_w + DLEN - d) % DLEN;
            float yl = from16(dl_l[rd]), yr = from16(dl_r[rd]), m = (l[i] + r[i]) * 0.5f;
            z.dl_lp[0] += a * (yl - z.dl_lp[0]);
            z.dl_lp[1] += a * (yr - z.dl_lp[1]);
            dl_l[z.dl_w] = to16(m + z.dl_lp[1] * fb);   /* ping-pong: each side feeds the other */
            dl_r[z.dl_w] = to16(z.dl_lp[0] * fb);
            z.dl_w = (z.dl_w + 1u) % DLEN;
            l[i] += yl * mx;
            r[i] += yr * mx;
        }
    }
    /* 8 REVERB: SIZE, DAMP, MIX, WIDTH (four combs and two all-passes a side) */
    if (on & 1u << FX_VERB) {
        const uint8_t *p = s->p[FX_VERB];
        float fb = 0.7f + 0.27f * u01(p[0]), dmp = 0.05f + 0.6f * u01(p[1]), mx = u01(p[2]), w = u01(p[3]);
        for (i = 0; i < n; i++) {
            float inp = (l[i] + r[i]) * 0.5f * 0.12f, o[2];
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
            {                            /* the tail made up (four combs, a small input) */
                float wl = 2.5f * (o[0] * (0.5f + 0.5f * w) + o[1] * (0.5f - 0.5f * w));
                float wr = 2.5f * (o[1] * (0.5f + 0.5f * w) + o[0] * (0.5f - 0.5f * w));
                l[i] += wl * mx;
                r[i] += wr * mx;
            }
        }
    }
    /* 9 CHORUS: RATE, DEPTH, MIX */
    if (on & 1u << FX_CHORUS) {
        const uint8_t *p = s->p[FX_CHORUS];
        float rate = 0.05f + 4.0f * u01(p[0]) * u01(p[0]), dep = 0.007f * FS * u01(p[1]), mx = u01(p[2]);
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
    /* 10 PHASER: RATE, DEPTH, FEEDBACK, MIX (four all-pass stages a side) */
    if (on & 1u << FX_PHASER) {
        const uint8_t *p = s->p[FX_PHASER];
        float rate = 0.05f + 3.0f * u01(p[0]) * u01(p[0]), dep = u01(p[1]), fb = 0.85f * u01(p[2]), mx = u01(p[3]);
        for (i = 0; i < n; i++) {
            int c, j;
            z.ph_ph += rate * INV_FS;
            if (z.ph_ph >= 1.0f)
                z.ph_ph -= 1.0f;
            for (c = 0; c < 2; c++) {
                float lfo = 0.5f + 0.5f * fm_sin_turns(z.ph_ph + (c ? 0.25f : 0.0f));
                float fc = 200.0f * fm_exp2f(lfo * dep * 4.5f), t = fm_tanf(PI_F * fc * INV_FS), a = (t - 1.0f) / (t + 1.0f);
                float x = (c ? r[i] : l[i]) + z.ph_fb[c] * fb, y = x;
                for (j = 0; j < 4; j++) {
                    float o = a * y + z.ph_z[c][j];
                    z.ph_z[c][j] = fm_flush(y - a * o);
                    y = o;
                }
                z.ph_fb[c] = fm_flush(y);
                if (c)
                    r[i] += (y - r[i]) * mx * 0.5f;
                else
                    l[i] += (y - l[i]) * mx * 0.5f;
            }
        }
    }
    /* 11 FLANGER: RATE, DEPTH, FEEDBACK, MIX */
    if (on & 1u << FX_FLANGER) {
        const uint8_t *p = s->p[FX_FLANGER];
        float rate = 0.03f + 2.0f * u01(p[0]) * u01(p[0]), dep = u01(p[1]), fb = 0.9f * u01(p[2]), mx = u01(p[3]);
        for (i = 0; i < n; i++) {
            int c;
            z.fl_ph += rate * INV_FS;
            if (z.fl_ph >= 1.0f)
                z.fl_ph -= 1.0f;
            for (c = 0; c < 2; c++) {
                float *buf = c ? fl_r : fl_l, d = (0.001f + 0.008f * dep * (0.5f + 0.5f * fm_sin_turns(z.fl_ph + (c ? 0.5f : 0.0f)))) * FS;
                uint32_t a = (uint32_t)d;
                float fr = d - (float)a, x = c ? r[i] : l[i];
                float y = buf[(z.ch_w + CLEN - a) % CLEN] * (1.0f - fr) + buf[(z.ch_w + CLEN - a - 1u) % CLEN] * fr;
                buf[z.ch_w % CLEN] = fm_flush(x + y * fb);
                if (c)
                    r[i] = x + y * mx * 0.7f;
                else
                    l[i] = x + y * mx * 0.7f;
            }
            if (!(on & 1u << FX_CHORUS))
                z.ch_w++;
        }
    }
    /* 12 RING: FREQ 20 Hz .. 2.5 kHz, MIX */
    if (on & 1u << FX_RING) {
        const uint8_t *p = s->p[FX_RING];
        float f = 20.0f * fm_exp2f(u01(p[0]) * 7.0f), mx = u01(p[1]);
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
    /* 13 STUTTER: SLICE, REPEAT (a new slice every 1, 2, 4 or 8 steps), MIX; it loops while the sequencer plays */
    if (on & 1u << FX_STUTTER) {
        const uint8_t *p = s->p[FX_STUTTER];
        uint32_t len = (uint32_t)(SLICE_BEATS[fx_idx(p[0], FX_NSLICE)] * spb), rep = REP_STEPS[fx_idx(p[1], FX_NREP)];
        float mx = u01(p[2]);
        if (len > LLEN)
            len = LLEN;
        if (len < 32u)
            len = 32u;
        for (i = 0; i < n; i++) {
            st_rec[0][z.st_w] = to16(l[i]);
            st_rec[1][z.st_w] = to16(r[i]);
            z.st_w = (z.st_w + 1u) % SLEN;
        }
        if (z.st_grab && running && ((uint32_t)(z.st_grab - 1u) % rep) == 0u) {   /* this step: a new slice */
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
        if (running && z.st_have)
            for (i = 0; i < n; i++) {
                float yl = from16(st_loop[0][z.st_r]), yr = from16(st_loop[1][z.st_r]);
                z.st_r = z.st_r + 1u >= z.st_len ? 0u : z.st_r + 1u;
                l[i] += (yl - l[i]) * mx;
                r[i] += (yr - r[i]) * mx;
            }
    } else {
        z.st_have = 0;
        z.st_grab = 0;
    }
    /* 14 GATE: RATE (to the tempo), DEPTH, SMOOTH; open in the first half of each period */
    if (on & 1u << FX_GATE) {
        const uint8_t *p = s->p[FX_GATE];
        float per = GATE_BEATS[fx_idx(p[0], FX_NGATE)] * 960.0f, dep = u01(p[1]), sm = coef_ms(0.5f + 30.0f * u01(p[2]));
        for (i = 0; i < n; i++) {
            float pos = tick + inc * (float)i, ph = pos / per, target;
            ph -= fm_floorf(ph);
            target = !running || ph < 0.5f ? 1.0f : 1.0f - dep;
            z.gate_g += sm * (target - z.gate_g);
            l[i] *= z.gate_g;
            r[i] *= z.gate_g;
        }
    }
    /* 15 WIDTH: AMOUNT (the right side's highs a little later), BASS MONO (below it, the middle, not delayed) */
    if (on & 1u << FX_WIDTH) {
        const uint8_t *p = s->p[FX_WIDTH];
        uint32_t d = (uint32_t)(u01(p[0]) * 0.018f * FS);
        float a = onepole(40.0f + 260.0f * u01(p[1]));
        for (i = 0; i < n; i++) {
            float m = (l[i] + r[i]) * 0.5f, hl, hr;
            z.hz_lo[0] += a * (m - z.hz_lo[0]);
            hl = l[i] - z.hz_lo[0];
            hz_buf[z.hz_w % HLEN] = r[i] - z.hz_lo[0];
            hr = hz_buf[(z.hz_w + HLEN - d) % HLEN];
            z.hz_w++;
            l[i] = z.hz_lo[0] + hl;
            r[i] = z.hz_lo[0] + hr;
        }
    }
    /* 16 LIMITER: CEILING -12..0 dB, RELEASE 20..500 ms */
    if (on & 1u << FX_LIMIT) {
        const uint8_t *p = s->p[FX_LIMIT];
        float ceil = db2lin(-12.0f + 12.0f * u01(p[0])), cr = coef_ms(20.0f + 480.0f * u01(p[1]));
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
}
