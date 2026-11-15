// SPDX-License-Identifier: MPL-2.0
// FM-KICK's mirror page in a real (headless) Chromium on a fake Web MIDI with a simulated FM-1 behind it: the mirror answers
// its HELLO; a backup streams (device/backup.c's frames) and a restore pulls its pieces and asks for SEL. What the person
// does on the page, end to end: Connect, Read the FM-1, Save a project to a file, put a file into another slot, save and load
// everything, with the mirror stopped while the page has the ports and asked for the screen again after it. Render Audio
// too: the page is served over HTTP with the player's files (kick/docs/play) beside it, and the simulated FM-1's settings
// and bank A are FM-KICK's own (the player's engine, booted here), so the patterns it reads render. (From FM-FAM's.)
//   node kick/tests/projects_browser_test.mjs        (from the repository's root; needs playwright-core and a Chromium:
//   PLAYWRIGHT_CORE and CHROMIUM in the environment, or /opt/npm-tools and /opt/pw-browsers; skipped without them)
import fs from "fs";
import http from "http";
import os from "os";
import path from "path";
import { execFileSync } from "child_process";
import { fileURLToPath } from "url";

const root = path.join(path.dirname(fileURLToPath(import.meta.url)), "../..");
const core = process.env.PLAYWRIGHT_CORE || "/opt/npm-tools/node_modules/playwright-core/index.mjs";
const chromePath = process.env.CHROMIUM || (fs.existsSync("/opt/pw-browsers") ? fs.readdirSync("/opt/pw-browsers").filter((d) => d.startsWith("chromium-"))
  .map((d) => `/opt/pw-browsers/${d}/chrome-linux64/chrome`).find((p) => fs.existsSync(p)) : null);
if (!fs.existsSync(core) || !chromePath) { console.log("SKIP no playwright-core or Chromium"); process.exit(0); }
const { chromium } = await import(core);

let fails = 0;
const check = (ok, msg) => { console.log(`${ok ? "ok  " : "FAIL"} ${msg}`); if (!ok) fails++; };

const html = execFileSync("python3", ["-c",
  "import sys; sys.path.insert(0, 'tools'); from pathlib import Path; import make_site; " +
  "sys.stdout.write(make_site.tool_pages(Path('kick'))['mirror/index.html'])"], { cwd: root, maxBuffer: 1 << 28 }).toString("utf8");
const work = fs.mkdtempSync(path.join(os.tmpdir(), "kick-page-"));
const pagePath = path.join(work, "mirror.html");
fs.writeFileSync(pagePath, html);

