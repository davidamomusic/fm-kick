// SPDX-License-Identifier: MPL-2.0
// FM-KICK's browser player (kick/build/emu) in a real (headless) Chromium: the page
// loads, "Switch on" starts the module, and the FM-1's screen is drawn.
//   node kick/tests/play_browser_test.mjs        (from the repository's root; needs playwright-core and a Chromium:
//   PLAYWRIGHT_CORE and CHROMIUM in the environment, or /opt/npm-tools and /opt/pw-browsers; skipped without them)
import fs from "fs";
import http from "http";
import path from "path";
import { fileURLToPath } from "url";

const dir = path.join(path.dirname(fileURLToPath(import.meta.url)), process.env.PLAY_DIR || "../docs/play");
const core = process.env.PLAYWRIGHT_CORE || "/opt/npm-tools/node_modules/playwright-core/index.mjs";
const chromePath = process.env.CHROMIUM || (fs.existsSync("/opt/pw-browsers") ? fs.readdirSync("/opt/pw-browsers").filter((d) => d.startsWith("chromium-"))
  .map((d) => `/opt/pw-browsers/${d}/chrome-linux64/chrome`).find((p) => fs.existsSync(p)) : null);
if (!fs.existsSync(core) || !chromePath) { console.log("SKIP no playwright-core or Chromium"); process.exit(0); }
const { chromium } = await import(core);

let fails = 0;
const check = (ok, msg) => { console.log(`${ok ? "ok  " : "FAIL"} ${msg}`); if (!ok) fails++; };
const TYPE = { ".html": "text/html", ".js": "text/javascript", ".wasm": "application/wasm" };
const missing = [];
const srv = http.createServer((q, r) => {
  let p = path.join(dir, decodeURIComponent(q.url.split("?")[0]));
  if (p.endsWith(path.sep)) p += "index.html";
  if (!p.startsWith(dir) || !fs.existsSync(p)) { missing.push(q.url); r.writeHead(404); r.end(); return; }
  r.writeHead(200, { "content-type": TYPE[path.extname(p)] || "application/octet-stream" });
  r.end(fs.readFileSync(p));
}).listen(0);
const browser = await chromium.launch({ executablePath: chromePath, args: ["--autoplay-policy=no-user-gesture-required"] });
const page = await browser.newPage({ viewport: { width: 1100, height: 900 } });
const errors = [];
page.on("pageerror", (e) => errors.push(String(e)));
await page.goto(`http://127.0.0.1:${srv.address().port}/`);
check((await page.title()).includes("FM-KICK"), "the page is FM-KICK's");
await page.click("#go");
await page.waitForTimeout(4000);
const lit = await page.evaluate(() => {
  const d = document.getElementById("screen").getContext("2d").getImageData(0, 0, 240, 240).data;
  let n = 0;
  for (let i = 0; i < d.length; i += 4) if (d[i] + d[i + 1] + d[i + 2] > 60) n++;
  return n;
});
check(lit > 5000, `the screen is drawn (${lit} lit pixels)`);
// Render Audio: a copy of the running player, the pattern playing, 2 bars with the effects, then the kick dry
await page.selectOption('#render [data-r="bars"]', "2");
await page.click('#render [data-r="go"]');
await page.waitForFunction(() => /BPM/.test(document.querySelector('#render [data-r="info"]').textContent), null, { timeout: 30000 }).catch(() => {});
const info = await page.evaluate(() => document.querySelector('#render [data-r="info"]').textContent);
check(/A0\d · 128 BPM · 2 bars/.test(info), `Render Audio: ${info}`);
const peak = Number((/peak (-?[\d.]+) dBFS/.exec(info) || [])[1]);
check(peak > -30 && peak <= 0, `Render Audio: the pattern sounds (peak ${peak} dBFS)`);
check(await page.evaluate(() => !document.querySelector('#render [data-r="save"]').disabled), "Render Audio: Save WAV ready");
await page.click('#render [data-w="1"]');
await page.waitForFunction(() => /-dry-/.test(document.querySelector('#render [data-r="file"]').textContent), null, { timeout: 30000 }).catch(() => {});
const file = await page.evaluate(() => document.querySelector('#render [data-r="file"]').textContent);
check(/^fm-kick-A0\d-dry-128bpm\.wav$/.test(file), `Render Audio, the kick dry: ${file}`);
await page.screenshot({ path: process.env.SHOT || "/tmp/fm-kick-page.png", fullPage: true });
check(errors.length === 0, `no script error (${errors.slice(0, 2).join("; ")})`);
// the page asks for two things it can do without: the screen reader's words and an icon
const needed = missing.filter((u) => !u.includes("reader/") && !u.includes("favicon"));
check(needed.length === 0, `every file the page needs is there (${needed.join(" ")})`);
await browser.close();
srv.close();
console.log(fails ? `${fails} FAILED` : "all passed");
process.exit(fails ? 1 : 0);
