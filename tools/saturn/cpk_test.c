/* Host check of the Saturn's SCPK player (src/platform/saturn/video_sat.c): decodes every frame of a file with the
 * player's own Cinepak decoder and writes a few as PPM.
 *   cc -O2 -Isrc -Isrc/platform/saturn -DREAL_FIXED tools/saturn/cpk_test.c src/fx.c -o /tmp/cpk_test && /tmp/cpk_test X.CPK outdir */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static FILE *g_file;
static uint32_t g_us;
/* the platform's pieces, stubbed */
#define rsat_video_open  test_video_open
#define rsat_video_close test_video_close
#define rsat_video_draw  test_video_draw
#include "../../src/platform/saturn/video_sat.c"
static void (*t_hook)(void *, volatile uint16_t *, int); static void *t_ud;
bool test_video_open(int w, int h, void (*hook)(void *, volatile uint16_t *, int), void *ud) { (void)w; (void)h; t_hook = hook; t_ud = ud; return true; }
void test_video_close(void) { t_hook = NULL; }
void test_video_draw(const RFRect *d) { (void)d; }
static uint32_t played;
bool pcm_sat_start(int rate, const int16_t *lead, int n) { (void)rate; (void)lead; (void)n; return true; }
int pcm_sat_space(void) { return 1 << 20; }
void pcm_sat_write(const int16_t *s, int n) { (void)s; (void)n; }
uint32_t pcm_sat_played(void) { return played; }
void pcm_sat_finish(const int16_t *t, int n) { (void)t; (void)n; }
void pcm_sat_stop(void) { }
size_t cd_sat_available(FILE *f) { (void)f; return 1 << 20; }
uint32_t sat_timer_us(void) { return g_us; }

int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    Video *v = open_path(argv[1]);
    if (!v) return 1;
    int pitch = (v->w + 7) & ~7;
    uint16_t *px = calloc((size_t)pitch * v->h, 2);
    for (int f = 0; !v->done; f++) {
        played = (uint32_t)((uint64_t)(f + 1) * (uint32_t)v->rate * v->fps_den / (uint32_t)v->fps_num);
        g_us += 50000;
        t_hook(t_ud, px, pitch);
        if (f % 20 == 0 || v->done) {
            char name[256]; snprintf(name, sizeof name, "%s/f%04d.ppm", argv[2], f);
            FILE *o = fopen(name, "wb"); fprintf(o, "P6 %d %d 255\n", v->w, v->h);
            for (int y = 0; y < v->h; y++) for (int x = 0; x < v->w; x++) {
                uint16_t p = px[y * pitch + x];
                unsigned char c[3] = { (unsigned char)((p & 31) << 3), (unsigned char)((p >> 5 & 31) << 3), (unsigned char)((p >> 10 & 31) << 3) };
                fwrite(c, 1, 3, o);
            }
            fclose(o);
        }
    }
    printf("decoded %d frames\n", v->next);
    return 0;
}
