// SPDX-License-Identifier: MPL-2.0
// FM-KICK's screen mirror page (kick/docs/FM-KICK-mirror.html) opens in Chromium without a script error.
//   node kick/tests/mirror_page_test.mjs
import fs from "fs";
import path from "path";
import { fileURLToPath } from "url";

const file = path.join(path.dirname(fileURLToPath(import.meta.url)), "../docs/FM-KICK-mirror.html");
const core = process.env.PLAYWRIGHT_CORE || "/opt/npm-tools/node_modules/playwright-core/index.mjs";
const chromePath = process.env.CHROMIUM || (fs.existsSync("/opt/pw-browsers") ? fs.readdirSync("/opt/pw-browsers").filter((d) => d.startsWith("chromium-"))
  .map((d) => `/opt/pw-browsers/${d}/chrome-linux64/chrome`).find((p) => fs.existsSync(p)) : null);
if (!fs.existsSync(core) || !chromePath) { console.log("SKIP no playwright-core or Chromium"); process.exit(0); }
const { chromium } = await import(core);
let fails = 0;
const check = (ok, msg) => { console.log(`${ok ? "ok  " : "FAIL"} ${msg}`); if (!ok) fails++; };
const browser = await chromium.launch({ executablePath: chromePath });
const page = await browser.newPage({ viewport: { width: 1100, height: 900 } });
const errors = [];
page.on("pageerror", (e) => errors.push(String(e)));
await page.goto("file://" + file);
await page.waitForTimeout(1500);
check((await page.title()).includes("FM-KICK"), "the page is FM-KICK's");
check(errors.length === 0, `no script error (${errors.join("; ")})`);
await page.screenshot({ path: process.env.SHOT || "/tmp/mirror_page.png" });
await browser.close();
console.log(fails ? `${fails} failed` : "all passed");
process.exit(fails ? 1 : 0);
