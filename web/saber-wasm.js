/* Saber Rider and the Star Sheriffs — the WebAssembly port's page.
 *
 * What this does, in order:
 *   1. reads manifest.json (staged by tools/wasm/build_web.py) for the file list and sizes;
 *   2. instantiates saber_rider.wasm with two imports: js_now_ms (performance.now) and js_log (the console);
 *   3. fetches the demo's .pck packs and our assets/ files, copies each into the module's memory and hands it
 *      over (wasm_vfs_add). The PNGs are decoded here, by the browser, into the RGBA the module draws from
 *      (wasm_vfs_add_image) - so there is no inflate in the module, and the PNG bytes never enter it;
 *   4. calls wasm_boot once, then drives wasm_frame from requestAnimationFrame with the elapsed time;
 *   5. blits the framebuffer: the module renders into a 426x240 RGBA buffer in its linear memory, this makes a
 *      Uint8ClampedArray view on it and putImageData's it to a canvas of the same size. The canvas is CSS-scaled,
 *      so the frame cost does not depend on the window. A memory.grow detaches the view, so it is re-made
 *      whenever wasm_mem_total changes;
 *   6. mixes the audio: the module writes S16 stereo into a ring, this copies out whatever is ready and posts it
 *      to an AudioWorklet (saber-audio-worklet.js);
 *   7. owns the input: the sidebar's controls panel is where the bindings live (the game hides its own
 *      OPTIONS > CONTROLS for this port), and the keyboard, the gamepad and the touch overlay are resolved into
 *      the game's nine buttons plus the two shoulders and pushed to the module each frame.
 *
 * Nothing here interprets anything: the game is the C in the module, and this file is the window around it. */

const BTN = { LEFT: 0, RIGHT: 1, UP: 2, DOWN: 3, JUMP: 4, SHOOT: 5, AIM: 6, PAUSE: 7, POWER: 8 };
const BTN_NAME = ['LEFT', 'RIGHT', 'UP', 'DOWN', 'JUMP', 'SHOOT', 'AIM', 'PAUSE', 'POWER'];
const DBG_KEY = { F1: 0, F2: 1, LEFT: 2, RIGHT: 3, FAST: 4 };
const DBG_CODES = { F1: DBG_KEY.F1, F2: DBG_KEY.F2, ArrowLeft: DBG_KEY.LEFT, ArrowRight: DBG_KEY.RIGHT, ShiftLeft: DBG_KEY.FAST, ShiftRight: DBG_KEY.FAST };

// which build this is, from the file build_web.py writes next to the page (web/build-flags.js holds the
// developer build's). On a redistributable build the page offers a player what a player needs and nothing else:
// a top-right button opening the controls menu, and no level select, no debug switches, no log console and no
// scripting hook. The matching module is built without the SABER_* environment and debug-key exports, so what
// is removed is removed from the binary as well and not just hidden here.
const BUILD = Object.assign({ redist: false, label: 'developer build' }, window.SABER_BUILD || {});
const REDIST = !!BUILD.redist;

// the pad's two shoulders. They are not buttons (Input.shoulder in input.h, not a BTN_*), and stage 6 phase 1
// strafes on them alone - ramrod.c player_control(): the triggers are AIM as well, so AIM on its own walks and
// AIM with L or R held slides sideways. A pad gets them from the triggers; a keyboard needs keys of its own, and
// the touch overlay's L / R buttons drive the same two, so all three feed one pair of bits.
const SHOULDER_NAME = ['STRAFE_L', 'STRAFE_R'];
const ALL_BIND_NAMES = BTN_NAME.concat(SHOULDER_NAME);
// the touch overlay's own bits: the shoulder buttons sit above the nine, and the module reads them as the
// shoulders rather than as buttons, so they are masked back out of the button word
const TOUCH_SHOULDER_L = 1 << 16, TOUCH_SHOULDER_R = 1 << 17;
const BTN_MASK = (1 << BTN_NAME.length) - 1;

const CONFIG_KEY = 'saber.wasm.config.v1';

// the demo's keyboard layout, the same one the PC build starts with (input_sdl.c's defaults)
const DEFAULT_KEYS = {
  LEFT: ['ArrowLeft'], RIGHT: ['ArrowRight'], UP: ['ArrowUp'], DOWN: ['ArrowDown'],
  JUMP: ['KeyW', 'KeyA'], SHOOT: ['KeyS', 'KeyD'], AIM: ['KeyQ', 'KeyE'],
  PAUSE: ['Enter'], POWER: ['KeyX', 'KeyF'],
  STRAFE_L: ['KeyZ'], STRAFE_R: ['KeyC'],
};
// and its pad layout: the d-pad, south jump, east/west shoot, shoulders aim, start pause, north power
const DEFAULT_PAD = {
  LEFT: [12], RIGHT: [13], UP: [14], DOWN: [15],
  JUMP: [0], SHOOT: [2, 1], AIM: [6, 7], PAUSE: [9], POWER: [3],
};
const PAD_NAMES = ['A', 'B', 'X', 'Y', 'LB', 'RB', 'LT', 'RT', 'BACK', 'START', 'L3', 'R3', 'UP', 'DOWN', 'LEFT', 'RIGHT', 'GUIDE'];

/* the stages, for the level select. The names are the ones the game prints on its title card - game.c's TITLE
 * table, which title_idx() picks with the stage number - so keep the two in step. 0 is the front end: the module
 * boots into the menu rather than a level (game_init: `if (start_level)`, so 0 is the only "no level"). */
const LEVELS = [
  { id: 0, label: 'Front end (title screen)' },
  { id: 1, label: 'Stage 1 — The Frontier Town' },
  { id: 2, label: 'Stage 2 — The All Galaxy Grand Prix' },
  { id: 3, label: 'Stage 3 — Hyperjumper Pass' },
  { id: 4, label: 'Stage 4 — The Red Palm Jungle' },
  { id: 5, label: 'Stage 5 — The Cavern Laboratory' },
  { id: 6, label: 'Stage 6 — Power Stride' },
  { id: 7, label: 'Stage 6, final phase — The Battle Cruiser' },
];
const MAX_LEVEL = LEVELS[LEVELS.length - 1].id;

/* the elements */
const $ = (id) => document.getElementById(id);
const el = {
  stage: $('stage'), wrap: $('screenWrap'), canvas: $('screen'),
  overlay: $('overlay'), loadingCard: $('loadingCard'), pausedCard: $('pausedCard'), errorCard: $('errorCard'),
  loadBar: $('loadBar'), loadStatus: $('loadStatus'), loadHint: $('loadHint'),
  errorText: $('errorText'), errorLog: $('errorLog'),
  panel: $('panel'), panelBtn: $('panelBtn'), panelClose: $('panelClose'), fullscreenBtn: $('fullscreenBtn'),
  pauseBtn: $('pauseBtn'), resetBtn: $('resetBtn'), resumeBtn: $('resumeBtn'),
  levelSelect: $('levelSelect'), levelGo: $('levelGo'),
  scaleMode: $('scaleMode'), screenMode: $('screenMode'), pixelPerfect: $('pixelPerfect'),
  volume: $('volume'), audioState: $('audioState'),
  bindings: $('bindings'), resetKeys: $('resetKeys'), resetPad: $('resetPad'),
  sens: $('sens'), padState: $('padState'),
  touch: $('touch'), touchDpad: $('touchDpad'), dpadKnob: $('dpadKnob'),
  touchEnabled: $('touchEnabled'), touchOpacity: $('touchOpacity'), touchSize: $('touchSize'), touchLandscape: $('touchLandscape'),
  statFps: $('statFps'), statFrame: $('statFrame'), statDraws: $('statDraws'), statFloor: $('statFloor'),
  statAudio: $('statAudio'), statHeap: $('statHeap'), statMem: $('statMem'), statPad: $('statPad'),
  showFps: $('showFps'), devText: $('devText'), devApply: $('devApply'), log: $('log'),
};

