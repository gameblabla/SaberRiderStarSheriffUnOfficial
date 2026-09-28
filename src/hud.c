#include "hud.h"
#include "gfx.h"

static void rect(Ren *r, int x1, int y1, int x2, int y2, uint32_t argb)
{
    r_set_draw_blend(r, R_BLEND_BLEND);
    r_set_draw_color(r, (argb >> 16) & 255, (argb >> 8) & 255, argb & 255, argb >> 24);
    RFRect q = { r_int(x1), r_int(y1), r_int(x2 - x1), r_int(y2 - y1) };
    r_fill_rect(r, &q);
}

void hud_draw(Ren *r, int character, int max_hearts, int lives, int hearts, int ammo)
{
    Sprite *font = sprite_get(0x87A5333C);
    if (max_hearts > 4) max_hearts = 4;
    /* the panel's left column runs down past the heart slots (8 px each from 0x18): 0x2b for the original's 2, 0x3b for 4 */
    int c11 = max_hearts > 0 ? 0x1b + max_hearts * 8 : 0x18, c9 = c11 - 1, c6 = c11 - 2;
    const uint32_t bg = (uint32_t)HUD_ALPHA(0x9f) << 24;
    rect(r, 8, 10, 9, c6, bg);
    rect(r, 9, 9, 10, c9, bg);
    if (max_hearts <= 0) rect(r, 10, 8, 0x18, c11, bg);
    else { rect(r, 10, 8, 0x16, c11, bg); rect(r, 0x16, 8, 0x17, c9, bg); rect(r, 0x17, 8, 0x18, c6, bg); }
    rect(r, 0x18, 8, 0x4b, 0x18, bg);
    rect(r, 0x4b, 9, 0x4c, 0x17, bg);
    rect(r, 0x4c, 10, 0x4d, 0x16, bg);
    if (!font) return;
    for (int x = 0; x < 0x50; x += 8)
        for (int gy = 0x26; gy < 0x3e; gy += 8)
            sprite_draw(font, (gy >> 3) * 16 + (x >> 3), r_int(x + 3), r_int(gy - 0x23), false);
    for (int i = 0; i < max_hearts; i++) sprite_draw(font, hearts > i ? 0x0e + i * 0x10 : 0x4e, R(0xc), r_int(0x18 + i * 8), false);
    int a0 = 0xa0 + character * 3, a1 = a0 + 1, b0 = 0xb0 + character * 3, b1 = b0 + 1;
    sprite_draw(font, a0, R(8), R(8), false); sprite_draw(font, a1, R(0x10), R(8), false);
    sprite_draw(font, b0, R(8), R(0x10), false); sprite_draw(font, b1, R(0x10), R(0x10), false);
    if (lives < 10) sprite_draw(font, lives + 0x80, R(0x1d), R(0xe), false);
    else { sprite_draw(font, lives / 10 + 0x80, R(0x1d), R(0xe), false); sprite_draw(font, lives % 10 + 0x80, R(0x22), R(0xe), false); }
    sprite_draw(font, ammo + 0x80, R(0x3c), R(0xe), false);
}
