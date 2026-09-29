#pragma once
/* <stdio.h> for the WASM port: the core reads the demo's .pck packs and our assets through these (pack.c's
 * fopen/fread/fseek/ftell, assets.c's file_read, the atlas text files through fscanf), and vfs_wasm.c backs every
 * FILE with a block the page fetched and put in the module's memory. Writing (level.c's dump, mode7.c's map) goes to
 * a null sink: there is no file system in a browser, and both are debug-only paths. */
#include <stddef.h>
#include <stdarg.h>

#define EOF (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

typedef struct WASMFile FILE;

extern FILE *stdout;
extern FILE *stderr;

FILE *fopen(const char *path, const char *mode);
int   fclose(FILE *f);
size_t fread(void *dst, size_t size, size_t n, FILE *f);
size_t fwrite(const void *src, size_t size, size_t n, FILE *f);
int   fseek(FILE *f, long off, int whence);
long  ftell(FILE *f);
void  rewind(FILE *f);
int   fgetc(FILE *f);
int   getc(FILE *f);
char *fgets(char *s, int n, FILE *f);
int   fputc(int c, FILE *f);
int   fputs(const char *s, FILE *f);
int   fflush(FILE *f);
int   setvbuf(FILE *f, char *buf, int mode, size_t size);
#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2
int   remove(const char *path);
int   feof(FILE *f);
int   ferror(FILE *f);

int   printf(const char *fmt, ...);
int   fprintf(FILE *f, const char *fmt, ...);
int   sprintf(char *buf, const char *fmt, ...);
int   snprintf(char *buf, size_t cap, const char *fmt, ...);
int   vprintf(const char *fmt, va_list ap);
int   vfprintf(FILE *f, const char *fmt, va_list ap);
int   vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap);

int   sscanf(const char *s, const char *fmt, ...);
int   fscanf(FILE *f, const char *fmt, ...);