/* the state */
let wasm = null, exp = null, memory = null;
let view = null, viewBuffer = null, viewMemTotal = -1;   // the Uint8ClampedArray over the framebuffer
let manifest = null;
let booted = false, paused = false, running = false;
let lastT = 0, frameMs = 16.7, fps = 0, fpsFrames = 0, fpsSince = 0;
let drawCount = 0, floorUs = 0, audioQueued = 0, audioUnderruns = 0, audioPeak = 0, audioStarted = false;
let config = loadConfig();
let listening = null;      // { dev, btn } while a binding cell is waiting for a press
let touchState = 0;        // a bitmask of the touch buttons held
let touchDirs = { up: false, down: false, left: false, right: false };
let audioCtx = null, worklet = null;
let audioDeviceRate = 44100;   /* the worklet runs at the device's rate; the module mixes at 44100 */
let rsNext = 0;                /* source-time (module frames) of the next resampled output sample */
let rsTailL = 0, rsTailR = 0;  /* the last source frame of the previous chunk, for interpolation across it */
let lastInput = { mask: 0, l: 0, r: 0, ax: 0, ay: 0 };   /* what pushInput last handed the module, for the harness */

/* ------------------------------------------------------------------ config */
function loadConfig() {
  const base = {
    keys: structuredClone(DEFAULT_KEYS),
    pad: structuredClone(DEFAULT_PAD),
    level: 0,               // which stage the page boots into, and what "Soft reset" returns to
    scaleMode: 'fit',
    screenMode: 0,
    pixelPerfect: true,
    volume: 0.8,
    sens: 5,
    touchEnabled: null,       // null: decide from the device
    touchOpacity: 70,
    touchSize: 100,
    touchLandscape: false,
    showFps: false,
    dev: '',
  };
  try {
    const raw = localStorage.getItem(CONFIG_KEY);
    if (raw) {
      const saved = JSON.parse(raw);
      Object.assign(base, saved);
      // per control, not wholesale: a config saved before the strafe keys existed still gets them, and a control
      // the player cleared stays cleared (an empty list is a choice, a missing one is not)
      base.keys = mergeBindings(DEFAULT_KEYS, saved.keys);
      base.pad = mergeBindings(DEFAULT_PAD, saved.pad);
    }
  } catch (e) { /* a private-mode browser: the defaults are fine */ }
  dedupeBindings(base.keys, ALL_BIND_NAMES);
  dedupeBindings(base.pad, BTN_NAME);
  return base;
}
function mergeBindings(defs, saved) {
  const out = {};
  for (const name of new Set([...Object.keys(defs), ...Object.keys(saved || {})])) {
    const have = saved && Array.isArray(saved[name]) ? saved[name] : null;
    out[name] = have ? have.slice(0, 2) : (defs[name] ? defs[name].slice() : []);
  }
  return out;
}
/* one key (one pad button) belongs to one control at a time, so a config that was hand-edited or written by an
 * older build cannot leave the same key on two rows and have both of them fire */
function dedupeBindings(table, names) {
  const seen = new Set();
  for (const name of names) {
    const list = (table[name] || []).filter((v) => v != null && !seen.has(v));
    for (const v of list) seen.add(v);
    table[name] = list.slice(0, 2);
  }
}
function saveConfig() {
  try { localStorage.setItem(CONFIG_KEY, JSON.stringify(config)); } catch (e) { /* ignore */ }
}

/* ------------------------------------------------------------------ logging
 * The panel's log panel is a console, and a redist build has no console: there the element is not in the page and
 * the module's own output goes to the browser's devtools as usual. The error card is not a console - it only
 * ever appears when the game could not start - so it is written either way. */
function log(msg) {
  if (el.log) {
    const line = `${new Date().toLocaleTimeString()}  ${msg}`;
    el.log.textContent = (el.log.textContent + '\n' + line).split('\n').slice(-40).join('\n');
    el.log.scrollTop = el.log.scrollHeight;
  }
  if (!el.errorCard.classList.contains('hidden') && el.errorCard.offsetParent) el.errorLog.textContent += msg + '\n';
}

/* ------------------------------------------------------------------ the module */
const jsLog = (isError, ptr, len) => {
  const s = decoder.decode(new Uint8Array(memory.buffer, ptr, len));
  if (isError) console.warn('[saber]', s); else console.log('[saber]', s);
  if (el.log && el.log.textContent.length < 4000) log(s);
};
const jsNowMs = () => performance.now();

async function loadModule() {
  // the manifest first: it carries the module's version, so the wasm fetch below can never serve a stale
  // cached build (which would sound like time-scrambled garbage, not silence)
  let version = '';
  try {
    const mres = await fetch('manifest.json');
    if (mres.ok) {
      const m = await mres.json();
      manifest = m;
      if (m.wasmVersion) version = `?v=${m.wasmVersion}`;
    }
  } catch (e) { /* the data loader reports a missing manifest for us */ }
  setLoad('fetching the module', 0);
  const res = await fetch(`saber_rider.wasm${version}`);
  if (!res.ok) throw new Error(`saber_rider.wasm: ${res.status}`);
  const bin = await res.arrayBuffer();
  const { instance } = await WebAssembly.instantiate(bin, {
    env: { js_log: jsLog, js_now_ms: jsNowMs },
  });
  wasm = instance;
  exp = instance.exports;
  memory = exp.memory;
  viewMemTotal = -1;
  log(`module: ${(bin.byteLength / 1024).toFixed(0)} KB, ${Object.keys(exp).length - 1} exports`);
  try {
    const idPtr = exp.wasm_build_id();
    const raw = new Uint8Array(memory.buffer, idPtr, 64);
    let n = 0;
    while (n < raw.length && raw[n]) n++;
    const id = new TextDecoder().decode(raw.subarray(0, n));
    log(`module build: ${id}`);
    if (window.__saber) window.__saber.build = id;
  } catch (e) { /* an older module without the stamp: nothing to report */ }
}

/* a Uint8Array over the module's memory, for handing it bytes */
function u8() { return new Uint8Array(memory.buffer); }
function refreshView() {
  const total = exp.wasm_mem_total();
  if (total !== viewMemTotal || !view || view.buffer !== memory.buffer) {
    viewMemTotal = total;
    view = new Uint8ClampedArray(memory.buffer, exp.wasm_frame_ptr(), exp.wasm_frame_w() * exp.wasm_frame_h() * 4);
  }
  return view;
}
const ctx2d = el.canvas.getContext('2d', { alpha: false, desynchronized: true });
let imageData = null, imageDataView = null;

