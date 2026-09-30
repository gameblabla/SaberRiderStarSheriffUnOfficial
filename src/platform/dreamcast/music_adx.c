/* ADX block decoding adapted from libADX v1.0.1 (src/libadx.c).
 * Copyright (C) 2011-2013 Josh 'PH3NOM' Pearson
 * Copyright (C) 2024-2025 The KOS Team and contributors
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * The game's baker emits unencrypted version-3, encoding-3, 18-byte, 4-bit ADX. Small
 * tracks (including both victory tracks) are read completely before playback.
 * Larger tracks use two aligned 64 KiB reads ahead of the decoder, avoiding
 * libADX's unaligned stdio reads of one 36-byte stereo group at a time.
 * No reader or decoder thread owns, initializes, or shuts down KOS audio. */
#include "music_adx.h"
#include <kos.h>
#include <malloc.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ADX_CHUNK (64 * 1024)
#define ADX_SLOTS 2
#define ADX_RESIDENT_MAX (512 * 1024)

struct DcAdx {
    file_t fd;
    size_t file_size, file_pos, data_offset;
    uint8_t *resident, *chunk[ADX_SLOTS];
    size_t chunk_size[ADX_SLOTS], chunk_pos;
    unsigned read_slot;
    bool have_slot, reader_initialized, loop, failed;
    semaphore_t free_slots, full_slots;
    kthread_t *reader;
    atomic_bool quit;
    unsigned rate, channels;
    uint32_t samples, remaining;
    int coef[2], history[2][2];
    int16_t block[32 * 2];
    unsigned block_pos, block_frames;
};

static unsigned be16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }
static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void *read_worker(void *arg)
{
    DcAdx *a = arg;
    unsigned slot = 0;
    for (;;) {
        sem_wait(&a->free_slots);
        if (atomic_load(&a->quit)) break;
        ssize_t n = fs_read(a->fd, a->chunk[slot], ADX_CHUNK);
        if (n == 0 && a->loop && fs_seek(a->fd, 0, SEEK_SET) == 0)
            n = fs_read(a->fd, a->chunk[slot], ADX_CHUNK);
        a->chunk_size[slot] = n > 0 ? (size_t)n : 0;
        sem_signal(&a->full_slots);
        if (n <= 0) break;   /* the consumer treats an unexpected EOF as an error */
        slot = (slot + 1) % ADX_SLOTS;
    }
    return NULL;
}

static bool read_bytes(DcAdx *a, void *dst, size_t bytes)
{
    if (bytes > a->file_size - a->file_pos) return false;
    if (a->resident) {
        memcpy(dst, a->resident + a->file_pos, bytes);
        a->file_pos += bytes;
        return true;
    }
    uint8_t *out = dst;
    while (bytes) {
        if (!a->have_slot) {
            sem_wait(&a->full_slots);
            a->have_slot = true;
            a->chunk_pos = 0;
            if (!a->chunk_size[a->read_slot]) return false;
        }
        size_t n = a->chunk_size[a->read_slot] - a->chunk_pos;
        if (n > bytes) n = bytes;
        memcpy(out, a->chunk[a->read_slot] + a->chunk_pos, n);
        out += n; bytes -= n; a->chunk_pos += n; a->file_pos += n;
        if (a->chunk_pos == a->chunk_size[a->read_slot]) {
            a->have_slot = false;
            a->read_slot = (a->read_slot + 1) % ADX_SLOTS;
            sem_signal(&a->free_slots);
        }
    }
    return true;
}

static bool skip_bytes(DcAdx *a, size_t bytes)
{
    uint8_t scratch[256];
    while (bytes) {
        size_t n = bytes < sizeof scratch ? bytes : sizeof scratch;
        if (!read_bytes(a, scratch, n)) return false;
        bytes -= n;
    }
    return true;
}

void dc_adx_close(DcAdx *a)
{
    if (!a) return;
    atomic_store(&a->quit, true);
    if (a->reader) {
        sem_signal(&a->free_slots);
        thd_join(a->reader, NULL);
    }
    if (a->reader_initialized) {
        sem_destroy(&a->free_slots);
        sem_destroy(&a->full_slots);
    }
    if (a->fd >= 0) fs_close(a->fd);
    free(a->resident);
    for (unsigned i = 0; i < ADX_SLOTS; i++) free(a->chunk[i]);
    free(a);
}

