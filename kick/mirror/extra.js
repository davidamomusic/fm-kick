// ---------------------------------------------------------------- Projects and backup (kick/mirror/extra.js, extra.html;
// kick/tests/projects_browser_test.mjs; from FM-FAM's). The FM-1's backup and restore (device/backup.c) over the ports this
// page has, the mirror stopped meanwhile: a backup of everything {{NAME}} saves, and a project taken out of it or put into it.
{{BACKUP_PROTOCOL}}
// the ports are one tool's at a time: a backup, a project, a render (each tool's own update() is hooked in)
const TOOLS = { on: false, hooks: [], set(v) { this.on = v; this.hooks.forEach((f) => f()); } };
// the beta notice (extra.html, #beta) goes to the very bottom of the page; it also ends every question that writes to the FM-1
const BETA_NOTE = "{{NAME}} is beta software and can fail: you do this at your own risk.";
{
  const beta = document.getElementById("beta"), main = document.querySelector("main");
  if (beta && main) main.append(beta);
}

// --- projects: begin
// A project is nine of the backup's parts (app.h PJ_OBJ): its head (app.c prj_t: "KPJ1", u16 tempo x 10, u8 pattern,
// u8 bank, u8 patterns with steps, ...), then the 48 patterns of banks A-C, six a part. Slot p (0..3) holds parts
// {{OBJ_PJ0}} + 9 * p + 0..8. A project file: "FMKP", u16 file version, u16 backup version, u16 count (9), u16 length of
// the app's version, that text, then the nine parts (u16 index, u16 length, u32 CRC-32, the bytes) and the CRC-32 of
// everything before.
const PJ = (() => {
  const PROJ0 = {{OBJ_PJ0}}, SLOTS = 4, PER = 9, MAXLEN = 3840, FILE_VERSION = 1, APP = "{{APP_VERSION}}";
  const PRJ_MAGIC = [0x4B, 0x50, 0x4A, 0x31];                    // app.c PRJ_MAGIC 0x314A504B, little-endian: "KPJ1"
  const FILE_MAGIC = [0x46, 0x4D, 0x4B, 0x50];                   // "FMKP"
  const u16 = (b, o) => b[o] | b[o + 1] << 8;
  const u32 = (b, o) => (b[o] | b[o + 1] << 8 | b[o + 2] << 16 | b[o + 3] << 24) >>> 0;
  const le16 = (v) => [v & 0xFF, (v >>> 8) & 0xFF];
  const le32 = (v) => [v & 0xFF, (v >>> 8) & 0xFF, (v >>> 16) & 0xFF, (v >>> 24) & 0xFF];
  const fail = (m) => Object.assign(new Error(m), { code: "file" });
  const id = (p, j) => PROJ0 + p * PER + j;
  const concat = (parts) => {
    const out = new Uint8Array(parts.reduce((a, p) => a + p.length, 0));
    let o = 0;
    for (const p of parts) { out.set(p, o); o += p.length; }
    return out;
  };
  const patName = (bank, cur) => ("ABC"[bank] || "?") + String(cur + 1).padStart(2, "0");

  // slot p of a backup's parts ([{obj, data}]) -> {used, name (what it holds, in words), parts: 9 x Uint8Array, bytes}
  function slot(objs, p) {
    const parts = Array.from({ length: PER }, (_, j) => (objs.find((o) => o.obj === id(p, j)) || { data: new Uint8Array(0) }).data);
    const head = parts[0];
    const used = head.length >= 12 && PRJ_MAGIC.every((b, i) => head[i] === b) && parts.slice(1).every((d) => d.length === MAXLEN);
    let name = "";
    if (used) {
      const bpm = u16(head, 4) / 10;
      name = `${bpm % 1 ? bpm.toFixed(1) : bpm} BPM, ${head[8]} patterns, ${patName(head[7], head[6])}`;
    }
    return { used, name, parts, bytes: parts.reduce((a, d) => a + d.length, 0) };
  }
  // a project (from slot) as a file
  function projectFile(s) {
    if (!s.used) throw fail("There is no project in that slot.");
    const app = [...APP].map((c) => c.charCodeAt(0));
    const out = [...FILE_MAGIC, ...le16(FILE_VERSION), ...le16(PB.VERSION), ...le16(PER), ...le16(app.length), ...app];
    const bytes = [Uint8Array.from(out)];
    s.parts.forEach((d, j) => bytes.push(Uint8Array.from([...le16(j), ...le16(d.length), ...le32(PB.crc32(d))]), d));
    const body = concat(bytes);
    return concat([body, Uint8Array.from(le32(PB.crc32(body)))]);
  }
  // a project file checked whole -> {appVersion, backupVersion, name, parts}; throws (code "file") with what is wrong in words
  function readProjectFile(f) {
    if (f.length < 16 || FILE_MAGIC.some((b, i) => f[i] !== b)) throw fail(`This file is not a project saved from ${PB.NAME}.`);
    const fileVer = u16(f, 4), backupVer = u16(f, 6), n = u16(f, 8), al = u16(f, 10);
    if (fileVer !== FILE_VERSION) throw fail(`This project file is from another version of this page (file version ${fileVer}).`);
    if (backupVer !== PB.VERSION) throw fail(`This project was saved by another version of ${PB.NAME}'s backup (${backupVer}). It can't be put in.`);
    if (n !== PER || 12 + al > f.length) throw fail("This project file is damaged.");
    const appVersion = String.fromCharCode(...f.subarray(12, 12 + al));
    let pos = 12 + al;
    const parts = [];
    for (let j = 0; j < PER; j++) {
      if (pos + 8 > f.length) throw fail("This project file is cut short.");
      const idx = u16(f, pos), ln = u16(f, pos + 2), crc = u32(f, pos + 4);
      pos += 8;
      if (idx !== j || ln > MAXLEN || pos + ln + 4 > f.length) throw fail(`This project file is damaged (part ${j + 1}).`);
      const d = f.slice(pos, pos + ln);
      if (PB.crc32(d) !== crc) throw fail(`This project file is damaged (part ${j + 1}: a byte changed).`);
      parts.push(d);
      pos += ln;
    }
    if (pos + 4 !== f.length || u32(f, pos) !== PB.crc32(f, 0, pos)) throw fail("This project file is damaged (the whole file's check).");
    const s = slot(parts.map((data, j) => ({ obj: id(0, j), data })), 0);
    if (!s.used) throw fail("This file holds no project.");
    return { appVersion, backupVersion: backupVer, name: s.name, parts };
  }
  // a backup image from parts ([{obj, data}]): the head, each part with its CRC-32, the CRC-32 of all of it
  function buildImage(objs, flags = 0) {
    const head = Uint8Array.from([...PB.MAGIC, ...le16(PB.VERSION), ...le16(objs.length), ...le32(flags)]);
    const out = [head, Uint8Array.from(le32(PB.crc32(head)))];
    for (const { obj, data } of objs) out.push(Uint8Array.from([...le16(obj), ...le16(data.length), ...le32(PB.crc32(data))]), data);
    const body = concat(out);
    return concat([body, Uint8Array.from(le32(PB.crc32(body)))]);
  }
  // a backup (PB.parse's result) with project p's nine parts replaced -> the backup image to restore
  function putProject(parsed, p, parts) {
    const objs = parsed.objs.map((o) => ({ obj: o.obj, data: o.data }));
    for (let j = 0; j < PER; j++) {
      const at = objs.findIndex((o) => o.obj === id(p, j));
      if (at < 0) throw fail(`This FM-1's backup has no room for project ${p + 1}.`);
      objs[at] = { obj: id(p, j), data: parts[j] };
    }
    return buildImage(objs, 0);
  }
  return { SLOTS, APP, slot, projectFile, readProjectFile, buildImage, putProject };
})();
// --- projects: end

