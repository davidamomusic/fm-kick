#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# Builds the FM-KICK cheat sheet: one self-contained HTML page (dark on screen, light in print) and its A4 PDF
# (Chrome prints it). cheatsheet-1.png and cheatsheet-2.png, the previews in the READMEs, are its pages at 100 dpi
# (pdftoppm). From fm1-base's root, with absolute paths:
#   python3 kick/docs/make_cheatsheet.py $PWD/kick/card.png $PWD/kick/docs
# Change the text below when the controls change; VERSION comes from kick/app.h.
import base64, glob, html, os, re, subprocess, sys

card_png, out_dir = sys.argv[1], sys.argv[2]
os.makedirs(out_dir, exist_ok=True)

HERE = os.path.dirname(os.path.abspath(__file__))
VERSION = re.search(r'#define APP_VERSION "(.*)"', open(os.path.join(HERE, "..", "app.h")).read()).group(1)
DESCRIPTION = ("A kick drum synthesizer with a sequencer of up to 32 steps: seven kinds of kick and 48 parameters, "
               "p-locks of any parameter on any step, step conditions and chance, nine generators, four LFOs, live and "
               "final effects with a hall reverb, three pattern banks, four projects and six themes.")
REPO = "https://github.com/davidamomusic/fm-kick"
BETA = ("Use it at your own risk: it can have bugs and fail, and installing it or restoring a backup writes to the "
        "FM-1's flash. Save what matters first and never unplug the USB cable while it writes. It is provided "
        "\u201cas is\u201d, without warranty (MPL-2.0, sections 6 and 7); the author is not responsible for any damage "
        "to your FM-1, your data or anything connected to it.")
RELEASE = REPO + "/releases/latest"

