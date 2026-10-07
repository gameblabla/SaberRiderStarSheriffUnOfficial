/* Files on the CD for the core's stdio (pack.c, assets.c, SABER.ENV): fopen over libyaul's cdfs file list, read-only.
 * Everything the disc builder writes sits in the ISO root under an 8.3 upper-case name, so a path is matched by its
 * last component, ignoring case (fopen("SaberRider/data/levels.pck") opens LEVELS.PCK).
 * Reads go sector by sector through the CD block: whole sectors straight into the caller's buffer when the file
 * position and the buffer allow it, the rest through a read-ahead buffer shared by the open files (RA_SECTORS).
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
/* The read-ahead: disc sectors ra_fad.. (ra_count of them), filled with as many as the CD block already holds. Every
 * CD block command takes milliseconds on the real firmware (at once in mednafen and Ymir's HLE): one sector a read was
 * ~60 sectors a second, the intro FMV (~88) starved and stalled on the console. */
enum { RA_SECTORS = 16 };
static uint8_t *ra_buf;
static fad_t ra_fad;
static uint32_t ra_count;
static unsigned long reads_total, bytes_total, seeks_total;
static unsigned long waits_total, stale_total, fails_total;   /* cd_sat_report: WAIT answers, stale sectors dropped, failed reads */
static unsigned long retries_total;
static int cd_cmd(uint16_t hirq_mask, uint16_t cr1, uint16_t cr2, uint16_t cr3, uint16_t cr4);

void cd_sat_init(void)
{
    /* CDC_CdInit: maximum ECC (six passes) and 15 finite read retries;
     * never select the modes which deliver uncorrectable data. No soft reset. */
    if (cd_cmd(0, 0x0480, 0xFFFF, 0, 0x050F)) {
        printf("cd: cannot configure error correction\n");
        return;
    }
    enum { MAX_FILES = 64 };   /* the root holds ~15 files; libyaul's default (-1) allocates 4096 entries, 112 KB */
    cdfs_filelist_entry_t *entries = cdfs_entries_alloc(MAX_FILES);
    cdfs_config_default_set();
    cdfs_filelist_init(&filelist, entries, MAX_FILES);
    cdfs_filelist_root_read(&filelist);
    ra_buf = lw_malloc(RA_SECTORS * 2048u);
    fs_ready = ra_buf != NULL;
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
    const char *name;
} CdFile;

/* The drive streams from where the last read ended: a read that continues it takes the sectors the CD block already
 * buffered; any other position resets the buffer and seeks (libyaul's cd_block_sectors_read seeks on every call:
 * ~120 ms each in mednafen, a level load was ~50 s). A stream runs to the end of the file (the drive pauses when the
 * CD block's buffer is full and goes on as sectors are taken). */
static fad_t st_next, st_end;
static bool st_on;

void cd_sat_stream_stop(void) { st_on = false; }   /* the decoder no longer needs the buffered data sectors */