/* The framebuffer lives in the module's linear memory, and a memory.grow detaches every view over the old
 * buffer - so the Uint8ClampedArray behind the ImageData has to be re-made when the memory grows, not only the
 * view the module's pointer is read through. refreshView() returns null when the buffer changed, and the
 * ImageData is rebuilt then. */
function present() {
  const px = refreshView();
  const w = exp.wasm_frame_w(), h = exp.wasm_frame_h();
  if (!imageData || imageData.width !== w || imageData.height !== h || !isLive(imageData.data)) {
    imageData = new ImageData(new Uint8ClampedArray(memory.buffer, exp.wasm_frame_ptr(), w * h * 4), w, h);
    el.canvas.width = w;
    el.canvas.height = h;
  } else {
    imageData.data.set(px);
  }
  ctx2d.putImageData(imageData, 0, 0);
  if (config.showFps) drawFps();
}
/* an ArrayBuffer is detached when byteLength reads 0, and wasm's does not report that, so the view is compared
 * against the current buffer instead */
function isLive(arr) { try { return arr.length !== 0 && arr.buffer === memory.buffer; } catch { return false; } }

function drawFps() {
  const w = exp.wasm_frame_w();
  ctx2d.font = '10px monospace';
  ctx2d.fillStyle = 'rgba(0,0,0,0.55)';
  ctx2d.fillRect(2, 2, 66, 13);
  ctx2d.fillStyle = '#7fe';
  ctx2d.fillText(`${fps.toFixed(0)} fps  ${frameMs.toFixed(1)}ms`, 5, 12);
}

/* ------------------------------------------------------------------ the data */
function setLoad(text, frac, hint) {
  el.loadStatus.textContent = text;
  el.loadBar.style.width = `${Math.round(frac * 100)}%`;
  if (hint !== undefined) el.loadHint.textContent = hint;
}

async function loadData() {
  setLoad('reading the file list', 0.01);
  if (!manifest) {
    const res = await fetch('manifest.json');
    if (!res.ok) throw new Error(`manifest.json: ${res.status} (run tools/wasm/build_web.py)`);
    manifest = await res.json();
  }
  if (manifest.missingPacks && manifest.missingPacks.length) {
    throw new Error(`the manifest is missing ${manifest.missingPacks.join(', ')}; stage them with --data`);
  }

  // PNGs go through the browser's decoder; everything else is handed over as it is
  const off = document.createElement('canvas');
  off.width = off.height = 1;
  const offCtx = off.getContext('2d', { willReadFrequently: true });

  const files = manifest.files;
  let done = 0;
  const totalBytes = manifest.totalBytes || files.reduce((a, f) => a + f.size, 0);
  let doneBytes = 0;
  const CONCURRENCY = 6;
  let next = 0;

  const worker = async () => {
    for (;;) {
      const i = next++;
      if (i >= files.length) return;
      const f = files[i];
      setLoad(`loading ${f.path}`, doneBytes / totalBytes);
      const buf = await (await fetch(f.path)).arrayBuffer();
      if (f.kind === 'image') {
        const bmp = await createImageBitmap(new Blob([buf], { type: 'image/png' }));
        off.width = bmp.width;
        off.height = bmp.height;
        offCtx.clearRect(0, 0, bmp.width, bmp.height);
        offCtx.drawImage(bmp, 0, 0);
        const img = offCtx.getImageData(0, 0, bmp.width, bmp.height);
        bmp.close?.();
        // ImageData is RGBA in exactly the order render.h's textures are, so the bytes go over unchanged
        const ptr = alloc(img.data.byteLength);
        u8().set(new Uint8Array(img.data.buffer), ptr);
        const namePtr = writeString(f.path);
        exp.wasm_vfs_add_image(namePtr, img.width, img.height, ptr);
      } else {
        const ptr = alloc(buf.byteLength);
        u8().set(new Uint8Array(buf), ptr);
        const namePtr = writeString(f.path);
        exp.wasm_vfs_add(namePtr, ptr, buf.byteLength);
      }
      doneBytes += f.size;
      done++;
      setLoad(`loaded ${done} of ${files.length}`, doneBytes / totalBytes);
    }
  };
  await Promise.all(Array.from({ length: CONCURRENCY }, worker));
  setLoad(`ready: ${done} files, ${(totalBytes / 1e6).toFixed(1)} MB`, 1);
  log(`data: ${done} files, ${(exp.wasm_vfs_bytes() / 1e6).toFixed(1)} MB in the module's memory`);
}

/* Copying bytes into the module's memory. The module has no allocator export, so the page asks the module to
 * malloc the block it is about to fill: wasm_alloc is a thin export over the same heap, and the module takes
 * ownership of the block when it registers the file (wasm_vfs_add), so nothing has to be freed here. */
function alloc(bytes) {
  return exp.wasm_alloc(bytes);
}
function writeString(s) {
  const b = enc.encode(s);
  const ptr = alloc(b.length + 1);
  const u = u8();
  u.set(b, ptr);
  u[ptr + b.length] = 0;
  return ptr;
}

/* ------------------------------------------------------------------ audio */
async function ensureAudio() {
  if (audioCtx) return;
  const AC = window.AudioContext || window.webkitAudioContext;
  if (!AC) { el.audioState.textContent = 'Web Audio is not available in this browser.'; return; }
  audioCtx = new AC({ latencyHint: 'interactive' });
  const sr = Math.round(audioCtx.sampleRate || 44100);
  audioDeviceRate = sr;
  rsNext = 0;
  if (audioCtx.audioWorklet) {
    await audioCtx.audioWorklet.addModule('saber-audio-worklet.js');
    worklet = new AudioWorkletNode(audioCtx, 'saber-audio-processor', {
      numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [2],
    });
    worklet.port.onmessage = (ev) => {
      const m = ev.data || {};
      if (m.type === 'stat') {
        audioQueued = m.queued | 0;
        audioUnderruns += m.underruns | 0;
        audioStarted = !!m.started;
        if (typeof m.peak === 'number') audioPeak = Math.max(audioPeak, m.peak);
      }
    };
    worklet.port.postMessage({ type: 'config', preroll: Math.floor(sr * 0.10), lowWater: Math.floor(sr * 0.04), maxQueue: Math.floor(sr * 0.40) });
    worklet.connect(audioCtx.destination);
  } else {
    el.audioState.textContent = 'AudioWorklet is not available; the game will be silent.';
  }
  exp.wasm_audio_set_volume(config.volume);
  if (audioCtx.state !== 'running') await audioCtx.resume();
  el.audioState.textContent = `running at ${sr} Hz`;
}

