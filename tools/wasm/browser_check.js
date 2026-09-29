/* Drives the staged page in headless Chromium: loads it, waits for the game to boot, runs some frames with a
 * synthetic key held, and reports what the page logged, what the module reported, and a PNG of the canvas.
 *
 *   node tools/wasm/browser_check.js [--url http://127.0.0.1:8009/] [--seconds 12] [--shot out.png]
 *
 * Chromium is driven over the DevTools protocol directly (no puppeteer, nothing to install): the page is opened
 * with --remote-debugging-port, the console and the exceptions are collected, and the canvas is read back as a
 * data URL. The checks are the ones a WASM port can fail in ways the headless harness cannot see: a rejected
 * promise, an exception in a frame, a blank canvas, or a module that never boots. */
import { spawn, execSync } from 'node:child_process';
import { writeFileSync, mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const arg = (n, d) => { const i = process.argv.indexOf(`--${n}`); return i >= 0 && i + 1 < process.argv.length ? process.argv[i + 1] : d; };
const URL_ = arg('url', 'http://127.0.0.1:8009/');
const SECONDS = +arg('seconds', 12);
const LEVEL = +arg('level', 0);
const SHOT = arg('shot', '/tmp/saber-wasm.png');
const PORT = +arg('port', 9222);

const CHROME = ['chromium', 'chromium-browser', 'google-chrome', 'chrome'].find((c) => {
  try { execSync(`command -v ${c}`, { stdio: 'ignore' }); return true; } catch { return false; }
});
if (!CHROME) { console.error('no chromium found'); process.exit(2); }

const profile = mkdtempSync(join(tmpdir(), 'saber-chrome-'));
const chrome = spawn(CHROME, [
  '--headless=new', `--remote-debugging-port=${PORT}`, `--user-data-dir=${profile}`,
  '--no-sandbox', '--disable-gpu', '--no-first-run', '--disable-dev-shm-usage',
  '--window-size=1280,900', '--autoplay-policy=no-user-gesture-required',
  'about:blank',
], { stdio: ['ignore', 'ignore', 'pipe'] });
let chromeErr = '';
chrome.stderr.on('data', (d) => { chromeErr += d; });

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function devtoolsUrl() {
  for (let i = 0; i < 60; i++) {
    try {
      const res = await fetch(`http://127.0.0.1:${PORT}/json/version`);
      const j = await res.json();
      if (j.webSocketDebuggerUrl) return j.webSocketDebuggerUrl;
    } catch { /* not up yet */ }
    await sleep(250);
  }
  throw new Error(`chromium's devtools never came up\n${chromeErr.slice(0, 800)}`);
}

const wsUrl = await devtoolsUrl();
const { WebSocket } = await import('node:worker_threads').then(() => ({ WebSocket: globalThis.WebSocket }));
const ws = new WebSocket(wsUrl);
await new Promise((res, rej) => { ws.onopen = res; ws.onerror = () => rej(new Error('devtools socket failed')); });

let msgId = 0;
const pending = new Map();
const events = [];
ws.onmessage = (ev) => {
  const m = JSON.parse(ev.data);
  if (m.id !== undefined) {
    const p = pending.get(m.id);
    if (p) { pending.delete(m.id); m.error ? p.rej(new Error(JSON.stringify(m.error))) : p.res(m.result); }
  } else events.push(m);
};
const send = (method, params = {}, sessionId) => new Promise((res, rej) => {
  const id = ++msgId;
  pending.set(id, { res, rej });
  ws.send(JSON.stringify({ id, method, params, sessionId }));
});

/* attach to a fresh tab */
const { targetId } = await send('Target.createTarget', { url: 'about:blank' });
const { sessionId } = await send('Target.attachToTarget', { targetId, flatten: true });
const S = (m, p) => send(m, p, sessionId);

await S('Runtime.enable');
await S('Log.enable');
await S('Page.enable');
await S('Network.enable');

const logs = [];
ws.addEventListener('message', (ev) => {
  const m = JSON.parse(ev.data);
  if (m.method === 'Runtime.consoleAPICalled') {
    const text = (m.params.args || []).map((a) => a.value ?? a.description ?? a.unserializableValue ?? '').join(' ');
    logs.push({ level: m.params.type, text });
  } else if (m.method === 'Runtime.exceptionThrown') {
    const d = m.params.exceptionDetails;
    logs.push({ level: 'exception', text: d.exception?.description || d.text });
  } else if (m.method === 'Log.entryAdded') {
    const e = m.params.entry;
    logs.push({ level: e.level, text: e.text, url: e.url, line: e.lineNumber });
  } else if (m.method === 'Network.responseReceived' && m.params.response.status >= 400) {
    logs.push({ level: 'http', text: `${m.params.response.status} ${m.params.response.url}` });
  } else if (m.method === 'Network.loadingFailed') {
    logs.push({ level: 'http', text: `failed ${m.params.errorText} ${m.params.type}` });
  }
});

const evaluate = async (expr, awaitPromise = false) => {
  const r = await S('Runtime.evaluate', { expression: expr, returnByValue: true, awaitPromise });
  if (r.exceptionDetails) throw new Error(r.exceptionDetails.exception?.description || r.exceptionDetails.text);
  return r.result.value;
};

console.log(`opening ${URL_}${LEVEL ? `?level=${LEVEL}` : ''}`);
await S('Page.navigate', { url: URL_ });

/* wait for the module to boot: the page sets a flag once wasm_boot has returned */
let booted = false;
for (let i = 0; i < SECONDS * 4; i++) {
  await sleep(250);
  try {
    booted = await evaluate(`!!(window.__saber && window.__saber.booted)`);
    const err = await evaluate(`(window.__saber && window.__saber.error) || ''`);
    if (err) { console.error('page reported:', err); }
    if (booted) break;
  } catch { /* the page may still be parsing */ }
}

const state = await evaluate(`JSON.stringify(window.__saber ? window.__saber.stats() : {})`).catch(() => '{}');
console.log('booted:', booted);
console.log('state:', state);

if (!booted) {
  const load = await evaluate(`(document.getElementById('loadStatus')||{}).textContent || ''`).catch(() => '');
  const err = await evaluate(`(document.getElementById('errorText')||{}).textContent || ''`).catch(() => '');
  console.error('did not boot. loading said:', JSON.stringify(load));
  if (err) console.error('page error:', JSON.stringify(err));
  for (const l of logs.slice(-25)) console.log(`  [${l.level}] ${l.text}`);
  ws.close();
  chrome.kill();
  process.exit(1);
}

/* Play for a while: run right, jump, shoot, so a level is really animating rather than sitting on a menu. */
/* Get into a level. The menu is splash -> main (which ignores input until its zoom-in finishes) -> briefing ->
 * character select -> level, and it reads every one of those as "confirm", so keep pressing until the module
 * says it is in a level (bit 16 of wasm_state) or we give up. Bound confirm keys only: PAUSE/Enter, JUMP KeyW,
 * SHOOT KeyS, POWER KeyX. KeyJ is not bound to anything and does nothing. */
const inLevel = async () => JSON.parse(await evaluate(`JSON.stringify(window.__saber.stats())`)).state >> 16;
const tap = async (code, holdMs = 90) => {
  await evaluate(`window.__saber.press(${JSON.stringify(code)}, true)`);
  await sleep(holdMs);
  await evaluate(`window.__saber.press(${JSON.stringify(code)}, false)`);
};
let reachedLevel = false;
for (let i = 0; i < 40 && !reachedLevel; i++) {
  await tap('Enter');
  await sleep(400);
  if (await inLevel()) { reachedLevel = true; break; }
  if (i % 2 === 1) { await tap('KeyS'); await sleep(400); if (await inLevel()) { reachedLevel = true; break; } }
}
console.log(reachedLevel ? 'reached a level' : 'never left the menu');

const PLAY_MS = +arg('play', 8000);
const script = [
  ['ArrowRight', true], ['KeyS', true], ['KeyW', true],
];
for (const [code, down] of script) await evaluate(`window.__saber.press(${JSON.stringify(code)}, ${down})`);
await sleep(PLAY_MS * 0.45);
for (const code of ['KeyS', 'KeyW', 'KeyA', 'KeyD']) await evaluate(`window.__saber.press(${JSON.stringify(code)}, true)`);
await sleep(PLAY_MS * 0.3);
for (const code of ['KeyS', 'KeyW', 'KeyA', 'KeyD']) await evaluate(`window.__saber.press(${JSON.stringify(code)}, false)`);
await sleep(PLAY_MS * 0.25);
for (const [code, down] of script) await evaluate(`window.__saber.press(${JSON.stringify(code)}, ${!down})`);
await sleep(1000);

const after = JSON.parse(await evaluate(`JSON.stringify(window.__saber.stats())`));
console.log('after 7.5 s of play:', after);

const shot = await evaluate(`document.getElementById('screen').toDataURL('image/png')`);
if (shot && shot.startsWith('data:image/png;base64,')) {
  writeFileSync(SHOT, Buffer.from(shot.slice('data:image/png;base64,'.length), 'base64'));
  console.log(`wrote ${SHOT}`);
}

const warnings = logs.filter((l) => l.level === 'exception' || l.level === 'error');
console.log(`console: ${logs.length} lines, ${warnings.length} errors/exceptions`);
for (const l of warnings.slice(0, 10)) console.log(`  [${l.level}] ${String(l.text).slice(0, 300)}`);

const failed = !booted || warnings.length > 0 || after.fps === 0 || after.distinctColours <= 1;
ws.close();
chrome.kill();
process.exit(failed ? 1 : 0);
