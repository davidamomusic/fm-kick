# FM-KICK 0.3.8

A kick drum synthesizer with a sequencer of up to 32 steps for the M-VAVE FM-1, built on fm1-base.

Beta software: it may fail and contain errors. Use it at your own risk.

## Install

The installer is in `kick/releases/` (`fm-kick-0.3.8.fwsc`, identity `FM-1_038`: the identity follows the version, so
M-UPGRADE tells one version from the next). Install it with M-UPGRADE or The Firmware Hub's installer.

## A hit over a hit

A new hit while the kick still rings does not cut it: the old one goes on for 6 ms, fading out under the new one, so
the low wave never jumps (no click).

## Pattern banks

Three banks of sixteen patterns: A, B and C, on the first three black keys of PATTERNS. Only the bank in use is in
memory: another bank is read in from the flash, and what changed in the one left is written first (if it plays, the
sound stops for that moment). A bank never used starts as sixteen plain patterns. The pattern number stays.

## Screen mirror

[FM-KICK-mirror.html](https://davidamomusic.github.io/fm-kick/kick/docs/FM-KICK-mirror.html) shows the FM-1's screen and
panel live on your computer, over the USB cable (Chrome or Edge; allow MIDI with SysEx when it asks). With Control on,
you can play the FM-1 from the page. It is fm1-base's mirror page filled in for FM-KICK (`tools/make_site.py`); to make
it again:

    python3 -c "import sys; sys.path.insert(0, 'tools'); from pathlib import Path; import make_site; sys.stdout.write(make_site.tool_pages(Path('kick'))['mirror/index.html'])" > kick/docs/FM-KICK-mirror.html

## The kick

Six kinds of body, each with four CHARACTER knobs of its own:

| Type | CHARACTER knobs |
|---|---|
| 808 | a resonator rung by a 1 ms pulse, heading down first: TONE (the low-pass after it, and the pulse's click), SNAP (its centre up to two octaves higher for the first ~6 ms: the punch), SIGH (the pitch settling a few semitones down over a few hundred ms), BOOM (a longer tail) |
| 909 | a triangle reset on the hit, heading up, through a diode-like shaper, the bottom squashed a touch: ATTACK (a pulse and a burst of low-passed noise), SHAPE (raw triangle, rounded, overloaded), NOISE (the burst's brightness and length, 4-20 ms), COMP (the body held up); its fall from ~230 Hz is the PITCH page's BEND and TIME |
| 606 | the 808's resonator, higher and shorter, its jump ~3 ms: TONE, CLICK (the pulse's edges), SNAP, SIGH |
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
- LIVE, OCT- + a white key: the effect left on (a bar at its foot) until OCT- + the key again; OCT+ + a white key
  (LIVE or FINAL): that effect picked for the knobs, nothing played or switched.
- **FINAL:** the master chain, each on or off with its white key (1-5): TRANSIENT, COMP, EQ, DISTORTION, LIMITER.
  DISTORTION's knobs: TYPE (CLIP, SOFT, TUBE, FUZZ, FOLD, RECT), DRIVE, TONE and MIX.
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
| HOME | FM-KICK's boxing glove (pixel art, `kick/glove.h` from `kick/tools/make_glove.py`): each hit throws it up, a shock round it; no knobs |
| SEQ | KICK: the picked step's TYPE, ATTACK, BEND and DECAY, as p-locks |
| ARP | GENERATE (ALGORITHM picks the generator, the knobs shape it), TRIG (length, division, direction: FWD, REV, PING, RAND, WANDER; swing) and FEEL (tempo, probability, humanize, NUDGE: the whole sequence a step later or earlier a click, round, its locks and a generator's steps with it) |
| EDIT | the kick's pages, one after another, then STORE; ENV goes to AMP; the waveform shows the pattern's kick, the step playing while the sequencer runs (with its locks), or a step held or picked with OCT+ |
| LFO | the four LFOs (each press: the next one; SELECT: its A and B pages) |
| FX | LIVE (held white keys) and FINAL (white keys on and off) |
| GLO | PATTERNS (three banks, A, B and C, on the first three black keys, sixteen patterns each on the white keys; ACTION: PLAY, copy, paste, clear, init, random; DO IT does it; a generator's pattern shows its steps too), MIDI, THEMES, AUDIO and SYSTEM |
| SELECT | the page |
| ALGORITHM | on SEQ and the EDIT pages: a step, round and round both ways (its number in the header, its key blinking, a line under it on EDIT's strip); on ARP: the generator; on FX: the effect the knobs edit, round and round (on FINAL it stays on or off as it was); on PATTERNS: the pattern. Never the tempo (FEEL has it) |
| PRESETS | the pattern's kick from the presets |
| Any knob | its name and value shown big for half a second; every click shows a new value (a knob of a few choices moves to the next one) |
| White keys | the steps: a tap toggles off, on, accent |
| White key held + knobs, on an ARP page | the step's pitch, ratchet, slide, reversed sweep |
| White key held + knobs, on an EDIT page or SEQ's KICK | a p-lock of that parameter for the step (up to four a step) |
| OCT+ held + knobs, on an EDIT page | a p-lock for the step ALGORITHM picked (the line under it turns the lock's colour; the footer shows the step's values) |
| White key held + PRESETS | the kick of that step (a p-lock), or back to the pattern's |
| White key held + REC | the step's p-locks cleared |
| Black keys | the kick, a semitone apart (OCT+ held: accented) |
| REC | recording on or off: the 27 keys become a chromatic keyboard (C4 is the kick's own note), and the keys and MIDI notes go into the steps: while playing into the nearest step, while stopped into the selected step and then the next one |
| OCT- + HOME | the page you are on back to its start (on SEQ the sequence is cleared too, and the generator set to MANUAL; on GLO's PATTERNS, as OCT+ + HOME) |
| OCT+ + HOME | the whole pattern as new: the first factory kick, no steps, locks or generator, the LFOs off; off GLO the effects too (not the other patterns, the user kicks or the settings) |
| OCT- + GLO, on EDIT | the kick moved a little, each knob round where it is (its kind, the choices and the level stay); the steps stay |
| OCT+ + GLO, on EDIT | a new kick at random; the steps stay |
| OCT- + GLO | random kick parameters and trigs (with p-locks, also of the kick type); never the tempo, the LFOs or the effects |
| OCT+ + GLO | the same, and the four LFOs as well |
| OCT- + REC | LISTEN on or off: the 27 keys play the kick chromatically, nothing is recorded (a ring in the header) |
| OCT- + LFO | the four LFOs at random, gently: two or three lanes, smooth shapes, slow and shallow, on the tone |
| OCT+ + LFO | the four LFOs at random, wildly: all four, any target, shape, speed and depth |
| OCT+ + REC | metronome on or off: a click on each beat (higher on the bar) and a metronome in the header |
| OCT- + SAVE | print the generator's steps into the TRIG lane |
| OCT- + any encoder | faster (not while REC or LISTEN play the keyboard) |
| SEL | steps 1-16 or 17-32 on the white keys |
| PLAY | start / stop |
| SAVE | save now (it also saves by itself, stopped, 1.5 s after a change) |

## AUDIO and SYSTEM

GLO's fourth page, AUDIO, turns each output on or off with K1-K3: USB AUDIO (the computer records silence), the
HEADPHONE JACK and the SPEAKER (one output feeds both, and the speaker goes off by itself with headphones in, so FM-KICK
switches it by what is plugged in). The fifth, SYSTEM, shows the audio load (now and its peak: K1 starts it again),
the xruns, the CPU's clock, the firmware's version, its identity and its build.

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
    JIELI_TOOLCHAIN=... AC79_SDK=... kick/release.sh   # the installer: kick/releases/fm-kick-X.Y.Z.fwsc

The browser player needs a web server (`python3 -m http.server` in `kick/build/emu/`, then http://localhost:8000).
The built player is kept in `kick/docs/play/`: copy `kick/build/emu/` there after each version
(`node kick/tests/play_browser_test.mjs` checks it in a headless Chromium).

Without Emscripten, `kick/tools/emcc` stands in for `emcc`: clang 18, wasm-ld and wasi-libc (`wasi-sdk-22`), with
`WASI_SYSROOT` pointing at wasi-libc's sysroot and `kick/tools` first on the `PATH`.

Tests: `basic`, `locks`, `rec`, `fx`, `store`, `themes`, `ws`, `v033`, `v034`, `audio` and `screens` in `kick/tests/`; `kicks_check.py`
hits the classic factory kicks and checks their pitch over time and which way they start;
`play_browser_test.mjs` and `mirror_page_test.mjs` open the player and the mirror page in Chromium.