// the device's parts (the firmware's backup's layout: app.h APP_BACKUP_OBJS, 50 parts)
const appH = fs.readFileSync(path.join(root, "kick/app.h"), "utf8");
const enumBody = /enum\s*\{([^}]*OBJ_SET[^}]*)\}/.exec(appH)[1];
const ids = Object.fromEntries(enumBody.split(",").map((x) => x.split("=")[0].trim()).filter(Boolean).map((n, i) => [n, i]));
const objsList = /#define APP_BACKUP_OBJS \{([^}]*)\}/.exec(appH)[1].replace(/\\/g, "").split(",").map((x) => {
  const m = /^(OBJ_[A-Z0-9_]+)(?:\s*\+\s*(\d+))?$/.exec(x.trim());
  return ids[m[1]] + (m[2] ? Number(m[2]) : 0);
});
const crcT = Uint32Array.from({ length: 256 }, (_, n) => { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1; return c >>> 0; });
const crc32 = (b, s = 0, e = b.length) => { let c = 0xFFFFFFFF; for (let i = s; i < e; i++) c = crcT[(c ^ b[i]) & 255] ^ (c >>> 8); return (c ^ 0xFFFFFFFF) >>> 0; };
const le16 = (v) => [v & 255, v >>> 8 & 255], le32 = (v) => [v & 255, v >>> 8 & 255, v >>> 16 & 255, v >>> 24 & 255];
let seed = 99;
const bytes = (n) => Uint8Array.from({ length: n }, () => ((seed = (Math.imul(seed, 1103515245) + 12345) >>> 0) >>> 16) & 255);
// a project's head (app.c prj_t): "KPJ1", u16 tempo x 10, u8 pattern, u8 bank, u8 patterns with steps, the rest
const prj = (bpm10, cur, bank, npat) => { const d = bytes(84); d.set([0x4B, 0x50, 0x4A, 0x31, bpm10 & 255, bpm10 >> 8, cur, bank, npat], 0); return d; };
const P = (p, j) => ids.OBJ_PJ0 + p * 9 + j;
const data = new Map(objsList.map((id) => [id, new Uint8Array(0)]));   // (absent: the FM-1 makes them as new)
data.set(P(0, 0), prj(1280, 0, 0, 20)); [1, 2, 3, 4, 5, 6, 7, 8].forEach((j) => data.set(P(0, j), bytes(3840)));
data.set(P(1, 0), prj(1365, 2, 1, 7)); [1, 2, 3, 4, 5, 6, 7, 8].forEach((j) => data.set(P(1, j), bytes(3840)));
function image(flags = 0, get = (id) => data.get(id)) {
  const head = Uint8Array.from([...Buffer.from("FMKB"), ...le16(1), ...le16(objsList.length), ...le32(flags)]);
  const parts = [head, Uint8Array.from(le32(crc32(head)))];
  for (const id of objsList) { const d = get(id); parts.push(Uint8Array.from([...le16(id), ...le16(d.length), ...le32(crc32(d))]), d); }
  const body = Buffer.concat(parts);
  return Buffer.concat([body, Buffer.from(le32(crc32(body)))]);
}
// the settings and bank A as FM-KICK saves them (app.wasm, the player's build, SAVE pressed): Render Audio plays them
{
  const wasm = fs.readFileSync(path.join(root, "kick/docs/play/app.wasm"));
  let mem;
  const stub = (t) => new Proxy(t, { get: (o, k) => (k in o ? o[k] : () => 0) });
  const { instance } = await WebAssembly.instantiate(wasm, { wasi_snapshot_preview1: stub({ clock_time_get: (a, b, out) => { new DataView(mem.buffer).setBigUint64(out, 0n, true); return 0; } }), env: stub({}) });
  const ex = instance.exports;
  mem = ex.memory;
  if (ex._initialize) ex._initialize();
  ex.web_boot();
  for (let i = 0; i < 100; i++) ex.web_render(1024);
  ex.web_button && ex.web_button(0);
  for (const id of [ids.OBJ_SET, ids.OBJ_P0, ids.OBJ_P1, ids.OBJ_P2, ids.OBJ_P3]) {
    const n = ex.web_store_len(id);
    if (n > 0) data.set(id, new Uint8Array(mem.buffer, ex.web_store(id), n).slice());
  }
}
const startImage = image();

