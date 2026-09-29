/* A headless harness for the WASM port, for the bring-up and for regression checks: it instantiates the module
 * with the same two imports the page gives it, loads the demo's packs and (optionally) the assets, boots, and runs
 * N frames with a fixed input script, printing the core's own console output and a per-frame timing summary.
 *
 *   node tools/wasm/smoke.js [--frames 120] [--data SaberRider/data] [--assets] [--level 1] [--script "60:R,30:RJ"]
 *
 * It draws into the module's framebuffer and checks that the frame is not blank and not one flat colour, which is
 * the two ways a software rasteriser silently fails. With --png it also writes the last frame out, so a rendering
 * regression can be eyeballed. */
import { readFileSync, existsSync, writeFileSync } from 'node:fs';
import { resolve, join, basename } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = resolve(fileURLToPath(new URL('../..', import.meta.url)));

function arg(name, def) {
  const i = process.argv.indexOf(`--${name}`);
  return i >= 0 && i + 1 < process.argv.length ? process.argv[i + 1] : def;
}
const has = (name) => process.argv.includes(`--${name}`);

const FRAMES = +arg('frames', 120);
const DATA = resolve(ROOT, arg('data', 'SaberRider/data'));
const LEVEL = +arg('level', 0);
const SCRIPT = arg('script', '');
const PACKS = ['pack.pck', 'common.pck', 'levels.pck', 'menu.pck', 'level1.pck'];

/* the module's console: the core writes a line per fprintf */
let logLines = 0;
const decoder = new TextDecoder();
let memory = null;
const jsLog = (isErr, ptr, len) => {
  const s = decoder.decode(new Uint8Array(memory.buffer, ptr, len));
  logLines++;
  if (logLines <= 40) console.log(isErr ? '  [game] ' : '  [game] ', s);
};
const jsNowMs = () => performance.now();

const bin = readFileSync(join(ROOT, 'build/wasm/saber_rider.wasm'));
const { instance } = await WebAssembly.instantiate(bin, { env: { js_log: jsLog, js_now_ms: jsNowMs } });
const exp = instance.exports;
memory = exp.memory;

const u8 = () => new Uint8Array(memory.buffer);
const enc = new TextEncoder();
function writeStr(s) {
  const b = enc.encode(s);
  const p = exp.wasm_alloc(b.length + 1);
  const u = u8();
  u.set(b, p);
  u[p + b.length] = 0;
  return p;
}
function addFile(path, bytes) {
  const p = exp.wasm_alloc(bytes.length);
  u8().set(bytes, p);
  const n = writeStr(path);
  exp.wasm_vfs_add(n, p, bytes.length);
}

/* ---- the data ---- */
let total = 0;
for (const name of PACKS) {
  const path = join(DATA, name);
  if (!existsSync(path)) { console.error(`missing ${path}`); process.exit(1); }
  const bytes = new Uint8Array(readFileSync(path));
  addFile(`data/${name}`, bytes);
  total += bytes.length;
}
console.log(`packs: ${(total / 1e6).toFixed(1)} MB`);

/* the assets: only the text ones here (no PNG decoder in node without a canvas), which is what the mode7 and
 * ramrod/space atlas parsers need. A browser run gets the PNGs too; see web/saber-wasm.js. */
/* The assets. The PNGs need a decoder, which node has none of by default: a browser run decodes them with its own
 * (web/saber-wasm.js), so the text and sound files are enough here for a bring-up check - --assets stages the
 * whole set through tools/wasm/build_web.py and the page, which is the real run. */
if (has('assets')) {
  const assetsDir = join(ROOT, 'assets');
  const { readdirSync } = await import('node:fs');
  let n = 0, skipped = 0;
  const walk = (dir) => {
    for (const e of readdirSync(dir, { withFileTypes: true })) {
      const p = join(dir, e.name);
      if (e.isDirectory()) { walk(p); continue; }
      if (e.name.endsWith('.png')) { skipped++; continue; }   // needs the browser's decoder
      addFile(`assets/${p.slice(assetsDir.length + 1)}`, new Uint8Array(readFileSync(p)));
      n++;
    }
  };
  if (existsSync(assetsDir)) {
    walk(assetsDir);
    console.log(`assets: ${n} text/sound files staged, ${skipped} PNGs skipped (no decoder in node)`);
  }
}

/* the debug switches */
for (const line of SCRIPT.split(';').map((s) => s.trim()).filter(Boolean)) {
  const eq = line.indexOf('=');
  if (eq < 0) continue;
  const name = line.slice(0, eq), value = line.slice(eq + 1);
  exp.wasm_env_put(writeStr(name), enc.encode(name).length, writeStr(value), enc.encode(value).length);
  console.log(`env: ${name}=${value}`);
}

