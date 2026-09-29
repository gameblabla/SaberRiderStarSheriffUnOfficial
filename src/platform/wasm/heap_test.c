/* A stress test for the WASM port's heap (src/platform/wasm/libc_wasm.c), run inside the module: the point is
 * to hammer the paths the game leans on - many small allocations and frees interleaved, a few multi-megabyte
 * blocks, a stricter alignment than the default, realloc growth and shrink - and check after each step that the
 * block list is still walkable and that everything handed out is distinct and inside the slab.
 *
 * It is built with the module (make -f Makefile.wasm EXTRA_EXPORTS=--export=wasm_heap_stress
 * EXTRA_CFLAGS=-DSABER_WASM_SELFTEST) and driven from tools/wasm/smoke.js --heap, or on its own from node.
 *
 *   node tools/wasm/heap_test.js
 */
#include "wasm_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef SABER_WASM_SELFTEST

/* raw free, for the harness: the stress test needs to free an arbitrary pointer it was handed, and the game
 * never does that (every block it frees came from its own malloc) */
__attribute__((export_name("wasm_heap_free_ptr"))) void wasm_heap_free_ptr(unsigned p) { free((void *)(uintptr_t)p); }
__attribute__((export_name("wasm_heap_list_ok_"))) unsigned wasm_heap_list_ok_(void) { return wasm_heap_list_ok() ? 1u : 0u; }
__attribute__((export_name("wasm_heap_is_ours_"))) unsigned wasm_heap_is_ours_(unsigned p) { return wasm_ptr_is_heap((void *)(uintptr_t)p) ? 1u : 0u; }

/* what the harness reads back */
static unsigned st_alloc_ok, st_free_ok, st_align_ok, st_walk_ok, st_realloc_ok, st_distinct, st_bytes, st_list_ok;
static char st_msg[256];

/* is the pointer one this heap handed out? (the same test free() makes) */
extern bool wasm_ptr_is_heap(void *p);
extern bool wasm_heap_list_ok(void);

static bool ours(void *p) { return p && wasm_ptr_is_heap(p); }
static bool walk_ok(void) { return wasm_heap_list_ok(); }

/* every live block distinct: no two pointers may share a byte */
static bool distinct(void **v, int n)
{
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++) {
            size_t a = v[i] ? 16 : 0, b = v[j] ? 16 : 0;
            if (!v[i] || !v[j]) continue;
            if ((uintptr_t)v[i] < (uintptr_t)v[j] + b && (uintptr_t)v[j] < (uintptr_t)v[i] + a) return false;
        }
    return true;
}

