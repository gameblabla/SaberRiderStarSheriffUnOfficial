#pragma once
/* <assert.h> for the WASM port. A failed assert traps (WebAssembly's unreachable), which in a browser shows up
 * in the console with the module's stack trace - as close to a debugger as a page gets. NDEBUG builds it out. */
#ifdef NDEBUG
#define assert(x) ((void)0)
#else
void wasm_assert_fail(const char *expr, const char *file, int line);
#define assert(x) do { if (!(x)) wasm_assert_fail(#x, __FILE__, __LINE__); } while (0)
#endif