{
// the page's side: the ports (mirrorTool) taken for a while, the buttons, the files
const $ = (i) => document.getElementById(i);
const readBtn = $("pjRead"), allBtn = $("pjAll"), allLoadBtn = $("pjAllLoad"), statusEl = $("pjStatus"), bar = $("pjBar"), list = $("pjList");
const askBox = $("pjAsk"), askText = $("pjAskText"), askYes = $("pjAskYes"), askNo = $("pjAskNo");
const fileProject = $("pjFileProject"), fileAll = $("pjFileAll");
let busy = false, last = null, wantSlot = 0, lastSaid = 0;

function pjSay(text, kind = "") {
  statusEl.textContent = text;
  statusEl.className = "msg" + (kind ? " " + kind : "");
  lastSaid = performance.now();
}
function pjProgress(v) {                                         // false: none, null: busy without a figure, 0..100
  bar.hidden = v === false;
  if (typeof v === "number") bar.value = v; else bar.removeAttribute("value");
}
const stamp = () => {                                            // the computer's own date and time: 2026-11-08-1116
  const d = new Date(), z = (n) => String(n).padStart(2, "0");
  return `${d.getFullYear()}-${z(d.getMonth() + 1)}-${z(d.getDate())}-${z(d.getHours())}${z(d.getMinutes())}`;
};
const safe = (t) => String(t).replace(/[^A-Za-z0-9._-]+/g, "-").replace(/^-+|-+$/g, "") || "project";
function download(bytes, name) {
  const a = document.createElement("a");
  a.href = URL.createObjectURL(new Blob([bytes], { type: "application/octet-stream" }));
  a.download = name;
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 1000);
}
const pause = (ms) => new Promise((r) => setTimeout(r, ms));

