/* Exercise the actual Saturn video backend with decoder/audio/renderer stubs.
 * Pixel decoding and hardware timing are covered separately by movie_hold.py. */
#include <assert.h>
#include "../../../src/platform/saturn/video_sat.c"

static unsigned resets, ends, holds, closes, unbinds, stream_stops, tasks;
static playback_status_t next_status;
void film_audio_reset(void) { resets++; }
void aud_movie_end(void) { ends++; }
void rsat_video_hold(void) { holds++; }
void rsat_video_close(void) { closes++; }
void film_buff_io_clear(void) { unbinds++; }
void cd_sat_stream_stop(void) { stream_stops++; }
void cpk_task(decode_work_t *w) { tasks++; w->play_status = next_status; }
void cpk_display_finished(decode_work_t *w) { w->isDisplayReady = false; }
void rsat_video_draw(const RFRect *dst) { (void)dst; }

static Video *clip(bool streamed, bool keep)
{
    Video *v = calloc(1, sizeof *v);
    assert(v);
    v->work = calloc(1, sizeof *v->work);
    v->sample_mem = malloc(SAMPLE_BUFFER_BYTES);
    v->ram = streamed ? NULL : malloc(128);
    v->stream = streamed ? tmpfile() : NULL;
    assert(v->work && v->sample_mem && (streamed ? v->stream != NULL : v->ram != NULL));
    v->w = v->logical_w = 256; v->h = v->logical_h = 104;
    v->surface = true; v->keep_bufs = keep;
    v->work->play_status = PLAY;
    return v;
}

int main(void)
{
    volatile uint16_t pixels[8] = {0x8123, 0x8456, 0x8789};
    playback_status_t terminal[] = {END, ERROR, PAUSE};
    for (unsigned status = 0; status < 3; status++)
    for (int streamed = 0; streamed < 2; streamed++)
    for (int keep = 0; keep < 2; keep++) {
        Video *v = clip(streamed, keep);
        decode_work_t *work = v->work;
        uint8_t *samples = v->sample_mem;
        unsigned r = resets, e = ends, h = holds, c = closes, s = stream_stops;
        next_status = terminal[status];
        hook(v, pixels, 256);
        assert(v->done && v->surface && !v->work && !v->sample_mem && !v->ram && !v->stream);
        assert(resets == r + 1 && ends == e + 1 && holds == h + 1 && closes == c);
        assert(stream_stops == s + streamed && !video_update(v, R(1)));
        int w, hgt;
        video_size(v, &w, &hgt);
        assert(w == 256 && hgt == 104);
        video_draw_rect(v, NULL, 0, 0, R(256), R(104));
        assert(pixels[0] == 0x8123 && pixels[1] == 0x8456 && pixels[2] == 0x8789);
        if (keep) assert(spare_work == work && spare_samples == samples);
        unsigned t = tasks, u = unbinds;
        hook(v, pixels, 256); /* even a stale callback cannot decode again */
        video_close(v);     /* closing the held image cannot reset game audio */
        assert(tasks == t && resets == r + 1 && ends == e + 1 && unbinds == u && closes == c + 1);
        if (keep) assert(spare_work == work && spare_samples == samples);
        spares_release();
    }
    Video *v = clip(true, false);
    unsigned e = ends;
    video_close(v); /* early skip releases playback exactly once */
    assert(ends == e + 1);
    v = clip(false, true); v->surface = false;
    e = ends;
    video_close(v); /* discarding a preload must not restart the game driver */
    assert(ends == e);
    spares_release();
    puts("PASS: END/ERROR/PAUSE, RAM/stream cleanup, retained image, reusable power buffers, close after end, skip and unused preload");
}