function pushAudio() {
  if (!worklet) return;
  let frames = exp.wasm_audio_ready();
  if (frames <= 0) return;
  // the unread run starts at the module's read index and may wrap around the ring's end, so it is copied in
  // one or two contiguous pieces (read pointer + contiguous count) and assembled here before the resample below
  const copy = new Int16Array(frames * 2);
  let off = 0;
  while (frames > 0) {
    const ptr = exp.wasm_audio_get_ptr();
    const n = Math.min(frames, exp.wasm_audio_contig());
    if (n <= 0) break;
    copy.set(new Int16Array(memory.buffer, ptr, n * 2), off);
    off += n * 2;
    exp.wasm_audio_take(n);
    frames -= n;
  }
  const got = off >> 1;
  if (!got) return;
  // the module mixes at 44100 Hz and the worklet drains at the device's rate: resample the chunk when they
  // differ (a browser commonly runs at 48000), with linear interpolation kept continuous across chunks, so a
  // 44100 Hz device gets the samples unchanged and any other rate neither drifts nor clicks.
  if (audioDeviceRate === 44100) {
    worklet.port.postMessage({ type: 'audio', data: copy.subarray(0, off), frames: got }, [copy.buffer]);
    return;
  }
  const step = 44100 / audioDeviceRate;
  const n = got;
  const getL = (i) => (i < 0 ? rsTailL : copy[i * 2]);
  const getR = (i) => (i < 0 ? rsTailR : copy[i * 2 + 1]);
  const out = [];
  let t = rsNext;
  for (;;) {
    const i = Math.floor(t), f = t - i;
    if (i + 1 > n - 1) break;
    if (i >= -1) {
      const a0 = getL(i), a1 = getL(i + 1), b0 = getR(i), b1 = getR(i + 1);
      out.push(Math.round(a0 + (a1 - a0) * f), Math.round(b0 + (b1 - b0) * f));
    }
    t += step;
  }
  rsNext = t - n;
  rsTailL = copy[(n - 1) * 2];
  rsTailR = copy[(n - 1) * 2 + 1];
  if (!out.length) return;
  const data = new Int16Array(out);
  worklet.port.postMessage({ type: 'audio', data, frames: data.length >> 1 }, [data.buffer]);
}

/* ------------------------------------------------------------------ input */
const keyHeld = new Set();
let padIndex = -1;

function keyBindingsFor(btn) { return config.keys[btn] || []; }
function padBindingsFor(btn) { return config.pad[btn] || []; }

function keyboardMask() {
  let m = 0;
  for (let b = 0; b < BTN_NAME.length; b++) {
    for (const code of keyBindingsFor(BTN_NAME[b])) if (keyHeld.has(code)) { m |= 1 << b; break; }
  }
  return m;
}

function keyHeldFor(name) {
  for (const code of keyBindingsFor(name)) if (keyHeld.has(code)) return true;
  return false;
}

function gamepadState() {
  const pads = navigator.getGamepads ? navigator.getGamepads() : [];
  let pad = null;
  for (const p of pads) if (p && p.connected) { pad = p; break; }
  if (!pad) { padIndex = -1; return { mask: 0, l: false, r: false, ax: 0, ay: 0, active: false }; }
  if (pad.index !== padIndex) { padIndex = pad.index; el.padState.textContent = pad.id; if (el.statPad) el.statPad.textContent = pad.id.slice(0, 22); }
  let mask = 0;
  for (let b = 0; b < BTN_NAME.length; b++) {
    for (const idx of padBindingsFor(BTN_NAME[b])) {
      const btn = pad.buttons[idx];
      if (btn && (btn.pressed || btn.value > 0.5)) { mask |= 1 << b; break; }
    }
  }
  const trig = (i) => (pad.buttons[i] ? (pad.buttons[i].value > 0.4 || pad.buttons[i].pressed) : false);
  const l = trig(6), r = trig(7);
  // the left stick, in the -32768..32768 range the module's input_stick expects
  const ax = Math.round((pad.axes[0] || 0) * 32767);
  const ay = Math.round((pad.axes[1] || 0) * 32767);
  const active = pad.axes.length >= 2 && (ax || ay);
  return { mask, l, r, ax, ay, active: !!active };
}

function touchMask() {
  let m = touchState & BTN_MASK;
  if (touchDirs.up) m |= 1 << BTN.UP;
  if (touchDirs.down) m |= 1 << BTN.DOWN;
  if (touchDirs.left) m |= 1 << BTN.LEFT;
  if (touchDirs.right) m |= 1 << BTN.RIGHT;
  return m;
}

function pushInput() {
  const keys = keyboardMask();
  const pad = gamepadState();
  const touch = touchMask();
  const mask = keys | pad.mask | touch;
  // the two shoulders, from all three sources: the pad's triggers, the touch overlay's L / R, and the strafe keys
  const l = pad.l || !!(touchState & TOUCH_SHOULDER_L) || keyHeldFor(SHOULDER_NAME[0]);
  const r = pad.r || !!(touchState & TOUCH_SHOULDER_R) || keyHeldFor(SHOULDER_NAME[1]);
  // the pause key on a pad and the touch overlay both land in PAUSE, so a touch button can pause too
  lastInput = { mask, l: l ? 1 : 0, r: r ? 1 : 0, ax: pad.ax, ay: pad.ay };
  exp.wasm_input_push(mask, l ? 1 : 0, r ? 1 : 0, pad.ax, pad.ay, pad.active ? 1 : 0);
}

/* ---- keyboard ---- */
function keyLabel(code) {
  if (!code) return '-';
  if (code.startsWith('Key')) return code.slice(3);
  if (code.startsWith('Digit')) return code.slice(5);
  if (code.startsWith('Arrow')) return { Up: '↑', Down: '↓', Left: '←', Right: '→' }[code.slice(5)] || code;
  return { Enter: 'ENTER', Space: 'SPACE', ShiftLeft: 'L-SHIFT', ShiftRight: 'R-SHIFT', ControlLeft: 'L-CTRL', Escape: 'ESC', Tab: 'TAB', Backspace: 'BKSP', Delete: 'DEL' }[code] || code.toUpperCase();
}
function padLabel(i) { return PAD_NAMES[i] !== undefined ? PAD_NAMES[i] : `#${i}`; }

