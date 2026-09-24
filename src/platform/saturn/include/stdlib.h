#pragma once
/* libyaul's <stdlib.h> plus what it lacks (platform/saturn/libc_sat.c) */
#include_next <stdlib.h>
#include <stddef.h>

#define RAND_MAX 2147483647

void  *calloc(size_t n, size_t size);
void  *aligned_alloc(size_t align, size_t size);
void   qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));
#undef abs   /* libyaul's gamemath/defs.h has an abs() macro that returns 0 for positive values */
int    abs(int v);
long   labs(long v);
double strtod(const char *s, char **end);
float  strtof(const char *s, char **end);
double atof(const char *s);
char  *getenv(const char *name);

/* the heaps (libc_sat.c): malloc prefers low work RAM for big blocks (CPU-read data); hw_malloc is always high work
 * RAM, the only RAM the SCU DMA reaches (DMA sources, per-frame structures) */
void  *hw_malloc(size_t n);
void  *hw_memalign(size_t align, size_t n);
void   sat_heap_stats(size_t *hw_free, size_t *lw_free, size_t *hw_used, size_t *lw_used);
