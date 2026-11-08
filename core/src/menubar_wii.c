/*
 * In-game menu bar (see gemu/menubar_wii.h).
 *
 * Rendering and navigation follow upstream core/src/gemu_menubar_sdl.c
 * (DOSBox-X style bar, drop-downs with submenus, Windows 3.1 About box, IBM
 * 8x16 font, same colours and metrics at UI scale 1); the bar contents follow
 * upstream gemu_display_build_menubar().  There is no mouse, so no hover.
 */
#include "gemu/menubar_wii.h"
#include "gemu/video_wii.h"
#include "gemu/input_wii.h"
#include "gemu/audio_wii.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GLYPH_W   WII_VIDEO_GLYPH16_W
#define GLYPH_H   WII_VIDEO_GLYPH16_H
#define MAX_DEPTH 4                 /* bar menu + 3 submenu levels */
#define BAR_H     (GLYPH_H + 4)

typedef struct { int x, y, w, h; } Rect;

typedef struct {
    GemuMenu *menu;
    int       sel;                  /* highlighted item, -1 = none */
    Rect      rect;                 /* where it was drawn */
} Level;

struct WiiMenubar {
    WiiMenubarHooks h;
    GemuMenu *bar;
    int       open;                 /* open bar menu, -1 = closed */
    Level     levels[MAX_DEPTH];
    int       depth;
    Rect      title_rect[16];
    int       n_titles;
    bool      paused;
    bool      about;                /* About dialog showing */
    WiiExit   leave;                /* set by Back to ROM list / Exit */
};

int wii_menubar_height(void) { return BAR_H; }

/* ── Standard bar (cf. upstream gemu_display_build_menubar) ──────────────── */

static bool is_paused(void *ud, int arg)    { (void)arg; return ((WiiMenubar *)ud)->paused; }
static void toggle_pause(void *ud, int arg) { (void)arg; WiiMenubar *mb = ud; mb->paused = !mb->paused; }
static void do_reset(void *ud, int arg)
{
    (void)arg;
    WiiMenubar *mb = ud;
    if (mb->h.reset) mb->h.reset(mb->h.ud);
}
static void do_leave(void *ud, int why)     { ((WiiMenubar *)ud)->leave = (WiiExit)why; }
static void do_about(void *ud, int arg)     { (void)arg; ((WiiMenubar *)ud)->about = true; }

static const char about_text[] =
    "GEMU " GEMU_VERSION_STR " - Generic EMUlator\n"
    "\n"
    "Emulating everything in existence!\n"
    "\n"
    "Build date: " __DATE__;

static GemuMenu *build_bar(WiiMenubar *mb)
{
    GemuMenu *bar = gemu_menu_new("");
    GemuMenuItem *it;

    GemuMenu *m = gemu_menu_add_submenu(bar, "Machine");
    it = gemu_menu_add(m, GEMU_MENU_CHECK, "Pause", toggle_pause, mb, 0);
    it->checked = is_paused;
    gemu_menu_add(m, GEMU_MENU_ACTION, "Reset", do_reset, mb, 0);
    if (mb->h.machine_items) mb->h.machine_items(mb->h.ud, m);
    gemu_menu_add_separator(m);
    gemu_menu_add(m, GEMU_MENU_ACTION, "Back to ROM list", do_leave, mb, WII_EXIT_TO_MENU);
    gemu_menu_add(m, GEMU_MENU_ACTION, "Exit to Homebrew Channel", do_leave, mb, WII_EXIT_TO_LOADER);

    GemuMenu *input = gemu_menu_add_submenu(bar, "Input");
    if (mb->h.input_items) mb->h.input_items(mb->h.ud, input);
    if (input->n == 0) {                /* no tab when there is nothing in it */
        gemu_menu_free(input);
        bar->n--;
    }

    if (mb->h.menus) mb->h.menus(mb->h.ud, bar);

    GemuMenu *help = gemu_menu_add_submenu(bar, "Help");
    gemu_menu_add(help, GEMU_MENU_ACTION, "About GEMU", do_about, mb, 0);
    return bar;
}

/* ── Model / state (as upstream) ─────────────────────────────────────────── */

static void first_enabled(Level *lv)
{
    lv->sel = -1;
    if (!lv->menu) return;
    /* A pure list of choices (Speed, Palette...) opens on the current one;
     * anything else on its first item. */
    bool choices = lv->menu->n > 0;
    for (int i = 0; i < lv->menu->n; i++)
        if (lv->menu->items[i].type != GEMU_MENU_RADIO &&
            lv->menu->items[i].type != GEMU_MENU_SEPARATOR) choices = false;
    for (int i = 0; choices && i < lv->menu->n; i++)
        if (gemu_menu_item_checked(&lv->menu->items[i])) { lv->sel = i; return; }
    for (int i = 0; i < lv->menu->n; i++)
        if (gemu_menu_item_enabled(&lv->menu->items[i])) { lv->sel = i; return; }
}

