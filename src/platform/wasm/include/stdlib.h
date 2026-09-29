#pragma once
/* <stdlib.h> for the WASM port (platform/wasm/libc_wasm.c). */
#include <stddef.h>

#define RAND_MAX 2147483647

void  *malloc(size_t n);
void  *calloc(size_t n, size_t size);
void  *realloc(void *p, size_t n);
void   free(void *p);
void  *aligned_alloc(size_t align, size_t size);
char  *strdup(const char *s);

int    abs(int v);
int    atoi(const char *s);
long   strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);
float  strtof(const char *s, char **end);
double strtod(const char *s, char **end);
double atof(const char *s);

void   srand(unsigned seed);
int    rand(void);

void   qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));

char  *getenv(const char *name);   /* the SABER_* switches (plat_getenv reads its own table) */
