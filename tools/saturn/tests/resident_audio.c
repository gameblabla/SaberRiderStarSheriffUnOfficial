/* Host regression harness for the actual Saturn backend. The runner supplies yaul.h
 * stubs and fixed-address RAM; no console or timing claims are made by this test. */
#include <assert.h>
#include <sys/mman.h>
#include "../../../src/platform/saturn/aud_sat.c"

static uint32_t clock_us = 1000000;
static unsigned disc_reads;
static bool auto_ack;
const char *plat_getenv(const char *name) { (void)name; return NULL; }
uint32_t sat_timer_us(void) {
    clock_us += 100;
    if (auto_ack) SCSP_SCIPD = 0;
    return clock_us;
}
void smpc_smc_sndoff_call(void) {}
void smpc_smc_sndon_call(void) {}
void cd_sat_cdda_update(void) {}
void cd_sat_cdda_stop(void) {}
void cd_sat_stream_stop(void) {}
void packs_release_type(uint32_t key, ResType type) { (void)key; (void)type; }
static PackEntry entry;
static uint8_t data[131072];
const PackEntry *packs_peek_type(uint32_t key, ResType type) {
    assert(type == RES_SAMPLE);
    AudSample *s = sample_find(key);
    assert(s);
    entry.declen = entry.size = s->bytes + 8;
    entry.data = data;
    return &entry;
}
const PackEntry *packs_find_type(uint32_t key, ResType type) {
    disc_reads++;
    return packs_peek_type(key, type);
}
static void ack(void) { SCSP_SCIPD = 0; driver_ack(); }

int main(int argc, char **argv) {
    assert(argc == 2);
    assert(mmap((void *)SND_RAM, 0x110000, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == (void *)SND_RAM);
    driver_ok = true;
    samples[0] = (AudSample){.key = 123, .addr = BANK_OFF, .bytes = 32, .samples = 44100};
    nsamples = 1;
    AudSample *s = &samples[0];
    int h = aud_play(s, R(1), false);
    aud_update(); /* queued PLAY must survive the first update, before acknowledgement */
    assert(aud_playing(h) && !voices[h & 255].started);
    assert(voices[h & 255].pending_action == ACT_PLAY);
    clock_us += 2000000; /* a delayed mailbox must not expire the queued voice */
    aud_update();
    assert(aud_playing(h));
    ack();
    assert(voices[h & 255].started);
    uint32_t end = voices[h & 255].end_us;
    clock_us = end - 1000;
    aud_update(); assert(aud_playing(h));
    clock_us = end + 1000;
    aud_update(); assert(!aud_playing(h));
    ack(); assert(!s->users && s->addr == BANK_OFF && !disc_reads);
    aud_unkeep(s); assert(s->addr == BANK_OFF);

    /* Replacement of the same sample while its previous PLAY awaits acknowledgement. */
    h = aud_play(s, R(1), false); send();
    aud_stop(h);
    int replacement = aud_play(s, R(1), false);
    ack();
    assert(!voices[replacement & 255].started);
    send(); ack();
    assert(voices[replacement & 255].started && !aud_playing(h));
    aud_stop(replacement); send(); ack();
    assert(s->users == 0);

    /* Expiry across the wrapping microsecond clock. */
    clock_us = UINT32_MAX - 500000;
    h = aud_play(s, R(1), false); send(); ack();
    aud_update(); assert(aud_playing(h));
    clock_us = voices[h & 255].end_us + 1000;
    aud_update(); assert(!aud_playing(h)); ack();
    s->addr = 0;
    assert(aud_play(s, R(1), false) == -1 && disc_reads == 0);

    /* Load the build's real manifest, test EVERY stage/hero bank and repeated playback.
     * Fake pack payloads have the manifest's exact padded size, so allocation and bounds
     * are exercised without mocking the allocator or scene loader. */
    FILE *f = fopen(argv[1], "rb"); assert(f);
    uint8_t row[12];
    assert(fread(row, 1, 8, f) == 8 && !memcmp(row, "SBN1", 4));
    nsamples = (int)rd32le(row + 4); assert(nsamples <= MAX_SAMPLES);
    memset(samples, 0, sizeof samples);
    for (int i = 0; i < nsamples; i++) {
        assert(fread(row, 1, 12, f) == 12);
        samples[i].key = rd32le(row); samples[i].bytes = rd32le(row + 4); samples[i].scene_mask = rd32le(row + 8);
    }
    fclose(f);
    memcpy(data, "ADPK\x44\xac\x00\x00\x00\x01", 10);
    auto_ack = true;
    for (int stage = 0; stage < 8; stage++) for (int hero = 0; hero < 4; hero++) {
        assert(aud_prepare_scene(stage, hero));
        uint32_t at = BANK_OFF, mask = 1u << (stage * 4 + hero);
        for (int i = 0; i < nsamples; i++) {
            if (samples[i].scene_mask & mask) { assert(samples[i].addr == at); at += samples[i].bytes; }
            else assert(!samples[i].addr);
        }
        assert(at == bank_next && at <= BANK_END);
        unsigned reads = disc_reads;
        for (int pass = 0; pass < 3; pass++) for (int i = 0; i < nsamples; i++) if (samples[i].addr) {
            h = aud_play(&samples[i], R(1), pass == 1);
            assert(h >= 0); send(); ack(); aud_stop(h); send(); ack();
            assert(samples[i].addr && !samples[i].users);
        }
        assert(reads == disc_reads);
    }
    /* A failed stop must leave all sample addresses intact. */
    uint32_t saved = bank_next;
    auto_ack = false; SCSP_SCIPD = IRQ_DRIVER;
    assert(!aud_prepare_scene(0, 0) && bank_next == saved);
    puts("PASS: queued/acknowledged expiry, generations, clock wrap, no playback reads, all 32 resident banks, stop timeout");
    return 0;
}