// one MIDI pair with the SysEx messages put together from whatever pieces the browser delivers
function makeLink(input, output) {
  const q = []; let waiter = null, cur = [], inSx = false;
  return {
    sink(data) {
      for (const v of data) {
        if (v === 0xF0) { inSx = true; cur = [v]; }
        else if (!inSx || v >= 0xF8) continue;
        else if (v === 0xF7) { inSx = false; cur.push(v); q.push(cur); if (waiter) { const w = waiter; waiter = null; w(); } }
        else if (v & 0x80) inSx = false;
        else cur.push(v);
      }
    },
    send(bytes) { try { output.send(bytes); return true; } catch (_) { return false; } },
    drain() { q.length = 0; },
    async read(ms) {
      if (!q.length) {
        let tm;
        await Promise.race([new Promise((r) => { waiter = r; }), new Promise((r) => { tm = setTimeout(r, Math.max(ms, 0)); })]);
        clearTimeout(tm);
        waiter = null;
      }
      return q.shift() || null;
    },
    get lost() { return input.state === "disconnected" || output.state === "disconnected"; },
    close() {},
  };
}
async function session(fn) {
  const ports = mirrorTool.ports();
  if (!ports) throw Object.assign(new Error(`Connect to the FM-1 first (Connect, above).`), { code: "noport" });
  const link = makeLink(ports.input, ports.output);
  mirrorTool.take(link.sink);
  try { return await fn(link); } finally { mirrorTool.give(); }
}

function update() {
  const on = mirrorTool.connected && !busy && !TOOLS.on;
  readBtn.disabled = allBtn.disabled = allLoadBtn.disabled = !on;
  list.querySelectorAll("button").forEach((b) => { b.disabled = !on || b.dataset.needs === "never" || (b.dataset.needs === "used" && !last); });
  if (TOOLS.on && !busy) return;                                 // (another tool has the ports: its own words)
  if (!mirrorTool.connected && !busy) pjSay("Connect to the FM-1 above first.");
  else if (mirrorTool.connected && !busy && !last && statusEl.textContent.startsWith("Connect")) pjSay(`Press "Read the FM-1" to see its projects.`);
}
function render() {
  list.replaceChildren();
  for (let p = 0; p < PJ.SLOTS; p++) {
    const s = last ? PJ.slot(last.parsed.objs, p) : null;
    const li = document.createElement("li");
    const n = p + 1;
    li.innerHTML = `<span class="slot">Project ${n}</span><span class="pname"></span>` +
      `<button type="button" data-do="save" data-needs="used" aria-label="Save project ${n} to a file">Save to file</button>` +
      `<button type="button" data-do="put" aria-label="Replace project ${n} with a file">Replace with a file&hellip;</button>`;
    const nm = li.querySelector(".pname");
    nm.textContent = !s ? "Not read yet" : s.used ? s.name || "(no name)" : "Empty";
    nm.classList.toggle("empty", !s || !s.used);
    const save = li.querySelector('[data-do="save"]');
    save.dataset.slot = li.querySelector('[data-do="put"]').dataset.slot = String(p);
    if (s && !s.used) save.dataset.needs = "never";
    list.appendChild(li);
  }
  update();
}

