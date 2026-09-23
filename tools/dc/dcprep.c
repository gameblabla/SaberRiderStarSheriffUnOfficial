/* dcprep: pulls the demo's audio and video out of its packs for the Dreamcast disc (tools/dc/build_disc.py), with
 * the game's own pack reader, LZO1Z and sfx decoder, so the result is exactly what the PC version plays.
 *   dcprep list  <data_dir>                 id, type, size of every block
 *   dcprep sfx   <data_dir> <out_dir>       every sfx block -> <out>/<ID>.wav (PCM16, the demo's ADPCM decoded)
 *   dcprep music <data_dir> <out_dir>       every music block -> <out>/<ID>.ogg
 *   dcprep video <data_dir> <out_dir>       every video block -> <out>/<ID>.m4v (MPEG-4 ASP elementary stream) and
 *                                           its soundtrack as <out>/<ID>.ogg (MUPS) or <out>/<ID>.wav (RIFF ADPCM)
 * Build: cc -O2 -Isrc tools/dc/dcprep.c src/pack.c src/lzo1z.c src/platform/common/sfx_decode.c src/platform/common/mups.c */
#include "pack.h"
#include "platform/common/sfx_decode.h"
#include "platform/common/mups.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const PACKS[] = { "pack.pck", "common.pck", "levels.pck", "menu.pck", "level1.pck", "video.pck" };
static const char *TYPES[] = { "unknown", "font", "sprite", "cblock", "data", "sfx", "music", "video" };

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static void put16(FILE *f, int v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); }
static void put32(FILE *f, uint32_t v) { for (int i = 0; i < 4; i++) fputc((v >> (8 * i)) & 255, f); }
static bool write_wav(const char *path, const int16_t *pcm, int frames, int ch, int rate)
{
    FILE *f = fopen(path, "wb"); if (!f) return false;
    uint32_t bytes = (uint32_t)frames * ch * 2;
    fwrite("RIFF", 1, 4, f); put32(f, 36 + bytes); fwrite("WAVEfmt ", 1, 8, f); put32(f, 16);
    put16(f, 1); put16(f, ch); put32(f, rate); put32(f, rate * ch * 2); put16(f, ch * 2); put16(f, 16);
    fwrite("data", 1, 4, f); put32(f, bytes); fwrite(pcm, 1, bytes, f);
    fclose(f); return true;
}
static bool write_file(const char *path, const void *d, size_t n)
{
    FILE *f = fopen(path, "wb"); if (!f) return false;
    bool ok = fwrite(d, 1, n, f) == n; fclose(f); return ok;
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: dcprep list|sfx|music|video <data_dir> [out_dir]\n"); return 2; }
    const char *cmd = argv[1], *data = argv[2], *out = argc > 3 ? argv[3] : ".";
    int done = 0;
    for (size_t pi = 0; pi < sizeof PACKS / sizeof *PACKS; pi++) {
        char path[1024]; snprintf(path, sizeof path, "%s/%s", data, PACKS[pi]);
        Pack pk;
        if (!pack_load(&pk, path)) { pack_free(&pk); continue; }
        for (int i = 0; i < pk.count; i++) {
            PackEntry *pe = &pk.entries[i];
            char o[1024];
            if (!strcmp(cmd, "list")) { printf("%08X %-7s %8u %s\n", pe->id, TYPES[pe->type], pe->declen, PACKS[pi]); continue; }
            if (!strcmp(cmd, "sfx") && pe->type == RES_SFX) {
                const PackEntry *e = pack_find(&pk, pe->id); if (!e) continue;
                int frames, ch; int16_t *pcm = sfx_decode_pcm(e->data, e->size, &frames, &ch);
                snprintf(o, sizeof o, "%s/%08X.wav", out, pe->id);
                if (pcm && write_wav(o, pcm, frames, ch, 44100)) done++;
                else fprintf(stderr, "sfx %08X: failed\n", pe->id);
                free(pcm);
            } else if (!strcmp(cmd, "music") && pe->type == RES_MUSIC) {
                const PackEntry *e = pack_find(&pk, pe->id); if (!e) continue;
                size_t n; uint8_t *ogg = mups_to_ogg(e->data, e->size, &n);
                snprintf(o, sizeof o, "%s/%08X.ogg", out, pe->id);
                if (ogg && write_file(o, ogg, n)) done++;
                else fprintf(stderr, "music %08X: failed\n", pe->id);
                free(ogg);
            } else if (!strcmp(cmd, "video") && pe->type == RES_VIDEO) {
                const PackEntry *e = pack_find(&pk, pe->id); if (!e || e->size < 64) continue;
                const uint8_t *d = e->data;
                uint32_t audio_off = rd32(d + 8), audio_size = rd32(d + 12), nframes = rd32(d + 16);
                const uint8_t *sizes = d + 32, *p = d + 32 + nframes * 4;
                snprintf(o, sizeof o, "%s/%08X.m4v", out, pe->id);
                FILE *f = fopen(o, "wb"); if (!f) continue;
                for (uint32_t k = 0; k < nframes; k++) {
                    uint32_t sz = rd32(sizes + k * 4);
                    uint8_t head[16];
                    for (uint32_t b = 0; b < 16 && b < sz; b++) head[b] = (uint8_t)(p[b] - (0x35 + b));   /* the frame heads are obfuscated */
                    fwrite(head, 1, sz < 16 ? sz : 16, f);
                    if (sz > 16) fwrite(p + 16, 1, sz - 16, f);
                    p += sz;
                }
                fclose(f);
                printf("video %08X: %ux%u, %u frames\n", pe->id, rd32(d + 4) & 0xffff, rd32(d + 4) >> 16, nframes);
                if (audio_off + audio_size <= e->size && audio_size > 12) {
                    const uint8_t *a = d + audio_off;
                    if (!memcmp(a, "MUPS", 4)) {
                        size_t n; uint8_t *ogg = mups_to_ogg(a, audio_size, &n);
                        snprintf(o, sizeof o, "%s/%08X.ogg", out, pe->id);
                        if (ogg) write_file(o, ogg, n);
                        free(ogg);
                    } else if (!memcmp(a, "RIFF", 4)) {
                        int frames, ch; int16_t *pcm = sfx_decode_pcm(a, audio_size, &frames, &ch);
                        snprintf(o, sizeof o, "%s/%08X.wav", out, pe->id);
                        if (pcm) write_wav(o, pcm, frames, ch, 44100);
                        free(pcm);
                    }
                }
                done++;
            }
            if (pe->owned) { free((void *)pe->data); pe->data = NULL; pe->owned = false; }
        }
        pack_free(&pk);
    }
    if (strcmp(cmd, "list")) fprintf(stderr, "dcprep %s: %d written\n", cmd, done);
    return 0;
}
