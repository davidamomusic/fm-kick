/* SPDX-License-Identifier: MPL-2.0 */
/* FM-KICK: the kick voice (dsp.h). Compiled on its own at -O2 (fm1.toml o2). */
#include "dsp.h"
#include "fastmath.h"

#define FS 44100.0f
#define INV_FS (1.0f / 44100.0f)
#define PI_F 3.14159265f
#define TWO_PI 6.2831853f

static const float SINT_ST[4] = {-12.0f, 0.0f, 7.0f, 12.0f};
static const float FM_RATIO[12] = {0.5f, 1.0f, 1.5f, 2.0f, 2.5f, 3.0f, 3.5f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f};

int kp_lo(int k) { return k == KP_TUNE ? KP_TUNE_LO : 0; }
int kp_hi(int k)
{
    switch (k) {
    case KP_TUNE: return KP_TUNE_HI;
    case KP_TYPE: return KT_N - 1;
    case KP_CTYPE: return CK_N - 1;
    case KP_SINT: return 3;
    case KP_DTYPE: return DT_N - 1;
    case KP_FTYPE: return FT_N - 1;
    default: return 127;
    }
}

static inline uint32_t xr(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *s = x;
}
static inline float rnd_bi(uint32_t *s) { return (float)(int32_t)xr(s) * (1.0f / 2147483648.0f); }
static inline float u01(float v) { return v * (1.0f / 127.0f); }
static inline float sq01(float v) { float u = v * (1.0f / 127.0f); return u * u; }
static inline int sel(float v, int n) { int i = (int)(v + 0.5f); return i < 0 ? 0 : i >= n ? n - 1 : i; }
static inline float onepole(float fc) { float a = 1.0f - fm_expf(-TWO_PI * fc * INV_FS); return a > 1.0f ? 1.0f : a; }

float kdsp_note_hz(float note) { return 440.0f * fm_exp2f((note - 69.0f) * (1.0f / 12.0f)); }

static float sweep_time(float v) { return 0.001f * fm_exp2f(v * (8.6439f / 127.0f)); }       /* 1 ms .. 400 ms */
static float tail_time(float v) { return 0.01f * fm_exp2f(v * (8.2288f / 127.0f)); }         /* 10 ms .. 3 s */
static float dec_time(float v) { return 0.02f * fm_exp2f(v * (8.2288f / 127.0f)); }          /* 20 ms .. 6 s */
static float atk_time(float v) { float u = u01(v); return 0.05f * u * u; }                   /* 0 .. 50 ms */
static float hold_time(float v) { float u = u01(v); return 0.3f * u * u; }                   /* 0 .. 300 ms */
static float glide_time(float v) { float u = u01(v); return 0.001f + 0.5f * u * u; }         /* 1 .. 500 ms */

/* the sweep's shape at x = t / time: CURVE 0 a long hyperbolic tail, 64 exponential, 127 held, then fast */
static float sweep_shape(float x, float u)
{
    float a = fm_expf(-x);
    if (u < 0.5f) {
        float b = 1.0f / ((1.0f + 2.0f * x) * (1.0f + 2.0f * x));
        return b + (a - b) * (2.0f * u);
    }
    return a + (fm_expf(-x * x) - a) * (2.0f * u - 1.0f);
}

float kdsp_pitch_at(const kparm_t *p, float t, int rev)
{
    float bend = p->p[KP_BEND] * (60.0f / 127.0f) * (rev ? -1.0f : 1.0f);
    float tail = p->p[KP_TAIL] * (24.0f / 127.0f);
    return p->p[KP_TUNE] + bend * sweep_shape(t / sweep_time(p->p[KP_TIME]), u01(p->p[KP_CURVE])) -
           tail * (1.0f - fm_expf(-t / tail_time(p->p[KP_TTIME])));
}

