// SPDX-License-Identifier: MPL-2.0
// Render Audio (the player's page, kick/web/emu/index.html, and the mirror page, kick/mirror/extra.*): a pattern of
// FM-KICK rendered to audio in the browser by FM-KICK's own sound engine (app.wasm in render_worker.js), shown as a
// waveform, played, saved as a WAV file or dragged out of the page. With the effects, or the kick dry.
//
//   setupRender(section, { wasmUrl, workerUrl, source, playing, ready })
//     section   the page's <section>: its heading and words are the page's; the controls go in after them
//     source    async (say) => ({ memory }) or ({ store: { obj: Uint8Array } }): the state to render from, fresh each time
//               Render Audio is pressed (say(text) tells what it is doing)
//     playing   true: the first choice is the pattern playing (the player); false: the patterns with steps (the mirror)
//     ready     () => "" when a render can start, or why not (the player: switch on first; the mirror: connect first)
//   returns { update() }: the page calls it when ready() may have changed
const RATE = 44100;
const pname = (p) => String.fromCharCode(65 + Math.floor(p / 16)) + String(p % 16 + 1).padStart(2, "0");
const fmtBpm = (b) => (Math.round(b * 10) % 10 ? (Math.round(b * 10) / 10).toFixed(1) : String(Math.round(b)));
const CSS = `
.render .opts { display: flex; flex-wrap: wrap; gap: .8rem 1.6rem; align-items: flex-end; margin: .7rem 0; }
.render .f { display: grid; gap: .2rem; font-size: .85rem; color: var(--muted); }
.render select { font: inherit; font-size: 1rem; color: var(--ink); background: var(--ground); border: 1px solid var(--rule); border-radius: 8px; padding: .25em .5em; }
.render .seg { display: inline-flex; border: 1px solid var(--rule); border-radius: 9px; overflow: hidden; }
.render .seg button { font: inherit; font-size: 1rem; color: var(--ink); border: 0; border-radius: 0; background: var(--ground); padding: .3em .9em; cursor: pointer; }
.render .seg button + button { border-left: 1px solid var(--rule); }
.render .seg button[aria-pressed="true"] { background: var(--ink); color: var(--ground); font-weight: 600; }
.render .check { display: inline-flex; gap: .4rem; align-items: center; font-size: .92rem; }
.render .check input { accent-color: var(--teal); width: 1rem; height: 1rem; }
.render .row { display: flex; flex-wrap: wrap; gap: .5rem .8rem; align-items: center; margin: .6rem 0; }
.render .row button { font: inherit; color: var(--ink); background: var(--ground); border: 1px solid var(--rule); border-radius: 8px; padding: .25em .7em; cursor: pointer; }
.render .row button.primary { background: var(--accent); border-color: var(--accent); color: #2a140a; font-weight: 600; }
.render .row button:disabled { opacity: .5; cursor: default; }
.render .rmsg { color: var(--ink); }
.render .rmsg.err { color: var(--accent-ink, var(--accent)); font-weight: 600; }
.render .wave { position: relative; margin: .8rem 0 .4rem; background: #15120f; border: 1px solid var(--rule); border-radius: 10px; overflow: hidden; }
.render .wave canvas { display: block; width: 100%; height: 170px; }
.render .wave .empty { position: absolute; inset: 0; display: grid; place-items: center; color: #8d7d6b; font-size: .92rem; pointer-events: none; text-align: center; padding: 0 1rem; }
.render .wave .empty[hidden] { display: none; }
.render .info { display: flex; flex-wrap: wrap; gap: .3rem 1.2rem; font-size: .85rem; color: var(--muted); font-variant-numeric: tabular-nums; min-height: 1.3em; }
.render .drag { display: inline-flex; gap: .5rem; align-items: center; padding: .3em .8em; border: 1px dashed var(--teal); border-radius: 8px;
                cursor: grab; user-select: none; -webkit-user-select: none; background: var(--ground); color: var(--ink); font-size: .92rem; }
.render .drag[aria-disabled="true"] { opacity: .45; cursor: default; }
.render progress { width: 14rem; vertical-align: middle; }
.render progress[hidden] { display: none; }`;