async function readDevice() {
  pjSay("Reading the FM-1…");
  pjProgress(null);
  let r;
  try {
    r = await session((link) => PB.backup(link, (n) => {
      if (performance.now() - lastSaid > 1500) pjSay(`Reading the FM-1… ${Math.round(n / 1024)} kB`);
    }));
  } catch (e) {
    if (e.code === "stopped" && /didn't answer/.test(e.message)) {
      e.message = `${PB.NAME} didn't answer. Projects and backup need ${PB.NAME} ${PJ.APP} or later on the FM-1: install it first, and plug the FM-1 in with the screen on.`;
    }
    throw e;
  } finally {
    pjProgress(false);
  }
  last = { image: r.image, flags: r.flags, parsed: PB.parse(r.image), at: new Date() };
  render();
  return last;
}

// an image to the FM-1 (it asks for its pieces) -> true when restored; false (and said) when not
async function sendImage(image) {
  pjSay("Sending to the FM-1…");
  pjProgress(null);
  let code;
  try {
    code = await session((link) => PB.restore(link, image, (ev, pct) => {
      if (ev === "ask") { pjProgress(false); pjSay("Press SEL on the FM-1 to confirm, or HOME to cancel.", "wait"); }
      else if (ev === "progress" && performance.now() - lastSaid > 1500) { pjProgress(pct); pjSay(`Sending to the FM-1… ${Math.round(pct)}%`); }
    }));
  } finally {
    pjProgress(false);
  }
  if (code === 0) return true;
  if (code === PB.PLAYING) pjSay(`${PB.NAME} is playing. Press PLAY on the FM-1 to stop it, then try again. Nothing was changed.`, "err");
  else if (code === PB.DECLINED) pjSay("HOME was pressed on the FM-1. Nothing was changed.");
  else pjSay(`The FM-1 did not take it: ${PB.ERRORS[code] || "error " + code}. Nothing was changed${code === 12 ? ", unless it finishes the last step when it is switched on again" : ""}.`, "err");
  return false;
}

function confirmAsk(text, yes) {
  return new Promise((resolve) => {
    askText.textContent = text;
    askYes.textContent = yes;
    askBox.hidden = false;
    askYes.focus();
    const done = (v) => { askBox.hidden = true; askYes.onclick = askNo.onclick = null; resolve(v); };
    askYes.onclick = () => done(true);
    askNo.onclick = () => done(false);
  });
}

async function run(fn) {
  if (busy || TOOLS.on) return;
  busy = true;
  TOOLS.set(true);
  try { await fn(); }
  catch (e) { pjSay(e.message || String(e), "err"); }
  finally { busy = false; pjProgress(false); TOOLS.set(false); }
}

