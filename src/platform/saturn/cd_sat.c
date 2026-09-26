/* Files on the CD for the core's stdio (pack.c, assets.c, SABER.ENV): fopen over libyaul's cdfs file list, read-only.
 * Everything the disc builder writes sits in the ISO root under an 8.3 upper-case name, so a path is matched by its
 * last component, ignoring case (fopen("SaberRider/data/levels.pck") opens LEVELS.PCK).
 * Reads go sector by sector through the CD block: whole sectors straight into the caller's buffer when the file
 * position and the buffer allow it, the rest through a one-sector cache per open file.
 * The music is CD-DA (cd_sat_cdda_*, plan 7.1), which needs the same drive: stages load everything up front, and a
 * read that happens anyway takes the drive and the music comes back where it was once the reads stop. */
#include "sat_internal.h"
#include <yaul.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static cdfs_filelist_t filelist;
static bool fs_ready;
static unsigned long reads_total, bytes_total, seeks_total;

void cd_sat_init(void)
{
    enum { MAX_FILES = 64 };   /* the root holds ~15 files; libyaul's default (-1) allocates 4096 entries, 112 KB */
    cdfs_filelist_entry_t *entries = cdfs_entries_alloc(MAX_FILES);
    cdfs_config_default_set();
    cdfs_filelist_init(&filelist, entries, MAX_FILES);
    cdfs_filelist_root_read(&filelist);
    fs_ready = true;
    printf("cd: %u files\n", (unsigned)filelist.entries_count);
}

static const cdfs_filelist_entry_t *find(const char *path)
{
    if (!fs_ready) return NULL;
    const char *base = strrchr(path, '/'); base = base ? base + 1 : path;
    for (uint32_t i = 0; i < filelist.entries_count; i++) {
        const cdfs_filelist_entry_t *e = &filelist.entries[i];
        if (e->type != CDFS_ENTRY_TYPE_FILE) continue;
        const char *a = e->name, *b = base;
        while (*a && *a != ';' && *b && toupper((unsigned char)*a) == toupper((unsigned char)*b)) a++, b++;
        if ((*a == 0 || *a == ';') && *b == 0) return e;
    }
    return NULL;
}

const cdfs_filelist_entry_t *cd_sat_entry(const char *name) { return find(name); }

typedef struct {
    fad_t fad, fad_end;       /* the file's first sector and the one after its last */
    uint32_t size, pos;
    int32_t cached;           /* the sector in cache (relative to the file), -1 none */
    const char *name;
    uint8_t cache[2048] __attribute__((aligned(4)));
} CdFile;

/* The drive streams from where the last read ended: a read that continues it takes the sectors the CD block already
 * buffered; any other position resets the buffer and seeks (libyaul's cd_block_sectors_read seeks on every call:
 * ~120 ms each in mednafen, a level load was ~50 s). A stream runs to the end of the file (the drive pauses when the
 * CD block's buffer is full and goes on as sectors are taken). */
static fad_t st_next, st_end;
static bool st_on;

void cd_sat_stream_stop(void) { st_on = false; }   /* the decoder no longer needs the buffered data sectors */

static void cdda_interrupt(void);
static bool data_busy;          /* a synchronous disc read owns the drive while sectors are copied */

static bool stream_start(fad_t fad, uint32_t count)
{
    cdda_interrupt();
    st_on = false;
    if (cd_block_cmd_selector_reset(0, 0) || cd_block_cmd_cd_dev_connection_set(0) || cd_block_cmd_disk_play(0, fad, (int32_t)count))
        return false;
    st_next = fad; st_end = fad + count; st_on = true;
    seeks_total++;
    return true;
}

/* n whole sectors from fad into dst (2-byte aligned); file_end: the sector after the file's last */
static const char *read_name;
static bool music_on(void);
static bool read_sectors(fad_t fad, void *dst, uint32_t n, fad_t file_end)
{
    if (music_on()) printf("cd: a read under the music (it stops): %s, sector %u, %u\n", read_name ? read_name : "?", (unsigned)fad, (unsigned)n);
    reads_total++; bytes_total += n * 2048u;
    data_busy = true;
    bool ok = false;
    if (!st_on || fad != st_next || fad + n > st_end)
        if (!stream_start(fad, file_end > fad + n ? file_end - fad : n)) goto done;
    uint8_t *p = dst;
    while (n) {
        int32_t available;
        uint32_t spins = 0;
        while ((available = cd_block_cmd_sector_number_get(0)) <= 0) {
            if (available < 0 || ++spins > 2000000u) { st_on = false; goto done; }   /* error or stalled drive */
        }
        uint32_t ready = (uint32_t)available;
        if (ready > n) ready = n;
        if (cd_block_transfer_data(0, 0, p, ready * 2048u)) { st_on = false; goto done; }
        p += ready * 2048u; n -= ready; st_next += ready;
    }
    ok = true;
done:
    data_busy = false;
    return ok;
}