const HTML = `
<div class="opts">
  <label class="f">Pattern <select data-r="pat"></select></label>
  <div class="f" role="group" aria-label="What to render"><span aria-hidden="true">Render</span>
    <span class="seg" data-r="what">
      <button type="button" data-w="0" aria-pressed="true">With effects</button>
      <button type="button" data-w="1" aria-pressed="false">Kick dry</button>
    </span>
  </div>
  <label class="f">Length
    <select data-r="bars"><option value="1">1 bar</option><option value="2">2 bars</option><option value="4" selected>4 bars</option><option value="8">8 bars</option><option value="16">16 bars</option></select>
  </label>
  <label class="check"><input type="checkbox" data-r="tail" checked> Reverb tail</label>
</div>
<div class="row">
  <button type="button" class="primary" data-r="go">Render Audio</button>
  <progress data-r="prog" max="1" value="0" hidden></progress>
  <span class="rmsg" data-r="msg" role="status"></span>
</div>
<div class="wave">
  <canvas data-r="wave" height="340" role="img" aria-label="The rendered audio's waveform"></canvas>
  <div class="empty" data-r="empty">Nothing rendered yet. Press Render Audio.</div>
</div>
<div class="info" data-r="info"></div>
<div class="row">
  <button type="button" data-r="play" disabled>Play</button>
  <button type="button" data-r="save" disabled>Save WAV</button>
  <span class="drag" data-r="drag" draggable="false" aria-disabled="true" title="Drag the audio file into your DAW or a folder (Chrome and Edge)">
    <svg width="16" height="16" viewBox="0 0 16 16" aria-hidden="true"><path d="M3 8h2v4H3zM7 4h2v10H7zM11 6h2v6h-2z" fill="currentColor"/></svg>
    <span data-r="file">Drag the audio</span></span>
</div>`;

// 32-bit float stereo WAV (WAVE_FORMAT_IEEE_FLOAT, with its fact chunk): nothing clips, and the three renders add up
function wav(L, R) {
  const n = L.length, data = n * 8, d = new DataView(new ArrayBuffer(58 + data));
  const s = (o, t) => { for (let i = 0; i < t.length; i++) d.setUint8(o + i, t.charCodeAt(i)); };
  s(0, "RIFF"); d.setUint32(4, 50 + data, true); s(8, "WAVE");
  s(12, "fmt "); d.setUint32(16, 18, true); d.setUint16(20, 3, true); d.setUint16(22, 2, true);
  d.setUint32(24, RATE, true); d.setUint32(28, RATE * 8, true); d.setUint16(32, 8, true); d.setUint16(34, 32, true); d.setUint16(36, 0, true);
  s(38, "fact"); d.setUint32(42, 4, true); d.setUint32(46, n, true);
  s(50, "data"); d.setUint32(54, data, true);
  for (let i = 0, o = 58; i < n; i++, o += 8) { d.setFloat32(o, L[i], true); d.setFloat32(o + 4, R[i], true); }
  return new Blob([d], { type: "audio/wav" });
}