// ---- the browser's side: a fake Web MIDI and the FM-1 behind it (plain JS, run before the page's own scripts)
const fakeMidi = `(() => {
  const IMG0 = Uint8Array.from(atob(__IMG__), (c) => c.charCodeAt(0));
  const dev = window.__fm1 = { image: IMG0, playing: false, answer: true, mirrorOps: [], asks: 0, restores: 0, backups: 0, selDelay: 150, busyNow: false };
  const pack7 = (d) => { const o = []; let acc = 0, nb = 0; for (const b of d) { acc |= b << nb; nb += 8; while (nb >= 7) { o.push(acc & 127); acc >>>= 7; nb -= 7; } } if (nb) o.push(acc & 127); return o; };
  const unpack7 = (s) => { const o = []; let acc = 0, nb = 0; for (const b of s) { acc |= (b & 127) << nb; nb += 7; while (nb >= 8) { o.push(acc & 255); acc >>>= 8; nb -= 8; } } return o; };
  const T = Uint32Array.from({ length: 256 }, (_, n) => { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1; return c >>> 0; });
  const crc = (b, s = 0, e = b.length) => { let c = 0xFFFFFFFF; for (let i = s; i < e; i++) c = T[(c ^ b[i]) & 255] ^ (c >>> 8); return (c ^ 0xFFFFFFFF) >>> 0; };
  const le32 = (v) => [v & 255, (v >>> 8) & 255, (v >>> 16) & 255, (v >>> 24) & 255];
  const u32 = (b, o) => (b[o] | b[o + 1] << 8 | b[o + 2] << 16 | b[o + 3] << 24) >>> 0;
  const frame = (cmd, body) => { const s = body.reduce((a, v) => a + v, 0); return [0xF0, ...pack7([0, 0x59, cmd, body.length & 255, body.length >> 8, 0, ...body, ~s & 255]), 0xF7]; };
  const input = { id: "in1", name: "FM-KICK FM-1", type: "input", state: "connected", connection: "open", onmidimessage: null, open: async () => input, close: async () => input };
  const emit = (arr) => { if (input.onmidimessage) input.onmidimessage({ data: Uint8Array.from(arr) }); };
  const later = (fn, ms = 0) => setTimeout(fn, ms);
  let pull = null, wanted = null;
  function ask(addr, n) { const b = [0, ...le32(addr >>> 0), n & 255, n >> 8, 0]; wanted = [addr >>> 0, n]; later(() => emit([0xF0, ...pack7([0, 0x59, 0x30, 8, 0, 0, ...b, ~b.reduce((a, v) => a + v, 0) & 255]), 0xF7])); }
  function* puller(n, c) {
    if (dev.playing) { yield [(0xCF000000 | 10) >>> 0, 8]; return; }
    const got = [...(yield [0, 16])];
    let pos = 16;
    const count = got[6] | got[7] << 8;
    for (let i = 0; i < count; i++) {
      const r = yield [pos, 8]; got.push(...r);
      const ln = r[2] | r[3] << 8; pos += 8;
      for (let k = 0; k < ln; k += 512) got.push(...(yield [pos + k, Math.min(512, ln - k)]));
      pos += ln;
    }
    got.push(...(yield [pos, 4]));
    const g = Uint8Array.from(got);
    if (g.length !== n || crc(g) !== c) { yield [(0xCF000000 | 6) >>> 0, 8]; return; }
    dev.asks++;
    yield [0xC1000000, 8];
    yield ["SEL"];
    if (!dev.answer) { yield [(0xCF000000 | 11) >>> 0, 8]; return; }
    dev.image = g; dev.restores++;
    yield [0xC0000000, 8];
  }
  function next(d) {
    const r = pull.next(d);
    if (r.done) { pull = null; return; }
    if (r.value[0] === "SEL") { later(() => next(), dev.selDelay); return; }
    ask(...r.value);
  }
  function stream() {
    dev.backups++; dev.busyNow = true;
    const img = Uint8Array.from(dev.image); img.set(le32(dev.playing ? 1 : 0), 8); img.set(le32(crc(img, 0, 12)), 12); img.set(le32(crc(img, 0, img.length - 4)), img.length - 4);
    let off = 0;
    const step = () => {
      if (off < img.length) { emit(frame(0x42, [...le32(off), ...img.subarray(off, off + 128)])); off += 128; later(step, 0); }
      else { emit(frame(0x43, [...le32(img.length), ...le32(crc(img)), 0, dev.playing ? 1 : 0])); dev.busyNow = false; }
    };
    later(step, 5);
  }
  const output = { id: "out1", name: "FM-KICK FM-1", type: "output", state: "connected", connection: "open", open: async () => output, close: async () => output,
    send(arr) {
      const d = Uint8Array.from(arr), u = unpack7(Array.from(d.slice(1, -1)));
      if (u[0] !== 0 || u[1] !== 0x59) return;
      const cmd = u[2];
      if (cmd === 0x50) {                                      // the mirror's request: op 0 off, 1 keep-alive, 2 refresh
        dev.mirrorOps.push(u[6]);
        if (dev.busyNow) return;
        later(() => emit(frame(0x50, [4, u[6] ? 1 : 0, 239, 239])));
      } else if (cmd === 0x42) { stream(); }
      else if (cmd === 0x41) { pull = puller(u32(u, 6), u32(u, 10)); next(undefined); }
      else if (cmd === 0x30 && pull && wanted) {
        const addr = u32(u, 7), n = u[11] | u[12] << 8;
        if (addr !== wanted[0] || n !== wanted[1]) return;
        next(Uint8Array.from(u.slice(14, 14 + n)));
      }
    } };
  const access = { inputs: new Map([[input.id, input]]), outputs: new Map([[output.id, output]]), onstatechange: null, sysexEnabled: true };
  navigator.requestMIDIAccess = async () => access;
  Object.defineProperty(navigator, "permissions", { value: { query: () => Promise.reject(new Error("no")) }, configurable: true });
})();`;

