/* SPDX-License-Identifier: MPL-2.0 */
/* FM-KICK: the kick voice (dsp.h). Compiled on its own at -O2 (fm1.toml o2). */
#include "dsp.h"
#include "fastmath.h"

#define FS 44100.0f
#define INV_FS (1.0f / 44100.0f)
#define PI_F 3.14159265f

static const float SUBT_ST[4] = {-12.0f, 0.0f, 7.0f, 12.0f};

static inline uint32_t xr(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *s = x;
}
static inline float rnd_bi(uint32_t *s) { return (float)(int32_t)xr(s) * (1.0f / 2147483648.0f); }   /* -1..1 */

float kdsp_note_hz(float note) { return 440.0f * fm_exp2f((note - 69.0f) * (1.0f / 12.0f)); }

/* the parameters' times */
static float sweep_time(float v) { return 0.001f * fm_exp2f(v * (8.6439f / 127.0f)); }       /* 1 ms .. 400 ms */
static float tail_time(float v) { return 0.01f * fm_exp2f(v * (8.2288f / 127.0f)); }         /* 10 ms .. 3 s */
static float dec_time(float v) { return 0.02f * fm_exp2f(v * (8.2288f / 127.0f)); }          /* 20 ms .. 6 s */
static float atk_time(float v) { float u = v * (1.0f / 127.0f); return 0.05f * u * u; }      /* 0 .. 50 ms */
static float hold_time(float v) { float u = v * (1.0f / 127.0f); return 0.3f * u * u; }      /* 0 .. 300 ms */
static float snap_time(float v) { return 0.0002f * fm_exp2f(v * (6.6439f / 127.0f)); }       /* 0.2 .. 20 ms */
static float glide_time(float v) { return 0.001f + 0.5f * (v / 127.0f) * (v / 127.0f); }     /* 1 .. 500 ms */
static float sq01(float v) { float u = v * (1.0f / 127.0f); return u * u; }

/* the sweep's shape at x = t / time: CURVE 0 a long hyperbolic tail, 64 exponential, 127 held then fast (gaussian) */
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
    float u = p->p[KP_CURVE] * (1.0f / 127.0f);
    float tail = p->p[KP_TAIL] * (24.0f / 127.0f);
    return p->p[KP_TUNE] + bend * sweep_shape(t / sweep_time(p->p[KP_TIME]), u)
           - tail * (1.0f - fm_expf(-t / tail_time(p->p[KP_TTIME])));
}

float kdsp_length(const kparm_t *p)
{
    float r = p->p[KP_RESO] * (1.0f / 127.0f);
    float ring = 0.015f * fm_exp2f(r * 9.0f) * 3.0f;   /* a dull resonator dies before its envelope does */
    float d = 0.55f * dec_time(p->p[KP_DEC]);
    float body = atk_time(p->p[KP_ATK]) + hold_time(p->p[KP_HOLD]) + (d < ring || p->p[KP_SUB] > 8.0f ? d : ring);
    return body < 0.03f ? 0.03f : body;
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
}

void kdsp_hit(kick_t *k, const kparm_t *p, float semis, float vel, int slide, int rev)
{
    float note = p->p[KP_TUNE] + semis;
    float dr = p->p[KP_DRIFT] * (1.0f / 127.0f);
    if (vel < 0.0f)
        vel = 0.0f;
    if (vel > 1.0f)
        vel = 1.0f;
    if (slide && k->active) {               /* a slide: no strike, the note glides there; the envelopes go on */
        float g = k->glide_t / glide_time(p->p[KP_GLIDE]);
        k->note_from = g >= 1.0f ? k->note : k->note_from + (k->note - k->note_from) * g;
        k->note = note;
        k->glide_t = 0.0f;
        if (vel > k->vel)
            k->vel = vel;
        return;
    }
    k->tail = k->last;                      /* what was sounding fades out under the new hit: no click */
    k->note = k->note_from = note;
    k->glide_t = 10.0f;
    k->sign = rev ? -1.0f : 1.0f;
    k->drift = rnd_bi(&k->rng) * dr * 0.4f;
    k->vel = vel;
    k->t = 0.0f;
    k->amp = 0.0f;
    k->stage = 0;
    k->zr = k->zi = 0.0f;
    k->sub_ph = 0.0f;
    k->clk = 1.0f;
    k->nse = 1.0f;
    k->crush_n = 1e9f;                      /* the crusher takes its first sample at once */
    {   /* the strike: a quarter of a period at the starting pitch, the resonator's sine starts at its zero */
        float f = kdsp_note_hz(note + k->drift + p->p[KP_BEND] * (60.0f / 127.0f) * k->sign);
        if (f < 10.0f)
            f = 10.0f;
        if (f > 8000.0f)
            f = 8000.0f;
        k->exc_left = FS / (4.0f * f);
        if (k->exc_left < 1.0f)
            k->exc_left = 1.0f;
        k->exc_h = 1.0f;
        k->exc_x = k->exc_h / (0.9f * k->exc_left);   /* a quarter period's pushes add up to EXC_H */
    }
    k->active = 1;
}

