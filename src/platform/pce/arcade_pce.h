#pragma once
#include "pce_config.h"
bool arcade_detect(void);
bool arcade_fill(uint32_t address, uint8_t value, uint16_t size);
bool arcade_seek(uint8_t port, uint32_t address);
bool arcade_read(uint8_t port, uint32_t address, void *out, uint16_t size);
bool arcade_write(uint8_t port, uint32_t address, const void *in, uint16_t size);
bool arcade_vram(uint32_t address, uint16_t word, uint16_t size);
uint8_t arcade_selftest(void);