typedef struct { uint16_t hirq_mask, cr1, cr2, cr3, cr4; } CdRegs;
/* The host harness supplies a CD firmware model at this register boundary. */
#ifndef CD_REG_READ
#define CD_REG_READ(off) (*(volatile uint16_t *)(0x25890000u + (off)))
#define CD_REG_WRITE(off, value) (*(volatile uint16_t *)(0x25890000u + (off)) = (value))
#define CD_CPU_DMA 1
#endif
#define CD_HIRQ     CD_REG_READ(0x08)
#define CD_DTR      CD_REG_READ(0x00)
#ifndef CD_DATA_LONG
#ifdef CD_CPU_DMA
#define CD_DATA_LONG() (*(volatile uint32_t *)0x25810000u)
#else
static uint32_t cd_data_long(void) { uint32_t hi=CD_DTR;return hi<<16|CD_DTR; }
#define CD_DATA_LONG() cd_data_long()
#endif
#endif
#define HIRQ_CMOK   0x0001u
#define HIRQ_DRDY   0x0002u
#define HIRQ_CSCT   0x0004u
#define HIRQ_PEND   0x0010u
#define HIRQ_ESEL   0x0040u
#define HIRQ_EHST   0x0080u
#define HIRQ_SCDQ   0x0400u
enum { COMMAND_US = 2000000u, SECTOR_US = 5000000u, READ_US = 20000000u, READ_RETRIES = 3 };
static uint16_t pending_flags;
static uint8_t drive_status;
static bool transfer_active, command_inflight, command_sent;
static void hirq_clear(uint16_t bits) { CD_REG_WRITE(0x08, (uint16_t)(0x3FFFu & ~bits)); }
static bool expired(uint32_t start, uint32_t us) { return (uint32_t)(sat_timer_us() - start) >= us; }
static bool hirq_wait(uint16_t bits, uint32_t us)
{
    uint32_t start = sat_timer_us();
    while ((CD_HIRQ & bits) != bits) if (expired(start, us)) return false;
    return true;
}
static bool completion_wait(uint16_t flags)
{
    flags &= pending_flags;
    if (!hirq_wait(flags, COMMAND_US)) return false;
    pending_flags &= (uint16_t)~flags;
    return true;
}
static void response_read(CdRegs *st)
{
    st->cr1 = CD_REG_READ(0x18); st->cr2 = CD_REG_READ(0x1C);
    st->cr3 = CD_REG_READ(0x20); st->cr4 = CD_REG_READ(0x24);
}
/* Periodic responses allow status polling without sending commands (CDC 3.2/3.3).
 * Only the main CPU's foreground code owns the command registers. */
static bool periodic_status(void)
{
    if (!(CD_HIRQ & HIRQ_SCDQ) || !(CD_HIRQ & HIRQ_CMOK) || command_inflight) return false;
    uint8_t sr = cpu_intc_mask_get(); cpu_intc_mask_set(15);
    hirq_clear(HIRQ_SCDQ);
    CdRegs st;
    response_read(&st);
    cpu_intc_mask_set(sr);
    uint8_t status = (uint8_t)(st.cr1 >> 8);
    if (status == 0xFF || !(status & 0x20)) return false;
    drive_status = status & 0x0F;
    return true;
}
static bool drive_unavailable(void) { return drive_status == 6 || drive_status == 7; }
static void cdda_interrupt(void);
static int sel_cmd(uint16_t cr1, uint16_t cr2, uint16_t cr3, uint16_t cr4);
static int32_t sectors_ready(void);
static int32_t sector_fad(uint16_t pos);
static bool sectors_drop(uint16_t sectors);
static bool sectors_take(void *dst, uint32_t sectors);
static bool transfer_end(uint32_t *words);
static bool stream_recover(void);
static bool data_busy;          /* a synchronous disc read owns the drive while sectors are copied */

/* Filter 0 passes only the sectors of this stream (FAD range) to partition 0 and drops the rest: without the range a
 * sector still coming from the drive's previous play (another file, or the position a pause left it at) could land in
 * the partition just cleared and be taken as the first sector asked for - a pack block read as garbage (a sound sample
 * failing its ADPK check and the stage refusing to start). */