/* the per-block coefficients */
static void prep(kick_t *k, const kparm_t *p)
{
    float r = p->p[KP_RESO] * (1.0f / 127.0f), q;
    k->c_bend = p->p[KP_BEND] * (60.0f / 127.0f);
    k->c_tinv = 1.0f / sweep_time(p->p[KP_TIME]);
    k->c_curve = p->p[KP_CURVE] * (1.0f / 127.0f);
    k->c_tail = p->p[KP_TAIL] * (24.0f / 127.0f);
    k->c_ttinv = 1.0f / tail_time(p->p[KP_TTIME]);
    k->c_glide = 1.0f / glide_time(p->p[KP_GLIDE]);
    k->c_atk = atk_time(p->p[KP_ATK]);
    k->c_hold = hold_time(p->p[KP_HOLD]);
    k->c_dec = fm_expf(-9.2103f / (dec_time(p->p[KP_DEC]) * FS));    /* to -80 dB in DEC */
    k->c_level = sq01(p->p[KP_LEVEL]) * 1.1f;
    k->c_clk = fm_expf(-1.0f / (snap_time(p->p[KP_SNAP]) * FS));
    k->c_nse = fm_expf(-1.0f / (2.5f * snap_time(p->p[KP_SNAP]) * FS));
    {
        float fc = 200.0f * fm_exp2f(p->p[KP_COLOUR] * (6.3219f / 127.0f));   /* 200 Hz .. 16 kHz */
        k->c_colour = 1.0f - fm_expf(-2.0f * PI_F * fc * INV_FS);
    }
    k->c_pulse = sq01(p->p[KP_PULSE]) * 1.1f;
    k->c_noise = sq01(p->p[KP_NOISE]) * 0.9f;
    k->c_body = sq01(p->p[KP_BODY]) * 1.15f;
    k->c_sub = sq01(p->p[KP_SUB]) * 0.95f;
    {
        int s = (int)(p->p[KP_SUBT] + 0.5f);
        k->c_subt = SUBT_ST[s < 0 ? 0 : s > 3 ? 3 : s];
    }
    /* RESO: how long the resonator rings on its own, 15 ms (a thud) .. 7 s; at the top it rings on, held by the
     * saturation, and only the amplitude envelope stops it */
    q = 0.015f * fm_exp2f(r * 9.0f);
    k->c_pole = r >= 0.995f ? 1.00004f : fm_expf(-1.0f / (q * FS));
    {
        int d = (int)(p->p[KP_DTYPE] + 0.5f);
        k->c_dtype = d < 0 ? 0 : d >= DT_N ? DT_N - 1 : d;
    }
    k->c_drive = 1.0f + sq01(p->p[KP_DRIVE]) * 24.0f;
    k->c_shape = p->p[KP_SHAPE] * (1.0f / 127.0f);
    k->c_crush_step = 1.0f + sq01(p->p[KP_DRIVE]) * 30.0f;
    k->c_crush_q = fm_exp2f(15.0f - 12.0f * k->c_shape);
    {
        float fc = 200.0f * fm_exp2f(p->p[KP_TONE] * (6.6439f / 127.0f));     /* 200 Hz .. 20 kHz */
        k->c_tone = p->p[KP_TONE] >= 126.5f ? 1.0f : 1.0f - fm_expf(-2.0f * PI_F * fc * INV_FS);
    }
    k->c_dwob = p->p[KP_DRIFT] * (0.15f / 127.0f);
}

static inline float drive(kick_t *k, float x)
{
    float s = k->c_shape, y;
    switch (k->c_dtype) {
    default:
    case DT_SOFT: {
        float a = s * 0.6f;
        y = fm_tanhf(x * k->c_drive + a) - fm_tanhf(a);
        break;
    }
    case DT_HARD: {
        float v = x * k->c_drive, c = v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v;
        y = fm_tanhf(v) + (c - fm_tanhf(v)) * s;
        break;
    }
    case DT_FOLD: {
        float v = fm_tanhf(x * 0.8f) * (1.0f + (k->c_drive - 1.0f) * 0.25f) * (1.0f + 3.0f * s);
        y = fm_sinf(v * (PI_F * 0.5f));
        break;
    }
    case DT_CRUSH:
        k->crush_n += 1.0f;
        if (k->crush_n >= k->c_crush_step) {
            float v = fm_tanhf(x * 1.3f) * k->c_crush_q;
            k->crush_n = k->crush_n >= 1e8f ? 0.0f : k->crush_n - k->c_crush_step;
            k->crush_hold = fm_floorf(v + 0.5f) / k->c_crush_q;
        }
        y = k->crush_hold;
        break;
    }
    return y;
}