function onKeyDown(e) {
  if (listening) {
    e.preventDefault();
    if (e.code === 'Escape') { listening = null; renderBindings(); return; }
    if (e.code === 'Delete' || e.code === 'Backspace') {
      (listening.dev === 'key' ? config.keys : config.pad)[listening.btn] = [];
      listening = null; saveConfig(); renderBindings(); return;
    }
    assignBinding(listening.dev, listening.btn, e.code);
    listening = null; saveConfig(); renderBindings();
    return;
  }
  // the debug keys (game.h's DBG_KEY_*): the collision overlay, the free camera, the camera arrows and the
  // fast-forward. The PC build feeds these and the game input from the same key events, so this does both too:
  // arrows bound to LEFT/RIGHT still move the character (they used to return here and never reach the game,
  // which is why remapping to arrows changed nothing). A redist build has no debug keys at all, and its
  // module is built without wasm_input_key, so there is nothing to push them to.
  const dbg = REDIST ? undefined : DBG_CODES[e.code];
  if (dbg !== undefined) exp.wasm_input_key(dbg, 1);
  if (e.code === 'KeyP' && !e.repeat) { togglePause(); e.preventDefault(); return; }
  if (e.code === 'F11' || (e.code === 'Enter' && e.altKey)) { toggleFullscreen(); e.preventDefault(); return; }
  if (isBound(e.code)) { keyHeld.add(e.code); e.preventDefault(); return; }
  if (dbg !== undefined) e.preventDefault();
}
function onKeyUp(e) {
  const dbg = REDIST ? undefined : DBG_CODES[e.code];
  if (dbg !== undefined) exp.wasm_input_key(dbg, 0);
  keyHeld.delete(e.code);
}
function isBound(code) {
  for (const b of ALL_BIND_NAMES) for (const c of keyBindingsFor(b)) if (c === code) return true;
  return false;
}
/* a key or a pad button sits on one control at a time: binding it here takes it off every other row, so a
 * control can never be triggered by a key another control also owns. The PC build swaps the displaced code into
 * the slot it came from (input_sdl.c assign()); with a list per control there is no slot to put it in, so it is
 * simply released - the row shows as unbound rather than lying about what it does. */
function assignBinding(dev, btn, value) {
  const table = dev === 'key' ? config.keys : config.pad;
  for (const name of Object.keys(table)) {
    if (name === btn || !Array.isArray(table[name])) continue;
    const at = table[name].indexOf(value);
    if (at >= 0) table[name] = table[name].filter((v) => v !== value);
  }
  const list = (table[btn] || []).filter((v) => v !== value);
  table[btn] = [value, ...list].slice(0, 2);
}

/* ---- the touch overlay ---- */
function setupTouch() {
  // the d-pad: a knob in a box, the four wedges (the same 8-way rule input_stick uses, but a coarse one here)
  const pad = el.touchDpad;
  const knob = el.dpadKnob;
  const arrows = {};
  for (const a of pad.querySelectorAll('.dpad-arrow')) arrows[a.dataset.dir] = a;
  const R = () => pad.getBoundingClientRect();
  let pid = null;

  const move = (e) => {
    const r = R();
    const cx = r.left + r.width / 2, cy = r.top + r.height / 2;
    const max = r.width / 2 - 8;
    let dx = e.clientX - cx, dy = e.clientY - cy;
    const len = Math.hypot(dx, dy);
    if (len > max) { dx = dx / len * max; dy = dy / len * max; }
    knob.style.transform = `translate(${dx}px, ${dy}px)`;
    // the wedges: a diagonal once the minor axis passes tan(22.5 deg) of the major one
    const ax = Math.abs(dx), ay = Math.abs(dy);
    const dead = max * 0.28;
    touchDirs.left = touchDirs.right = touchDirs.up = touchDirs.down = false;
    if (ax > dead || ay > dead) {
      const h = ax * 256 >= ay * 106, v = ay * 256 >= ax * 106;
      if (h) touchDirs[dx < 0 ? 'left' : 'right'] = true;
      if (v) touchDirs[dy < 0 ? 'up' : 'down'] = true;
    }
    for (const k in arrows) arrows[k].classList.toggle('on', !!touchDirs[k]);
  };
  const end = () => {
    pid = null;
    knob.style.transform = '';
    touchDirs.left = touchDirs.right = touchDirs.up = touchDirs.down = false;
    for (const k in arrows) arrows[k].classList.remove('on');
  };
  pad.addEventListener('pointerdown', (e) => { pid = e.pointerId; pad.setPointerCapture(pid); move(e); e.preventDefault(); });
  pad.addEventListener('pointermove', (e) => { if (e.pointerId === pid) { move(e); e.preventDefault(); } });
  pad.addEventListener('pointerup', end);
  pad.addEventListener('pointercancel', end);

  // the buttons: a press-and-hold, with a pointer capture so a finger that slides off still releases
  for (const b of el.touch.querySelectorAll('.tbtn')) {
    const name = b.dataset.btn;
    const bit = name === 'shoulderL' ? TOUCH_SHOULDER_L
      : name === 'shoulderR' ? TOUCH_SHOULDER_R
      : 1 << BTN[name];
    let id = null;
    b.addEventListener('pointerdown', (e) => {
      id = e.pointerId;
      b.setPointerCapture(id);
      b.classList.add('on');
      touchState |= bit;
      if (audioCtx && audioCtx.state !== 'running') audioCtx.resume();
      e.preventDefault();
    });
    const up = () => {
      b.classList.remove('on');
      touchState &= ~bit;
      id = null;
    };
    b.addEventListener('pointerup', up);
    b.addEventListener('pointercancel', up);
    b.addEventListener('contextmenu', (e) => e.preventDefault());
  }
}

function touchShouldBeOn() {
  if (config.touchEnabled !== null) return config.touchEnabled;
  return matchMedia('(pointer: coarse)').matches || 'ontouchstart' in window;
}
function applyTouch() {
  const on = touchShouldBeOn();
  el.touch.hidden = !on;
  el.touch.style.opacity = (config.touchOpacity / 100).toFixed(2);
  el.touch.classList.toggle('tiny', config.touchSize < 85);
  el.touch.classList.toggle('landscape', config.touchLandscape);
  el.touch.style.setProperty('--touch-scale', (config.touchSize / 100).toFixed(2));
}

/* ------------------------------------------------------------------ the bindings table */
// the shoulders get a key cell but no pad cell of their own: on a pad they are the two triggers, which are
// AIM as well, and the page reads them straight off the pad rather than through a binding
const PAD_TRIGGER_LABEL = { STRAFE_L: 'LT / L2', STRAFE_R: 'RT / R2' };

function renderBindings() {
  el.bindings.innerHTML = '';
  for (const name of ALL_BIND_NAMES) {
    const row = document.createElement('div');
    row.className = 'bind-row';
    const label = document.createElement('span');
    label.className = 'bind-name';
    label.textContent = name;
    row.appendChild(label);
    for (const dev of ['key', 'pad']) {
      if (dev === 'pad' && PAD_TRIGGER_LABEL[name]) {
        const fixed = document.createElement('span');
        fixed.className = 'bind-cell fixed';
        fixed.textContent = PAD_TRIGGER_LABEL[name];
        row.appendChild(fixed);
        continue;
      }
      const cell = document.createElement('button');
      cell.type = 'button';
      cell.className = 'bind-cell';
      const list = dev === 'key' ? keyBindingsFor(name) : padBindingsFor(name);
      const text = list.length ? list.map(dev === 'key' ? keyLabel : padLabel).join(' / ') : '-';
      cell.textContent = text;
      if (!list.length) cell.classList.add('unbound');
      if (listening && listening.dev === dev && listening.btn === name) cell.classList.add('listening');
      cell.addEventListener('click', () => {
        listening = { dev, btn: name };
        renderBindings();
      });
      row.appendChild(cell);
    }
    el.bindings.appendChild(row);
  }
}