float kdsp_length(const kparm_t *p)
{
    float d = dec_time(p->p[KP_DEC]), len;
    int type = sel(p->p[KP_TYPE], KT_N);
    if (type == KT_808)
        d *= 1.0f + 2.0f * u01(p->p[KP_CD]);
    if (type == KT_ANALOG) {
        float r = u01(p->p[KP_CC]), ring = 0.015f * fm_exp2f(r * 9.0f) * 3.0f;
        if (ring < d * 0.55f)
            d = ring / 0.55f;
    }
    len = atk_time(p->p[KP_ATK]) + hold_time(p->p[KP_HOLD]) + 0.55f * d / fm_exp2f((p->p[KP_DROP] - 64.0f) / 32.0f);
    return len < 0.03f ? 0.03f : len;
}

void kdsp_init(kick_t *k, uint32_t seed)
{
    uint32_t i;
    uint8_t *b = (uint8_t *)k;
    for (i = 0; i < sizeof *k; i++)
        b[i] = 0;
    k->rng = seed ? seed : 0x1234567u;
    k->sign = 1.0f;
    k->t = 10.0f;
    k->note = k->note_from = 36.0f;
    k->glide_t = 10.0f;
    for (i = 0; i < 3u; i++)
        k->eq[i].b0 = 1.0f;
}

void kdsp_hit(kick_t *k, const kparm_t *p, float semis, float vel, int flags)
{
    float note = p->p[KP_TUNE] + semis, acc = u01(p->p[KP_ACC]), vs = u01(p->p[KP_VSENS]);
    if (vel < 0.0f)
        vel = 0.0f;
    if (vel > 1.0f)
        vel = 1.0f;
    if (!(flags & KH_ACC))                  /* ACCENT: how much quieter the plain steps are */
        vel *= 1.0f - 0.45f * acc;
    if ((flags & KH_SLIDE) && k->active) {  /* a slide: no strike, the note glides there; the envelopes go on */
        float g = k->glide_t / glide_time(p->p[KP_GLIDE]);
        k->note_from = g >= 1.0f ? k->note : k->note_from + (k->note - k->note_from) * g;
        k->note = note;
        k->glide_t = 0.0f;
        return;
    }
    k->tail = k->last;                      /* what was sounding fades out under the new hit: no click */
    k->note = k->note_from = note;
    k->glide_t = 10.0f;
    k->sign = (flags & KH_REV) ? -1.0f : 1.0f;
    k->sign *= 1.0f - u01(p->p[KP_VBEND]) * (1.0f - vel);    /* a soft hit sweeps less */
    k->drift = rnd_bi(&k->rng) * u01(p->p[KP_DRIFT]) * 0.4f;
    k->vel = 1.0f - vs * (1.0f - vel);
    k->t = 0.0f;
    k->stage = 0;
    k->amp = k->amp2 = 0.0f;
    k->ph = k->ph2 = k->fb = 0.0f;
    k->zr = k->zi = 0.0f;
    k->menv = 1.0f;
    k->mclk = 1.0f;
    k->mnse = 1.0f;
    k->sub_ph = 0.0f;
    k->sub_amp = 1.0f;
    k->nse_env = 1.0f;
    k->clk_env = 1.0f;
    k->clk_ph = 0.0f;
    k->crush_n = 1e9f;
    {   /* ANALOG's strike: a quarter of a period at the starting pitch, the resonator's sine starts at zero */
        float f = kdsp_note_hz(note + k->drift + p->p[KP_BEND] * (60.0f / 127.0f) * (k->sign > 0.0f ? 1.0f : -1.0f));
        f = f < 10.0f ? 10.0f : f > 8000.0f ? 8000.0f : f;
        k->exc_left = FS / (4.0f * f);
        if (k->exc_left < 1.0f)
            k->exc_left = 1.0f;
        k->exc_x = 1.0f / (0.9f * k->exc_left);
    }
    k->active = 1;
}