void kdsp_render(kick_t *k, const kparm_t *p, float *out, uint32_t n)
{
    uint32_t i;
    float wob_inc = 0.37f * INV_FS;
    prep(k, p);
    for (i = 0; i < n; i++) {
        float t = k->t, note, f, mix, y;
        /* the note: the base (gliding after a slide), the sweep, the tail, the drift */
        if (k->glide_t < 1.0f / k->c_glide) {
            note = k->note_from + (k->note - k->note_from) * (k->glide_t * k->c_glide);
            k->glide_t += INV_FS;
        } else {
            note = k->note;
        }
        k->wob += wob_inc;
        if (k->wob >= 1.0f)
            k->wob -= 1.0f;
        note += k->drift + k->c_dwob * fm_sin_turns(k->wob);
        note += k->sign * k->c_bend * sweep_shape(t * k->c_tinv, k->c_curve) - k->c_tail * (1.0f - fm_expf(-t * k->c_ttinv));
        mix = 0.0f;
        if (k->active) {
            float bp, x = 0.0f, a;
            /* amplitude: attack, hold, decay */
            if (k->stage == 0) {             /* the attack, then the hold */
                if (t < k->c_atk) {
                    k->amp = t / k->c_atk;
                } else {
                    k->amp = 1.0f;
                    if (t >= k->c_atk + k->c_hold)
                        k->stage = 1;
                }
            } else {
                k->amp *= k->c_dec;
            }
            a = k->amp;
            /* the resonator, struck: a phasor turning at the note, pushed by the strike, decaying at RESO */
            f = kdsp_note_hz(note);
            if (f < 8.0f)
                f = 8.0f;
            if (f > 12000.0f)
                f = 12000.0f;
            if (k->exc_left > 0.0f) {
                x = k->exc_x * (k->exc_left >= 1.0f ? 1.0f : k->exc_left);
                k->exc_left -= 1.0f;
            }
            {
                float ph = f * INV_FS, cs = fm_sin_turns(ph + 0.25f), sn = fm_sin_turns(ph), zr, zi, m2;
                zr = (k->zr * cs - k->zi * sn) * k->c_pole + x;
                zi = (k->zr * sn + k->zi * cs) * k->c_pole;
                m2 = zr * zr + zi * zi;
                if (m2 > 0.5f) {             /* the ring saturates, as an analogue one does: round 1.2 at most */
                    float m = fm_sqrtf(m2), g2 = fm_tanhf(m * 0.8f) * 1.5f / m;
                    zr *= g2;
                    zi *= g2;
                }
                k->zr = fm_flush(zr);
                k->zi = fm_flush(zi);
                bp = zi;
            }
            mix += bp * k->c_body * a;
            /* the sub */
            k->sub_ph += kdsp_note_hz(note + k->c_subt) * INV_FS;
            if (k->sub_ph >= 1.0f)
                k->sub_ph -= 1.0f;
            mix += fm_sin_turns(k->sub_ph) * k->c_sub * a;
            /* the click: a pulse and a burst of noise, through the colour */
            k->clk_lp += k->c_colour * (k->clk - k->clk_lp);
            k->nse_lp += k->c_colour * (rnd_bi(&k->rng) * k->nse - k->nse_lp);
            k->clk *= k->c_clk;
            k->nse *= k->c_nse;
            mix += k->clk_lp * k->c_pulse + k->nse_lp * k->c_noise * 1.6f;
            mix *= 0.35f + 0.65f * k->vel;
            k->t = t + INV_FS;
            if (t > k->c_atk + k->c_hold && a < 1e-5f && k->clk < 1e-5f && k->nse < 1e-5f)
                k->active = 0;
        }
        mix += k->tail;
        k->tail *= 0.985f;
        k->tail = fm_flush(k->tail);
        k->last = mix;
        /* drive, tone, DC */
        y = drive(k, mix);
        k->tone_lp += k->c_tone * (y - k->tone_lp);
        y = k->tone_lp;
        k->dc_y = y - k->dc_x + 0.9993f * k->dc_y;
        k->dc_x = y;
        k->dc_y = fm_flush(k->dc_y);
        out[i] = k->dc_y * k->c_level;
    }
}