export function setupRender(section, opt) {
  if (!document.getElementById("kick-render-css")) {
    const st = document.createElement("style");
    st.id = "kick-render-css";
    st.textContent = CSS;
    document.head.appendChild(st);
  }
  section.insertAdjacentHTML("beforeend", HTML);
  const q = (k) => section.querySelector(`[data-r="${k}"]`);
  const pat = q("pat"), bars = q("bars"), tail = q("tail"), go = q("go"), prog = q("prog"), msg = q("msg");
  const cv = q("wave"), empty = q("empty"), info = q("info"), play = q("play"), save = q("save"), drag = q("drag"), file = q("file");
  let what = 0, worker = null, busy = false, out = null, url = null, ac = null, src = null, t0 = 0, raf = 0, known = null;

  const say = (t, err) => { msg.textContent = t; msg.classList.toggle("err", !!err); };
  function patOptions(cur, used) {
    const keep = pat.value;
    pat.replaceChildren();
    const add = (v, t) => { const o = document.createElement("option"); o.value = String(v); o.textContent = t; pat.appendChild(o); };
    if (opt.playing) add(-1, cur === undefined ? "Playing now" : `Playing now (${pname(cur)})`);
    for (const p of used || []) add(p, pname(p));
    if (!pat.options.length) add(-1, opt.playing ? "Playing now" : "Read from the FM-1 first");
    if ([...pat.options].some((o) => o.value === keep)) pat.value = keep;
  }
  patOptions();

  function update() {
    const why = opt.ready ? opt.ready() : "";
    go.disabled = busy || !!why;
    for (const b of section.querySelectorAll(".seg button")) b.disabled = busy;
    pat.disabled = bars.disabled = tail.disabled = busy;
    if (!busy && !out && !msg.classList.contains("err")) say(why || "");
  }

  function draw() {
    const w = cv.width = Math.max(300, Math.round(cv.clientWidth * 2)), h = cv.height, g = cv.getContext("2d");
    g.fillStyle = "#15120f"; g.fillRect(0, 0, w, h);
    if (!out) return;
    const n = out.L.length, colour = ["#f28a5a", "#f2e8d8"][out.what], beats = out.steps / 4;
    const beatPx = w * out.stepLen * 4 / n, endPx = beats * beatPx;
    for (let b = 0; b <= beats; b++) {
      const bar = b % 4 === 0, x = Math.round(b * beatPx);
      if (!bar && beatPx < 6) continue;
      g.fillStyle = bar ? "rgba(242,232,216,.22)" : "rgba(242,232,216,.07)";
      g.fillRect(x, 0, bar ? 2 : 1, h);
      if (bar && b < beats && (beats <= 32 || (b / 4) % 4 === 0)) {
        g.fillStyle = "rgba(242,232,216,.55)"; g.font = "600 20px Barlow, sans-serif"; g.fillText(String(b / 4 + 1), x + 8, 24);
      }
    }
    if (endPx < w) { g.fillStyle = "rgba(242,232,216,.05)"; g.fillRect(endPx, 0, w - endPx, h); }
    const per = n / w, win = 700, mid = h / 2 + 10, amp = (h - 40) / 2 / Math.max(out.peak, 1e-3);
    for (let x = 0; x < w; x++) {
      let pk = 0, sq = 0, c = 0;
      for (let i = Math.max(0, Math.floor(x * per - win / 2)), e = Math.min(n, Math.floor((x + 1) * per + win / 2)); i < e; i++) {
        const v = (out.L[i] + out.R[i]) / 2, a = Math.abs(v);
        if (a > pk) pk = a;
        sq += v * v; c++;
      }
      const rms = Math.min(pk, Math.sqrt(sq / Math.max(1, c)) * 1.4);
      g.globalAlpha = .45; g.fillStyle = colour; g.fillRect(x, mid - pk * amp, 1, Math.max(1, 2 * pk * amp));
      g.globalAlpha = 1; g.fillRect(x, mid - rms * amp, 1, Math.max(1, 2 * rms * amp));
    }
    if (src && ac) {
      g.fillStyle = "#79adff";
      g.fillRect(((ac.currentTime - t0) * RATE / n) * w, 0, 2, h);
    }
  }
  new ResizeObserver(() => draw()).observe(cv);

  const name = () => out && `fm-kick-${pname(out.pattern)}-${["fx", "dry"][out.what]}-${fmtBpm(out.bpm)}bpm.wav`;
  function stop() {
    if (src) { src.onended = null; try { src.stop(); } catch (_) {} src = null; }
    cancelAnimationFrame(raf);
    play.textContent = "Play";
    draw();
  }
  play.addEventListener("click", () => {
    if (src) { stop(); return; }
    if (!out) return;
    ac = ac || new AudioContext();
    ac.resume();
    const ab = ac.createBuffer(2, out.L.length, RATE);
    ab.copyToChannel(out.L, 0); ab.copyToChannel(out.R, 1);
    src = ac.createBufferSource(); src.buffer = ab; src.connect(ac.destination); src.onended = stop;
    t0 = ac.currentTime + 0.02; src.start(t0);
    play.textContent = "Stop";
    const loop = () => { draw(); raf = requestAnimationFrame(loop); };
    loop();
  });
  save.addEventListener("click", () => { if (!url) return; const a = document.createElement("a"); a.href = url; a.download = name(); a.click(); });
  drag.addEventListener("dragstart", (e) => { if (url) e.dataTransfer.setData("DownloadURL", `audio/wav:${name()}:${url}`); });

  function shown(m) {
    stop();
    let peak = 0;
    for (let i = 0; i < m.left.length; i++) { const a = Math.max(Math.abs(m.left[i]), Math.abs(m.right[i])); if (a > peak) peak = a; }
    out = { L: m.left, R: m.right, what: m.what, pattern: m.pattern, bpm: m.bpm, steps: m.steps, stepLen: m.stepLen, peak };
    if (url) URL.revokeObjectURL(url);
    url = URL.createObjectURL(wav(out.L, out.R));
    empty.hidden = true;
    play.disabled = save.disabled = false;
    drag.setAttribute("aria-disabled", "false"); drag.draggable = true; file.textContent = name();
    const secs = out.L.length / RATE, body = Math.round(out.steps * out.stepLen) / RATE, extra = secs - body;
    const db = peak > 0 ? (20 * Math.log10(peak)).toFixed(1) : "-inf";
    info.textContent = `${pname(out.pattern)} · ${fmtBpm(out.bpm)} BPM · ${out.steps / 16} bar${out.steps > 16 ? "s" : ""} · ` +
      `${body.toFixed(2)} s${extra > 0.01 ? ` + ${extra.toFixed(2)} s tail` : ""} · peak ${db} dBFS · 44.1 kHz, 32-bit float stereo WAV`;
    if (peak < 1e-4) say("The pattern made no sound (no steps on, or the kick's level is down).");
    else say("Rendered.");
    draw();
  }

  // a render of the state the worker holds, with the choices as they are now
  function renderNow() {
    return new Promise((resolve, reject) => {
      worker.onmessage = (e) => {
        const m = e.data;
        if (m.op === "progress") prog.value = m.value;
        else if (m.op === "done") { m.what = what; resolve(m); }
        else if (m.op === "error") reject(new Error(m.message));
      };
      worker.onerror = (e) => reject(new Error(e.message || "the renderer stopped"));
      prog.value = 0;
      worker.postMessage({ op: "render", what, pattern: Number(pat.value), bars: Number(bars.value), tail: tail.checked });
    });
  }
  async function job(fn) {
    if (busy) return;
    busy = true; update(); prog.hidden = false;
    try { await fn(); } catch (e) { say(e && e.message || String(e), true); }
    finally { busy = false; prog.hidden = true; update(); }
  }
  let wasm = null;
  go.addEventListener("click", () => job(async () => {
    say("Getting the pattern…");
    prog.removeAttribute("value");
    const state = await opt.source(say);
    say("Rendering…");
    if (!wasm) {
      const r = await fetch(opt.wasmUrl);
      if (!r.ok) throw new Error(`The sound engine did not load (${r.status}).`);
      wasm = await r.arrayBuffer();
    }
    if (worker) worker.terminate();
    worker = new Worker(opt.workerUrl);
    known = await new Promise((resolve, reject) => {
      worker.onmessage = (e) => (e.data.op === "info" ? resolve(e.data) : e.data.op === "error" && reject(new Error(e.data.message)));
      worker.onerror = (e) => reject(new Error(e.message || "the renderer did not start"));
      worker.postMessage({ op: "init", wasm: wasm.slice(0), ...state }, state.memory ? [state.memory] : []);
    });
    patOptions(known.cur, known.used);
    if (!opt.playing && !known.used.length) { say("The FM-1 has no pattern with steps on.", true); return; }
    shown(await renderNow());
    if (state.note && !msg.classList.contains("err")) say(`${msg.textContent} ${state.note}`);
  }));
  // another choice after a render: the same pattern again, from the same start (nothing is read again)
  const again = () => { if (worker && known && !busy) job(async () => { say("Rendering…"); shown(await renderNow()); }); };
  for (const b of section.querySelectorAll(".seg button")) b.addEventListener("click", () => {
    what = Number(b.dataset.w);
    for (const c of section.querySelectorAll(".seg button")) c.setAttribute("aria-pressed", String(c === b));
    again();
  });
  for (const el of [pat, bars, tail]) el.addEventListener("change", again);
  update();
  return { update };
}