readBtn.addEventListener("click", () => run(async () => {
  await readDevice();
  const used = [0, 1, 2, 3].filter((p) => PJ.slot(last.parsed.objs, p).used).length;
  pjSay(`Read the FM-1: ${used} of ${PJ.SLOTS} projects in use.`);
}));
allBtn.addEventListener("click", () => run(async () => {
  await readDevice();
  download(last.image, `${safe(PB.NAME)}-backup-${stamp()}.{{EXT}}`);
  pjSay(`Saved everything to a file (${Math.round(last.image.length / 1024)} kB).`);
}));
list.addEventListener("click", (e) => {
  const b = e.target.closest("button");
  if (!b || b.disabled) return;
  const p = Number(b.dataset.slot);
  if (b.dataset.do === "save") {
    run(async () => {
      await readDevice();
      const s = PJ.slot(last.parsed.objs, p);
      if (!s.used) { pjSay(`Project ${p + 1} is empty. Nothing to save.`); return; }
      download(PJ.projectFile(s), `${safe(PB.NAME)}-project-${p + 1}-${stamp()}.fmkpj`);
      pjSay(`Saved project ${p + 1} (${s.name}) to a file.`);
    });
  } else if (b.dataset.do === "put") {
    wantSlot = p;
    fileProject.value = "";
    fileProject.click();
  }
});
fileProject.addEventListener("change", () => {
  const f = fileProject.files && fileProject.files[0];
  if (!f) return;
  const p = wantSlot;
  run(async () => {
    const proj = PJ.readProjectFile(new Uint8Array(await f.arrayBuffer()));
    const note = proj.appVersion && proj.appVersion !== PJ.APP ? ` It was saved with ${PB.NAME} ${proj.appVersion}; this page is for ${PJ.APP}.` : "";
    const yes = await confirmAsk(`Replace project ${p + 1} with the one in this file (${proj.name})? What project ${p + 1} holds now is lost.${note} ` +
      `Stop the sequencer first. The FM-1 asks you to confirm with SEL. ${BETA_NOTE}`, `Replace project ${p + 1}`);
    if (!yes) { pjSay("Cancelled. Nothing was changed."); return; }
    const d = await readDevice();                                // the merge is made on what the FM-1 holds now
    if (d.flags & 1) { pjSay(`${PB.NAME} is playing. Press PLAY on the FM-1 to stop it, then try again. Nothing was changed.`, "err"); return; }
    if (!(await sendImage(PJ.putProject(d.parsed, p, proj.parts)))) return;
    pjSay(`Project ${p + 1} is now the file's (${proj.name}). Load it on the FM-1: GLO, PROJECTS, LOAD.`);
    await pause(1500);
    try { await readDevice(); pjSay(`Project ${p + 1} is now the file's (${proj.name}). Load it on the FM-1: GLO, PROJECTS, LOAD.`); } catch (_) { /* (the list stays as it was: Read the FM-1 again) */ }
  });
});
allLoadBtn.addEventListener("click", () => { fileAll.value = ""; fileAll.click(); });
fileAll.addEventListener("change", () => {
  const f = fileAll.files && fileAll.files[0];
  if (!f) return;
  run(async () => {
    const image = new Uint8Array(await f.arrayBuffer());
    PB.parse(image);                                             // (checked whole: a bad file is never sent)
    const yes = await confirmAsk(`Load everything from "${f.name}"? It replaces all that ${PB.NAME} has saved in the FM-1: the patterns, ` +
      `the user kicks, the settings and the four projects. Stop the sequencer first. The FM-1 asks you to confirm with SEL. ${BETA_NOTE}`, "Load everything");
    if (!yes) { pjSay("Cancelled. Nothing was changed."); return; }
    if (!(await sendImage(image))) return;
    pjSay("Everything is loaded.");
    await pause(1500);
    try { await readDevice(); pjSay("Everything is loaded."); } catch (_) { last = null; render(); }
  });
});
mirrorTool.onports(() => { if (!busy) update(); });
TOOLS.hooks.push(update);
render();

// ---------------------------------------------------------------- Render Audio (extra.html #render; play/render.js, the
// player's): a pattern rendered by FM-KICK's own sound engine in this browser, from a backup read from the FM-1 just now
// (the ports taken as for Projects and backup). The FM-1 is only read: nothing is written to it.
const renderEl = document.getElementById("render");
if (renderEl && !/^https?:$/.test(location.protocol)) renderEl.hidden = true;   // (a page opened from a file: no modules)
else if (renderEl) import("./play/render.js").then(({ setupRender }) => {
  const part = setupRender(renderEl, {
    wasmUrl: "play/app.wasm", workerUrl: "play/render_worker.js", playing: false,
    ready: () => (!mirrorTool.connected ? "Connect to the FM-1 above first." : busy || TOOLS.on ? "Another tool is using the FM-1." : ""),
    source: async (say) => {
      if (busy || TOOLS.on) throw new Error("Another tool is using the FM-1. Wait for it, then try again.");
      busy = true;
      TOOLS.set(true);
      try {
        say("Reading the FM-1…");
        const r = await session((link) => PB.backup(link, (n) => say(`Reading the FM-1… ${Math.round(n / 1024)} kB`)));
        const store = {};
        for (const o of PB.parse(r.image).objs) store[o.obj] = o.data;
        return { store, note: r.flags & 1 ? "The FM-1 was playing, so its latest changes may not be in it: stop it and render again to have them." : "" };
      } catch (e) {
        if (e.code === "stopped" && /didn't answer/.test(e.message)) e.message = `${PB.NAME} didn't answer. Render Audio needs ${PB.NAME} ${PJ.APP} or later on the FM-1.`;
        throw e;
      } finally {
        busy = false;
        TOOLS.set(false);
      }
    },
  });
  mirrorTool.onports(() => part.update());
  TOOLS.hooks.push(() => part.update());
}).catch(() => { renderEl.hidden = true; });
}
