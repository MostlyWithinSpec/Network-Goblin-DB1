#pragma once
#include <stdint.h>
#include "assets.h"

// Band renderer: every primitive takes absolute screen coordinates and is
// clipped to the band currently being drawn. The scene is re-drawn once per
// band, so there is never a full 115 KB framebuffer in RAM.

#define RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

void gfx_band(uint16_t *buf, int y0, int h);
void gfx_fill(uint16_t c);
void gfx_vgrad(int x, int y, int w, int h, uint16_t top, uint16_t bottom);
void gfx_rect(int x, int y, int w, int h, uint16_t c);
void gfx_frame(int x, int y, int w, int h, uint16_t c);
void gfx_rrect(int x, int y, int w, int h, int r, uint16_t c);
void gfx_circle(int cx, int cy, int r, uint16_t c);
void gfx_line(int x0, int y0, int x1, int y1, uint16_t c);
void gfx_sprite4(const uint8_t *px, int w, int h, int x, int y, int scale, const uint16_t *pal);
int  gfx_text(const font_t *f, int x, int y, const char *s, uint16_t c);   // y = top; returns end x
int  gfx_text_w(const font_t *f, const char *s);
void gfx_text_c(const font_t *f, int cx, int y, const char *s, uint16_t c);
uint16_t gfx_blend(uint16_t a, uint16_t b, uint8_t alpha /*0..255 of b*/);