static bool stream_start(fad_t fad, uint32_t count)
{
    cdda_interrupt();
    st_on = false;
    if (!transfer_end(NULL)) return false;
    int e, step = 0;
    /* Disconnect before changing the selector: an old play must not deposit
     * sectors in the partition while it is being reset/reconfigured. */
    if ((++step, e = sel_cmd(0x3000, 0, 0xFF00, 0)) ||
        (++step, e = sel_cmd(0x4000 | (fad >> 16), (uint16_t)fad, (uint16_t)((count >> 16) & 0xFF), (uint16_t)count)) ||   /* filter 0: FAD range */
        (++step, e = sel_cmd(0x4440, 0, 0, 0)) ||                                    /* filter 0: range mode */
        (++step, e = sel_cmd(0x4603, 0x00FF, 0, 0)) ||                               /* filter 0: true -> partition 0, false -> none */
        (++step, e = sel_cmd(0x4800, 0, 0, 0)) ||                                    /* reset selector 0: partition 0 emptied */
        (++step, e = sel_cmd(0x3000, 0, 0x0000, 0)) ||                               /* the drive into filter 0 */
        (++step, e = cd_cmd(HIRQ_PEND | HIRQ_CSCT, 0x1080 | (fad >> 16), (uint16_t)fad, (uint16_t)(0x0080 | ((count >> 16) & 0xFF)), (uint16_t)count))) {
        printf("cd: stream start failed at command %d (%d), sector %u\n", step, e, (unsigned)fad);
        return false;
    }
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
    uint8_t *p = dst;
    uint32_t started = sat_timer_us(), progress = started;
    unsigned retries = 0;
    while (n) {
        if (expired(started, READ_US)) goto done;
        if (!st_on || fad != st_next || fad + n > st_end)
            if (!stream_start(fad, file_end > fad + n ? file_end - fad : n)) goto retry;
        int32_t available;
        /* Clear before querying, so a sector arriving during the command keeps
         * CSCT set. An empty partition is polled via HIRQ, never command 51. */
        hirq_clear(HIRQ_CSCT);
        while ((available = sectors_ready()) <= 0) {
            if (available < 0) goto retry;
            while (!(CD_HIRQ & HIRQ_CSCT)) {
                if ((periodic_status() && (drive_unavailable() || drive_status == 9 || drive_status == 10)) ||
                    expired(progress, SECTOR_US) || expired(started, READ_US)) goto retry;
            }
            hirq_clear(HIRQ_CSCT);
        }
        uint32_t ready = (uint32_t)available;
        if (ready > n) ready = n;
        /* only the sector asked for: the filter's range is the rest of the file, and sectors the drive was still
         * reading from its previous position in the same file pass it - SND.PCK's sector 203 taken for sector 80, a
         * sound sample read as garbage and stage 5 refused after stage 4. Stale ones come first; drop them. */
        int32_t got = sector_fad(0);
        if (got < 0) goto retry;
        if ((fad_t)got != st_next) {
            stale_total++;
            /* A forward gap cannot be repaired by draining the rest of the file.
             * Restart at the missing sector; old sectors below it may be dropped. */
            if ((fad_t)got > st_next || expired(progress, SECTOR_US) || !sectors_drop(1)) goto retry;
            continue;
        }
        if (ready > 1 && sector_fad((uint16_t)(ready - 1)) != (int32_t)(st_next + ready - 1)) ready = 1;
        if (!sectors_take(p, ready)) goto retry;
        p += ready * 2048u; n -= ready; fad += ready; st_next = fad;
        progress = sat_timer_us();
        continue;
retry:
        st_on = false; ra_count = 0;
        if (drive_unavailable() || retries == READ_RETRIES || expired(started, READ_US)) goto done;
        retries++; retries_total++;
        printf("cd: retry %u/%u reading %s at sector %u (status %02X, hirq %04X)\n",
               retries, READ_RETRIES, read_name ? read_name : "?", (unsigned)fad, drive_status, CD_HIRQ);
        if (!stream_recover()) goto done;
        progress = sat_timer_us();
    }
    ok = true;
done:
    if (!ok) {
        transfer_end(NULL);   /* also close the final failed attempt */
        st_on = false; ra_count = 0; fails_total++;
        printf("cd: read failed: %s, sector %u, after %u ms\n", read_name ? read_name : "?",
               (unsigned)fad, (unsigned)(sat_timer_us() - started) / 1000u);
    }
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
        /* Consume read-ahead before the direct aligned path. A previous
         * unaligned packet may have already removed these sectors from the
         * CD partition; reading them again would seek backwards every other
         * movie packet and discard the drive's streaming lead. */
        fad_t cached = c->fad + sec;
        if (cached >= ra_fad && cached < ra_fad + ra_count) {
            uint32_t from = (cached - ra_fad) * 2048u + off;
            uint32_t take = ra_count * 2048u - from;
            if (take > left) take = left;
            memcpy(dst + done, ra_buf + from, take);
            done += take; c->pos += take;
            continue;
        }
        if (off == 0 && left >= 2048 && ((uintptr_t)(dst + done) & 1) == 0) {
            uint32_t whole = left & ~2047u;
            if (!read_sectors(c->fad + sec, dst + done, whole / 2048u, c->fad_end)) break;
            done += whole; c->pos += whole;
            continue;
        }
        fad_t at = c->fad + sec;
        if (at < ra_fad || at >= ra_fad + ra_count) {   /* the read-ahead: at least this sector, more if they are in */
            uint32_t k = 1, most = c->fad_end - at < RA_SECTORS ? c->fad_end - at : RA_SECTORS;
            if (st_on && at == st_next) { int32_t r = sectors_ready(); if (r > 1) k = (uint32_t)r < most ? (uint32_t)r : most; }
            ra_count = 0;
            if (!read_sectors(at, ra_buf, k, c->fad_end)) break;
            ra_fad = at; ra_count = k;
        }
        uint32_t from = (at - ra_fad) * 2048u + off, k = ra_count * 2048u - from;
        if (k > left) k = left;
        memcpy(dst + done, ra_buf + from, k);
        done += k; c->pos += k;
    }
    if (done < n) f->flags |= F_ERR;
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
    c->fad = e->starting_fad; c->size = (uint32_t)e->size; c->pos = 0; c->name = e->name;
    c->fad_end = c->fad + (c->size + 2047u) / 2048u;
    f->fd = -1; f->cookie = c;
    f->read = cd_read; f->write = cd_write; f->seek = cd_seek; f->close = cd_close;
    return f;
}