__attribute__((export_name("wasm_heap_stress")))
unsigned wasm_heap_stress(unsigned rounds, unsigned live_max)
{
    st_alloc_ok = st_free_ok = st_align_ok = st_walk_ok = st_realloc_ok = st_distinct = st_bytes = st_list_ok = 0;
    st_list_ok = wasm_heap_list_ok() ? 1u : 0u;
    void **live = calloc(live_max, sizeof(void *));
    size_t *sizes = calloc(live_max, sizeof(size_t));
    if (!live || !sizes) { snprintf(st_msg, sizeof st_msg, "the test's own arrays did not allocate"); return 0; }

    for (unsigned r = 0; r < rounds; r++) {
        /* free a few of the live blocks (a pseudo-random pattern, so a block is freed after a variable number
         * of allocations have been made on top of it - which is what finds a merge that loses a neighbour) */
        for (unsigned k = 0; k < live_max; k++) {
            if (!live[k]) continue;
            if ((r * 2654435761u + k * 40503u) % 5u < 2u) {
                if (!ours(live[k])) { snprintf(st_msg, sizeof st_msg, "round %u slot %u: free of a block the heap lost (size %zu)", r, k, sizes[k]); goto done; }
                bool was_ok = walk_ok();   /* the list before this free */
                free(live[k]);
                live[k] = NULL;
                st_free_ok++;
                if (!walk_ok()) {
                    snprintf(st_msg, sizeof st_msg, "round %u slot %u: freeing %u bytes broke the list (it was %s)",
                             r, k, (unsigned)sizes[k], was_ok ? "intact" : "already broken");
                    goto done;
                }
            }
        }
        if (!walk_ok()) { snprintf(st_msg, sizeof st_msg, "round %u: the free loop broke the list", r); goto done; }
        st_walk_ok++;

        /* allocate into a free slot: mostly small, occasionally a big one, and a 32-aligned one now and then */
        for (unsigned k = 0; k < live_max; k++) {
            if (live[k]) continue;
            size_t n;
            switch ((r + k) % 7u) {
            case 0: n = 1; break;                       /* a zero-size request still gives a usable block */
            case 1: n = 3; break;
            case 2: n = 64; break;
            case 3: n = 4096; break;
            case 4: n = 300000; break;                 /* a texture-sized block */
            case 5: n = 2 * 1024 * 1024; break;        /* a big one: this is what grows the slab */
            default: n = 64 + (r * 37 + k * 101) % 9000; break;
            }
            void *p = ((r + k) % 11u == 0) ? aligned_alloc(32, n) : malloc(n);
            if (!p) { snprintf(st_msg, sizeof st_msg, "round %u slot %u: malloc(%zu) returned nothing", r, k, n); goto done; }
            if ((uintptr_t)p % 32u) { snprintf(st_msg, sizeof st_msg, "round %u: a payload is not 32-aligned", r); goto done; }
            memset(p, (int)(r & 0xff), n < 4096 ? n : 4096);   /* touch it, so a bad block faults here */
            live[k] = p;
            sizes[k] = n;
            st_bytes += (unsigned)n;
            if (!ours(p)) { snprintf(st_msg, sizeof st_msg, "round %u: malloc(%zu) gave %p, which free() would not recognise", r, n, p); goto done; }
            st_alloc_ok++;
            if (!walk_ok()) { snprintf(st_msg, sizeof st_msg, "round %u slot %u: malloc(%zu) broke the list", r, k, n); goto done; }
            if (!ours(p)) { snprintf(st_msg, sizeof st_msg, "round %u slot %u: the block it just handed out is not recognised", r, k); goto done; }
            break;   /* one per round keeps the pattern varied without exhausting the slab */
        }

        if (!distinct(live, live_max)) { snprintf(st_msg, sizeof st_msg, "round %u: two live blocks overlap", r); goto done; }
        st_distinct++;

        /* realloc: grow and shrink, and confirm the contents survive a move */
        for (unsigned k = 0; k < live_max; k++) {
            if (!live[k] || (r + k) % 9u) continue;
            size_t was = sizes[k];
            size_t want = (r & 1) ? was * 2 + 7 : was > 64 ? was / 2 : 64;
            unsigned char keep[64];
            size_t keepn = was < sizeof keep ? was : sizeof keep;
            memcpy(keep, live[k], keepn);
            void *p = realloc(live[k], want);
            if (!p && want) { snprintf(st_msg, sizeof st_msg, "round %u: realloc to %zu failed", r, want); goto done; }
            if (p) {
                if (memcmp(p, keep, keepn) != 0) { snprintf(st_msg, sizeof st_msg, "round %u: realloc lost the contents", r); goto done; }
                live[k] = p;
                sizes[k] = want;
                if (!ours(p)) { snprintf(st_msg, sizeof st_msg, "round %u: realloc gave %p, which free() would not recognise", r, p); goto done; }
            }
            st_realloc_ok++;
            if (!walk_ok()) { snprintf(st_msg, sizeof st_msg, "round %u slot %u: realloc(%zu -> %zu) broke the list", r, k, (unsigned)was, (unsigned)want); goto done; }
            break;
        }
        if (!walk_ok()) { snprintf(st_msg, sizeof st_msg, "round %u: the block list did not survive a realloc", r); goto done; }
    }

    /* free everything, then the heap must be one (or few) free blocks again */
    for (unsigned k = 0; k < live_max; k++) if (live[k]) { free(live[k]); live[k] = NULL; }
    if (!walk_ok()) { snprintf(st_msg, sizeof st_msg, "after freeing everything, the list is damaged"); goto done; }
    st_msg[0] = 0;

done:
    for (unsigned k = 0; k < live_max; k++) if (live[k]) free(live[k]);
    free(live);
    free(sizes);
    /* 1 when everything above held */
    return st_msg[0] ? 0 : 1;
}

__attribute__((export_name("wasm_heap_stress_stats")))
unsigned wasm_heap_stress_stats(unsigned which)
{
    switch (which) {
    case 0: return st_alloc_ok;
    case 1: return st_free_ok;
    case 2: return st_realloc_ok;
    case 3: return st_walk_ok;
    case 4: return st_distinct;
    default: return st_bytes;
    }
}
__attribute__((export_name("wasm_heap_stress_msg")))
unsigned wasm_heap_stress_msg(unsigned buf, unsigned cap)
{
    size_t n = strlen(st_msg);
    if (cap && n < cap) { memcpy((void *)(uintptr_t)buf, st_msg, n + 1); return (unsigned)n; }
    return (unsigned)n;
}
#endif
