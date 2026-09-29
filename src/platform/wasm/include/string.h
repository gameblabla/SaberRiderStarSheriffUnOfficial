#pragma once
/* <string.h> for the WASM port (wasm32-unknown-unknown has no libc at all: platform/wasm/libc_wasm.c). */
#include <stddef.h>

void  *memset(void *s, int c, size_t n);
void  *memcpy(void *d, const void *s, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
void  *memmove(void *d, const void *s, size_t n);
void  *memchr(const void *s, int c, size_t n);

size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strcpy(char *d, const char *s);
char  *strncat(char *d, const char *s, size_t n);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
char  *strstr(const char *h, const char *n);
char  *strtok(char *s, const char *delim);
size_t strnlen(const char *s, size_t n);
char  *strdup(const char *s);
