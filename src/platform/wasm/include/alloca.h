#pragma once
/* <alloca.h> for the WASM port: a freestanding C library has no <alloca.h>, and stb_vorbis includes one on the
 * platforms it lists. Under -ffreestanding clang does not treat the bare name as a builtin, and declaring it does
 * not help (the __builtin_alloca__ attribute is not a declaration attribute), so it is defined as the builtin it
 * is: a stack-pointer move, which is what a variable-length array would compile to anyway. */
#ifdef __cplusplus
extern "C" {
#endif
#include <stddef.h>
#undef alloca
#define alloca(n) __builtin_alloca(n)
#ifdef __cplusplus
}
#endif
