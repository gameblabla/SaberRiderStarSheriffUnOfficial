#pragma once
/* E2DM bitmap fonts: header {FF 80 00 FF, u32 count, u16 width[count]} + sprite blob; glyph = char - 0x21. */
#include "platform/render.h"
#include "platform/plat.h"
#include <stdint.h>
typedef struct { uint32_t id; int count; const uint8_t *widths; struct Sprite *spr; int h; } Font;
Font *font_get(uint32_t id);
int   font_text_width(const Font *f, const char *s);
int   font_text_width_n(const Font *f, const char *s, int n);   /* the first n characters */
void  font_draw(const Font *f, const char *s, real x, real y, uint8_t r, uint8_t g, uint8_t b);
void  font_draw_n(const Font *f, const char *s, int n, real x, real y, uint8_t r, uint8_t g, uint8_t b);
/* word-wrap text into up to max lines no wider than width px; returns the line count */
int   font_wrap(const Font *f, const char *text, real width, char out[][96], int max);
void  font_draw_scaled(const Font *f, const char *s, real x, real y, real scale, uint8_t r, uint8_t g, uint8_t b);   /* glyphs and advances x scale */