# (section title, [(controls, description), ...]); `x` in controls marks a button, key or knob.
SECTIONS = [
    ("Getting around", [
        ("`SEQ` `ARP` `EDIT` `LFO` `FX` `GLO`",
         "The modes. A press goes to the mode, on the page you left; pressed again, the next page (round). `ENV` "
         "does nothing for now."),
        ("`SELECT` turn", "The page inside the mode."),
        ("`KNOB 1`-`KNOB 4`",
         "The four parameters named at the foot of the screen. Each click shows the new value big for half a second; a "
         "knob of a few choices moves to the next one."),
        ("`OCT-` + any encoder", "Faster."),
        ("`PLAY`", "Start or stop."),
        ("`SEL`", "Steps 1-16 or 17-32 on the white keys."),
        ("`SAVE`", "Save now. It also saves by itself, stopped, 1.5 s after a change."),
        ("`HOME`", "The boxing glove: it punches with every hit."),
    ]),
    ("Steps", [
        ("`STEP KEY` tap", "The 16 white keys are the steps: off, on, accent."),
        ("`ALGORITHM` turn",
         "On SEQ and the EDIT pages: picks a step (its key blinks). Stopped, you hear it if it has a trig. On ARP the "
         "generator, on FX the effect the knobs edit, on PATTERNS the pattern."),
        ("`STEP KEY` hold + knobs on ARP", "The step's pitch (-24..+24), ratchet (x1-x4), slide and reversed sweep."),
        ("`STEP KEY` hold + knobs on EDIT or SEQ", "A p-lock of that parameter on the step."),
        ("`OCT+` hold + knobs on EDIT", "A p-lock on the step `ALGORITHM` picked."),
        ("`STEP KEY` hold + `PRESETS`", "Another kick on that step (a p-lock), or back to the pattern's."),
        ("`STEP KEY` hold + `REC`", "The step's p-locks cleared."),
        ("P-locks", "Up to 16 on a step, 74 in a pattern. On EDIT, each page marks the steps with locks of its own four "
         "parameters."),
        ("`OCT-` + `STEP KEY` hold on SEQ",
         "Repeat: that many steps (key 1 = one, 16 = sixteen) round and round while held; let go and it carries on."),
    ]),
    ("Sequencer pages", [
        ("`SEQ` KICK", "The picked step's TYPE, TUNE, ATTACK and DECAY, as p-locks."),
        ("`SEQ` KICK 2",
         "K1 COND: 1:2 ... 4:4 (the X-th pass of every Y), 1ST, !1ST, PRE, !PRE. K2 PROB, the step's chance (5% a "
         "click, 1% with `OCT-`)."),
        ("`ARP` GENERATE",
         "`ALGORITHM` picks MANUAL, EUCLID, POLY, METRIC, SYNCOPA, BOUNCE, MIRROR, FRACTAL, BITS or ROLLS; the knobs shape it. "
         "`OCT-` + `SAVE` prints its steps into the TRIG lane."),
        ("`ARP` TRIG", "LENGTH (1-32), DIVISION, DIRECTION (FWD, REV, PING, RAND, WANDER), NUDGE (the whole sequence a "
         "step a click, locks with it)."),
        ("`ARP` FEEL", "TEMPO, PROB, HUMANIZE, SWING."),
        ("`LFO`", "Four LFOs; each press the next one, `SELECT` its two pages. Each moves one parameter: length, "
         "division, direction, shape (MANUAL, RAMP UP, RAMP DN, TRI, SINE, SQUARE, RANDOM, MEANDER), depth, smoothing, "
         "seed."),
    ]),
    ("Recording", [
        ("`REC`",
         "Recording on or off. The 27 keys become a keyboard (C4 = the kick's note); keys and MIDI notes go into the "
         "steps: playing, the nearest step; stopped, the selected step and then the next."),
        ("`REC` on + knobs on SEQ or EDIT",
         "The knobs recorded as p-locks. Playing: on the step of the kick sounding, carrying on from where the knob "
         "stood (a sweep over several kicks). Stopped: on the step REC wrote last. `OCT-` held: fast."),
        ("`OCT-` + `REC`", "LISTEN: the keys play the kick, nothing is recorded."),
        ("`OCT+` + `REC`", "Metronome on or off."),
        ("Black keys", "Play the kick a semitone apart (`OCT+` held: accented)."),
    ]),
    ("The kick (EDIT)", [
        ("12 pages, then STORE",
         "KICK (TYPE, TUNE, DECAY, LEVEL), CHARACTER, PITCH, SWEEP, AMP, CLICK, SUB, NOISE, DIST, FILTER, EQ, OUTPUT: "
         "48 parameters."),
        ("TYPE", "808, 909, 606, ANALOG, FM, WSHAPER, THUNDER; each has its own four CHARACTER knobs."),
        ("STORE", "32 user kicks with a name of 8 letters: K1 slot, K2 cursor, K3 letter, K4 right saves, left loads. "
         "18 factory kicks."),
        ("`PRESETS` turn", "The pattern's kick from the presets."),
        ("`OCT-` + `GLO` on EDIT", "The kick moved a little."),
        ("`OCT+` + `GLO` on EDIT", "A new kick at random; the steps stay."),
    ]),
    ("Effects (FX)", [
        ("LIVE: `STEP KEY` hold",
         "On only while held: DISTORT, CRUSH, FILTER, RING MOD, CHORUS, PHASER, FLANGER, DELAY, REVERB, STUTTER, GATE, "
         "WIDTH. The knobs set the last one played."),
        ("`OCT-` + `STEP KEY` on LIVE", "That effect left on, until `OCT-` + the key again."),
        ("`OCT+` + `STEP KEY`", "That effect picked for the knobs, nothing played."),
        ("`OCT+` alone", "Every effect off."),
        ("FINAL: keys 1-5", "The master chain on or off: TRANSIENT, COMP, EQ, DISTORTION, LIMITER."),
        ("DELAY", "TIME, FEEDBACK, MIX, FILTER (middle open, left low-pass, right high-pass, on the echoes)."),
        ("REVERB", "A hall: DECAY (0.6-8 s), DAMP, MIX, PRE (0-100 ms). Below 90 Hz stays dry."),
    ]),
    ("Random and reset", [
        ("`OCT-` + `GLO`", "Random kick parameters and trigs (p-locks too). Never the tempo, the LFOs or the effects. Any random keeps "
         "CLICK's LEVEL at 4 or less and no LFO on CLICK."),
        ("`OCT+` + `GLO`", "The same, and the four LFOs."),
        ("`OCT-` + `LFO`", "The LFOs at random, gently."),
        ("`OCT+` + `LFO`", "The LFOs at random, wildly."),
        ("`OCT-` + `HOME`", "The page you are on back to its start (on SEQ, the sequence cleared too)."),
        ("`OCT+` + `HOME`", "The whole pattern as new (other patterns, user kicks and settings stay)."),
    ]),
    ("Patterns and projects (GLO)", [
        ("PATTERNS",
         "Black keys 1-3: banks A, B, C (shown; the pattern playing goes on). White keys: the 16 patterns; playing, a "
         "pick starts at the end of the bar. ACTION: PLAY, COPY, PASTE, CLEAR, INIT, RANDOM; DO IT does it."),
        ("PROJECTS", "4 slots: the 3 banks, tempo and effects. K1 slot, K2 LOAD, SAVE or CLEAR, K4 DO IT (turn twice "
         "within 3 s). Stopped only."),
        ("Other pages", "MIDI, THEMES (6 themes: EMBER, NIGHT, ICE, ACID, MONO, PAPER; dials or bars, ring or grid), "
         "POPUPS (solid or clear), AUDIO (USB, jack, speaker), SYSTEM (load, xruns, flash, version, cores)."),
    ]),
    ("Computer and MIDI", [
        ("MIDI", "In: any note plays the kick (36 = its own tune). Out: each hit as a note, and the clock. CLOCK = MIDI "
         "follows an incoming clock."),
        ("Screen mirror", "The screen and panel over USB (Chrome or Edge): Render Audio to WAV, projects and backups."),
        ("Play in the browser", "The firmware itself, running in a web page."),
        ("Leave FM-KICK", "Install another firmware with The Firmware Hub or M-UPGRADE. Identity: FM-1_222."),
    ]),
]


