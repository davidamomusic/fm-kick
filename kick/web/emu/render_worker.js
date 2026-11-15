// SPDX-License-Identifier: MPL-2.0
// Render Audio's worker (kick/web/emu/render.js starts it): a copy of FM-KICK (app.wasm, the browser player's build) that
// plays one pattern from its first step, as fast as this computer goes, and sends the audio back. Its state comes
// either as the player's whole memory (a copy of the running app, everything as it is) or as the saved objects of a
// backup (the mirror page: read from the FM-1), which the copy boots from. The state is kept, so the same pattern can be
// rendered again with other choices from the same start.
//   in:  { op: "init", wasm, memory } or { op: "init", wasm, store: { obj: Uint8Array } }
//        { op: "render", what (0 with the effects, 1 the kick dry), pattern (-1 the one playing), bars, tail }
//   out: { op: "info", cur, bpm, used: [pattern indexes] }, { op: "progress", value 0..1 },
//        { op: "done", left, right, rate, bpm, pattern, steps, stepLen }, { op: "error", message }
"use strict";
const RATE = 44100, BLOCK = 1024, TAIL_MAX = 8, QUIET = 1e-4;
let ex = null, mem = null, base = null;

function imports() {
  const wasi = {
    clock_time_get: (id, prec, out) => { new DataView(mem.buffer).setBigUint64(out, BigInt(Math.round(performance.now() * 1e6)), true); return 0; },
    fd_write: (fd, iov, n, out) => { new DataView(mem.buffer).setUint32(out, 0, true); return 0; },
    proc_exit: () => {},
  };
  const stub = (t) => new Proxy(t, { get: (o, k) => (k in o ? o[k] : () => 0) });
  return { wasi_snapshot_preview1: stub(wasi), env: stub({}) };
}

async function init(m) {
  const { instance } = await WebAssembly.instantiate(m.wasm, imports());
  ex = instance.exports;
  mem = ex.memory;
  if (m.memory) {                                  // the player's app as it is: its memory, whole
    const want = m.memory.byteLength, have = mem.buffer.byteLength;
    if (want > have) mem.grow(Math.ceil((want - have) / 65536));
    new Uint8Array(mem.buffer).set(new Uint8Array(m.memory));
  } else {                                         // a backup's objects: the app boots from them
    if (ex._initialize) ex._initialize();
    const max = ex.web_store_max();
    for (const [obj, bytes] of Object.entries(m.store || {})) {
      const b = new Uint8Array(bytes).subarray(0, max);
      new Uint8Array(mem.buffer, ex.web_store(+obj), b.length).set(b);
      ex.web_store_set_len(+obj, b.length);
    }
    ex.web_master(4096);
    ex.web_boot();
    for (let i = 0; i < 60; i++) ex.web_render(BLOCK);   // about 1.4 s: the app is up
  }
  base = new Uint8Array(mem.buffer).slice();
  const used = [];
  for (let p = 0; p < ex.kick_npat(); p++) if (ex.kick_pat_used(p)) used.push(p);
  postMessage({ op: "info", cur: ex.kick_pat_cur(), bpm: ex.kick_bpm(), used });
}

function render(m) {
  const now = new Uint8Array(mem.buffer);
  now.set(base);                                   // the same start every time
  if (now.length > base.length) now.fill(0, base.length);
  ex.web_master(4096);                             // full scale: the MASTER knob's level is left out
  const steps = Math.max(1, m.bars | 0) * 16;
  const pattern = ex.kick_render_begin(m.what | 0, m.pattern ?? -1, steps);
  const stepLen = ex.kick_step_len(), bars = Math.round(steps * stepLen);
  const max = bars + (m.tail ? TAIL_MAX * RATE : 0);
  let L = new Float32Array(max), R = new Float32Array(max), n = 0, quietFrom = -1, said = 0;
  while (n < max) {
    ex.web_render(BLOCK);
    const l = new Float32Array(mem.buffer, ex.web_out_l(), BLOCK), r = new Float32Array(mem.buffer, ex.web_out_r(), BLOCK);
    const take = Math.min(BLOCK, max - n);
    L.set(l.subarray(0, take), n);
    R.set(r.subarray(0, take), n);
    if (n + take > bars) {                          // the tail: until it has been quiet for half a second
      let loud = false;
      for (let i = Math.max(0, bars - n); i < take; i++) if (Math.abs(l[i]) > QUIET || Math.abs(r[i]) > QUIET) { loud = true; break; }
      if (loud) quietFrom = -1;
      else if (quietFrom < 0) quietFrom = n;
      if (quietFrom >= 0 && n + take - quietFrom > RATE / 2) { n = Math.max(bars, quietFrom); break; }
    }
    n += take;
    const v = n / (m.tail ? bars + RATE : bars);
    if (v - said > 0.02) { said = v; postMessage({ op: "progress", value: Math.min(1, v) }); }
  }
  if (m.tail && n > bars) {                         // a short fade, so the end is never a click
    const f = Math.min(441, n - bars);
    for (let i = 0; i < f; i++) { const g = i / f; L[n - 1 - i] *= g; R[n - 1 - i] *= g; }
  }
  L = L.slice(0, n); R = R.slice(0, n);
  postMessage({ op: "done", left: L, right: R, rate: RATE, bpm: ex.kick_bpm(), pattern, steps, stepLen }, [L.buffer, R.buffer]);
}

onmessage = async (e) => {
  try {
    if (e.data.op === "init") await init(e.data);
    else if (e.data.op === "render") render(e.data);
  } catch (err) {
    postMessage({ op: "error", message: String(err && err.message || err) });
  }
};