static size_t cd_read(FILE *f, unsigned char *dst, size_t n)
{
    CdFile *c = f->cookie;
    read_name = c->name;
    if (c->pos >= c->size) { f->flags |= F_EOF; return 0; }
    if (n > c->size - c->pos) n = c->size - c->pos;
    size_t done = 0;
    while (done < n) {
        uint32_t sec = c->pos / 2048, off = c->pos % 2048, left = (uint32_t)(n - done);
        if (off == 0 && left >= 2048 && ((uintptr_t)(dst + done) & 1) == 0) {
            uint32_t whole = left & ~2047u;
            if (!read_sectors(c->fad + sec, dst + done, whole / 2048u, c->fad_end)) break;
            done += whole; c->pos += whole;
            continue;
        }
        if (c->cached != (int32_t)sec) {
            if (!read_sectors(c->fad + sec, c->cache, 1, c->fad_end)) break;
            c->cached = (int32_t)sec;
        }
        uint32_t k = 2048 - off; if (k > left) k = left;
        memcpy(dst + done, c->cache + off, k);
        done += k; c->pos += k;
    }
    return done;
}

static off_t cd_seek(FILE *f, off_t off, int whence)
{
    CdFile *c = f->cookie;
    off_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (off_t)c->pos : (off_t)c->size;
    if (base + off < 0) return -1;
    c->pos = (uint32_t)(base + off);
    return (off_t)c->pos;
}

static size_t cd_write(FILE *f, const unsigned char *s, size_t n) { (void)f; (void)s; (void)n; return 0; }
static int cd_close(FILE *f) { free(f->cookie); free(f); return 0; }

FILE *fopen(const char *restrict path, const char *restrict mode)
{
    if (!path || !mode || mode[0] != 'r') return NULL;
    const cdfs_filelist_entry_t *e = find(path);
    if (!e) return NULL;
    /* FILE and its 2 KiB sector cache are CPU-owned.  Keep both in low work
     * RAM: gameplay can leave only a few KiB in the high-RAM heap, and movie
     * fopen() must not compete with VDP/renderer allocations there. */
    FILE *f = lw_malloc(sizeof *f);
    CdFile *c = lw_malloc(sizeof *c);
    if (!f || !c) { free(f); free(c); return NULL; }
    memset(f, 0, sizeof *f);
    c->fad = e->starting_fad; c->size = (uint32_t)e->size; c->pos = 0; c->cached = -1; c->name = e->name;
    c->fad_end = c->fad + (c->size + 2047u) / 2048u;
    f->fd = -1; f->cookie = c;
    f->read = cd_read; f->write = cd_write; f->seek = cd_seek; f->close = cd_close;
    return f;
}

void cd_sat_stats(unsigned long *reads, unsigned long *bytes, unsigned long *seeks) { *reads = reads_total; *bytes = bytes_total; if (seeks) *seeks = seeks_total; }

/* how much a sequential read of f can take now without waiting for the drive (video_sat.c streams with it): the rest of
 * the sector in cache and the sectors the CD block holds; starts the stream at f's position when it isn't there */
size_t cd_sat_available(FILE *f)
{
    CdFile *c = f->cookie;
    if (c->pos >= c->size) return 0;
    uint32_t sec = c->pos / 2048, off = c->pos % 2048;
    size_t avail = 0;
    if (off && c->cached == (int32_t)sec) { avail = 2048 - off; sec++; }
    fad_t next = c->fad + sec;
    if (next >= c->fad_end) return avail;
    if (!st_on || next != st_next) { if (!avail) stream_start(next, c->fad_end - next); return avail; }
    int32_t ready = cd_block_cmd_sector_number_get(0);
    if (ready < 0) st_on = false;
    else if (ready > 0) avail += (size_t)(uint32_t)ready * 2048u;
    return avail > c->size - c->pos ? c->size - c->pos : avail;
}

/* ---- CD-DA music ----
 * The CD block plays an audio track into the SCSP's CD input (the sound driver mixes it: aud_sat.c). Tracks come from
 * the TOC; a track loops with the drive's own repeat (0xF: for ever). A data read (stream_start) takes the drive: the
 * position is kept, and cd_sat_cdda_update resumes once the synchronous sector read releases the drive. */
