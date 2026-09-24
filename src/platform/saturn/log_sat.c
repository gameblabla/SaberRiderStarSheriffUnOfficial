/* printf / fprintf(stderr) on the Saturn: a ring in work RAM that the test harness reads through the emulator's
 * debugger (tools/saturn/mednafen_run.py finds `saber_log` in the ELF's symbols). It replaces the Dreamcast's serial
 * console. Layout: "SABERLOG", u32 size, u32 head (bytes written so far; the text is buf[i % size]), buf. */
#include "sat_internal.h"
#include <stdio.h>
#include <string.h>

#define LOG_SIZE 16384

struct SaberLog { char magic[8]; uint32_t size; volatile uint32_t head; char buf[LOG_SIZE]; };
struct SaberLog saber_log __attribute__((aligned(16))) = { { 'S', 'A', 'B', 'E', 'R', 'L', 'O', 'G' }, LOG_SIZE, 0, { 0 } };

void log_sat_write(const char *s, size_t n)
{
    uint32_t h = saber_log.head;
    for (size_t i = 0; i < n; i++) saber_log.buf[(h + i) % LOG_SIZE] = s[i];
    saber_log.head = h + (uint32_t)n;
}

static size_t log_file_write(FILE *f, const unsigned char *s, size_t n) { (void)f; log_sat_write((const char *)s, n); return n; }
static size_t log_file_read(FILE *f, unsigned char *d, size_t n) { (void)f; (void)d; (void)n; return 0; }
static off_t log_file_seek(FILE *f, off_t o, int w) { (void)f; (void)o; (void)w; return -1; }
static int log_file_close(FILE *f) { (void)f; return 0; }

/* libyaul's stdin / stdout / stderr are weak and empty */
FILE __stdout_FILE = { .fd = 1, .read = log_file_read, .write = log_file_write, .seek = log_file_seek, .close = log_file_close };
FILE __stderr_FILE = { .fd = 2, .read = log_file_read, .write = log_file_write, .seek = log_file_seek, .close = log_file_close };
FILE __stdin_FILE  = { .fd = 0, .read = log_file_read, .write = log_file_write, .seek = log_file_seek, .close = log_file_close };
