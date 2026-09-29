/* Runs the module's own heap stress test (src/platform/wasm/heap_test.c) and reports what it found. The heap is
 * the riskiest code in the port - a block list walked by pointer arithmetic, with a free() that has to find a
 * header from a payload address - so it gets its own test, run inside the module so the wasm32 build is what is
 * tested, not a host build of the same source.
 *
 *   make -f Makefile.wasm build SELFTEST=1
 *   node tools/wasm/heap_test.js [rounds] [live_max]
 */
import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = resolve(fileURLToPath(new URL('../..', import.meta.url)));
const ROUNDS = +(process.argv[2] || 4000);
const LIVE = +(process.argv[3] || 64);

const decoder = new TextDecoder();
const bin = readFileSync(resolve(ROOT, 'build/wasm/saber_rider.wasm'));
const { instance } = await WebAssembly.instantiate(bin, {
  env: { js_log: () => {}, js_now_ms: () => 0 },
});
const e = instance.exports;
if (typeof e.wasm_heap_stress !== 'function') {
  console.error('the module was not built with the stress test: make -f Makefile.wasm build SELFTEST=1');
  process.exit(2);
}

const t0 = performance.now();
const ok = e.wasm_heap_stress(ROUNDS, LIVE);
const ms = performance.now() - t0;

const buf = e.wasm_alloc(512);
const msgLen = e.wasm_heap_stress_msg(buf, 400);
const msg = msgLen ? decoder.decode(new Uint8Array(e.memory.buffer, buf, msgLen)) : '';
const [allocs, frees, reallocs, walks, distinct, bytes] = [0, 1, 2, 3, 4, 5].map((i) => e.wasm_heap_stress_stats(i));

console.log(`heap stress: ${ROUNDS} rounds, ${LIVE} live slots, ${ms.toFixed(0)} ms`);
console.log(`  ${allocs} allocations, ${frees} frees, ${reallocs} reallocs, ${(bytes / 1e6).toFixed(1)} MB requested`);
console.log(`  ${walks} list checks passed, ${distinct} overlap checks passed`);
console.log(`  heap free ${(e.wasm_heap_free() / 1e6).toFixed(1)} MB, memory ${(e.wasm_mem_total() / 1e6).toFixed(0)} MB`);

if (!ok) {
  console.error(`FAILED: ${msg}`);
  process.exit(1);
}
console.log('OK');