static void close_menus(WiiMenubar *mb) { mb->open = -1; mb->depth = 0; }

static void open_menu(WiiMenubar *mb, int index)
{
    if (!mb->bar || index < 0 || index >= mb->bar->n) return;
    mb->open = index;
    mb->depth = 1;
    mb->levels[0].menu = mb->bar->items[index].submenu;
    first_enabled(&mb->levels[0]);
}

static void move_sel(WiiMenubar *mb, int dir)
{
    Level *lv = &mb->levels[mb->depth - 1];
    int n = lv->menu ? lv->menu->n : 0;
    for (int k = 1; k <= n; k++) {
        int i = ((lv->sel < 0 ? (dir > 0 ? -1 : 0) : lv->sel) + dir * k + n * 4) % n;
        if (gemu_menu_item_enabled(&lv->menu->items[i])) { lv->sel = i; return; }
    }
}

/* Activate the selected item of level l: open a submenu, or run it. */
static void activate(WiiMenubar *mb, int l)
{
    Level *lv = &mb->levels[l];
    if (lv->sel < 0 || !lv->menu) return;
    const GemuMenuItem *it = &lv->menu->items[lv->sel];
    if (it->type == GEMU_MENU_SUBMENU) {
        if (gemu_menu_item_enabled(it) && l + 1 < MAX_DEPTH) {
            mb->depth = l + 2;
            mb->levels[l + 1].menu = it->submenu;
            first_enabled(&mb->levels[l + 1]);
        }
        return;
    }
    if (!gemu_menu_item_enabled(it)) return;
    GemuMenuItem copy = *it;            /* the model may be rebuilt */
    close_menus(mb);
    gemu_menu_item_activate(&copy);
}

/* ── Rendering (metrics and colours as upstream at scale 1) ──────────────── */

static int text_w(const char *s) { return (int)strlen(s) * GLYPH_W; }

static int item_h(const GemuMenuItem *it)
{
    return it->type == GEMU_MENU_SEPARATOR ? 6 : GLYPH_H + 4;
}

static void draw_level(Level *lv, int x, int y)
{
    const int W = wii_video_width(), H = wii_video_height();
    GemuMenu *m = lv->menu;
    int pad = 8, label_w = 0, short_w = 0, h = 2;
    for (int i = 0; i < m->n; i++) {
        int lw = text_w(m->items[i].label), sw = text_w(m->items[i].shortcut);
        if (m->items[i].type == GEMU_MENU_SUBMENU) sw = text_w(">");
        if (lw > label_w) label_w = lw;
        if (sw > short_w) short_w = sw;
        h += item_h(&m->items[i]);
    }
    int w = pad + 2 * GLYPH_W + label_w + (short_w ? 3 * GLYPH_W + short_w : 0) + pad;
    if (x + w > W) x = W - w;
    if (y + h > H) y = H - h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    lv->rect = (Rect){ x, y, w, h };

    wii_video_fill(x + 3, y + 3, w, h, WII_COL_BLACK);           /* drop shadow */
    wii_video_fill(x, y, w, h, WII_COL_MENU_BG);
    wii_video_frame(x, y, w, h, 1, WII_COL_BORDER);
    int iy = y + 1;
    for (int i = 0; i < m->n; i++) {
        const GemuMenuItem *it = &m->items[i];
        int ih = item_h(it);
        if (it->type == GEMU_MENU_SEPARATOR) {
            wii_video_fill(x + 4, iy + ih / 2, w - 8, 1, WII_COL_MENU_DIS);
        } else {
            bool en = gemu_menu_item_enabled(it), hi = i == lv->sel && en;
            if (hi) wii_video_fill(x + 1, iy, w - 2, ih, WII_COL_MENU_HI);
            uint8_t fg = !en ? WII_COL_MENU_DIS : hi ? WII_COL_MENU_HIFG : WII_COL_MENU_FG;
            int ty = iy + 2;
            if (gemu_menu_item_checked(it))
                wii_video_text16(x + pad, ty, fg, it->type == GEMU_MENU_RADIO ? "\x07" : "\xFB");
            wii_video_text16(x + pad + 2 * GLYPH_W, ty, fg, it->label);
            const char *right = it->type == GEMU_MENU_SUBMENU ? "\x10" : it->shortcut;
            if (right[0]) wii_video_text16(x + w - pad - text_w(right), ty, fg, right);
        }
        iy += ih;
    }
}

