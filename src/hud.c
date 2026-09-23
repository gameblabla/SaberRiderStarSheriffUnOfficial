#include "hud.h"
#include "gfx.h"

static void rect(Ren *r, int x1, int y1, int x2, int y2, uint32_t argb)
{
    r_set_draw_blend(r, R_BLEND_BLEND);
    r_set_draw_color(r, (argb >> 16) & 255, (argb >> 8) & 255, argb & 255, argb >> 24);
    RFRect q = { (float)x1, (float)y1, (float)(x2 - x1), (float)(y2 - y1) };
    r_fill_rect(r, &q);
}

void hud_draw(Ren *r, int character, int difficulty, int lives, int hearts, int ammo)
{
    Sprite *font = sprite_get(0x87A5333C);
    int c11, c9, c6;
    if (difficulty == 0) { c11 = 0x3b; c9 = 0x3a; c6 = 0x39; }
    else if (difficulty == 2) { c11 = 0x18; c9 = 0x17; c6 = 0x16; }
    else { c11 = 0x2b; c9 = 0x2a; c6 = 0x29; }
    const uint32_t bg = 0x9f000000;
    rect(r, 8, 10, 9, c6, bg);
    rect(r, 9, 9, 10, c9, bg);
    if (difficulty == 2) rect(r, 10, 8, 0x18, c11, bg);
    else { rect(r, 10, 8, 0x16, c11, bg); rect(r, 0x16, 8, 0x17, c9, bg); rect(r, 0x17, 8, 0x18, c6, bg); }
    rect(r, 0x18, 8, 0x4b, 0x18, bg);
    rect(r, 0x4b, 9, 0x4c, 0x17, bg);
    rect(r, 0x4c, 10, 0x4d, 0x16, bg);
    if (!font) return;
    for (int x = 0; x < 0x50; x += 8)
        for (int gy = 0x26; gy < 0x3e; gy += 8)
            sprite_draw(font, (gy >> 3) * 16 + (x >> 3), (float)(x + 3), (float)(gy - 0x23), false);
    if (difficulty != 2) {
        sprite_draw(font, hearts >= 1 ? 0x0e : 0x4e, 0xc, 0x18, false);
        sprite_draw(font, hearts >= 2 ? 0x1e : 0x4e, 0xc, 0x20, false);
        if (difficulty == 0) {
            sprite_draw(font, hearts >= 3 ? 0x2e : 0x4e, 0xc, 0x28, false);
            sprite_draw(font, hearts >= 4 ? 0x3e : 0x4e, 0xc, 0x30, false);
        }
    }
    int a0 = 0xa0 + character * 3, a1 = a0 + 1, b0 = 0xb0 + character * 3, b1 = b0 + 1;
    sprite_draw(font, a0, 8, 8, false); sprite_draw(font, a1, 0x10, 8, false);
    sprite_draw(font, b0, 8, 0x10, false); sprite_draw(font, b1, 0x10, 0x10, false);
    if (lives < 10) sprite_draw(font, lives + 0x80, 0x1d, 0xe, false);
    else { sprite_draw(font, lives / 10 + 0x80, 0x1d, 0xe, false); sprite_draw(font, lives % 10 + 0x80, 0x22, 0xe, false); }
    sprite_draw(font, ammo + 0x80, 0x3c, 0xe, false);
}