const browser = await chromium.launch({ executablePath: chromePath, args: ["--no-sandbox"] });
const ctx = await browser.newContext({ acceptDownloads: true, viewport: { width: 1200, height: 1100 } });
const page = await ctx.newPage();
const errors = [];
page.on("pageerror", (e) => errors.push(e.message));
page.on("console", (m) => { if (m.type() === "error" && !/Failed to load resource/.test(m.text())) errors.push(m.text()); });   // (the page's web font, when offline)
await page.addInitScript(fakeMidi.replace("__IMG__", JSON.stringify(startImage.toString("base64"))));
const TYPE = { ".html": "text/html", ".js": "text/javascript", ".wasm": "application/wasm" };
const srv = http.createServer((q, r) => {
  const u = decodeURIComponent(q.url.split("?")[0]);
  const f = u.startsWith("/play/") ? path.join(root, "kick/docs", u) : path.join(work, u);
  if (!fs.existsSync(f) || fs.statSync(f).isDirectory()) { r.writeHead(404); r.end(); return; }
  r.writeHead(200, { "content-type": TYPE[path.extname(f)] || "application/octet-stream" });
  r.end(fs.readFileSync(f));
}).listen(0);
await page.goto(`http://127.0.0.1:${srv.address().port}/mirror.html`);

const text = (sel) => page.textContent(sel).then((t) => t.trim());
const waitStatus = (re, ms = 15000) => page.waitForFunction((src) => new RegExp(src).test(document.getElementById("pjStatus").textContent), re.source, { timeout: ms });

check((await text("#pjStatus")).startsWith("Connect"), "before Connect: the section says to connect first");
check(await page.isDisabled("#pjRead"), "... and Read the FM-1 is disabled");
await page.click("#connect");
await page.waitForFunction(() => /Mirroring/.test(document.getElementById("status").textContent), null, { timeout: 8000 });
check(true, "Connect: the page found \"FM-KICK FM-1\" and the mirror says Mirroring");
check(await page.isEnabled("#pjRead"), "... Read the FM-1 is enabled");

// 1. read
await page.click("#pjRead");
await waitStatus(/Read the FM-1: 2 of 4 projects in use/);
const rows = await page.$$eval("#pjList li", (l) => l.map((x) => x.querySelector(".pname").textContent));
const P1 = "128 BPM, 20 patterns, A01", P2 = "136.5 BPM, 7 patterns, B03";
check(rows.join("|") === `${P1}|${P2}|Empty|Empty`, `Read the FM-1: the four slots (${rows.join(", ")})`);
const dis = await page.$$eval("#pjList li", (l) => l.map((x) => [...x.querySelectorAll("button")].map((b) => b.disabled ? "x" : "o").join("")));
check(dis.join() === "oo,oo,xo,xo", "... Save to file is on for the two used slots only; Replace is on for all");
const ops0 = await page.evaluate(() => window.__fm1.mirrorOps.length);
await page.waitForTimeout(100);
check(await page.evaluate(() => window.__fm1.backups) === 1, "... one backup asked");

// Render Audio: a backup read for it, the patterns with steps offered, the first one rendered
const rMsg = () => page.textContent('#render [data-r="msg"]');
check(await page.isEnabled('#render [data-r="go"]'), "Render Audio: on once connected");
await page.click('#render [data-r="go"]');
await page.waitForFunction(() => /Rendered|no sound|didn't|not/.test(document.querySelector('#render [data-r="msg"]').textContent)
  && !document.querySelector('#render [data-r="go"]').disabled, null, { timeout: 60000 });