/* ---- boot ---- */
if (!exp.wasm_boot(LEVEL, 0)) {
  const p = exp.wasm_error_msg();
  console.error('boot failed:', decoder.decode(new Uint8Array(memory.buffer, p, 128)).replace(/\0.*$/s, ''));
  process.exit(1);
}
console.log(`booted: level ${LEVEL}, ${exp.wasm_frame_w()}x${exp.wasm_frame_h()}`);

/* ---- run ---- */
const BUTTON = { LEFT: 0, RIGHT: 1, UP: 2, DOWN: 3, JUMP: 4, SHOOT: 5, AIM: 6, PAUSE: 7, POWER: 8 };
/* the script is "N:KEYS" steps, the letters app.c's SABER_SCRIPT uses */
const script = SCRIPT.split(',').map((s) => s.trim()).filter(Boolean).map((s) => {
  const m = s.match(/^(\d+):([A-Z]*)$/);
  return m ? { n: +m[1], keys: m[2] } : null;
}).filter(Boolean);
let si = 0, left = script.length ? script[0].n : 0;
function scriptMask() {
  if (!script.length) return 0;
  if (left-- <= 0) { si = (si + 1) % script.length; left = script[si].n - 1; }
  let m = 0;
  for (const c of script[si].keys) {
    const b = { L: 'LEFT', R: 'RIGHT', U: 'UP', D: 'DOWN', J: 'JUMP', S: 'SHOOT', A: 'AIM', P: 'PAUSE', X: 'POWER' }[c];
    if (b) m |= 1 << BUTTON[b];
  }
  return m;
}

const W = exp.wasm_frame_w(), H = exp.wasm_frame_h();
let worst = 0, totalMs = 0, floorTotal = 0, draws = 0;
let clock = 0;
for (let f = 0; f < FRAMES; f++) {
  exp.wasm_input_push(scriptMask(), 0, 0, 0, 0, 0);
  clock += 1000 / 60;
  const t0 = performance.now();
  exp.wasm_frame(1 / 60);
  const ms = performance.now() - t0;
  totalMs += ms;
  if (ms > worst) worst = ms;
  floorTotal += exp.wasm_floor_us();
  draws = Math.max(draws, exp.wasm_prims());
}
console.log(`frames: ${FRAMES}, mean ${(totalMs / FRAMES).toFixed(2)} ms, worst ${worst.toFixed(2)} ms ` +
            `(${(1000 / (totalMs / FRAMES)).toFixed(0)} fps mean), floor ${(floorTotal / FRAMES / 1000).toFixed(2)} ms/frame, ` +
            `max ${draws} draws`);
console.log(`heap: ${(exp.wasm_heap_free() / 1e6).toFixed(1)} MB free, ` +
            `memory ${(exp.wasm_mem_used() / 1e6).toFixed(1)} / ${(exp.wasm_mem_total() / 1e6).toFixed(0)} MB, ` +
            `${exp.wasm_vfs_count()} files`);

/* ---- is anything actually on the screen? ---- */
const fb = new Uint32Array(memory.buffer, exp.wasm_frame_ptr(), W * H);
const seen = new Set();
for (let i = 0; i < fb.length; i += 7) seen.add(fb[i]);
const nonBlack = [...seen].filter((c) => (c & 0xffffff) !== 0).length;
console.log(`framebuffer: ${seen.size} distinct colours (sampled), ${nonBlack} of them not black`);
if (seen.size <= 1) { console.error('FAIL: the frame is a single flat colour'); process.exit(1); }
if (nonBlack === 0) { console.error('FAIL: the frame is entirely black'); process.exit(1); }

/* an ASCII thumbnail, so a run is visible in a terminal without writing a file */
const COLS = 72, ROWS = 26;
const chars = ' .:-=+*#%@';
let art = '';
for (let y = 0; y < ROWS; y++) {
  for (let x = 0; x < COLS; x++) {
    const sx = Math.floor((x / COLS) * W), sy = Math.floor((y / ROWS) * H);
    const c = fb[sy * W + sx];
    const lum = ((c & 0xff) * 0.3 + ((c >> 8 & 0xff) * 0.59) + ((c >> 16 & 0xff) * 0.11)) / 255;
    art += chars[Math.min(9, Math.floor(lum * 10))];
  }
  art += '\n';
}
console.log(art);

/* an optional PPM, so a rendering regression can be looked at properly (no PNG encoder needed) */
if (has('png') || has('ppm')) {
  const out = join(ROOT, 'build/wasm/frame.ppm');
  const buf = Buffer.alloc(W * H * 3);
  for (let i = 0; i < W * H; i++) {
    const c = fb[i];
    buf[i * 3] = c & 0xff;
    buf[i * 3 + 1] = (c >> 8) & 0xff;
    buf[i * 3 + 2] = (c >> 16) & 0xff;
  }
  writeFileSync(out, Buffer.concat([Buffer.from(`P6\n${W} ${H}\n255\n`), buf]));
  console.log(`wrote ${out}`);
}

console.log(logLines ? `game logged ${logLines} lines` : 'game logged nothing');
console.log('OK');
