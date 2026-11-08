#include "gemu/video_wii.h"
#include "cgafont8x8.h"
#include "vgafont16.h"

#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

static GXRModeObj *rmode;
static void       *xfb[2];
static int         back;
static uint8_t    *canvas;
static int         cw, ch;

/* Per palette entry: Y, Cb, Cr (BT.601, studio range). */
static uint8_t pal_y[256], pal_cb[256], pal_cr[256];
static uint32_t pal_word[256];   /* two pixels of one colour: Y Cb Y Cr */
static uint8_t *prev;            /* last presented canvas, for phosphor */
static bool     phosphor;

static uint8_t clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v; }

void wii_video_set_color(int idx, uint32_t rgb)
{
    if (idx < 0 || idx > 255) return;
    int r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    pal_y[idx]  = clamp8(( 66 * r + 129 * g +  25 * b + 128) / 256 + 16);
    pal_cb[idx] = clamp8((-38 * r -  74 * g + 112 * b + 128) / 256 + 128);
    pal_cr[idx] = clamp8((112 * r -  94 * g -  18 * b + 128) / 256 + 128);
    pal_word[idx] = ((uint32_t)pal_y[idx] << 24) | ((uint32_t)pal_cb[idx] << 16) |
                    ((uint32_t)pal_y[idx] << 8) | pal_cr[idx];
}

void wii_video_init(void)
{
    VIDEO_Init();
    rmode = VIDEO_GetPreferredMode(NULL);

    /* 16:9 TVs: narrow the VI output so square pixels stay square. */
    if (CONF_GetAspectRatio() == CONF_ASPECT_16_9) {
        rmode->viWidth   = 678;
        rmode->viXOrigin = (VI_MAX_WIDTH_PAL - 678) / 2;
    }

    xfb[0] = SYS_AllocateFramebuffer(rmode);
    xfb[1] = SYS_AllocateFramebuffer(rmode);
    cw = rmode->fbWidth;
    ch = rmode->xfbHeight;
    canvas = memalign(32, (size_t)cw * ch);
    memset(canvas, 0, (size_t)cw * ch);

    VIDEO_Configure(rmode);
    VIDEO_ClearFrameBuffer(rmode, xfb[0], COLOR_BLACK);
    VIDEO_ClearFrameBuffer(rmode, xfb[1], COLOR_BLACK);
    DCFlushRange(xfb[0], (u32)VIDEO_GetFrameBufferSize(rmode));
    DCFlushRange(xfb[1], (u32)VIDEO_GetFrameBufferSize(rmode));
    VIDEO_SetNextFramebuffer(xfb[0]);
    VIDEO_SetBlack(false);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (rmode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();
    back = 1;

    wii_video_set_color(WII_COL_BLACK,    0x000000);
    wii_video_set_color(WII_COL_WHITE,    0xF0F0F0);
    wii_video_set_color(WII_COL_GREY,     0x8A8A8A);
    wii_video_set_color(WII_COL_DARK,     0x202430);
    wii_video_set_color(WII_COL_ACCENT,   0xFFCC00);
    wii_video_set_color(WII_COL_SELECT,   0x2D4F8A);
    wii_video_set_color(WII_COL_ERROR,    0xFF5050);
    wii_video_set_color(WII_COL_GAME_BG,  0x000000);
    wii_video_set_color(WII_COL_GAME_FG,  0xFFFFFF);
    wii_video_set_color(WII_COL_KEY,      0x303848);
    wii_video_set_color(WII_COL_KEY_HELD, 0xFFCC00);
    wii_video_set_color(WII_COL_BAR_BG,    0x1E2A40);
    wii_video_set_color(WII_COL_BAR_HI,    0x3C5A8C);
    wii_video_set_color(WII_COL_MENU_BG,   0xF4F4EE);
    wii_video_set_color(WII_COL_MENU_FG,   0x000000);
    wii_video_set_color(WII_COL_MENU_DIS,  0x9A9A9A);
    wii_video_set_color(WII_COL_MENU_HI,   0x000080);
    wii_video_set_color(WII_COL_MENU_HIFG, 0xFFFFFF);
    wii_video_set_color(WII_COL_BORDER,    0x404048);
    wii_video_set_color(WII_COL_BTN_FACE,  0xC0C0C0);
    wii_video_set_color(WII_COL_BTN_DARK,  0x808080);
}

int wii_video_width(void)  { return cw; }
int wii_video_height(void) { return ch; }

void wii_video_clear(uint8_t c) { memset(canvas, c, (size_t)cw * ch); }

void wii_video_fill(int x, int y, int w, int h, uint8_t c)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > cw) w = cw - x;
    if (y + h > ch) h = ch - y;
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++)
        memset(canvas + (size_t)(y + j) * cw + x, c, (size_t)w);
}

