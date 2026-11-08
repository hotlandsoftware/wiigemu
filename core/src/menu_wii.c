#include "gemu/menu_wii.h"
#include "gemu/video_wii.h"
#include "gemu/input_wii.h"

#include <stdio.h>

/* ── Full-screen list ────────────────────────────────────────────────────── */

#define LIST_Y      84
#define LIST_ROW    24
#define BAR_H       64

int wii_list_rows(void)
{
    return (wii_video_height() - LIST_Y - BAR_H - 32) / LIST_ROW;
}

void wii_list_navigate(int n, unsigned repeat, int *sel, int *top)
{
    const int rows = wii_list_rows();
    if (n <= 0) { *sel = *top = 0; return; }
    if (repeat & WII_BTN_UP)    *sel = (*sel + n - 1) % n;
    if (repeat & WII_BTN_DOWN)  *sel = (*sel + 1) % n;
    if (repeat & WII_BTN_LEFT)  { *sel -= rows; if (*sel < 0) *sel = 0; }
    if (repeat & WII_BTN_RIGHT) { *sel += rows; if (*sel >= n) *sel = n - 1; }
    if (*sel >= n) *sel = n - 1;
    if (*sel < *top) *top = *sel;
    if (*sel >= *top + rows) *top = *sel - rows + 1;
}

void wii_list_draw(const WiiList *l, int sel, int top)
{
    const int W = wii_video_width(), H = wii_video_height();
    const int rows = wii_list_rows();
    wii_video_clear(WII_COL_BLACK);

    wii_video_fill(0, 0, W, BAR_H, WII_COL_DARK);
    int x = 32;
    if (l->pre)  x = wii_video_text(x, 24, 2, WII_COL_WHITE, l->pre);
    x = wii_video_text(x, 24, 2, WII_COL_ACCENT, "GEMU");
    if (l->post) wii_video_text(x, 24, 2, WII_COL_WHITE, l->post);
    wii_video_textf(W - 32 - wii_video_text_width(2, "v" GEMU_VERSION_STR), 24, 2,
                    WII_COL_GREY, "v%s", GEMU_VERSION_STR);

    for (int r = 0; r < rows && top + r < l->n; r++) {
        int i = top + r, y = LIST_Y + r * LIST_ROW;
        if (i == sel) wii_video_fill(24, y - 4, W - 48, LIST_ROW, WII_COL_SELECT);
        char label[37];
        snprintf(label, sizeof(label), "%s", l->labels[i]);
        wii_video_text(40, y, 2,
                       l->highlight && l->highlight[i] ? WII_COL_ACCENT : WII_COL_WHITE,
                       label);
    }
    if (l->n > rows)
        wii_video_textf(W - 40 - wii_video_text_width(2, "000/000"),
                        LIST_Y + rows * LIST_ROW + 4, 2, WII_COL_GREY,
                        "%3d/%d", sel + 1, l->n);
    if (l->empty_hint)
        wii_video_text(40, LIST_Y + (l->n + 1) * LIST_ROW, 2, WII_COL_GREY, l->empty_hint);

    wii_video_fill(0, H - BAR_H, W, BAR_H, WII_COL_DARK);
    if (l->status && *l->status)
        wii_video_text(32, H - 56, 2, l->status_error ? WII_COL_ERROR : WII_COL_GREY,
                       l->status);
    if (l->footer)
        wii_video_text(32, H - 30, 2, WII_COL_GREY, l->footer);
    wii_video_present();
}