// while a pad cell is listening, a gamepad button press binds it
function pollGamepadCapture() {
  if (!listening || listening.dev !== 'pad') return;
  const pads = navigator.getGamepads ? navigator.getGamepads() : [];
  for (const p of pads) {
    if (!p) continue;
    for (let i = 0; i < p.buttons.length; i++) {
      const b = p.buttons[i];
      if (b.pressed || b.value > 0.5) {
        assignBinding('pad', listening.btn, i);
        listening = null;
        saveConfig();
        renderBindings();
        return;
      }
    }
  }
}

/* ------------------------------------------------------------------ the panel
 * On the developer build this is the side panel: a column beside the game on a wide window, a drawer behind the
 * hamburger on a narrow one. On a redist build it is a menu that drops from the button at the top right, closed
 * until asked for, so nothing of the page sits over the game until the player wants it. */
function setPanelOpen(open) {
  el.panel.classList.toggle('hidden', !open);
  el.panelBtn.setAttribute('aria-expanded', open ? 'true' : 'false');
  if (open && REDIST) placePanel();
}
/* the redist menu is a fixed box hung under the top-right button. The button lives inside the game window, which
 * is padded and centred, so its position is measured rather than assumed - and the menu is re-placed when the
 * window changes shape or the game goes fullscreen, both of which move the button. */
function placePanel() {
  const b = el.panelBtn.getBoundingClientRect();
  const w = el.panel.offsetWidth || 340, gap = 6, edge = 8, min = 140;
  const left = Math.max(edge, Math.min(b.right - w, innerWidth - w - edge));
  // the game window is centred, so on a short window the button can be low down: the menu goes under it, and
  // its height is what is left, or the bottom of the bindings table ends up off the screen
  const top = Math.max(edge, Math.min(b.bottom + gap, innerHeight - min - gap));
  el.panel.style.left = `${Math.round(left)}px`;
  el.panel.style.top = `${Math.round(top)}px`;
  el.panel.style.maxHeight = `${Math.round(Math.max(160, innerHeight - top - edge))}px`;
}
function bindPanel() {
  el.panelBtn.addEventListener('click', () => setPanelOpen(el.panel.classList.contains('hidden')));
  el.panelClose.addEventListener('click', () => setPanelOpen(false));
  el.fullscreenBtn.addEventListener('click', toggleFullscreen);
  if (REDIST) {
    setPanelOpen(false);
    // the game is behind the menu, so a click on the game closes it rather than going through to a sprite
    el.stage.addEventListener('pointerdown', (e) => { if (!el.panel.contains(e.target)) setPanelOpen(false); });
    addEventListener('keydown', (e) => { if (e.key === 'Escape' && !el.panel.classList.contains('hidden')) setPanelOpen(false); });
    addEventListener('resize', () => { if (!el.panel.classList.contains('hidden')) placePanel(); });
    addEventListener('orientationchange', () => { if (!el.panel.classList.contains('hidden')) placePanel(); });
    document.addEventListener('fullscreenchange', () => { if (!el.panel.classList.contains('hidden')) placePanel(); });
  }

  el.pauseBtn.addEventListener('click', togglePause);
  el.resumeBtn.addEventListener('click', togglePause);
  el.resetBtn.addEventListener('click', softReset);

  // the dropdown only picks; "Go" applies it. Changing stage restarts the game, and a stray arrow key on a
  // focused <select> would otherwise throw away the run in progress. Neither is in a redist build.
  if (el.levelGo) {
    el.levelGo.addEventListener('click', () => goToLevel(el.levelSelect.value));
    el.levelSelect.addEventListener('change', updateLevelGo);
  }

  el.scaleMode.addEventListener('change', () => { config.scaleMode = el.scaleMode.value; saveConfig(); layout(); });
  el.screenMode.addEventListener('change', () => { config.screenMode = +el.screenMode.value; saveConfig(); layout(); });
  el.pixelPerfect.addEventListener('change', () => { config.pixelPerfect = el.pixelPerfect.checked; saveConfig(); layout(); });

  el.volume.addEventListener('input', () => { config.volume = el.volume.value / 100; saveConfig(); exp.wasm_audio_set_volume(config.volume); });
  el.sens.addEventListener('input', () => { config.sens = +el.sens.value; saveConfig(); exp.wasm_input_sens(config.sens); });
  el.resetKeys.addEventListener('click', () => { config.keys = structuredClone(DEFAULT_KEYS); saveConfig(); renderBindings(); });
  el.resetPad.addEventListener('click', () => { config.pad = structuredClone(DEFAULT_PAD); saveConfig(); renderBindings(); });

  el.touchEnabled.addEventListener('change', () => { config.touchEnabled = el.touchEnabled.checked; saveConfig(); applyTouch(); });
  el.touchOpacity.addEventListener('input', () => { config.touchOpacity = +el.touchOpacity.value; saveConfig(); applyTouch(); });
  el.touchSize.addEventListener('input', () => { config.touchSize = +el.touchSize.value; saveConfig(); applyTouch(); });
  el.touchLandscape.addEventListener('change', () => { config.touchLandscape = el.touchLandscape.checked; saveConfig(); applyTouch(); });
  // the status read-outs, the on-canvas FPS and the debug switches are the developer's window onto the module,
  // and none of them is in a redist build - the elements are not in the page there at all
  if (el.showFps) el.showFps.addEventListener('change', () => { config.showFps = el.showFps.checked; saveConfig(); });
  if (el.devApply) el.devApply.addEventListener('click', applyDevSwitches);

  // reflect the stored config
  el.scaleMode.value = config.scaleMode;
  el.screenMode.value = String(config.screenMode);
  el.pixelPerfect.checked = config.pixelPerfect;
  el.volume.value = Math.round(config.volume * 100);
  el.sens.value = config.sens;
  el.touchEnabled.checked = touchShouldBeOn();
  el.touchOpacity.value = config.touchOpacity;
  el.touchSize.value = config.touchSize;
  el.touchLandscape.checked = config.touchLandscape;
  if (el.showFps) el.showFps.checked = config.showFps;
  if (el.devText) el.devText.value = config.dev;
}

function applyDevSwitches() {
  config.dev = el.devText.value;
  saveConfig();
  const lines = config.dev.split('\n');
  for (const line of lines) {
    const t = line.trim();
    if (!t || t.startsWith('#')) continue;
    const eq = t.indexOf('=');
    if (eq < 0) continue;
    const name = t.slice(0, eq).trim(), value = t.slice(eq + 1).trim();
    const n = enc.encode(name), v = enc.encode(value);
    const np = writeString(name), vp = writeString(value);
    exp.wasm_env_put(np, n.length, vp, v.length);
  }
  log(`dev switches applied (${lines.filter((l) => l.includes('=')).length}) — a soft reset picks them up`);
  softReset();
}

const enc = new TextEncoder();
const decoder = new TextDecoder();

/* ---- the window ----
 * What goes fullscreen is the game window, so the page's frame and the developer panel stay out of it. On a
 * redistributable build that is wrong: the settings menu is a box hung under the top-right button, and the
 * panel it is in lives in the page beside the stage rather than inside the game window. If the game window
 * alone went fullscreen the menu would be in a part of the document the browser is no longer showing, and
 * the button would open nothing at all. So there the page itself is what goes fullscreen - the stage is the
 * whole viewport, and the menu comes with it. */