static void bq_set(kbq_t *q, int kind, float f, float db)    /* 0 low shelf, 1 peak, 2 high shelf (RBJ) */
{
    float A = fm_exp2f(db * 0.0830482f), w = TWO_PI * f * INV_FS, cs = fm_cosf(w), sn = fm_sinf(w);
    float b0, b1, b2, a0, a1, a2, sa = fm_sqrtf(A);
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
        float al = sn * 0.5f * 1.41421356f, t = 2.0f * sa * al;
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
static inline float bq_run(kbq_t *q, float x)
{
    float y = q->b0 * x + q->z1;
    q->z1 = fm_flush(q->b1 * x - q->a1 * y + q->z2);
    q->z2 = fm_flush(q->b2 * x - q->a2 * y);
    return y;
}

static void prep(kick_t *k, const kparm_t *p)
{
    float d = dec_time(p->p[KP_DEC]);
    k->c_type = sel(p->p[KP_TYPE], KT_N);
    k->c_bend = p->p[KP_BEND] * (60.0f / 127.0f);
    k->c_tinv = 1.0f / sweep_time(p->p[KP_TIME]);
    k->c_curve = u01(p->p[KP_CURVE]);
    k->c_tail = p->p[KP_TAIL] * (24.0f / 127.0f);
    k->c_ttinv = 1.0f / tail_time(p->p[KP_TTIME]);
    k->c_glide = 1.0f / glide_time(p->p[KP_GLIDE]);
    k->c_dwob = p->p[KP_DRIFT] * (0.15f / 127.0f);
    k->c_atk = atk_time(p->p[KP_ATK]);
    k->c_hold = hold_time(p->p[KP_HOLD]);
    k->c_dec = fm_expf(-9.2103f / (d * FS));
    k->c_drop = fm_exp2f((p->p[KP_DROP] - 64.0f) / 32.0f);
    k->c_punch = u01(p->p[KP_PUNCH]) * 1.5f;
    k->c_a = u01(p->p[KP_CA]);
    k->c_b = u01(p->p[KP_CB]);
    k->c_c = u01(p->p[KP_CC]);
    k->c_d = u01(p->p[KP_CD]);
    k->c_dec2 = fm_expf(-9.2103f / (d * 3.0f * FS));
    switch (k->c_type) {
    case KT_808: k->c_lp_a = k->c_a >= 0.99f ? 1.0f : onepole(200.0f * fm_exp2f(k->c_a * 6.0f)); break;
    case KT_909: k->c_lp_a = onepole(500.0f * fm_exp2f(k->c_c * 5.0f)); break;
    case KT_606: k->c_lp_a = k->c_a >= 0.99f ? 1.0f : onepole(300.0f * fm_exp2f(k->c_a * 6.0f)); break;
    case KT_ANALOG: k->c_lp_a = onepole(200.0f * fm_exp2f(k->c_a * 6.32f)); break;
    default: k->c_lp_a = 1.0f; break;
    }
    if (k->c_type == KT_ANALOG) {
        float q = 0.015f * fm_exp2f(k->c_c * 9.0f);
        k->c_pole = k->c_c >= 0.995f ? 1.00004f : fm_expf(-1.0f / (q * FS));
    }
    k->c_ratio = FM_RATIO[sel(k->c_a * 11.0f, 12)];
    k->c_mdec = fm_expf(-1.0f / (0.001f * fm_exp2f(k->c_c * 8.97f) * FS));    /* 1 ms .. 500 ms */
    k->c_mclk_d = fm_expf(-1.0f / (0.0008f * FS));
    k->c_mclk_a = onepole(2500.0f);
    k->c_ctype = sel(p->p[KP_CTYPE], CK_N);
    k->c_clvl = sq01(p->p[KP_CLVL]);
    k->c_ctone = onepole(300.0f * fm_exp2f(u01(p->p[KP_CTONE]) * 5.64f));
    k->c_cdec = fm_expf(-1.0f / (0.0002f * fm_exp2f(u01(p->p[KP_CLEN]) * 7.23f) * FS));   /* 0.2 .. 30 ms */
    k->c_slvl = sq01(p->p[KP_SLVL]);
    k->c_sint = SINT_ST[sel(p->p[KP_SINT], 4)];
    k->c_sdec = fm_expf(-9.2103f / (d * 0.25f * fm_exp2f(u01(p->p[KP_SDEC]) * 4.0f) * FS));
    k->c_sdrv = 1.0f + 4.0f * u01(p->p[KP_SDRV]);
    k->c_nlvl = sq01(p->p[KP_NLVL]);
    k->c_nf = fm_tanf(PI_F * 100.0f * fm_exp2f(u01(p->p[KP_NTONE]) * 7.0f) * INV_FS);
    k->c_nq = 0.1f + 1.9f * u01(p->p[KP_NBW]);
    k->c_ndec = fm_expf(-1.0f / (0.005f * fm_exp2f(u01(p->p[KP_NDEC]) * 7.64f) * FS));    /* 5 ms .. 1 s */
    k->c_dtype = sel(p->p[KP_DTYPE], DT_N);
    k->c_drive = 1.0f + sq01(p->p[KP_DRIVE]) * 24.0f;
    k->c_shape = u01(p->p[KP_SHAPE]);
    k->c_dmix = u01(p->p[KP_DMIX]);
    k->c_cstep = 1.0f + sq01(p->p[KP_DRIVE]) * 30.0f;
    k->c_cq = fm_exp2f(15.0f - 12.0f * k->c_shape);
    k->c_ftype = sel(p->p[KP_FTYPE], FT_N);
    k->c_cut = 20.0f * fm_exp2f(u01(p->p[KP_CUT]) * 10.0f);
    k->c_res = 2.0f - 1.95f * u01(p->p[KP_RES]);
    k->c_fenv = (p->p[KP_FENV] - 64.0f) / 64.0f * 5.0f;
    bq_set(&k->eq[0], 0, 90.0f, (p->p[KP_EQLO] - 64.0f) / 64.0f * 12.0f);
    bq_set(&k->eq[1], 1, 60.0f * fm_exp2f(u01(p->p[KP_EQFRQ]) * 6.7f), (p->p[KP_EQMID] - 64.0f) / 64.0f * 12.0f);
    bq_set(&k->eq[2], 2, 6000.0f, (p->p[KP_EQHI] - 64.0f) / 64.0f * 12.0f);
    k->c_body = sq01(p->p[KP_BODY]) * 1.2f;
    k->c_level = sq01(p->p[KP_LEVEL]) * 1.1f;
    k->c_clip = u01(p->p[KP_CLIP]);
}

static inline float tri_turns(float ph)    /* a triangle that starts at zero, rising */
{
    return ph < 0.25f ? 4.0f * ph : ph < 0.75f ? 2.0f - 4.0f * ph : 4.0f * ph - 4.0f;
}

static inline float dist(kick_t *k, float x)
{
    float g = k->c_drive, s = k->c_shape, y;
    switch (k->c_dtype) {
    default:
    case DT_SOFT: {
        float a = s * 0.6f;
        y = fm_tanhf(x * g + a) - fm_tanhf(a);
        break;
    }
    case DT_HARD: {
        float v = x * g, c = v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v, t = fm_tanhf(v);
        y = t + (c - t) * s;
        break;
    }
    case DT_FOLD:
        y = fm_sinf(fm_tanhf(x * 0.8f) * (1.0f + (g - 1.0f) * 0.25f) * (1.0f + 3.0f * s) * (PI_F * 0.5f));
        break;
    case DT_CRUSH:
        k->crush_n += 1.0f;
        if (k->crush_n >= k->c_cstep) {
            float v = fm_tanhf(x * 1.3f) * k->c_cq;
            k->crush_n = k->crush_n >= 1e8f ? 0.0f : k->crush_n - k->c_cstep;
            k->crush_hold = fm_floorf(v + 0.5f) / k->c_cq;
        }
        y = k->crush_hold;
        break;
    case DT_TUBE: {                      /* asymmetric: the even harmonics of a valve */
        float v = x * g;
        y = fm_tanhf(v + s * 0.9f * v * v);
        break;
    }
    case DT_RECT: {                      /* towards a full-wave rectifier: an octave up */
        float t = fm_tanhf(x * g);
        y = t + s * ((t < 0.0f ? -t : t) - t);
        break;
    }
    }
    return x + (y - x) * k->c_dmix;
}

void kdsp_render(kick_t *k, const kparm_t *p, float *out, uint32_t n)
{
    uint32_t i;
    prep(k, p);
    for (i = 0; i < n; i++) {
        float t = k->t, note, f, mix = 0.0f, y, ae = 0.0f;
        if (k->glide_t < 1.0f / k->c_glide) {
            note = k->note_from + (k->note - k->note_from) * (k->glide_t * k->c_glide);
            k->glide_t += INV_FS;
        } else {
            note = k->note;
        }
        k->wob += 0.37f * INV_FS;
        if (k->wob >= 1.0f)
            k->wob -= 1.0f;
        note += k->drift + k->c_dwob * fm_sin_turns(k->wob);
        note += k->sign * k->c_bend * sweep_shape(t * k->c_tinv, k->c_curve) - k->c_tail * (1.0f - fm_expf(-t * k->c_ttinv));
        if ((k->c_type == KT_808 || k->c_type == KT_606) && k->active)
            note += (k->c_type == KT_808 ? k->c_b : k->c_c) * 36.0f * fm_expf(-t * (1.0f / 0.0015f));   /* SNAP */
        if (k->active) {
            float a, body = 0.0f, x = 0.0f;
            /* the amplitude: attack, hold, decay shaped by DROP, PUNCH on top */
            if (k->stage == 0) {
                if (t < k->c_atk) {
                    k->amp = t / k->c_atk;
                } else {
                    k->amp = 1.0f;
                    if (t >= k->c_atk + k->c_hold)
                        k->stage = 1;
                }
                k->amp2 = k->amp;
            } else {
                k->amp *= k->c_dec;
                k->amp2 *= k->c_dec2;
            }
            a = k->c_drop == 1.0f ? k->amp : (k->amp > 1e-6f ? fm_exp2f(k->c_drop * fm_log2f(k->amp)) : 0.0f);
            if (k->c_type == KT_808 && k->c_d > 0.0f && k->amp2 * k->c_d * 0.8f > a)   /* BOOM: a slower tail */
                a = k->amp2 * k->c_d * 0.8f;
            if (k->c_type == KT_909 && k->c_d > 0.0f && a > 1e-6f)                     /* COMP: held up */
                a = fm_exp2f((1.0f - 0.7f * k->c_d) * fm_log2f(a));
            a *= 1.0f + k->c_punch * fm_expf(-t * (1.0f / 0.008f));
            ae = a;
            f = kdsp_note_hz(note);
            f = f < 8.0f ? 8.0f : f > 12000.0f ? 12000.0f : f;
            k->ph += f * INV_FS;
            if (k->ph >= 1.0f)
                k->ph -= 1.0f;
            switch (k->c_type) {
            case KT_808: {
                float s = fm_sin_turns(k->ph), g = 1.0f + 5.0f * k->c_c;
                if (k->c_c > 0.0f)
                    s = fm_tanhf(s * g) / fm_tanhf(g);
                k->body_lp += k->c_lp_a * (s - k->body_lp);
                body = k->body_lp + k->c_a * 0.5f * k->mclk;
                break;
            }
            case KT_909: {
                float s = fm_sin_turns(k->ph), tr = tri_turns(k->ph), nz = rnd_bi(&k->rng) * k->mnse;
                body = tr + (s - tr) * k->c_b;
                k->mclk_lp += k->c_lp_a * (nz - k->mclk_lp);
                x = k->c_a * (0.9f * k->mclk + 1.4f * k->mclk_lp);
                break;
            }
            case KT_606: {
                float nz = rnd_bi(&k->rng) * k->mnse;
                k->ph2 += f * 1.6f * INV_FS;
                if (k->ph2 >= 1.0f)
                    k->ph2 -= 1.0f;
                body = fm_sin_turns(k->ph) + k->c_d * 0.5f * fm_sin_turns(k->ph2) * a;
                k->body_lp += k->c_lp_a * (body - k->body_lp);
                body = k->body_lp;
                k->mclk_lp += 0.35f * (nz - k->mclk_lp);          /* a band of noise round 3 kHz */
                k->mlp2 += 0.1f * (k->mclk_lp - k->mlp2);
                x = k->c_b * 1.6f * (k->mclk_lp - k->mlp2);
                break;
            }
            case KT_ANALOG: {                                     /* a phasor struck, ringing at RESO, saturating */
                float cs = fm_sin_turns(f * INV_FS + 0.25f), sn = fm_sin_turns(f * INV_FS), zr, zi, m2, ex = 0.0f;
                float lim = 1.6f - k->c_d;
                if (k->exc_left > 0.0f) {
                    ex = k->exc_x * (k->exc_left >= 1.0f ? 1.0f : k->exc_left);
                    k->exc_left -= 1.0f;
                }
                zr = (k->zr * cs - k->zi * sn) * k->c_pole + ex;
                zi = (k->zr * sn + k->zi * cs) * k->c_pole;
                m2 = zr * zr + zi * zi;
                if (m2 > lim * lim * 0.35f) {
                    float m = fm_sqrtf(m2), g = fm_tanhf(m / lim) * lim / m;
                    zr *= g;
                    zi *= g;
                }
                k->zr = fm_flush(zr);
                k->zi = fm_flush(zi);
                body = zi;
                k->mclk_lp += k->c_lp_a * (k->mclk - k->mclk_lp);
                x = k->c_b * 1.1f * k->mclk_lp;
                break;
            }
            case KT_FM: {
                float m;
                k->ph2 += f * k->c_ratio * INV_FS;
                if (k->ph2 >= 1.0f)
                    k->ph2 -= 1.0f;
                m = fm_sin_turns(k->ph2 + k->fb * k->c_d * 0.2f);
                k->fb = m;
                body = fm_sin_turns(k->ph + m * k->c_b * 1.3f * k->menv);
                k->menv *= k->c_mdec;
                break;
            }
            case KT_SHAPER: {
                float s = fm_sin_turns(k->ph), src, yv;
                src = (s + k->c_d * (0.5f * fm_sin_turns(2.0f * k->ph) + 0.33f * fm_sin_turns(3.0f * k->ph))) /
                      (1.0f + 0.83f * k->c_d);
                yv = src + k->c_a * 0.6f * (4.0f * src * src * src - 3.0f * src);
                if (k->c_b > 0.0f)
                    yv = fm_sinf(yv * (1.0f + 3.0f * k->c_b) * (PI_F * 0.5f));
                body = yv + k->c_c * 0.6f * (yv * yv - 0.5f);
                break;
            }
            }
            k->mclk *= k->c_mclk_d;
            k->mnse *= k->c_type == KT_909 ? 0.9992f : 0.9975f;
            mix = body * k->c_body * a + x;
            /* the sub */
            if (k->c_slvl > 0.0f) {
                float s;
                k->sub_ph += kdsp_note_hz(note + k->c_sint) * INV_FS;
                if (k->sub_ph >= 1.0f)
                    k->sub_ph -= 1.0f;
                if (k->stage)
                    k->sub_amp *= k->c_sdec;
                s = fm_sin_turns(k->sub_ph);
                if (k->c_sdrv > 1.0f)
                    s = fm_tanhf(s * k->c_sdrv) / fm_tanhf(k->c_sdrv);
                mix += s * k->c_slvl * k->sub_amp * (k->stage ? 1.0f : k->amp);
            }
            /* the noise: a band, its own decay */
            if (k->c_nlvl > 0.0f) {
                float nz = rnd_bi(&k->rng), hp, bp;
                hp = (nz - (k->c_nq + k->c_nf) * k->nse_s1 - k->nse_s2) / (1.0f + k->c_nf * (k->c_nf + k->c_nq));
                bp = k->c_nf * hp + k->nse_s1;
                k->nse_s1 = fm_flush(bp + k->c_nf * hp);
                k->nse_s2 = fm_flush(k->nse_s2 + 2.0f * k->c_nf * bp);
                mix += bp * k->c_nq * 1.5f * k->c_nlvl * k->nse_env;
                k->nse_env *= k->c_ndec;
            }
            /* the click */
            if (k->c_clvl > 0.0f && k->clk_env > 1e-5f) {
                float src;
                switch (k->c_ctype) {
                default:
                case CK_PULSE: src = k->clk_env; break;
                case CK_NOISE: src = rnd_bi(&k->rng) * k->clk_env * 1.5f; break;
                case CK_TICK:
                    k->clk_hp += 0.02f * (k->clk_env - k->clk_hp);
                    src = (k->clk_env - k->clk_hp) * 2.0f;
                    break;
                case CK_ZAP:
                    k->clk_ph += (300.0f + 6000.0f * k->clk_env) * INV_FS;
                    if (k->clk_ph >= 1.0f)
                        k->clk_ph -= 1.0f;
                    src = fm_sin_turns(k->clk_ph) * k->clk_env;
                    break;
                case CK_SNAP: {
                    float nz = rnd_bi(&k->rng) * k->clk_env;
                    k->clk_hp += 0.15f * (nz - k->clk_hp);
                    src = (nz - k->clk_hp) * 2.0f;
                    break;
                }
                }
                k->clk_lp += k->c_ctone * (src - k->clk_lp);
                mix += k->clk_lp * k->c_clvl;
                k->clk_env *= k->c_cdec;
            }
            mix *= k->vel;
            k->t = t + INV_FS;
            if (k->stage && k->amp < 1e-5f && k->clk_env < 1e-5f && k->mclk < 1e-5f &&
                (k->c_nlvl == 0.0f || k->nse_env < 1e-5f) && (k->c_slvl == 0.0f || k->sub_amp < 1e-5f) &&
                (k->c_type != KT_808 || k->amp2 * k->c_d < 1e-5f))
                k->active = 0;
        }
        mix += k->tail;
        k->tail = fm_flush(k->tail * 0.985f);
        k->last = mix;
        /* distortion */
        y = k->c_dmix > 0.0f ? dist(k, mix) : mix;
        /* the filter, its envelope following the body's */
        if (!(k->c_ftype == FT_LP && k->c_cut >= 19000.0f && k->c_fenv == 0.0f)) {
            float fc = k->c_cut * (k->c_fenv != 0.0f ? fm_exp2f(k->c_fenv * ae) : 1.0f), g, h, hp, bp, lp;
            fc = fc < 20.0f ? 20.0f : fc > 18000.0f ? 18000.0f : fc;
            g = fm_tanf(PI_F * fc * INV_FS);
            h = 1.0f / (1.0f + g * (g + k->c_res));
            hp = (y - (g + k->c_res) * k->f_s1 - k->f_s2) * h;
            bp = g * hp + k->f_s1;
            k->f_s1 = fm_flush(bp + g * hp);
            lp = g * bp + k->f_s2;
            k->f_s2 = fm_flush(lp + g * bp);
            y = k->c_ftype == FT_LP ? lp : k->c_ftype == FT_HP ? hp : k->c_ftype == FT_BP ? bp * k->c_res : y - bp * k->c_res;
        }
        /* EQ, clip, DC */
        y = bq_run(&k->eq[2], bq_run(&k->eq[1], bq_run(&k->eq[0], y)));
        if (k->c_clip > 0.0f) {
            float g = 1.0f + 3.0f * k->c_clip;
            y = y + (fm_tanhf(y * g) / fm_tanhf(g) - y) * k->c_clip;
        }
        k->dc_y = fm_flush(y - k->dc_x + 0.9993f * k->dc_y);
        k->dc_x = y;
        out[i] = k->dc_y * k->c_level;
    }
}