def md(text):
    out, parts = [], text.split("`")
    for i, part in enumerate(parts):
        part = html.escape(part)
        out.append("<code>%s</code>" % part if i % 2 else part)
    return "".join(out)


def section(title, rows):
    body = "".join('<div class="e"><div class="k">%s</div><div class="d">%s</div></div>' % (md(k), md(d))
                   for k, d in rows)
    return '<section><h2>%s</h2>%s</section>' % (html.escape(title), body)


with open(card_png, "rb") as f:
    card = base64.b64encode(f.read()).decode()

PAGE = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>FM-KICK {version} cheat sheet</title>
<meta name="description" content="{desc_attr}">
<style>
:root {{ --bg:#151213; --panel:#1e1a1c; --line:#35302f; --text:#ece7e8; --dim:#a9a2a4; --acc:#FF481C; --code:#26211f; }}
* {{ box-sizing:border-box; }}
body {{ margin:0; background:var(--bg); color:var(--text);
  font:15px/1.5 -apple-system,BlinkMacSystemFont,"Segoe UI",Inter,Roboto,Helvetica,Arial,sans-serif; }}
main {{ max-width:1080px; margin:0 auto; padding:28px 20px 40px; }}
.hd {{ display:flex; gap:22px; align-items:flex-start; margin-bottom:22px; }}
.hd img {{ width:168px; height:168px; border-radius:10px; border:1px solid var(--line); image-rendering:pixelated; flex:none; }}
.kick {{ color:var(--acc); font-size:12px; letter-spacing:.12em; text-transform:uppercase; font-weight:600; }}
h1 {{ margin:4px 0 2px; font-size:32px; line-height:1.15; }}
.ver {{ color:var(--dim); margin:0 0 10px; }}
.sum {{ margin:0 0 14px; max-width:60ch; }}
.btns a {{ display:inline-block; margin:0 8px 6px 0; padding:7px 14px; border-radius:7px; border:1px solid var(--acc);
  color:var(--acc); text-decoration:none; font-weight:600; font-size:14px; }}
.btns a.solid {{ background:var(--acc); color:#fff; }}
.legend {{ color:var(--dim); font-size:13px; margin:0 0 18px; }}
.beta {{ margin:0 0 16px; padding:9px 14px; border:1px solid var(--line); border-left:5px solid var(--acc); border-radius:8px; background:var(--panel); font-size:13.5px; }}
.beta b {{ color:var(--acc); }}
.cols {{ columns:2 480px; column-gap:26px; }}
section {{ break-inside:avoid; margin:0 0 18px; background:var(--panel); border:1px solid var(--line); border-radius:10px; padding:4px 14px 8px; }}
h2 {{ font-size:15px; margin:10px 0 6px; color:var(--acc); letter-spacing:.02em; }}
.e {{ display:grid; grid-template-columns:38% 1fr; gap:4px 12px; padding:6px 0; border-top:1px solid var(--line); }}
.e:first-of-type {{ border-top:0; }}
.k {{ font-size:13px; }} .d {{ font-size:13.5px; }}
code {{ font:600 11.5px/1.2 ui-monospace,SFMono-Regular,Menlo,Consolas,monospace; background:var(--code);
  border:1px solid var(--line); border-radius:4px; padding:1px 5px; white-space:nowrap; color:var(--text); }}
footer {{ margin-top:10px; color:var(--dim); font-size:12.5px; border-top:1px solid var(--line); padding-top:12px; }}
footer a {{ color:var(--acc); }}
@page {{ size:A4; margin:7mm 8mm 6mm 8mm; }}
@media print {{
  :root {{ --bg:#fff; --panel:#fff; --line:#cfd4d1; --text:#161616; --dim:#555; --acc:#c2330e; --code:#f1f3f2; }}
  * {{ -webkit-print-color-adjust:exact; print-color-adjust:exact; }}
  body {{ font-size:7.1pt; line-height:1.16; }}
  main {{ max-width:none; padding:0; }}
  .hd {{ gap:12px; margin-bottom:5px; }} .hd img {{ width:22mm; height:22mm; border-radius:4px; }}
  h1 {{ font-size:15pt; margin:1px 0 0; }} .ver {{ margin-bottom:4px; }} .sum {{ max-width:none; margin-bottom:4px; }}
  .btns {{ display:none; }} .legend {{ margin-bottom:5px; font-size:7.4pt; }}
  .beta {{ margin-bottom:2.2mm; padding:1mm 2.6mm; font-size:7.4pt; line-height:1.25; border-radius:3px; }}
  .cols {{ columns:2; column-gap:4.5mm; }}
  section {{ margin:0 0 1.4mm; padding:0.4mm 2.6mm 0.8mm; border-radius:4px; }}
  h2 {{ font-size:9.4pt; margin:1.4mm 0 0.6mm; }}
  .e {{ padding:0.35mm 0; grid-template-columns:27% 1fr; gap:1mm 2.4mm; }}
  .k, .d {{ font-size:6.3pt; }} code {{ font-size:6.4pt; padding:0 1mm; }}
  footer {{ font-size:6.5pt; margin-top:0.5mm; padding-top:1mm; }}
}}
@media (max-width:640px) {{ .hd {{ flex-direction:column; }} .e {{ grid-template-columns:1fr; }} }}
</style>
</head>
<body>
<main>
<div class="hd">
  <img src="data:image/png;base64,{card}" alt="FM-KICK on the screen of the FM-1: the HOME page">
  <div>
    <div class="kick">FM-KICK &middot; Cheat sheet</div>
    <h1>FM-KICK at a glance</h1>
    <p class="ver">For FM-KICK {version} on the M-VAVE FM-1</p>
    <p class="sum">{desc}</p>
    <div class="btns">
      <a class="solid" href="{release}">Install FM-KICK {version}</a>
      <a href="{repo}">Source code</a>
      <a href="#" onclick="window.print();return false">Print</a>
    </div>
  </div>
</div>
<p class="beta"><b>FM-KICK is beta software.</b> {beta}</p>
<p class="legend"><code>BUTTON</code> a button, key or encoder of the FM-1 &nbsp;&middot;&nbsp; tap, hold, turn &nbsp;&middot;&nbsp;
<code>A</code> + <code>B</code> hold A while you use B &nbsp;&middot;&nbsp; K1-K4 are the four knobs, in the order of the screen footer &nbsp;&middot;&nbsp;
the white keys are the steps, the black keys play the kick</p>
<div class="cols">
{sections}
</div>
<footer>Community-made for the M-VAVE FM-1; not affiliated with or endorsed by M-VAVE. Everything on the screen is in
English. FM-KICK is open source under the MPL-2.0:
<a href="{repo}">{repo_short}</a>. All product names belong to their owners.</footer>
</main>
</body>
</html>
"""

page = PAGE.format(version=VERSION, desc=html.escape(DESCRIPTION), beta=html.escape(BETA), desc_attr=html.escape(DESCRIPTION, quote=True),
                   card=card, release=RELEASE, repo=REPO, repo_short=REPO.replace("https://", ""),
                   sections="\n".join(section(t, r) for t, r in SECTIONS))

html_path = os.path.join(out_dir, "FM-KICK-cheatsheet.html")
pdf_path = os.path.join(out_dir, "FM-KICK-cheatsheet.pdf")
with open(html_path, "w", encoding="utf-8") as f:
    f.write(page)

subprocess.run(["google-chrome", "--headless=new", "--no-sandbox", "--disable-gpu", "--no-pdf-header-footer",
                "--print-to-pdf=" + pdf_path, "file://" + html_path], check=True,
               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
print(html_path, pdf_path)

for old in glob.glob(os.path.join(out_dir, "cheatsheet-*.png")):
    os.remove(old)
subprocess.run(["pdftoppm", "-r", "100", "-png", pdf_path, os.path.join(out_dir, "cheatsheet")], check=True)
for f in sorted(glob.glob(os.path.join(out_dir, "cheatsheet-*.png"))):
    n = int(re.search(r"-(\d+)\.png$", f).group(1))
    os.replace(f, os.path.join(out_dir, "cheatsheet-%d.png" % n))