const rInfo = await page.textContent('#render [data-r="info"]');
check((await rMsg()) === "Rendered." && /^A\d\d · 128 BPM · 4 bars/.test(rInfo), `... the FM-1's first pattern with steps rendered (${await rMsg()} ${rInfo})`);
check(await page.evaluate(() => window.__fm1.backups) === 2, "... from a backup read for it");
const offered = await page.$eval('#render [data-r="pat"]', (s) => [...s.options].map((o) => o.textContent));
check(offered.length >= 16 && offered.includes("A01") && offered.includes("B01") && offered.includes("C16"), `... the patterns with steps on are offered, banks A to C (${offered.length})`);
await page.click('#render .seg button:has-text("Kick dry")');
await page.waitForFunction(() => /-dry-/.test(document.querySelector('#render [data-r="file"]').textContent), null, { timeout: 30000 });
check(await page.evaluate(() => window.__fm1.backups) === 2, "... the kick dry, from the same read (nothing asked again)");
await page.waitForFunction(() => !document.querySelector('#render [data-r="go"]').disabled, null, { timeout: 30000 });

// 2. save a project to a file
const [dl] = await Promise.all([page.waitForEvent("download"), page.click('#pjList li:nth-child(1) button[data-do="save"]')]);
const projPath = path.join(work, "p1.fmkpj");
await dl.saveAs(projPath);
const projFile = fs.readFileSync(projPath);
check(/^FM-KICK-project-1-\d{4}-\d\d-\d\d-\d{4}\.fmkpj$/.test(dl.suggestedFilename()) && projFile.subarray(0, 4).toString() === "FMKP", `Save to file: ${dl.suggestedFilename()}, an FMKP file of ${projFile.length} bytes`);
await waitStatus(/Saved project 1 \(128 BPM/);

// 3. put it into slot 3
const [chooser] = await Promise.all([page.waitForEvent("filechooser"), page.click('#pjList li:nth-child(3) button[data-do="put"]')]);
await chooser.setFiles(projPath);
await page.waitForSelector("#pjAsk:not([hidden])");
const q = await text("#pjAskText");
check(/Replace project 3 with the one in this file \(128 BPM, 20 patterns, A01\)/.test(q) && /SEL/.test(q), `Replace with a file: the page asks first ("${q.slice(0, 90)}...")`);
check(await page.evaluate(() => window.__fm1.restores) === 0, "... nothing sent to the FM-1 before the answer");
await page.click("#pjAskYes");
await waitStatus(/Press SEL on the FM-1/, 20000);
check(true, "after Yes: the page tells you to press SEL on the FM-1");
await waitStatus(/Project 3 is now the file's/, 20000);
await page.waitForFunction((t) => document.querySelector("#pjList li:nth-child(3) .pname").textContent === t, P1, { timeout: 15000 });
const rows2 = await page.$$eval("#pjList li", (l) => l.map((x) => x.querySelector(".pname").textContent));
check(rows2.join("|") === `${P1}|${P2}|${P1}|Empty`, `... the FM-1 holds it: ${rows2.join(", ")}`);
const held = await page.evaluate(() => Array.from(window.__fm1.image).join(","));
const expected = image(0, (id) => (id >= P(2, 0) && id <= P(2, 8) ? data.get(id - 18) : data.get(id)));
check(held === Array.from(expected).join(","), "... the image in the FM-1 is the old one with slot 3's nine parts replaced, byte for byte");
const ops1 = await page.evaluate((n) => window.__fm1.mirrorOps.slice(n), ops0);
check(ops1.includes(2), "the mirror asked for the whole screen again after the page gave the ports back");

// 4. a project file refused: a damaged one, and the FM-1 playing
const bad = Buffer.from(projFile); bad[300] ^= 1;
const badPath = path.join(work, "bad.fmkpj"); fs.writeFileSync(badPath, bad);
const [c2] = await Promise.all([page.waitForEvent("filechooser"), page.click('#pjList li:nth-child(4) button[data-do="put"]')]);
await c2.setFiles(badPath);
await waitStatus(/damaged/);
check(await page.isHidden("#pjAsk") && await page.evaluate(() => window.__fm1.restores) === 1, "a damaged project file: said, nothing asked, nothing sent");
await page.evaluate(() => { window.__fm1.playing = true; });
const [c3] = await Promise.all([page.waitForEvent("filechooser"), page.click('#pjList li:nth-child(4) button[data-do="put"]')]);
await c3.setFiles(projPath);
await page.waitForSelector("#pjAsk:not([hidden])");
await page.click("#pjAskYes");
await waitStatus(/is playing\. Press PLAY/, 20000);
check(await page.evaluate(() => window.__fm1.restores) === 1, "the FM-1 playing: STOP FIRST said, nothing restored");
await page.evaluate(() => { window.__fm1.playing = false; });

// 5. HOME on the FM-1
await page.evaluate(() => { window.__fm1.answer = false; });
const [c4] = await Promise.all([page.waitForEvent("filechooser"), page.click('#pjList li:nth-child(4) button[data-do="put"]')]);
await c4.setFiles(projPath);
await page.waitForSelector("#pjAsk:not([hidden])");
await page.click("#pjAskYes");
await waitStatus(/HOME was pressed on the FM-1\. Nothing was changed/, 20000);
check(await page.evaluate(() => window.__fm1.restores) === 1, "HOME on the FM-1: said, nothing changed");
await page.evaluate(() => { window.__fm1.answer = true; });

// 6. Cancel at the page's own question
const [c5] = await Promise.all([page.waitForEvent("filechooser"), page.click('#pjList li:nth-child(4) button[data-do="put"]')]);
await c5.setFiles(projPath);
await page.waitForSelector("#pjAsk:not([hidden])");
const before = await page.evaluate(() => window.__fm1.backups);
await page.click("#pjAskNo");
await waitStatus(/Cancelled/);
check(await page.evaluate(() => window.__fm1.backups) === before, "Cancel: nothing read, nothing sent");

// 7. everything: save, then load into a changed FM-1
const [dl2] = await Promise.all([page.waitForEvent("download"), page.click("#pjAll")]);
const allPath = path.join(work, "all.fmkbak");
await dl2.saveAs(allPath);
const all = fs.readFileSync(allPath);
check(/^FM-KICK-backup-\d{4}-\d\d-\d\d-\d{4}\.fmkbak$/.test(dl2.suggestedFilename()) && all.subarray(0, 4).toString() === "FMKB" && Buffer.compare(all, Buffer.from(held.split(",").map(Number))) === 0,
      `Save everything to a file: ${dl2.suggestedFilename()}, the FM-1's image byte for byte (${all.length} bytes)`);
await page.evaluate((i) => { window.__fm1.image = Uint8Array.from(atob(i), (c) => c.charCodeAt(0)); }, startImage.toString("base64"));
const [c6] = await Promise.all([page.waitForEvent("filechooser"), page.click("#pjAllLoad")]);
await c6.setFiles(allPath);
await page.waitForSelector("#pjAsk:not([hidden])");
check(/replaces all that FM-KICK has saved/.test(await text("#pjAskText")), "Load everything from a file: the page says what it replaces and asks");
await page.click("#pjAskYes");
await waitStatus(/Everything is loaded/, 25000);
const held2 = await page.evaluate(() => Array.from(window.__fm1.image).join(","));
check(held2 === held, "... the FM-1 holds the saved backup byte for byte");
check(JSON.stringify(await page.$$eval("#pjList li .pname", (l) => l.map((x) => x.textContent))) === JSON.stringify([P1, P2, P1, "Empty"]), "... and the list shows it");

// 8. not a backup
const junk = path.join(work, "junk.fmkbak"); fs.writeFileSync(junk, "this is not a backup of anything, only text, long enough to look like a file");
const [c7] = await Promise.all([page.waitForEvent("filechooser"), page.click("#pjAllLoad")]);
await c7.setFiles(junk);
await waitStatus(/not a FM-KICK backup|not an FM-KICK backup/);
check(await page.isHidden("#pjAsk"), "a file that is not a backup: said, nothing asked");

check(errors.length === 0, "no errors in the page's console" + (errors.length ? ": " + errors.slice(0, 3).join(" | ") : ""));
await page.screenshot({ path: path.join(work, "page.png"), fullPage: true });
console.log("screenshot " + path.join(work, "page.png"));
await browser.close();
srv.close();
console.log(fails ? `\n${fails} FAILED` : "\nall ok");
process.exit(fails ? 1 : 0);