void wii_video_frame(int x, int y, int w, int h, int t, uint8_t c)
{
    wii_video_fill(x, y, w, t, c);
    wii_video_fill(x, y + h - t, w, t, c);
    wii_video_fill(x, y, t, h, c);
    wii_video_fill(x + w - t, y, t, h, c);
}

int wii_video_text_width(int scale, const char *s)
{
    return (int)strlen(s) * CGA_FONT_WIDTH * scale;
}

int wii_video_text(int x, int y, int scale, uint8_t c, const char *s)
{
    for (; *s; s++, x += CGA_FONT_WIDTH * scale) {
        const uint8_t *g = &cgafont8x8[(uint8_t)*s * CGA_FONT_HEIGHT];
        for (int row = 0; row < CGA_FONT_HEIGHT; row++) {
            uint8_t bits = g[row];
            if (!bits) continue;
            for (int col = 0; col < CGA_FONT_WIDTH; col++)
                if (bits & (0x80u >> col))
                    wii_video_fill(x + col * scale, y + row * scale, scale, scale, c);
        }
    }
    return x;
}

int wii_video_text16(int x, int y, uint8_t c, const char *s)
{
    for (; *s; s++, x += FONT_WIDTH) {
        const uint8_t *g = &vgafont16[(uint8_t)*s * FONT_HEIGHT];
        for (int row = 0; row < FONT_HEIGHT; row++) {
            int py = y + row;
            if (py < 0 || py >= ch || !g[row]) continue;
            uint8_t *line = canvas + (size_t)py * cw;
            for (int col = 0; col < FONT_WIDTH; col++)
                if ((g[row] & (0x80u >> col)) && x + col >= 0 && x + col < cw)
                    line[x + col] = c;
        }
    }
    return x;
}

int wii_video_textf(int x, int y, int scale, uint8_t c, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return wii_video_text(x, y, scale, c, buf);
}

void wii_video_blit_mono(const uint8_t *src, int sw, int sh,
                     int x, int y, int scale, uint8_t fg, uint8_t bg)
{
    for (int sy = 0; sy < sh; sy++) {
        int dy = y + sy * scale;
        if (dy < 0 || dy + scale > ch) continue;
        uint8_t *line = canvas + (size_t)dy * cw;
        for (int sx = 0; sx < sw; sx++) {
            int dx = x + sx * scale;
            if (dx < 0 || dx + scale > cw) continue;
            memset(line + dx, src[sy * sw + sx] ? fg : bg, (size_t)scale);
        }
        for (int r = 1; r < scale; r++)   /* replicate the scanline */
            if (dy + r < ch)
                memcpy(canvas + (size_t)(dy + r) * cw + x,
                       line + x, (size_t)(sw * scale));
    }
}

uint8_t *wii_video_canvas(void) { return canvas; }

void wii_video_set_phosphor(bool on)
{
    if (on && !prev) {
        prev = memalign(32, (size_t)cw * ch);
        if (!prev) return;
        memcpy(prev, canvas, (size_t)cw * ch);
    }
    phosphor = on;
}

uint32_t wii_video_convert_us;

/* One YUYV word from two canvas pixels (chroma averaged). */
static inline uint32_t pair_word(uint8_t a, uint8_t b)
{
    if (a == b) return pal_word[a];
    uint32_t cb = ((uint32_t)pal_cb[a] + pal_cb[b]) >> 1;
    uint32_t cr = ((uint32_t)pal_cr[a] + pal_cr[b]) >> 1;
    return ((uint32_t)pal_y[a] << 24) | (cb << 16) | ((uint32_t)pal_y[b] << 8) | cr;
}