static int item_top(const Level *lv, int i)
{
    int y = lv->rect.y + 1;
    for (int k = 0; k < i; k++) y += item_h(&lv->menu->items[k]);
    return y;
}

/* Windows 3.1 push button (upstream button()), always the focused default. */
static void draw_button(Rect rc, const char *label)
{
    const int b = 2;
    wii_video_fill(rc.x + 1, rc.y, rc.w - 2, b, WII_COL_BLACK);
    wii_video_fill(rc.x + 1, rc.y + rc.h - b, rc.w - 2, b, WII_COL_BLACK);
    wii_video_fill(rc.x, rc.y + 1, b, rc.h - 2, WII_COL_BLACK);
    wii_video_fill(rc.x + rc.w - b, rc.y + 1, b, rc.h - 2, WII_COL_BLACK);
    Rect in = { rc.x + b, rc.y + b, rc.w - 2 * b, rc.h - 2 * b };
    wii_video_fill(in.x, in.y, in.w, in.h, WII_COL_BTN_FACE);
    wii_video_fill(in.x, in.y, in.w - 1, 2, WII_COL_MENU_HIFG);   /* bevel */
    wii_video_fill(in.x, in.y, 2, in.h - 1, WII_COL_MENU_HIFG);
    wii_video_fill(in.x + 1, in.y + in.h - 2, in.w - 1, 2, WII_COL_BTN_DARK);
    wii_video_fill(in.x + in.w - 2, in.y + 1, 2, in.h - 1, WII_COL_BTN_DARK);
    int tw = text_w(label), tx = rc.x + (rc.w - tw) / 2, ty = rc.y + (rc.h - GLYPH_H) / 2;
    wii_video_text16(tx, ty, WII_COL_MENU_FG, label);
    for (int x = tx - 2; x < tx + tw + 2; x += 2) {                 /* focus dots */
        wii_video_fill(x, ty, 1, 1, WII_COL_BLACK);
        wii_video_fill(x, ty + GLYPH_H - 1, 1, 1, WII_COL_BLACK);
    }
    for (int y = ty; y < ty + GLYPH_H; y += 2) {
        wii_video_fill(tx - 2, y, 1, 1, WII_COL_BLACK);
        wii_video_fill(tx + tw + 1, y, 1, 1, WII_COL_BLACK);
    }
}

/* About box: upstream draw_dialog_frame() + draw_about(). */
static void draw_about(void)
{
    const int W = wii_video_width(), H = wii_video_height(), lh = GLYPH_H + 2;
    int lines = 1, longest = 0, cur = 0;
    for (const char *p = about_text; ; p++) {
        if (*p == '\n' || !*p) { if (cur > longest) longest = cur; cur = 0; if (!*p) break; lines++; }
        else cur++;
    }
    int w = longest * GLYPH_W + 48, h = (GLYPH_H + 6) + 16 + lines * lh + 48;
    if (w < 240) w = 240;
    int x = (W - w) / 2, y = (H - h) / 2;
    if (y < BAR_H) y = BAR_H;

    const int fw = 5, th = GLYPH_H + 6;
    wii_video_fill(x - fw, y - fw, w + 2 * fw, h + 2 * fw, WII_COL_BLACK);
    wii_video_fill(x - fw + 1, y - fw + 1, w + 2 * fw - 2, h + 2 * fw - 2, WII_COL_BTN_FACE);
    wii_video_frame(x - 1, y - 1, w + 2, h + 2, 1, WII_COL_BLACK);
    wii_video_fill(x, y, w, h, WII_COL_MENU_HIFG);
    wii_video_fill(x, y, w, th, WII_COL_MENU_HI);                  /* title bar */
    wii_video_fill(x, y + th, w, 1, WII_COL_BLACK);
    wii_video_fill(x, y, th, th, WII_COL_BTN_FACE);               /* system-menu box */
    wii_video_fill(x + th, y, 1, th, WII_COL_BLACK);
    int dw = th / 2, dx = x + (th - dw) / 2, dy = y + th / 2 - 1;
    wii_video_fill(dx + 1, dy + 1, dw, 3, WII_COL_BTN_DARK);
    wii_video_fill(dx, dy, dw, 3, WII_COL_MENU_HIFG);
    wii_video_frame(dx, dy, dw, 3, 1, WII_COL_BLACK);
    const char *title = "About GEMU";
    wii_video_text16(x + th + (w - th - text_w(title)) / 2, y + 3, WII_COL_MENU_HIFG, title);

    int tx = x + 24, ty = y + th + 16;
    char line[128];
    for (const char *p = about_text; *p; ) {
        size_t n = strcspn(p, "\n");
        snprintf(line, sizeof(line), "%.*s", (int)n, p);
        wii_video_text16(tx, ty, WII_COL_MENU_FG, line);
        ty += lh;
        p += n + (p[n] == '\n');
    }
    int bw = 10 * GLYPH_W, bh = GLYPH_H + 8;
    draw_button((Rect){ x + (w - bw) / 2, y + h - bh - 12, bw, bh }, "Close");
}