typedef struct { uint16_t dtr, hirq, hirq_mask, cr1, cr2, cr3, cr4; } CdRegs;   /* libyaul's internal cd_block_regs */
extern int cd_block_cmd_execute(CdRegs *regs, CdRegs *status);
#define CD_HIRQ     (*(volatile uint16_t *)0x25890008u)
#define CD_DTR      (*(volatile uint16_t *)0x25890000u)
#define HIRQ_DRDY   0x0002u
#define HIRQ_CSCT   0x0004u
#define HIRQ_PEND   0x0010u
static uint32_t toc[102];       /* per track (1..99 at 0..98): control/address << 24 | FAD; [101] the lead-out */
static uint32_t playable_sectors[100];
static bool toc_ok;
static struct {
    int track;                  /* 0: none */
    bool loop, paused, pending, hold_pending, stop_pending;
    bool owned;                 /* the last music command succeeded and data has not taken the drive */
    bool partial;               /* playing on from where a read stopped it: the rest of the track, then a loop */
    fad_t at;                   /* where the music was when a read took the drive */
    uint32_t retry_at, status_at, start_at;
    uint8_t failures;
} cdda;

static bool music_on(void) { return cdda.track && cdda.owned && !cdda.pending && !cdda.paused; }

static bool toc_read(void)
{
    uint8_t st; uint16_t words;
    if (cd_block_cmd_toc_get(&st, &words) || words < 204) return false;
    for (uint32_t spins = 0; !(CD_HIRQ & HIRQ_DRDY); ) if (++spins > 2000000u) return false;
    for (int i = 0; i < 102; i++) { uint32_t hi = CD_DTR; toc[i] = hi << 16 | CD_DTR; }
    cd_block_cmd_data_transfer_end();
    return true;
}

static fad_t track_fad(int t) { return toc[t - 1] & 0xFFFFFFu; }
static fad_t track_end(int t)
{
    if (t > 0 && t < 100 && playable_sectors[t]) return track_fad(t) + playable_sectors[t];
    for (int k = t; k < 99; k++) if (toc[k] != 0xFFFFFFFFu) return track_fad(k + 1);
    return toc[101] & 0xFFFFFFu;
}

void cd_sat_cdda_track_length(int track, uint32_t sectors)
{
    if (track > 1 && track < 100) playable_sectors[track] = sectors;
}

static int play_cmd(uint16_t cr1, uint16_t cr2, uint16_t cr3, uint16_t cr4)
{
    CdRegs r = { .hirq_mask = HIRQ_PEND | HIRQ_CSCT, .cr1 = cr1, .cr2 = cr2, .cr3 = cr3, .cr4 = cr4 }, st;
    return cd_block_cmd_execute(&r, &st);
}

static bool cdda_start(void)
{
    if (!cdda.track || cdda.paused || !toc_ok) return false;
    cdda.owned = cdda.pending = false;
    st_on = false;
    if (cd_block_cmd_cd_dev_connection_set(0xFF)) {
        cdda.owned = false; cdda.retry_at = sat_timer_us() + 250000u;
        if (cdda.failures < 3) cdda.failures++;
        printf("cd: CD-DA connection failed for track %d\n", cdda.track);
        return false;
    }
    int result;
    if (cdda.partial) {
        fad_t end = track_end(cdda.track);
        if (cdda.at < track_fad(cdda.track) || cdda.at >= end) {
            cdda.partial = false;
            if (!cdda.loop) { cdda.track = 0; cdda.owned = false; return false; }
        }
        if (cdda.partial) {
            uint32_t count = end - cdda.at;
            if (!count) { cdda.owned = false; return false; }
            result = play_cmd(0x1080 | (cdda.at >> 16), (uint16_t)cdda.at,
                              (uint16_t)(0x0080 | ((count >> 16) & 0xFFFFu)), (uint16_t)count);
        } else {
            result = play_cmd(0x1000, (uint16_t)(cdda.track << 8 | 1),
                              (uint16_t)((cdda.loop ? 0x0F : 0x00) << 8), (uint16_t)(cdda.track << 8 | 99));
        }
    } else {   /* track mode: the track's index 1 to its last index, repeated (0xF: for ever) or once */
        result = play_cmd(0x1000, (uint16_t)(cdda.track << 8 | 1),
                          (uint16_t)((cdda.loop ? 0x0F : 0x00) << 8), (uint16_t)(cdda.track << 8 | 99));
    }
    if (result) {
        cdda.owned = false;
        cdda.retry_at = sat_timer_us() + (cdda.failures < 3 ? 250000u << cdda.failures : 1000000u);
        if (cdda.failures < 3) cdda.failures++;
        printf("cd: CD-DA play failed for track %d (status %d)\n", cdda.track, result);
        return false;
    }
    cdda.owned = true; cdda.pending = true; cdda.failures = 0;
    cdda.start_at = sat_timer_us(); cdda.status_at = cdda.start_at + 100000u;
    return true;
}

static bool cdda_hold(void)
{
    return play_cmd(0x11FF, 0xFFFF, 0xFFFF, 0xFFFF) == 0;   /* seek to "no change": the drive pauses there */
}

