/* SPDX-License-Identifier: MPL-2.0 */
/* FM-KICK's own exports for its browser pages (app.h APP_WEB): fm1-base's web/emu/emu_web.c takes this in after the
 * whole app, so it reaches the app's own state. Render Audio (kick/web/emu/render.js, on the player and the mirror
 * page) runs a copy of the app in a worker (render_worker.js): it picks a pattern, with or without the effects, and how
 * many 16ths, then asks for audio (web_render) until the sequencer stops by itself. Never on the FM-1. */

#define WEB_EXPORT __attribute__((used, visibility("default")))

WEB_EXPORT int kick_npat(void) { return NBANK * NPAT; }         /* A01..C16 */
WEB_EXPORT int kick_pat_cur(void) { return (int)PBANK * NPAT + sq.cur; }
WEB_EXPORT float kick_bpm(void) { return (float)set.bpm10 / 10.0f; }
WEB_EXPORT float kick_step_len(void) { return 44100.0f * 60.0f / ((float)set.bpm10 / 10.0f) / 4.0f; }   /* a 16th */

/* 1: pattern pi (bank pi / 16) has something to play: a step on, or a generator */
WEB_EXPORT int kick_pat_used(int pi)
{
    static pats_t t;
    int b = pi / NPAT, w = pi % NPAT;
    if (pi < 0 || b >= NBANK)
        return 0;
    if (b == (int)PBANK)
        return pat_used(&pats(w));
    if (plat_store_load(PAT_OBJ_ALL[b * 4 + w / 4], &t, sizeof t) != (int)sizeof t || !pats_take(&t))
        return 1;                        /* a bank never saved: a new bank's patterns, a kick on each beat */
    return pat_valid(&t.p[w % 4]) && pat_used(&t.p[w % 4]);
}

/* a render from the first step: pattern pi (-1 the one playing), what 0 with the effects, 1 the kick dry, for steps
 * 16ths. The sound starts from silence (the effects too) on the app's own tempo and clock; what was already made and not
 * yet asked for is dropped */
WEB_EXPORT int kick_render_begin(int what, int pi, int steps)
{
    seq_stop();
    sq.next = 0xFFu;
    set.clk = 0;
    if (pi >= 0 && pi < NBANK * NPAT) {
        if (pi / NPAT != (int)PBANK)
            bank_switch(pi / NPAT);
        sq.cur = (uint8_t)(pi % NPAT);
    }
    web_dry = (uint8_t)(what == 1);
    fx_live = 0;
    kdsp_init(&voice, 0xC0FFEEu);
    gleft = 0;
    fx_reset();
    tempo_set();
    eff_update();
    seq_start();
    web_end = steps > 0 ? (uint32_t)steps * (PPQ / 4u) : 0u;
    ring_r = ring_w;
    return (int)PBANK * NPAT + sq.cur;
}
