#include "video_pce.h"
/* video_sprite checks a complete object's SAT/scanline/clip budget before
 * committing pieces. Optional actors can therefore be refused atomically. */
bool video_sprite_optional(uint16_t id,int16_t x,int16_t y,bool flip,uint8_t scale) {
    extern uint8_t sprite_optional;
    sprite_optional=1;
    bool admitted=video_sprite(id,x,y,flip,scale);
    sprite_optional=0;
    if(!admitted)++pce_metrics.dropped_cosmetic;
    return admitted;
}
