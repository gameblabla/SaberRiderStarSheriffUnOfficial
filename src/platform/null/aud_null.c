/* platform/aud.h with no sound (headless tests, a port's bring-up): nothing loads, every play fails quietly */
#include "../aud.h"
#include <stddef.h>

bool aud_init(void) { return true; }
void aud_shutdown(void) { }
void aud_update(void) { }
AudSample *aud_sample_pack(uint32_t id) { (void)id; return NULL; }
AudSample *aud_sample_file(const char *path) { (void)path; return NULL; }
void aud_keep(AudSample *s, bool loop) { (void)s; (void)loop; }
void aud_prefetch(AudSample *s) { (void)s; }
int  aud_play(AudSample *s, float gain, bool loop) { (void)s; (void)gain; (void)loop; return -1; }
void aud_set_gain(int voice, float gain) { (void)voice; (void)gain; }
void aud_stop(int voice) { (void)voice; }
bool aud_playing(int voice) { (void)voice; return false; }
bool aud_music_play(uint32_t id, bool loop) { (void)id; (void)loop; return false; }
void aud_music_stop(void) { }
void aud_music_gain(float g) { (void)g; }
void aud_music_pause(bool pause) { (void)pause; }