function toggleFullscreen() {
  if (document.fullscreenElement) document.exitFullscreen();
  else (REDIST ? document.documentElement : el.wrap).requestFullscreen?.().catch(() => {});
}
function togglePause() {
  if (!booted) return;
  paused = !paused;
  el.pausedCard.classList.toggle('hidden', !paused);
  el.overlay.hidden = false;
  el.loadingCard.classList.add('hidden');
  el.pauseBtn.textContent = paused ? 'Resume' : 'Pause';
  lastT = performance.now();
  if (audioCtx) { if (paused) audioCtx.suspend?.(); else audioCtx.resume?.(); }
  if (worklet) worklet.port.postMessage({ type: 'reset' });
  rsNext = 0;
}
function softReset() {
  if (!exp) return;
  exp.wasm_shutdown();
  // the module's state is gone; the file system and the settings survive, so boot again from them
  booted = false;
  const ok = exp.wasm_boot(startLevel, 0);
  if (!ok) { showError('the reset failed: ' + readError()); return; }
  booted = true;
  exp.wasm_input_sens(config.sens);
  exp.wasm_audio_set_volume(config.volume);
  worklet?.port.postMessage({ type: 'reset' });
  rsNext = 0;
  log(startLevel ? `soft reset into ${levelName(startLevel)}` : 'soft reset');
}
let startLevel = 0;

/* ---- the level select ----
 * Changing stage is a soft reset with a different start_level: game_init skips the front end for any non-zero
 * one, and the packs and the decoded assets are already in the module's memory, so it is a re-entry rather than
 * a reload. The stage is remembered, so "Soft reset" and a reload both come back to it. */
function clampLevel(id) { return Math.max(0, Math.min(MAX_LEVEL, +id || 0)); }
function levelName(id) { return (LEVELS.find((l) => l.id === id) || LEVELS[0]).label; }
function renderLevelSelect() {
  el.levelSelect.innerHTML = '';
  for (const l of LEVELS) {
    const opt = document.createElement('option');
    opt.value = String(l.id);
    opt.textContent = l.label;
    el.levelSelect.appendChild(opt);
  }
  el.levelSelect.value = String(startLevel);
  updateLevelGo();
}
/* the button only lights up when the dropdown is somewhere else, so a pending pick is visible */
function updateLevelGo() {
  el.levelGo.disabled = +el.levelSelect.value === startLevel;
}
/* returns false when there is nothing to do (already there), so the caller can leave the game alone */
function goToLevel(id) {
  id = clampLevel(id);
  if (id === startLevel) { log(`already in ${levelName(id)}`); return false; }
  startLevel = id;
  config.level = id;
  saveConfig();
  el.levelSelect.value = String(id);
  updateLevelGo();
  softReset();
  // the new run has its own title card and music; nothing of the old one should be left showing
  if (paused) togglePause();
  el.overlay.hidden = true;
  el.loadingCard.classList.add('hidden');
  return true;
}

/* the module's own message (app.c writes a line to stderr and wasm_boot stores it) */
function readError() {
  const p = exp.wasm_error_msg();
  if (!p) return 'unknown error';
  return decoder.decode(new Uint8Array(memory.buffer, p, 128)).replace(/\0.*$/s, '');
}

function showError(msg) {
  if (window.__saber) window.__saber.error = msg;
  el.overlay.hidden = false;
  el.loadingCard.classList.add('hidden');
  el.pausedCard.classList.add('hidden');
  el.errorCard.classList.remove('hidden');
  el.errorText.textContent = msg;
  log(msg);
  running = false;
}

/* the canvas is always 426x240 in its backing store; only the CSS size follows the window */
function layout() {
  const W = exp ? exp.wasm_frame_w() : 426;
  const H = exp ? exp.wasm_frame_h() : 240;
  el.canvas.classList.toggle('smooth', !config.pixelPerfect);
  if (el.canvas.width !== W) el.canvas.width = W;
  if (el.canvas.height !== H) el.canvas.height = H;
  imageData = null;
  ctx2d.imageSmoothingEnabled = !config.pixelPerfect;

  // the stage is behind the fullscreen element (on the dev build the game window, on a redist build the page),
  // so in fullscreen the canvas is sized from the fullscreen box instead of from the stage
  const full = !!document.fullscreenElement;
  const box = (document.fullscreenElement || el.stage).getBoundingClientRect();
  // a redistributable build has no margin to keep: the page is black and the picture goes to the edges of it
  const pad = REDIST ? 0 : (full ? 0 : 16);   /* windowed keeps a margin; fullscreen goes edge to edge */
  const stage = { width: box.width, height: box.height };
  let w, h;
  if (REDIST) {
    // cover, not fit: a distributable build is not a picture in a window, it is the window. So the canvas is
    // scaled up by whole pixels until it covers the page in both axes and the page clips the overflow (the
    // stage is overflow: hidden), which is what leaves no black round the game. The scale is still a whole
    // number, so every game pixel is still the same square block of screen pixels - 1:1, just cropped.
    const cover = Math.max((stage.width - pad) / W, (stage.height - pad) / H);
    const mult = Math.max(1, Math.ceil(cover - 1e-6));   /* the epsilon: an exact 4 must not ask for 5 */
    // ...except when the page is nothing like 16:9. A phone held upright is 390x844: covering that at 4x would
    // crop 77% of the width and leave a stamp of the game in the middle. Past this much crop (the canvas more
    // than 1.5x the page on an axis) it falls back to fitting inside whole, which letterboxes - two black bars
    // on a shape where a picture that small needs them.
    const tooTall = mult * W > (stage.width - pad) * 1.5;
    const tooWide = mult * H > (stage.height - pad) * 1.5;
    if (tooTall || tooWide) {
      const fit = Math.max(1, Math.floor(Math.min((stage.width - pad) / W, (stage.height - pad) / H)));
      w = W * fit;
      h = H * fit;
    } else {
      w = W * mult;
      h = H * mult;
    }
  } else if (config.scaleMode === 'stretch') {
    w = stage.width - pad;
    h = stage.height - pad;
  } else if (config.scaleMode === 'integer' || config.screenMode > 0) {
    const mult = config.screenMode > 0 ? config.screenMode : Math.max(1, Math.floor(Math.min((stage.width - pad) / W, (stage.height - pad) / H)));
    w = W * mult;
    h = H * mult;
  } else {
    // fit: the largest scale that fits, not necessarily a whole number (a fractional scale of a 426-wide
    // canvas is fine because the browser's smoothing is off - the pixels stay square to within a pixel)
    const s = Math.min((stage.width - pad) / W, (stage.height - pad) / H);
    w = W * s;
    h = H * s;
  }
  el.canvas.style.width = `${Math.max(160, Math.floor(w))}px`;
  el.canvas.style.height = `${Math.max(90, Math.floor(h))}px`;
  applyTouch();
}
addEventListener('resize', () => layout());
document.addEventListener('fullscreenchange', () => layout());