void cd_sat_stats(unsigned long *reads, unsigned long *bytes, unsigned long *seeks) { *reads = reads_total; *bytes = bytes_total; if (seeks) *seeks = seeks_total; }
unsigned long cd_sat_failed_reads(void) { return fails_total; }

/* the disc's work since the last report (a stage's load: game.c lock_scene) */
void cd_sat_report(void)
{
    static unsigned long r0, b0, s0, w0, st0, f0, rt0;
    printf("cd: %lu reads (%lu KB), %lu seeks, %lu WAIT answers, %lu stale sectors, %lu failed reads, %lu retries\n", reads_total - r0,
           (bytes_total - b0) / 1024, seeks_total - s0, waits_total - w0, stale_total - st0, fails_total - f0, retries_total - rt0);
    r0 = reads_total; b0 = bytes_total; s0 = seeks_total; w0 = waits_total; st0 = stale_total; f0 = fails_total;
    rt0 = retries_total;
}

/* how much a sequential read of f can take now without waiting for the drive (video_sat.c streams with it): the rest of
 * the read-ahead and the sectors the CD block holds; starts the stream at f's position when it isn't there */
size_t cd_sat_available(FILE *f)
{
    CdFile *c = f->cookie;
    if (c->pos >= c->size) return 0;
    uint32_t off = c->pos % 2048;
    fad_t next = c->fad + c->pos / 2048;
    size_t avail = 0;
    if (next >= ra_fad && next < ra_fad + ra_count) { avail = (ra_fad + ra_count - next) * 2048u - off; next = ra_fad + ra_count; off = 0; }
    if (next >= c->fad_end) return avail > c->size - c->pos ? c->size - c->pos : avail;
    if (!st_on || next != st_next) { if (!avail) stream_start(next, c->fad_end - next); return avail; }
    int32_t ready = sectors_ready();
    if (ready < 0) st_on = false;
    else if (ready > 0) avail += (size_t)(uint32_t)ready * 2048u - off;
    return avail > c->size - c->pos ? c->size - c->pos : avail;
}

/* ---- CD-DA music ----
 * The CD block plays an audio track into the SCSP's CD input (the sound driver mixes it: aud_sat.c). Tracks come from
 * the TOC; a track loops with the drive's own repeat (0xF: for ever). A data read (stream_start) takes the drive: the
 * position is kept, and cd_sat_cdda_update resumes once the synchronous sector read releases the drive. */
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

static bool toc_read(void);

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

/* CDC 3.2: <=30 commands/6.7 ms and <=60/16.7 ms. Space ALL writes by
 * 300 us, including WAIT retries and CD-DA status. Keep interrupts enabled
 * while waiting: the FRT clock and diagnostic vblank must continue running.
 * No interrupt or slave code accesses these registers; only the short register
 * transactions need masking. libyaul's executor masks the whole spin wait,
 * uses an iteration timeout and clears HIRQ with a racy read/modify/write. */