static void cdda_interrupt(void)
{
    if (!cdda.owned) return;
    cd_block_status_t s;
    if (!cdda.paused && !cd_block_cmd_status_get(&s)) {
        fad_t end = track_end(cdda.track);
        if (s.fad >= track_fad(cdda.track) && s.fad < end) {
            cdda.at = s.fad; cdda.partial = true;
        } else if (s.fad >= end && !cdda.loop) {
            cdda.track = 0; cdda.partial = false;   /* a completed one-shot must not be revived by a later data read */
        }
    }
    cdda.owned = false;
    cdda.pending = false;
    cdda.retry_at = 0;
}

bool cd_sat_cdda_play(int track, bool loop)
{
    if (!toc_ok) toc_ok = toc_read();
    if (!toc_ok || track < 2 || track > 99 || toc[track - 1] == 0xFFFFFFFFu) return false;
    cdda.track = track; cdda.loop = loop; cdda.paused = false; cdda.partial = false;
    cdda.pending = cdda.hold_pending = cdda.stop_pending = false;
    cdda.at = 0; cdda.failures = 0; cdda.retry_at = 0;
    return cdda_start();
}

void cd_sat_cdda_stop(void)
{
    bool need_hold = cdda.owned || cdda.hold_pending || cdda.stop_pending;
    bool stopped = !need_hold || cdda_hold();
    cdda.track = 0; cdda.owned = cdda.pending = cdda.partial = cdda.hold_pending = false;
    cdda.stop_pending = !stopped; cdda.retry_at = stopped ? 0 : sat_timer_us() + 250000u;
    if (!stopped) printf("cd: CD-DA stop/hold failed; retry pending\n");
}

void cd_sat_cdda_pause(bool pause)
{
    if (!cdda.track || pause == cdda.paused) return;
    cdda.paused = pause;
    if (!pause) cdda.hold_pending = false;
    if (!cdda.owned) return;   /* a read has the drive: cd_sat_cdda_update brings the music back */
    if (pause) {
        cdda.pending = false;
        cd_block_status_t s;
        if (!cd_block_cmd_status_get(&s) && s.fad >= track_fad(cdda.track) && s.fad < track_end(cdda.track)) { cdda.at = s.fad; cdda.partial = true; }
        if (!cdda_hold()) {
            cdda.owned = false; cdda.hold_pending = true; cdda.retry_at = sat_timer_us() + 250000u;
            printf("cd: CD-DA pause failed; retry pending\n");
        }
    } else {
        cdda.owned = false; cdda_start();
    }
}

void cd_sat_cdda_update(void)
{
    if (data_busy) return;
    if (cdda.stop_pending) {
        uint32_t now = sat_timer_us();
        if ((int32_t)(now - cdda.retry_at) >= 0) {
            if (cdda_hold()) { cdda.stop_pending = false; cdda.owned = false; }
            else cdda.retry_at = now + 1000000u;
        }
        return;
    }
    if (cdda.hold_pending) {
        uint32_t now = sat_timer_us();
        if ((int32_t)(now - cdda.retry_at) >= 0) {
            if (cdda_hold()) { cdda.hold_pending = false; cdda.owned = true; }
            else cdda.retry_at = now + 1000000u;
        }
        return;
    }
    if (!cdda.track || cdda.paused) return;
    if (!cdda.owned) {
        uint32_t now = sat_timer_us();
        if ((int32_t)(now - cdda.retry_at) >= 0) cdda_start();
        return;
    }
    uint32_t now = sat_timer_us();
    if ((int32_t)(now - cdda.status_at) < 0) return;
    cdda.status_at = now + 100000u;
    cd_block_status_t s;
    if (cdda.pending) {
        fad_t start = track_fad(cdda.track), end = track_end(cdda.track);
        if (!cd_block_cmd_status_get(&s) && s.fad >= start && s.fad < end) {
            cdda.pending = false;
            return;
        }
        if (now - cdda.start_at >= 5000000u) {
            cdda.owned = cdda.pending = false;
            cdda.retry_at = now + (cdda.failures < 3 ? 250000u << cdda.failures : 1000000u);
            if (cdda.failures < 3) cdda.failures++;
            printf("cd: CD-DA status did not confirm track %d\n", cdda.track);
        }
        return;
    }
    bool ended = cdda.partial && (CD_HIRQ & HIRQ_PEND);
    if (!ended) {
        if (cd_block_cmd_status_get(&s)) return;
        ended = s.fad >= track_end(cdda.track);
    }
    if (ended) {
        cdda.owned = false;
        if (cdda.partial) {
            cdda.partial = false;
            if (cdda.loop) cdda_start(); else cdda.track = 0;
        } else if (!cdda.loop) {
            cdda.track = 0;
        }
    }
}