/* ------------------------------------------------------------------ the loop */
function frame(now) {
  requestAnimationFrame(frame);
  if (!booted || !running) return;
  const dt = (now - lastT) / 1000;
  lastT = now;
  if (paused) { present(); return; }

  // a long gap (a backgrounded tab) must not be turned into a hundred catch-up steps
  const step = Math.min(dt, 0.25);
  const t0 = performance.now();
  pushInput();
  exp.wasm_frame(step);
  frameMs = performance.now() - t0;
  present();
  pushAudio();

  drawCount = exp.wasm_prims();
  floorUs = exp.wasm_floor_us();

  fpsFrames++;
  if (now - fpsSince >= 500) { fps = fpsFrames * 1000 / (now - fpsSince); fpsFrames = 0; fpsSince = now; updateStats(); }
  pollGamepadCapture();
}

let statTick = 0;
function updateStats() {
  if (!el.statFps) return;   /* a redist build has no status read-outs */
  el.statFps.textContent = fps.toFixed(0);
  el.statFrame.textContent = `${frameMs.toFixed(1)} ms`;
  el.statDraws.textContent = drawCount;
  el.statFloor.textContent = `${(floorUs / 1000).toFixed(1)} ms`;
  el.statAudio.textContent = worklet
    ? `${(audioQueued / 1000).toFixed(2)}s${audioStarted ? '' : ' (waiting)'}${audioUnderruns ? ` ${audioUnderruns} under` : ''}`
    : 'off';
  el.statHeap.textContent = `${(exp.wasm_heap_free() / 1e6).toFixed(1)} MB`;
  el.statMem.textContent = `${(exp.wasm_mem_used() / 1e6).toFixed(1)} / ${(exp.wasm_mem_total() / 1e6).toFixed(0)} MB`;
  if (++statTick % 4 === 0) { audioPeak *= 0.5; }
}

/* ------------------------------------------------------------------ the page's state, for a harness
 * tools/wasm/browser_check.js drives the staged page over the DevTools protocol and reads this: booted, the error
 * if it did not boot, and the per-frame numbers the status panel shows. A hook like this is also what makes the
 * page scriptable from the console (window.__saber.press('ArrowRight', true)).
 *
 * A redist build keeps only the two fields a host page needs to know whether the game came up. Everything below
 * them is a developer surface - it can drive the game, jump stages and read the module's internals - so it is
 * left out rather than merely undocumented. */
window.__saber = REDIST ? {
  booted: false,
  error: '',
} : {
  booted: false,
  error: '',
  keyHeld,
  press(code, down) { down ? keyHeld.add(code) : keyHeld.delete(code); },
  pause() { togglePause(); },
  reset() { softReset(); },
  level(n) { return goToLevel(n) ? levelName(clampLevel(n)) : null; },
  levelNow() { return startLevel; },
  set(name, value) {
    if (name in config) { config[name] = value; saveConfig(); }
    if (name === 'scaleMode' || name === 'pixelPerfect') layout();
    if (name === 'touchEnabled' || name === 'touchOpacity' || name === 'touchSize' || name === 'touchLandscape') applyTouch();
  },
  get(name) { return config[name]; },
  input() { return { ...lastInput }; },
  stats() {
    const fb = refreshView();
    const seen = new Set();
    for (let i = 0; i < fb.length; i += 37) seen.add(fb[i]);
    let lit = 0;
    for (const c of seen) if (c & 0xffffff) lit++;
    return {
      fps: +fps.toFixed(1),
      frameMs: +frameMs.toFixed(2),
      draws: drawCount,
      floorMs: +(floorUs / 1000).toFixed(2),
      files: exp ? exp.wasm_vfs_count() : 0,
      dataMB: exp ? +(exp.wasm_vfs_bytes() / 1e6).toFixed(1) : 0,
      heapFreeMB: exp ? +(exp.wasm_heap_free() / 1e6).toFixed(1) : 0,
      memoryMB: exp ? +(exp.wasm_mem_total() / 1e6).toFixed(0) : 0,
      audioQueued: audioQueued,
      audioUnderruns: audioUnderruns,
      audioStarted,
      distinctColours: seen.size,
      litColours: lit,
      state: exp ? exp.wasm_state() : 0,
    };
  },
};

/* ------------------------------------------------------------------ boot */
async function boot() {
  try {
    // a redist build's menu is a dropdown from the top-right button rather than a side panel, and the panel's
    // subtitle says what the build is instead of claiming to be a developer one
    document.body.classList.toggle('redist', REDIST);
    // on a redist build the fullscreen and settings buttons come up with the game, not over the loading card
    document.body.classList.toggle('booting', REDIST);
    const sub = document.querySelector('.panel-head small');
    if (sub && BUILD.label) sub.textContent = BUILD.label;
    bindPanel();
    renderBindings();
    applyTouch();
    setupTouch();
    addEventListener('keydown', onKeyDown);
    addEventListener('keyup', onKeyUp);
    addEventListener('blur', () => keyHeld.clear());
    addEventListener('gamepadconnected', (e) => { el.padState.textContent = e.gamepad.id; });
    addEventListener('gamepaddisconnected', () => { el.padState.textContent = 'No gamepad seen yet.'; padIndex = -1; });

    await loadModule();
    await loadData();

    // A redist build reads no query string: ?level= is a stage select by another door and ?SABER_* are the
    // debug switches, and its module is built without wasm_env_put, so neither could be applied anyway.
    if (REDIST) {
      startLevel = 0;   /* the front end: the only way in */
    } else {
      // the URL's query string: ?level=3, and any SABER_* switch. ?level wins over the remembered stage, and 0
      // (or nothing) is the front end. The clamp used to stop at 6, which left stage 7 unreachable from the page.
      const q = new URLSearchParams(location.search);
      const asked = q.get('level');
      startLevel = asked === null ? clampLevel(config.level) : clampLevel(+asked);
      config.level = startLevel;
      renderLevelSelect();
      for (const [k, v] of q) {
        if (k.toUpperCase().startsWith('SABER_')) {
          const np = writeString(k), vp = writeString(v);
          exp.wasm_env_put(np, enc.encode(k).length, vp, enc.encode(v).length);
          if (el.devText && !el.devText.value.includes(k)) el.devText.value += (el.devText.value ? '\n' : '') + `${k}=${v}`;
        }
      }
    }

    if (!exp.wasm_boot(startLevel, 0)) {
      window.__saber.error = readError();
      showError(window.__saber.error);
      return;
    }
    booted = true;
    window.__saber.booted = true;
    exp.wasm_input_sens(config.sens);
    exp.wasm_audio_set_volume(config.volume);

    layout();
    el.overlay.hidden = true;
    document.body.classList.remove('booting');
    running = true;
    lastT = performance.now();
    requestAnimationFrame(frame);

    // the audio device needs a gesture: the first click or key starts it
    const kick = async () => { await ensureAudio(); exp.wasm_audio_set_volume(config.volume); };
    addEventListener('pointerdown', kick, { once: true });
    addEventListener('keydown', kick, { once: true });
    log('ready');
  } catch (e) {
    window.__saber.error = String(e && e.message ? e.message : e);
    showError(window.__saber.error);
    console.error(e);
  }
}

boot();