static void render(WiiMenubar *mb)
{
    const int W = wii_video_width();
    if (mb->h.draw_frame) mb->h.draw_frame(mb->h.ud);
    else wii_video_clear(WII_COL_BLACK);

    wii_video_fill(0, 0, W, BAR_H, WII_COL_BAR_BG);
    int x = 4;
    mb->n_titles = 0;
    for (int i = 0; mb->bar && i < mb->bar->n && i < 16; i++) {
        const char *t = mb->bar->items[i].label;
        int w = text_w(t) + 12;
        mb->title_rect[i] = (Rect){ x, 0, w, BAR_H };
        if (i == mb->open) wii_video_fill(x, 0, w, BAR_H, WII_COL_BAR_HI);
        wii_video_text16(x + 6, 2, WII_COL_MENU_HIFG, t);
        x += w;
        mb->n_titles++;
    }
    if (mb->paused) {
        const char *p = "[Paused]";
        wii_video_text16(W - 8 - text_w(p), 2, WII_COL_MENU_DIS, p);
    }

    if (mb->open >= 0 && mb->open < mb->n_titles) {
        int lx = mb->title_rect[mb->open].x, ly = BAR_H;
        for (int l = 0; l < mb->depth; l++) {
            Level *lv = &mb->levels[l];
            draw_level(lv, lx, ly);
            if (l + 1 < mb->depth) {      /* submenu opens beside its item */
                lx = lv->rect.x + lv->rect.w - 2;
                ly = item_top(lv, lv->sel) - 1;
            }
        }
    }
    if (mb->about) draw_about();
    wii_video_present();
}

/* ── Public ──────────────────────────────────────────────────────────────── */

WiiMenubar *wii_menubar_new(const WiiMenubarHooks *hooks)
{
    WiiMenubar *mb = calloc(1, sizeof(*mb));
    if (!mb) return NULL;
    mb->h = *hooks;
    mb->open = -1;
    return mb;
}

void wii_menubar_free(WiiMenubar *mb)
{
    if (!mb) return;
    gemu_menu_free(mb->bar);
    free(mb);
}

bool wii_menubar_paused(const WiiMenubar *mb) { return mb && mb->paused; }

WiiExit wii_menubar_open(WiiMenubar *mb)
{
    if (!mb) return WII_EXIT_NONE;
    gemu_menu_free(mb->bar);
    mb->bar = build_bar(mb);            /* fresh checkmarks, as upstream */
    mb->leave = WII_EXIT_NONE;
    mb->about = false;
    open_menu(mb, 0);
    wii_audio_beep(false);

    for (;;) {
        /* Same (sideways) layout as the game the menu was opened from. */
        WiiInput in = wii_input_poll(true);
        WiiExit sys = wii_system_exit_requested();
        if (sys != WII_EXIT_NONE) { close_menus(mb); return sys; }

        const unsigned ok = WII_BTN_ACCEPT | WII_BTN_X;   /* 2 / A, GC A */
        const unsigned back = WII_BTN_BACK | WII_BTN_Y;   /* 1 / B, GC B */

        if (mb->about) {
            if (in.pressed & (ok | back | WII_BTN_PAUSE)) mb->about = false;
        } else if (in.pressed & WII_BTN_PAUSE) {
            close_menus(mb);
        } else {
            int l = mb->depth - 1;
            if (in.repeat & WII_BTN_UP)   move_sel(mb, -1);
            if (in.repeat & WII_BTN_DOWN) move_sel(mb, +1);
            if (in.repeat & WII_BTN_RIGHT) {
                Level *lv = &mb->levels[l];
                if (lv->sel >= 0 && lv->menu->items[lv->sel].type == GEMU_MENU_SUBMENU)
                    activate(mb, l);
                else
                    open_menu(mb, (mb->open + 1) % mb->bar->n);
            }
            if (in.repeat & WII_BTN_LEFT) {
                if (mb->depth > 1) mb->depth--;
                else open_menu(mb, (mb->open + mb->bar->n - 1) % mb->bar->n);
            }
            if (in.pressed & ok) activate(mb, l);
            else if (in.pressed & back) {
                if (mb->depth > 1) mb->depth--;
                else close_menus(mb);
            }
        }

        if (mb->leave != WII_EXIT_NONE) return mb->leave;
        if (mb->open < 0 && !mb->about) {
            wii_input_flush();          /* the closing press stays in the menu */
            return WII_EXIT_NONE;
        }
        render(mb);
    }
}