/* Phosphor: the pair blended with last frame's pair. */
static inline uint32_t blend_word(uint8_t a, uint8_t b, uint8_t pa, uint8_t pb)
{
    if (a == b && pa == pb) {      /* per-byte floor average, same rounding as below */
        uint32_t x = pal_word[a], y = pal_word[pa];
        return (x & y) + (((x ^ y) >> 1) & 0x7F7F7F7Fu);
    }
    uint32_t y0 = ((uint32_t)pal_y[a] + pal_y[pa]) >> 1;
    uint32_t y1 = ((uint32_t)pal_y[b] + pal_y[pb]) >> 1;
    uint32_t cb = ((uint32_t)pal_cb[a] + pal_cb[b] + pal_cb[pa] + pal_cb[pb]) >> 2;
    uint32_t cr = ((uint32_t)pal_cr[a] + pal_cr[b] + pal_cr[pa] + pal_cr[pb]) >> 2;
    return (y0 << 24) | (cb << 16) | (y1 << 8) | cr;
}

/* Canvas rows [y0, y1) -> YUYV in the back framebuffer, without phosphor. */
static void convert_rows(int y0, int y1)
{
    uint32_t       *dst = (uint32_t *)xfb[back] + (size_t)y0 * cw / 2;
    const uint32_t *src = (const uint32_t *)(canvas + (size_t)y0 * cw);
    const size_t    n4  = (size_t)(y1 - y0) * cw / 4;
    for (size_t i = 0; i < n4; i++) {
        uint32_t q = src[i];
        if (((q ^ (q >> 8)) & 0x00FFFFFFu) == 0) {
            dst[2 * i] = dst[2 * i + 1] = pal_word[q & 0xFFu];
        } else {
            dst[2 * i]     = pair_word(q >> 24, (q >> 16) & 0xFFu);
            dst[2 * i + 1] = pair_word((q >> 8) & 0xFFu, q & 0xFFu);
        }
    }
}

static void flip(void)
{
    DCFlushRange(xfb[back], (u32)((size_t)cw * ch * 2));
    VIDEO_SetNextFramebuffer(xfb[back]);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    back ^= 1;
}

uint32_t *wii_video_direct(void) { return xfb[back]; }

uint32_t wii_video_yuyv(uint8_t a, uint8_t b) { return pair_word(a, b); }

void wii_video_present_direct(int y0, int y1)
{
    u64 t0 = gettime();
    if (y0 < 0) y0 = 0;
    if (y1 > ch) y1 = ch;
    if (y0 < y1) convert_rows(y0, y1);
    wii_video_convert_us = (uint32_t)ticks_to_microsecs(diff_ticks(t0, gettime()));
    flip();
}

void wii_video_present(void)
{
    u64 t0 = gettime();
    /* Two canvas pixels -> one YUYV word: Y0 Cb Y1 Cr.  Four at a time; a
     * run of four identical pixels (any scaled picture) is one lookup. */
    uint32_t       *dst = xfb[back];
    const uint32_t *src = (const uint32_t *)canvas;
    const size_t    n   = (size_t)cw * ch / 2;
    const size_t    n4  = n / 2;
    if (phosphor && prev) {
        uint32_t *old = (uint32_t *)prev;
        for (size_t i = 0; i < n4; i++) {
            uint32_t q = src[i], o = old[i];
            if (q == o && ((q ^ (q >> 8)) & 0x00FFFFFFu) == 0) {   /* unchanged, flat */
                dst[2 * i] = dst[2 * i + 1] = pal_word[q & 0xFFu];
                continue;
            }
            dst[2 * i]     = blend_word(q >> 24, (q >> 16) & 0xFFu, o >> 24, (o >> 16) & 0xFFu);
            dst[2 * i + 1] = blend_word((q >> 8) & 0xFFu, q & 0xFFu, (o >> 8) & 0xFFu, o & 0xFFu);
            old[i] = q;
        }
    } else {
        convert_rows(0, ch);
    }
    wii_video_convert_us = (uint32_t)ticks_to_microsecs(diff_ticks(t0, gettime()));
    flip();
}
