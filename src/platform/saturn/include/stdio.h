#pragma once
/* libyaul's <stdio.h> plus what it lacks (platform/saturn/libc_sat.c); fopen reads the CD (cd_sat.c) */
#include_next <stdio.h>

int   sscanf(const char *s, const char *fmt, ...);
int   vsscanf(const char *s, const char *fmt, va_list ap);
int   fscanf(FILE *f, const char *fmt, ...);
FILE *fmemopen(void *buf, size_t size, const char *mode);
