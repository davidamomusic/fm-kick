# FM-KICK 0.1.1

A kick drum synthesizer and generative sequencer for the M-VAVE FM-1, built on fm1-base.

Beta software: it may fail and contain errors. Use it at your own risk.

## The sound

A resonator struck by a short pulse makes the body. A pitch sweep of three points (start, curve, tail) bends it,
a click of pulse and noise gives the attack, a clean sine sub sits under it, and four kinds of drive (soft, hard,
fold, crush) and a tone filter shape the end. 24 parameters on six pages:

| Page | K1 | K2 | K3 | K4 |
|---|---|---|---|---|
| PITCH | TUNE (note) | BEND (sweep depth) | TIME (sweep time) | CURVE (sweep shape) |
| SWEEP | TAIL (slow drop) | T.TIME (its time) | GLIDE (slide time) | DRIFT |
| AMP | ATTACK | DECAY | HOLD | LEVEL |
| CLICK | COLOUR | PULSE | NOISE | SNAP |
| LAYERS | BODY | SUB | SUB INT (-12, 0, +7, +12) | RESO (how long the body rings) |
| DRIVE | TYPE | DRIVE | SHAPE | TONE |

12 factory sounds (PRESETS knob): DEEP, PUNCH, RUMBLE, HARD, GABBA, TOM, DUB, CRUSH, HOUSE, ZAP, SOFT, FOLD.

## The sequencer

- **TRIG lane:** 16 steps. Each step: off, on or accent, pitch (-24..+24), ratchet (x1..x4), slide (the note glides,
  no new strike) and a reversed sweep. Length, division (1/32 to 1 bar, with triplets), direction (forward, reverse,
  ping-pong, random, random walk), swing, probability and humanize.
- **Generators:** the TRIG lane can be played by hand (MANUAL) or by one of nine generators, each with four knobs:
  EUCLID, CLAVE, BREAK, WALK, CELL (cellular automaton), GOLDEN (golden ratio), POLY, LOGISTIC (chaos) and WAVE
  (two waves interfering). Some of them change every bar.
- **Three MOD lanes:** each moves one sound parameter, with its own length, division, direction, shape (manual
  values, ramps, triangle, sine, square, random, walk), depth, smoothing and seed. With different lengths the kick
  keeps changing.
- **P-locks:** each step can lock up to four sound parameters, and the kick itself: a step can play another
  factory kick than the pattern's, with its own locks on top. The MOD lanes move the locked sound too.
- **16 patterns**, each with its own sound. A pattern picked while playing starts at the next bar.

## Controls

| Control | What it does |
|---|---|
| HOME | the kick: its waveform and its sweep, with TUNE, BEND, DECAY and DRIVE |
| SEQ | the TRIG lane (TRIG, then FEEL: tempo, probability, humanize) |
| ARP | the generator page: ALGORITHM picks the generator |
| EDIT | the sound pages, one after another; ENV goes to AMP, FX to DRIVE |
| LFO | the MOD lanes (each press: the next lane) |
| GLO | patterns (white keys pick one; copy, paste, clear, init, random) and MIDI |
| SELECT | the page |
| ALGORITHM | the tempo (on the generator page: the generator; on PATTERNS: the pattern) |
| PRESETS | the sound's factory preset |
| White keys | the steps: a tap toggles off, on, accent |
| White key held + knobs, on a SEQ page | the step's pitch, ratchet, slide, reversed sweep |
| White key held + knobs, on HOME or a sound page | a p-lock of that parameter for the step (up to four a step) |
| White key held + PRESETS | the kick of that step (a p-lock): a factory kick, or back to the pattern's |
| White key held + REC | the step's p-locks cleared |
| White key held + knob, on a MOD page | the MOD lane's value at that step |
| Black keys | the kick, a semitone apart (OCT+ held: accented) |
| PLAY | start / stop |
| REC | random for this page |
| OCT+ + REC | random sound and generator |
| OCT- + REC | random sequence and p-locks (steps, generator, feel, MOD lanes, and on some steps another kick and parameter locks) |
| OCT- + SAVE | print the generator's steps into the TRIG lane |
| OCT- + knobs | faster |
| SEL | tap tempo |
| SAVE | save now (it also saves by itself, stopped, 1.5 s after a change) |

MIDI in: any note plays the kick chromatically (36 = the pattern's own tune). MIDI out: each hit as a note (from 36),
and the clock if CLOCK OUT is on. CLOCK = MIDI follows an incoming clock, start and stop.

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
