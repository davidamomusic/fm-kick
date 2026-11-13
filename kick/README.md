# FM-KICK 0.3.0

A kick drum synthesizer with a sequencer of up to 32 steps for the M-VAVE FM-1, built on fm1-base.

Beta software: it may fail and contain errors. Use it at your own risk.

## The kick

Six kinds of body, each with four CHARACTER knobs of its own:

| Type | CHARACTER knobs |
|---|---|
| 808 | TONE, SNAP (a pitch spike), SAT, BOOM (a longer tail) |
| 909 | ATTACK (its click), SHAPE (triangle to sine), NOISE (the click's tone), COMP (the body held up) |
| 606 | TONE, NOISE (a band of noise), SNAP, RING (a second partial) |
| ANALOG | COLOUR, PULSE, RESO (how long the struck resonator rings), SATUR |
| FM | RATIO, INDEX, MOD DEC, FEEDBACK |
| WSHAPER | CURVE, DRIVE, BIAS, SOURCE |

WSHAPER sends a sine (or, with SOURCE, a triangle) through a waveshaper you shape yourself: CURVE morphs through
eight transfer curves (SOFT, HARD, S.FOLD, T.FOLD, CHEBY, STAIRS, RECT, SQUARE), DRIVE pushes the wave into it, BIAS
moves it off-centre (even harmonics). The CHARACTER page draws the curve live.

48 parameters on twelve EDIT pages:

| Page | K1 | K2 | K3 | K4 |
|---|---|---|---|---|
| KICK | TYPE | TUNE | DECAY | LEVEL |
| CHARACTER | (by type, above) | | | |
| PITCH | BEND | TIME | CURVE | TAIL |
| SWEEP | T.TIME | GLIDE | DRIFT | VEL>BEND |
| AMP | ATTACK | HOLD | DROP (volume drop) | PUNCH |
| CLICK | TYPE (PULSE, NOISE, TICK, ZAP, SNAP) | LEVEL | TONE | LENGTH |
| SUB | LEVEL | INTERVAL (-12, 0, +7, +12) | DECAY | DRIVE |
| NOISE | LEVEL | TONE | DECAY | WIDTH |
| DIST | TYPE (SOFT, HARD, FOLD, CRUSH, TUBE, RECT) | DRIVE | SHAPE | MIX |
| FILTER | TYPE (LP, HP, BP, NOTCH) | CUTOFF | RESO | ENV |
| EQ | LOW | MID | MID FREQ | HIGH |
| OUTPUT | BODY | VEL SENS | ACCENT | CLIP |

The last EDIT page, STORE, keeps your kicks: 32 user slots with a name of eight letters (K1 slot, K2 cursor, K3 letter,
K4 to the right saves, to the left loads). 18 factory kicks, three of each type. PRESETS goes through all of them.

## The effects

One rack for every pattern, on two FX pages (SELECT goes from one to the other):

- **LIVE:** twelve performance effects, each on a white key and **on only while the key is held**: DISTORT, CRUSH,
  FILTER, RING MOD, CHORUS, PHASER, FLANGER, DELAY, REVERB, STUTTER, GATE, WIDTH. Delay and reverb let their tail
  ring out after the key is released; the stutter and gate follow the tempo. The knobs set the last effect played.
- **FINAL:** the master chain, each on or off with its white key (1-5): TRANSIENT, COMP, EQ, CLIPPER, LIMITER.
  Their state is saved.

## The sequencer

- **Steps:** 16 by default, up to 32 (LENGTH on the TRIG page). SEL shows steps 1-16 or 17-32 on the white keys.
  Each step: off, on or accent, pitch (-24..+24), ratchet (x1..x4), slide and a reversed sweep.
- **P-locks:** any step can lock up to four parameters (the type of kick, the pitch, the decay, any of the 48) and
  the whole kick (another preset in place of the pattern's).
- **Generators (ARP):** MANUAL, EUCLID, POLY, METRIC, SYNCOPA, BOUNCE, MIRROR, FRACTAL, BITS and ROLLS, each with
  four knobs. OCT- + SAVE prints the generated steps into the TRIG lane.
- **Four LFOs (MOD lanes):** each moves one parameter, with its own length, division, direction, shape, depth,
  smoothing and seed.
- **16 patterns**, each with its own kick. A pattern picked while playing starts at the next bar.

## Controls

| Control | What it does |
|---|---|
| HOME | the kick: its waveform and its sweep, with TYPE, TUNE, BEND and DECAY |
| SEQ | TRIG (length, division, direction, swing) and FEEL (tempo, probability, humanize) |
| ARP | GENERATE: ALGORITHM picks the generator, the knobs shape it |
| EDIT | the kick's pages, one after another, then STORE; ENV goes to AMP |
| LFO | the four LFOs (each press: the next one; SELECT: its A and B pages) |
| FX | LIVE (held white keys) and FINAL (white keys on and off) |
| GLO | PATTERNS (white keys pick one; copy, paste, clear, init, random), MIDI and THEMES |
| SELECT | the page |
| ALGORITHM | the tempo (on GENERATE: the generator; on PATTERNS: the pattern) |
| PRESETS | the pattern's kick from the presets |
| Any knob | its name and value shown big for a moment |
| White keys | the steps: a tap toggles off, on, accent |
| White key held + knobs, on a SEQ or GENERATE page | the step's pitch, ratchet, slide, reversed sweep |
| White key held + knobs, on HOME or an EDIT page | a p-lock of that parameter for the step (up to four a step) |
| White key held + PRESETS | the kick of that step (a p-lock), or back to the pattern's |
| White key held + REC | the step's p-locks cleared |
| Black keys | the kick, a semitone apart (OCT+ held: accented) |
| REC | recording on or off: the 27 keys become a chromatic keyboard (C4 is the kick's own note), and the keys and MIDI notes go into the steps: while playing into the nearest step, while stopped into the selected step and then the next one |
| OCT- + GLO | random kick parameters and trigs (with p-locks, also of the kick type); never the tempo, the LFOs or the effects |
| OCT+ + GLO | the same, and the four LFOs as well |
| OCT+ + REC | metronome on or off: a click on each beat (higher on the bar) and a metronome in the header |
| OCT- + SAVE | print the generator's steps into the TRIG lane |
| OCT- + knobs | faster |
| SEL | steps 1-16 or 17-32 on the white keys |
| PLAY | start / stop |
| SAVE | save now (it also saves by itself, stopped, 1.5 s after a change) |

## Themes

GLO's third page: K1 THEME (EMBER, NIGHT, ICE, ACID, MONO, PAPER), K2 the knobs as DIALS or BARS, K3 the scope FILLED
or as a LINE, K4 the steps as a RING or a GRID. Saved with the settings.

MIDI in: any note plays the kick chromatically (36 = the pattern's own tune), on the channel set on the MIDI page. MIDI
out: each hit as a note (from 36), and the clock if CLOCK OUT is on. CLOCK = MIDI follows an incoming clock, start and
stop.

## Build

From fm1-base, with this folder as `kick/`:

    tools/build.py kick --host       # the host simulator
    kick/build/host/fm-kick_host kick/tests/basic.txt kick/build/host
    tools/build.py kick --web        # the browser player: kick/build/emu/

The browser player needs a web server (`python3 -m http.server` in `kick/build/emu/`, then http://localhost:8000).
The built player is kept in `kick/docs/play/`: copy `kick/build/emu/` there after each version
(`node kick/tests/play_browser_test.mjs` checks it in a headless Chromium).

Without Emscripten, `kick/tools/emcc` stands in for `emcc`: clang 18, wasm-ld and wasi-libc (`wasi-sdk-22`), with
`WASI_SYSROOT` pointing at wasi-libc's sysroot and `kick/tools` first on the `PATH`.

Tests: `basic`, `locks`, `rec`, `fx`, `store`, `themes`, `ws`, `audio` and `screens` in `kick/tests/`.
