#include "gfx.h"
#include "lcd.h"
#include <stdlib.h>

static uint16_t *B;   // band buffer (big-endian RGB565 for the panel)
static int BY0, BH;

#define SWAP(c) ((uint16_t)(((c) >> 8) | ((c) << 8)))

static inline uint16_t unswap(uint16_t c) { return SWAP(c); }

void gfx_band(uint16_t *buf, int y0, int h)
{
    B = buf; BY0 = y0; BH = h;
}

uint16_t gfx_blend(uint16_t a, uint16_t b, uint8_t al)
{
    uint32_t ra = a >> 11, ga = (a >> 5) & 63, ba = a & 31;
    uint32_t rb = b >> 11, gb = (b >> 5) & 63, bb = b & 31;
    uint32_t r = (ra * (255 - al) + rb * al) / 255;
    uint32_t g = (ga * (255 - al) + gb * al) / 255;
    uint32_t bl = (ba * (255 - al) + bb * al) / 255;
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

static inline void px(int x, int y, uint16_t c)
{
    if ((unsigned)x >= LCD_W) return;
    y -= BY0;
    if ((unsigned)y >= (unsigned)BH) return;
    B[y * LCD_W + x] = SWAP(c);
}

static inline void px_blend(int x, int y, uint16_t c, uint8_t a)
{
    if ((unsigned)x >= LCD_W) return;
    y -= BY0;
    if ((unsigned)y >= (unsigned)BH) return;
    uint16_t *p = &B[y * LCD_W + x];
    *p = SWAP(gfx_blend(unswap(*p), c, a));
}

void gfx_fill(uint16_t c)
{
    uint16_t s = SWAP(c);
    for (int i = 0; i < LCD_W * BH; i++) B[i] = s;
}

void gfx_rect(int x, int y, int w, int h, uint16_t c)
{
    int y0 = y > BY0 ? y : BY0;
    int y1 = (y + h) < (BY0 + BH) ? (y + h) : (BY0 + BH);
    int x0 = x < 0 ? 0 : x;
    int x1 = (x + w) > LCD_W ? LCD_W : (x + w);
    if (y0 >= y1 || x0 >= x1) return;
    uint16_t s = SWAP(c);
    for (int yy = y0; yy < y1; yy++) {
        uint16_t *row = &B[(yy - BY0) * LCD_W];
        for (int xx = x0; xx < x1; xx++) row[xx] = s;
    }
}

void gfx_vgrad(int x, int y, int w, int h, uint16_t top, uint16_t bot)
{
    int y0 = y > BY0 ? y : BY0;
    int y1 = (y + h) < (BY0 + BH) ? (y + h) : (BY0 + BH);
    for (int yy = y0; yy < y1; yy++) {
        uint16_t c = gfx_blend(top, bot, (uint8_t)((yy - y) * 255 / (h > 1 ? h - 1 : 1)));
        gfx_rect(x, yy, w, 1, c);
    }
}

void gfx_frame(int x, int y, int w, int h, uint16_t c)
{
    gfx_rect(x, y, w, 1, c);
    gfx_rect(x, y + h - 1, w, 1, c);
    gfx_rect(x, y, 1, h, c);
    gfx_rect(x + w - 1, y, 1, h, c);
}

void gfx_rrect(int x, int y, int w, int h, int r, uint16_t c)
{
    if (y + h <= BY0 || y >= BY0 + BH) return;
    for (int yy = 0; yy < h; yy++) {
        int inset = 0;
        int dy = -1;
        if (yy < r) dy = r - yy - 1;
        else if (yy >= h - r) dy = yy - (h - r);
        if (dy >= 0) {
            // x inset so that (r-inset)^2 + dy^2 <= r^2
            int b2 = (2 * dy + 1) * (2 * dy + 1), r2 = 4 * r * r;
            while (inset < r && (2 * (r - inset) - 1) * (2 * (r - inset) - 1) + b2 > r2) inset++;
        }
        gfx_rect(x + inset, y + yy, w - 2 * inset, 1, c);
    }
}

void gfx_circle(int cx, int cy, int r, uint16_t c)
{
    for (int dy = -r; dy <= r; dy++) {
        int yy = cy + dy;
        if (yy < BY0 || yy >= BY0 + BH) continue;
        int dx = 0;
        while ((dx + 1) * (dx + 1) + dy * dy <= r * r) dx++;
        gfx_rect(cx - dx, yy, 2 * dx + 1, 1, c);
    }
}

void gfx_line(int x0, int y0, int x1, int y1, uint16_t c)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        px(x0, y0, c);
        px(x0, y0 + 1, c);   // 2px thick
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void gfx_sprite4(const uint8_t *p, int w, int h, int x, int y, int s, const uint16_t *pal)
{
    if (y + h * s <= BY0 || y >= BY0 + BH) return;
    int stride = (w + 1) / 2;
    int sy0 = (BY0 - y) / s; if (sy0 < 0) sy0 = 0;
    int sy1 = (BY0 + BH - y + s - 1) / s; if (sy1 > h) sy1 = h;
    for (int sy = sy0; sy < sy1; sy++) {
        const uint8_t *row = p + sy * stride;
        for (int sx = 0; sx < w; sx++) {
            uint8_t v = row[sx >> 1];
            uint8_t idx = (sx & 1) ? (v & 0x0F) : (v >> 4);
            if (!idx) continue;
            gfx_rect(x + sx * s, y + sy * s, s, s, pal[idx]);
        }
    }
}

static const glyph_t *glyph(const font_t *f, char ch)
{
    uint8_t c = (uint8_t)ch;
    if (c < f->first || c >= f->first + f->count) c = '?';
    return &f->g[c - f->first];
}

int gfx_text_w(const font_t *f, const char *s)
{
    int w = 0;
    while (*s) w += glyph(f, *s++)->adv;
    return w;
}

int gfx_text(const font_t *f, int x, int y, const char *s, uint16_t c)
{
    if (y + f->line_h <= BY0 || y >= BY0 + BH) return x + gfx_text_w(f, s);
    for (; *s; s++) {
        const glyph_t *g = glyph(f, *s);
        const uint8_t *d = f->data + g->off;
        for (int gy = 0; gy < g->h; gy++) {
            int yy = y + g->yo + gy;
            if (yy < BY0 || yy >= BY0 + BH) continue;
            for (int gx = 0; gx < g->w; gx++) {
                int i = gy * g->w + gx;
                uint8_t v = d[i >> 1];
                uint8_t a = (i & 1) ? (v & 0x0F) : (v >> 4);
                if (!a) continue;
                if (a == 15) px(x + g->xo + gx, yy, c);
                else px_blend(x + g->xo + gx, yy, c, a * 17);
            }
        }
        x += g->adv;
    }
    return x;
}

void gfx_text_c(const font_t *f, int cx, int y, const char *s, uint16_t c)
{
    gfx_text(f, cx - gfx_text_w(f, s) / 2, y, s, c);
}