DcAdx *dc_adx_open(const char *path, bool loop)
{
    DcAdx *a = calloc(1, sizeof *a);
    if (!a) return NULL;
    atomic_init(&a->quit, false);
    a->fd = fs_open(path, O_RDONLY);
    if (a->fd < 0) goto fail;
    ssize_t size = fs_total(a->fd);
    if (size < 30) goto fail;
    a->file_size = (size_t)size; a->loop = loop;
    if (a->file_size <= ADX_RESIDENT_MAX) {
        a->resident = memalign(32, (a->file_size + 31) & ~(size_t)31);
        if (!a->resident || fs_read(a->fd, a->resident, a->file_size) != size) goto fail;
        fs_close(a->fd); a->fd = -1;
    } else {
        sem_init(&a->free_slots, ADX_SLOTS); sem_init(&a->full_slots, 0);
        a->reader_initialized = true;
        for (unsigned i = 0; i < ADX_SLOTS; i++) {
            a->chunk[i] = memalign(32, ADX_CHUNK);
            if (!a->chunk[i]) goto fail;
        }
        a->reader = thd_create(false, read_worker, a);
        if (!a->reader) goto fail;
    }
    uint8_t header[24], signature[6];
    if (!read_bytes(a, header, sizeof header) || be16(header) != 0x8000 ||
        header[4] != 3 || header[5] != 18 || header[6] != 4 ||
        (header[7] != 1 && header[7] != 2) ||
        header[18] != 3 || header[19]) goto fail;
    a->channels = header[7]; a->rate = be32(header + 8);
    a->samples = a->remaining = be32(header + 12);
    a->data_offset = be16(header + 2) + 4;
    unsigned cutoff = be16(header + 16);
    if (!a->samples || a->rate < 1000 || a->rate > 48000 ||
        !cutoff || cutoff >= a->rate / 2 || a->data_offset < 30 ||
        a->data_offset > a->file_size ||
        ((uint64_t)a->samples + 31) / 32 * 18 * a->channels > a->file_size - a->data_offset)
        goto fail;
    if (!skip_bytes(a, a->data_offset - sizeof header - sizeof signature) ||
        !read_bytes(a, signature, sizeof signature) || memcmp(signature, "(c)CRI", 6)) goto fail;
    /* ADX's high-pass predictor, derived from the header's cutoff and rate.
     * At the baker's 44.1 kHz / 500 Hz these are libADX's coefficients / 4. */
    float root2 = sqrtf(2.0f);
    float x = root2 - cosf(6.283185307179586f * cutoff / a->rate);
    float y = root2 - 1.0f;
    float z = (x - sqrtf((x + y) * (x - y))) / y;
    a->coef[0] = (int)lrintf(8192.0f * z);
    a->coef[1] = (int)lrintf(-4096.0f * z * z);
    return a;
fail:
    dc_adx_close(a);
    return NULL;
}

unsigned dc_adx_rate(const DcAdx *a) { return a->rate; }
unsigned dc_adx_channels(const DcAdx *a) { return a->channels; }
bool dc_adx_done(const DcAdx *a)
{
    return a->failed || (!a->loop && !a->remaining && a->block_pos == a->block_frames);
}

static bool decode_group(DcAdx *a)
{
    if (!a->remaining) {
        if (!a->loop) return false;
        if (a->resident) a->file_pos = a->data_offset;
        else {
            /* The reader repeats the whole file. Consume its terminator, then
             * the next header, without seeking away from its pending reads. */
            if (!skip_bytes(a, a->file_size - a->file_pos)) return false;
            a->file_pos = 0;
            if (!skip_bytes(a, a->data_offset)) return false;
        }
        a->remaining = a->samples;
        memset(a->history, 0, sizeof a->history);
    }
    uint8_t group[18 * 2];
    if (!read_bytes(a, group, 18 * a->channels)) return false;
    for (unsigned ch = 0; ch < a->channels; ch++) {
        const uint8_t *p = group + 18 * ch;
        int scale = (int)be16(p), h1 = a->history[ch][0], h2 = a->history[ch][1];
        if (scale & 0x8000) return false;   /* an end marker is never audio */
        for (unsigned i = 0; i < 32; i++) {
            int nibble = (p[2 + i / 2] >> (i & 1 ? 0 : 4)) & 15;
            if (nibble & 8) nibble -= 16;
            int value = nibble * scale + ((a->coef[0] * h1 + a->coef[1] * h2) >> 12);
            if (value > 32767) value = 32767;
            if (value < -32768) value = -32768;
            a->block[i * a->channels + ch] = (int16_t)value;
            h2 = h1; h1 = value;
        }
        a->history[ch][0] = h1; a->history[ch][1] = h2;
    }
    a->block_frames = a->remaining < 32 ? a->remaining : 32;
    a->remaining -= a->block_frames; a->block_pos = 0;
    return true;
}

size_t dc_adx_read(DcAdx *a, int16_t *pcm, size_t frames)
{
    size_t done = 0;
    while (done < frames && !dc_adx_done(a)) {
        if (a->block_pos == a->block_frames && !decode_group(a)) {
            printf("music: invalid or unreadable ADX block at byte %lu\n", (unsigned long)a->file_pos);
            a->failed = true;
            break;
        }
        size_t n = a->block_frames - a->block_pos;
        if (n > frames - done) n = frames - done;
        memcpy(pcm + done * a->channels, a->block + a->block_pos * a->channels,
               n * a->channels * sizeof *pcm);
        a->block_pos += (unsigned)n; done += n;
    }
    return done;
}