static int cd_exec(CdRegs *r, CdRegs *st)
{
    static uint32_t last_command;
    uint32_t started = sat_timer_us();
    uint16_t owned = r->hirq_mask & (HIRQ_ESEL | HIRQ_EHST);
    command_sent = false;
    memset(st, 0, sizeof *st);
    if (!completion_wait(owned)) return -1;
    for (;;) {
        if (!hirq_wait(HIRQ_CMOK, COMMAND_US)) return -1;
        /* Consume a late response after a previous communication timeout. */
        if (command_inflight) { CdRegs late; response_read(&late); command_inflight = false; }
        while (!expired(last_command, 300u)) { }
        uint8_t sr = cpu_intc_mask_get(); cpu_intc_mask_set(15);
        last_command = sat_timer_us();
        hirq_clear(HIRQ_CMOK | r->hirq_mask);
        pending_flags |= owned;
        command_sent = true;
        command_inflight = true;
        CD_REG_WRITE(0x18, r->cr1); CD_REG_WRITE(0x1C, r->cr2);
        CD_REG_WRITE(0x20, r->cr3); CD_REG_WRITE(0x24, r->cr4);
        cpu_intc_mask_set(sr);
        if (!hirq_wait(HIRQ_CMOK, COMMAND_US)) return -1;
        sr = cpu_intc_mask_get(); cpu_intc_mask_set(15);
        response_read(st);
        command_inflight = false;
        cpu_intc_mask_set(sr);
        uint8_t status = (uint8_t)(st->cr1 >> 8);
        if (status != 0xFF) drive_status = status & 0x0F;
        if (status != 0xFF && !(status & 0x80)) return 0;
        /* REJECT/WAIT never raise a completion flag: no operation was started.
         * Keep that fact in software (CDC 3.2(6)); do not wait for a fake EHST. */
        pending_flags &= (uint16_t)~owned;
        if (status == 0xFF) return status;
        waits_total++;
        if (expired(started, COMMAND_US)) return status;
        /* Give the firmware a drive update to make progress between WAITs. */
        uint32_t wait = sat_timer_us();
        while (!expired(wait, 16700u)) { }
    }
}

/* The CD block commands this file issues, not libyaul's cd_block_cmd_*: those check the response's low byte (flags and
 * CD-DA repeat count) against the status codes, so once a looping track has repeated an odd number of times (1 & ERROR)
 * every one of them "fails" - the next track's connection never set (the boss music never starting) and no data read
 * possible (the victory screen blank) until something else restarts the play. The status code is the high byte. */
static int cd_cmd_st(uint16_t hirq_mask, uint16_t cr1, uint16_t cr2, uint16_t cr3, uint16_t cr4, CdRegs *st)
{
    CdRegs r = { .hirq_mask = hirq_mask, .cr1 = cr1, .cr2 = cr2, .cr3 = cr3, .cr4 = cr4 };
    int err = cd_exec(&r, st);
    if (err) return err;
    uint8_t code = (uint8_t)(st->cr1 >> 8) & 0x0F;
    return code == 0x06 || code == 0x07 || code == 0x09 || code == 0x0A ? -code : 0;   /* open, no disc, error, fatal */
}
static int cd_cmd(uint16_t hirq_mask, uint16_t cr1, uint16_t cr2, uint16_t cr3, uint16_t cr4)
{
    CdRegs st;
    return cd_cmd_st(hirq_mask, cr1, cr2, cr3, cr4, &st);
}
/* a selector command (device connection, filter, reset): the CD block finishes it after the answer and raises ESEL;
 * the next one waits for that (the real firmware answers WAIT to one sent before) */
static int sel_cmd(uint16_t cr1, uint16_t cr2, uint16_t cr3, uint16_t cr4)
{
    int err = cd_cmd(HIRQ_ESEL, cr1, cr2, cr3, cr4);
    if (err) return err;
    return completion_wait(HIRQ_ESEL) ? 0 : -1;
}

