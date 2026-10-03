#pragma once
#include "pce_config.h"
/* Checkpoint format is independent of compiler structure padding. */
typedef struct { uint8_t version,stage,hero,phase,reserved[2],checksum_lo,checksum_hi; } PceSave;
extern volatile uint8_t pce_save_status;
bool save_load(PceSave *save);
bool save_store(uint8_t stage,uint8_t hero,uint8_t phase);