/* partition 0's sector count (libyaul's cd_block_cmd_sector_number_get has the same status misread) */
static int32_t sectors_ready(void)
{
    CdRegs st;
    return cd_cmd_st(0, 0x5100, 0, 0x0000, 0, &st) ? -1 : (int32_t)st.cr4;
}

/* the FAD of partition 0's sector at pos (get sector info), -1 on an error */
static int32_t sector_fad(uint16_t pos)
{
    CdRegs st;
    return cd_cmd_st(0, 0x5400, pos, 0x0000, 0, &st) ? -1 : (int32_t)((uint32_t)(st.cr1 & 0xFF) << 16 | st.cr2);
}

/* partition 0's first `sectors` sectors into dst (2-byte aligned) and out of the partition. libyaul's
 * cd_block_transfer_data runs the same misread status check on "get then delete sector data": the command has
 * started the transfer when it "fails", and it returns without reading the data or ending the transfer - the CD
 * block then refuses every command after it (a white screen after MISSION ACCOMPLISHED, once the stage music had
 * looped). The transfer is always ended here. */
static bool sectors_drop(uint16_t sectors)   /* partition 0's first sectors, unread */
{
    if (cd_cmd(HIRQ_EHST, 0x6200, 0, 0x0000, sectors)) return false;
    return completion_wait(HIRQ_EHST);
}
/* A transfer accepted by the firmware must be closed, including a short or
 * discarded transfer. Wait for DRDY before DataEnd (CDC 3.4), then EHST before
 * touching the partition again (CDC_GetdelSctData). A rejected command starts
 * no transfer and must not run this sequence. */
static bool transfer_end(uint32_t *words)
{
    if (!transfer_active) return completion_wait(HIRQ_EHST);
    if (!hirq_wait(HIRQ_DRDY, COMMAND_US)) return false;
    CdRegs r = { .cr1 = 0x0600 }, st;
    int err = cd_exec(&r, &st);
    if (err) return false;
    transfer_active = false;
    hirq_clear(HIRQ_DRDY);
    if (words) *words = (uint32_t)(st.cr1 & 0xFF) << 16 | st.cr2;
    return completion_wait(HIRQ_EHST);
}
/* The data is read only once DRDY says it is there: EHST alone (the command over without a transfer) used to be taken
 * for it too, and the words read from the port then were not the sectors - the intro's stream garbage on the console,
 * the decoder writing outside its picture. The CD block's own count of the words sent (end data transfer's answer)
 * must be the sectors asked for; a short one fails the read and the stream starts again from the same sector. */
static bool sectors_take(void *dst, uint32_t sectors)
{
    if (!transfer_end(NULL)) return false;
    CdRegs r = { .hirq_mask = HIRQ_EHST | HIRQ_DRDY, .cr1 = 0x6300, .cr4 = (uint16_t)sectors }, st;
    int err = cd_exec(&r, &st);
    if (err > 0) return false;  /* WAIT/REJECT: nothing to end */
    transfer_active = command_sent;  /* also close a transfer whose response timed out */
    if (err || !hirq_wait(HIRQ_DRDY, COMMAND_US)) return false;
    /* Sega's CD communication supplement requires CPU accesses in longword
     * units. SH-2 DMAC can write low work RAM; SCU DMA cannot. Channel 0 is
     * owned only during this synchronous foreground read. */
#ifdef CD_CPU_DMA
    if(!((uintptr_t)dst&3u)) {
    cpu_dmac_channel_wait(0);
    cpu_cache_area_purge(dst,sectors*2048u);
    const cpu_dmac_cfg_t cfg={.channel=0,.src_mode=CPU_DMAC_SOURCE_FIXED,
        .dst_mode=CPU_DMAC_DESTINATION_INCREMENT,.stride=CPU_DMAC_STRIDE_4_BYTES,
        .src=0x25810000u,.dst=(uint32_t)(uintptr_t)dst,.len=sectors*2048u,.ihr=NULL};
    cpu_dmac_channel_config_set(&cfg);cpu_dmac_channel_start(0);cpu_dmac_channel_wait(0);
    cpu_cache_area_purge(dst,sectors*2048u);
    } else
#endif
    {
        uint16_t *d=dst;
        for(uint32_t i=0;i<sectors*512u;i++){uint32_t value=CD_DATA_LONG();d[2*i]=(uint16_t)(value>>16);d[2*i+1]=(uint16_t)value;}
    }
    uint32_t words = 0;
    bool ended = transfer_end(&words);
    if (!ended || words != sectors * 1024u) {
        printf("cd: transfer of %u sectors sent %u words (ended %d)\n", (unsigned)sectors, (unsigned)words, ended);
        return false;
    }
    return true;
}

static bool toc_read(void)
{
    if (!transfer_end(NULL)) return false;
    CdRegs r = { .hirq_mask = HIRQ_DRDY, .cr1 = 0x0200 }, st;
    int err = cd_exec(&r, &st);
    if (err > 0) return false;
    transfer_active = command_sent;
    if (err || !hirq_wait(HIRQ_DRDY, COMMAND_US)) { transfer_end(NULL); return false; }
    bool ok = st.cr2 == 204;
    if (ok) for (int i = 0; i < 102; i++) toc[i] = CD_DATA_LONG();
    uint32_t words = 0;
    return transfer_end(&words) && ok && words == 204;
}

static bool wait_drive(uint8_t state)
{
    uint32_t start = sat_timer_us();
    do {
        if (periodic_status()) {
            if (drive_unavailable()) return false;
            if (drive_status == state) return true;
        }
    } while (!expired(start, SECTOR_US));
    return false;
}

/* Sega Technical Bulletins #5/#21: retry reads, PAUSE and wait for PAUSE
 * before reissuing. Discard the old partition and seek from the first sector
 * not successfully transferred; Get/Delete may have erased an entire batch. */
static bool stream_recover(void)
{
    if (!transfer_end(NULL) || !completion_wait(HIRQ_ESEL)) return false;
    if (drive_unavailable()) return false;
    if (drive_status == 10) {  /* FATAL: home-position seek, then PAUSE (#21) */
        CdRegs home = { .cr1 = 0x1100 }, st;
        hirq_clear(HIRQ_SCDQ);
        if (cd_exec(&home, &st) || !wait_drive(0)) return false;
    }
    CdRegs pause = { .cr1 = 0x11FF, .cr2 = 0xFFFF }, st;
    hirq_clear(HIRQ_SCDQ);
    if (cd_exec(&pause, &st) || !wait_drive(1)) return false;
    return true;
}

static int status_get(cd_block_status_t *s)
{
    CdRegs st;
    int err = cd_cmd_st(0, 0, 0, 0, 0, &st);
    if (err) return err;
    s->cd_status = (uint8_t)(st.cr1 >> 8);
    s->fad = (uint32_t)(st.cr3 & 0xFF) << 16 | st.cr4;
    return 0;
}

static int play_cmd(uint16_t cr1, uint16_t cr2, uint16_t cr3, uint16_t cr4)
{
    CdRegs r = { .hirq_mask = HIRQ_PEND | HIRQ_CSCT, .cr1 = cr1, .cr2 = cr2, .cr3 = cr3, .cr4 = cr4 }, st;
    return cd_exec(&r, &st);
}

static bool cdda_start(void)
{
    if (!cdda.track || cdda.paused || !toc_ok) return false;
    cdda.owned = cdda.pending = false;
    st_on = false;
    if (!transfer_end(NULL) || sel_cmd(0x3000, 0, 0xFF00, 0)) {   /* the drive to no filter: audio goes to the SCSP */
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
    return play_cmd(0x11FF, 0xFFFF, 0, 0) == 0;   /* seek to "no change": the drive pauses there */
}

static void cdda_interrupt(void)
{
    if (!cdda.owned) return;
    cd_block_status_t s;
    if (!cdda.paused && !status_get(&s)) {
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
        if (!status_get(&s) && s.fad >= track_fad(cdda.track) && s.fad < track_end(cdda.track)) { cdda.at = s.fad; cdda.partial = true; }
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
        if (!status_get(&s) && s.fad >= start && s.fad < end) {
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
        if (status_get(&s)) return;
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
