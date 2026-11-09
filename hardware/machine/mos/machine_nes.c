/*
 * Nintendo Entertainment System, Wii edition.
 *
 * Implements the machine API declared in nes.h (nes_create / _run /
 * _destroy) like upstream's hardware/machine/mos/machine_nes.c, on the Wii
 * backends instead of SDL and the monitor.  The emulation is
 * nes_core.c (upstream's, trimmed to cartridges and standard controllers);
 * the 2A03 core, PPU, APU and boards are upstream's files, unchanged.
 *
 * Wii additions, in the HOME menu bar: Video (picture shape, overscan) and
 * Input (what is plugged into each port).  Battery-backed games save next
 * to the ROM, as <game>.sav.
 */
#include "nes.h"
#include "nes_core.h"
#include "gemu/wii.h"
#include "gemu/menubar_wii.h"
#include "gemu/video_wii.h"
#include "gemu/input_wii.h"
#ifdef GEMU_NETPLAY
#include "gemu/netplay_wii.h"
#include "gemu/sha256.h"
#endif

#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NES_CPU_HZ    1789773.0
#define NES_CPU_CYCLES_PER_FRAME 29780.5    /* NTSC: 60.10 frames a second */

typedef enum { SHAPE_TV, SHAPE_SQUARE, SHAPE_STRETCH } Shape;

/* A copy of the machine in memory: the state, and the CHR RAM it draws from. */
typedef struct { NesState *s; uint8_t *chr; size_t chr_len; } NesSnap;

/* NesState comes first, so the API's pointer is also a NesWii *. */
typedef struct {
    NesState  s;
    MosConfig cfg;              /* our copy; s.cfg points here (ports can change) */
    Shape     shape;
    bool      hide_overscan;
    int       map_shape, map_w; /* xmap below was built for these (-1 = not yet) */
    uint16_t  xmap[1024];       /* canvas column -> NES pixel */
    uint32_t  yuyv[64 * 64];    /* framebuffer word for NES colours a, b: [a * 64 + b] */
    bool      paused_view;
    uint32_t  emu_us, draw_us;                               /* last frame (autotest) */
    uint32_t  emu_sum, emu_max, emu_n, avg_us, max_us;       /* 60-frame stats */
    u64       fps_t0;
    uint32_t  fps_frames, fps;
    uint32_t  loop_us, present_us, n_run;
    uint32_t  shown_loop, shown_present, shown_run, shown_queue;
    uint32_t  iters, ips;
    uint32_t  underruns0, shown_underruns;
    bool      skip;             /* one frame per vblank doesn't fit: two per pass */
    int       last_run;
#ifdef GEMU_NETPLAY
    /* Netplay (see "Netplay" below) */
    NesSnap   poweron;          /* the machine as created: every player's start */
    NesSnap   before;           /* the player's own game, while a netplay game is on */
    bool      np_active;
    bool      np_reset;         /* Reset chosen: sent with our next input */
    uint64_t  np_frame;
    uint8_t   np_pad;           /* our controller 1, this pass */
    NesDeviceType np_ports[2];  /* the player's own ports, put back afterwards */
    int       np_n_ports;
#endif
} NesWii;

/* ── Battery-backed PRG RAM: <rom without extension>.sav ─────────────────── */

static void sav_path(const NesState *s, char *out, size_t len)
{
    snprintf(out, len, "%s", s->cart_path_buf);
    char *slash = strrchr(out, '/'), *dot = strrchr(out, '.');
    if (dot && (!slash || dot > slash)) *dot = '\0';
    strncat(out, ".sav", len - strlen(out) - 1);
}

static void sav_load(NesState *s)
{
    if (!s->cart.has_battery) return;
    char path[520];
    sav_path(s, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return;
    if (fread(s->prg_ram, 1, sizeof(s->prg_ram), f) == 0) { /* empty: keep zeros */ }
    fclose(f);
}

static void sav_save(NesState *s)
{
    if (!s->cart.has_battery) return;
    char path[520];
    sav_path(s, path, sizeof(path));
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(s->prg_ram, 1, sizeof(s->prg_ram), f);
    fclose(f);
}

/* ── Machine state copies ─────────────────────────────────────────────────── */
#if defined(GEMU_WII_AUTOTEST) || defined(GEMU_NETPLAY)

/* Overwrite s with from (this same build's NesState), keeping everything
 * that points into this run - as upstream's loadvm does. */
static void state_restore(NesState *s, const NesState *from)
{
    NesState *keep = malloc(sizeof(*keep));
    if (!keep) return;
    *keep = *s;
    *s = *from;
    s->cfg = keep->cfg;  s->prg = keep->prg;  s->chr = keep->chr;
    s->cpu.mem_read = keep->cpu.mem_read;  s->cpu.mem_write = keep->cpu.mem_write;
    s->cpu.mem_ud = keep->cpu.mem_ud;
    Rp2c02 *p = &s->ppu, *k = &keep->ppu;
    p->chr_read = k->chr_read;  p->chr_write = k->chr_write;  p->chr_ud = k->chr_ud;
    p->nt_read = k->nt_read;  p->nt_write = k->nt_write;  p->nt_ud = k->nt_ud;
    p->irq_scanline = k->irq_scanline;  p->irq_ud = k->irq_ud;
    p->cart_open = k->cart_open;  p->cart_open_ud = k->cart_open_ud;
    p->alt_palette_rgb = k->alt_palette_rgb;
    s->apu.mem_read = keep->apu.mem_read;  s->apu.mem_ud = keep->apu.mem_ud;
    s->apu.write_tap = keep->apu.write_tap;  s->apu.write_tap_ud = keep->apu.write_tap_ud;
    s->apu.audio_dev = keep->apu.audio_dev;  s->apu.frame_n = 0;
    s->display = NULL;  s->monitor = NULL;  s->hex_editor = NULL;
    s->crt_signal = NULL;  s->crt_decoded_argb = NULL;  s->crt_argb = NULL;
    free(keep);
    nes_core_resync(s);
}
#endif

#ifdef GEMU_WII_AUTOTEST
/* Autotest: resume from <rom>.nesstate if there is one - a raw NesState
 * of this same build, lifted out of a Dolphin save state (so a slow scene
 * can be measured). */
static void load_test_state(NesState *s)
{
    char path[520];
    sav_path(s, path, sizeof(path));
    strcpy(path + strlen(path) - 4, ".nesstate");
    FILE *f = fopen(path, "rb");
    if (!f) return;
    NesState *st = malloc(sizeof(*st));
    if (!st) { fclose(f); return; }
    size_t got = fread(st, 1, sizeof(*st), f);
    fclose(f);
    if (got == sizeof(*st) && st->cart.mapper == s->cart.mapper) state_restore(s, st);
    free(st);
}
#endif

#ifdef GEMU_NETPLAY
/* CHR RAM, which lives outside NesState (as upstream's nes_chr_ram). */
static uint8_t *chr_ram(const NesState *s, size_t *len)
{
    if (s->chr_is_ram) { *len = (size_t)s->cart.chr_banks * 0x2000u; return s->chr; }
    if (s->cart.mapper == 119 && s->chr) {
        *len = 0x2000u;
        return s->chr + (size_t)s->cart.chr_banks * 0x2000u;
    }
    *len = 0;
    return NULL;
}

static void snap_free(NesSnap *n) { free(n->s); free(n->chr); *n = (NesSnap){0}; }

static bool snap_take(const NesState *s, NesSnap *n)
{
    size_t len;
    const uint8_t *chr = chr_ram(s, &len);
    *n = (NesSnap){ malloc(sizeof(NesState)), len ? malloc(len) : NULL, len };
    if (!n->s || (len && !n->chr)) { snap_free(n); return false; }
    *n->s = *s;
    if (len) memcpy(n->chr, chr, len);
    return true;
}

static void snap_put(NesState *s, const NesSnap *n)
{
    size_t len;
    uint8_t *chr = chr_ram(s, &len);
    if (len == n->chr_len && len) memcpy(chr, n->chr, len);
    state_restore(s, n->s);
}

/* ── Netplay ────────────────────────────────────────────────────────────────
 *
 * As upstream's machine_nes.c: every player's NES runs the same frames with
 * the same input, so a netplay game starts every machine from the same
 * state - the one this machine had when it was created, before the battery
 * save was loaded, with PRG-RAM cleared.  The player's own game is kept
 * aside and put back afterwards, and nothing is saved to their battery
 * file meanwhile.  Controller 1 is ours; the session hands back every
 * player's, and players 1 and 2 drive the two ports.  Reset goes through
 * the session, so every machine resets on the same frame.
 *
 * The emulation is upstream's (make test-nes), so a Wii can play with GEMU
 * on a computer.  For that the game has to be listed the same way: under
 * upstream's default controller setup - a controller in port 1, port 2
 * empty - which is also how it's played, a second controller going into
 * port 2 when there's a player 2 (as upstream does for an empty port). */

#define NP_SYNC_FRAMES 300      /* compare state every 5 s */

/* The game's name in the lobby, exactly as upstream's nes_np_game_name:
 * the ROM's name, and a hash of what has to match for the games to stay
 * in step (ROM contents, region, controllers). */
static void np_game_name(const NesState *s, char *out, size_t len)
{
    const char *path = s->cart_path_buf, *base = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\') base = p + 1;
    const char *dot = strrchr(base, '.');
    int n = dot ? (int)(dot - base) : (int)strlen(base);
    if (n > 255) n = 255;
    GemuSha256Ctx ctx;
    uint8_t d[GEMU_SHA256_DIGEST_LEN];
    gemu_sha256_init(&ctx);
    if (s->prg) gemu_sha256_update(&ctx, s->prg, (size_t)s->cart.prg_banks * 0x4000u);
    if (s->chr && !s->chr_is_ram) gemu_sha256_update(&ctx, s->chr, (size_t)s->cart.chr_banks * 0x2000u);
    uint8_t cfg[4] = { (uint8_t)s->cfg->is_pal, 1, NES_DEVICE_CONTROLLER, NES_DEVICE_NONE };
    gemu_sha256_update(&ctx, cfg, sizeof(cfg));
    gemu_sha256_final(&ctx, d);
    snprintf(out, len, "%.*s [%02x%02x%02x%02x]", n > 100 ? 100 : n, base, d[0], d[1], d[2], d[3]);
}

/* The state compared between players: RAM, PRG-RAM and the CPU. */
static uint32_t np_state_hash(const NesState *s)
{
    uint32_t h = 2166136261u;
    const uint8_t *parts[] = { s->ram, s->prg_ram };
    const size_t sizes[] = { sizeof(s->ram), sizeof(s->prg_ram) };
    for (int p = 0; p < 2; p++)
        for (size_t i = 0; i < sizes[p]; i++) h = (h ^ parts[p][i]) * 16777619u;
    uint8_t cpu[6] = { s->cpu.A, s->cpu.X, s->cpu.Y, s->cpu.SP, (uint8_t)s->cpu.PC, (uint8_t)(s->cpu.PC >> 8) };
    for (int i = 0; i < 6; i++) h = (h ^ cpu[i]) * 16777619u;
    return h;
}

static void np_setup(NesWii *w)
{
    if (!snap_take(&w->s, &w->poweron)) return;
    char game[NETPLAY_GAME_LEN];
    np_game_name(&w->s, game, sizeof(game));
    wii_netplay_set_game(game);
}

static void np_begin(NesWii *w, int player, int players)
{
    NesState *s = &w->s;
    Netplay *np = wii_netplay();
    if (!w->poweron.s || !snap_take(s, &w->before)) {
        netplay_note(np, "Out of memory - leaving the game");
        netplay_leave_game(np);
        return;
    }
    sav_save(s);                     /* the player's battery save, as it is now */
    snap_put(s, &w->poweron);
    memset(s->prg_ram, 0, sizeof(s->prg_ram));
    w->np_ports[0] = w->cfg.ports[0];
    w->np_ports[1] = w->cfg.ports[1];
    w->np_n_ports = w->cfg.n_ports;
    w->cfg.n_ports = 1;
    w->cfg.ports[0] = NES_DEVICE_CONTROLLER;
    w->cfg.ports[1] = players > 1 ? NES_DEVICE_CONTROLLER : NES_DEVICE_NONE;
    w->np_active = true;
    w->np_frame = 0;
    w->np_reset = false;
    netplay_note(np, "Console powered on - player %d of %d%s", player, players,
                 players > 2 ? " (players 3 and up have no controller port)" : "");
    netplay_ready(np);
}

static void np_end(NesWii *w)
{
    if (!w->np_active) return;
    w->np_active = false;
    w->cfg.ports[0] = w->np_ports[0];
    w->cfg.ports[1] = w->np_ports[1];
    w->cfg.n_ports = w->np_n_ports;
    if (w->before.s) snap_put(&w->s, &w->before);
    snap_free(&w->before);
    w->s.ctrl_state[0] = w->s.ctrl_state[1] = 0;
    netplay_note(wii_netplay(), "Netplay game over - back to your own game");
}

/* Leaving the machine: out of the game - also one that hasn't started, or
 * it would start on whatever is loaded next - and our own game back.  The
 * session itself stays connected. */
static void np_teardown(NesWii *w)
{
    Netplay *np = wii_netplay();
    NetplayInfo in;
    if (np) netplay_info(np, &in);
    if (np && in.state >= NETPLAY_ROOM) netplay_leave_game(np);
    np_end(w);
    wii_netplay_set_game(NULL);
    snap_free(&w->poweron);
}

/* Before a frame: everyone's input for it.  1 to run it, 0 while the
 * others' input hasn't arrived (run nothing), -1 when the game has ended. */
static int np_frame(NesWii *w)
{
    NesState *s = &w->s;
    uint8_t local[NETPLAY_INPUT_BYTES] = { w->np_pad, w->np_reset ? NETPLAY_INPUT_RESET : 0 };
    uint8_t all[NETPLAY_MAX_PLAYERS * NETPLAY_INPUT_BYTES];
    int players;
    int r = netplay_frame(wii_netplay(), local, all, &players);
    if (r <= 0) return r;
    w->np_reset = false;
    for (int p = 0; p < players; p++)
        if (all[p * NETPLAY_INPUT_BYTES + 1] & NETPLAY_INPUT_RESET) {
            nes_core_reset(s);
            netplay_note(wii_netplay(), "Console reset (player %d)", p + 1);
            break;
        }
    s->ctrl_state[0] = all[0];
    s->ctrl_state[1] = players > 1 ? all[NETPLAY_INPUT_BYTES] : 0;
    w->np_frame++;
    return 1;
}

static void np_after_frame(NesWii *w)
{
    if (w->np_frame % NP_SYNC_FRAMES == 0)
        netplay_sync_hash(wii_netplay(), w->np_frame, np_state_hash(&w->s));
}
#endif

/* ── Machine API ─────────────────────────────────────────────────────────── */

NesState *nes_create(const MosConfig *cfg)
{
    NesWii *w = calloc(1, sizeof(*w));
    if (!w) { wii_set_error("Out of memory"); return NULL; }
    w->cfg = *cfg;
    w->map_shape = -1;
    NesState *s = &w->s;
    if (!cfg->cart_path || !nes_core_load(s, cfg->cart_path)) {
        wii_set_error("%s", cfg->cart_path ? nes_core_error : "No cartridge");
        free(w);
        return NULL;
    }
    nes_core_init(s, &w->cfg);
#ifdef GEMU_NETPLAY
    np_setup(w);                /* before the battery save: everyone's start */
#endif
    /* Sound 0.27% slow on a 59.94 Hz TV, so one frame per vblank (60.10 Hz)
     * makes exactly what is played and the queue holds without catch-up
     * frames (which need two frames to fit a vblank).  50 Hz TVs: as is,
     * catching up. */
    float ratio = wii_video_refresh_hz() / (float)(NES_CPU_HZ / NES_CPU_CYCLES_PER_FRAME);
    if (s->apu.audio_dev && ratio > 0.99f && ratio < 1.01f) sdl_audio_wii_set_speed(ratio);
    sav_load(s);
#ifdef GEMU_WII_AUTOTEST
    load_test_state(s);
#endif
    for (int i = 0; i < 64; i++)
        wii_video_set_color(WII_COL_MACHINE + i, rp2c02_palette_rgb[i] & 0xFFFFFFu);
    for (int a = 0; a < 64; a++)
        for (int b = 0; b < 64; b++)
            w->yuyv[a * 64 + b] = wii_video_yuyv((uint8_t)(WII_COL_MACHINE + a), (uint8_t)(WII_COL_MACHINE + b));
    return s;
}

void nes_destroy(NesState *s)
{
    if (!s) return;
#ifdef GEMU_NETPLAY
    np_teardown((NesWii *)s);   /* the player's own game back, then save it */
#endif
    sav_save(s);
    nes_core_free(s);
    free((NesWii *)s);
}

/* ── Input ───────────────────────────────────────────────────────────────── */
/*
 * Controller n is NES controller n.  A Wii Remote held sideways is laid out
 * like the NES pad: D-pad, 1 = B, 2 = A, - = Select, + = Start.  GameCube:
 * A, B, Y = Select, X = Start.
 */
static uint8_t pad_from(uint32_t b)
{
    uint8_t v = 0;
    if (b & WII_BTN_ACCEPT) v |= NES_BTN_A;
    if (b & WII_BTN_BACK)   v |= NES_BTN_B;
    if (b & WII_BTN_MINUS)  v |= NES_BTN_SELECT;
    if (b & WII_BTN_PLUS)   v |= NES_BTN_START;
    if (b & WII_BTN_UP)     v |= NES_BTN_UP;
    if (b & WII_BTN_DOWN)   v |= NES_BTN_DOWN;
    if (b & WII_BTN_LEFT)   v |= NES_BTN_LEFT;
    if (b & WII_BTN_RIGHT)  v |= NES_BTN_RIGHT;
    if ((v & (NES_BTN_UP | NES_BTN_DOWN)) == (NES_BTN_UP | NES_BTN_DOWN))
        v &= (uint8_t)~NES_BTN_DOWN;          /* a real pad can't press both */
    if ((v & (NES_BTN_LEFT | NES_BTN_RIGHT)) == (NES_BTN_LEFT | NES_BTN_RIGHT))
        v &= (uint8_t)~NES_BTN_RIGHT;
    return v;
}

#ifdef GEMU_WII_AUTOTEST
extern uint64_t nes_core_ppu_ticks, nes_core_apu_ticks;
static uint32_t ppu_us, apu_us;
#endif

/* ── Drawing ─────────────────────────────────────────────────────────────── */

/* Picture width on the 640-wide canvas: the NES's pixels are 8:7 on a TV. */
static int picture_width(const NesWii *w, int W)
{
    int pw = w->shape == SHAPE_STRETCH ? W
           : w->shape == SHAPE_SQUARE  ? 2 * RP2C02_WIDTH
           : (2 * RP2C02_WIDTH * 8 / 7) & ~3;
    return pw > W ? W : pw;
}

/* Where the picture goes on the 640-wide screen. */
typedef struct { int W, H, sy, top, lines, pw, x0, y0; } Layout;

static Layout layout(NesWii *w)
{
    Layout l;
    l.W = wii_video_width(); l.H = wii_video_height();
    l.sy = l.H >= 2 * RP2C02_HEIGHT ? 2 : 1;
    l.top = w->hide_overscan ? 8 : 0;
    l.lines = RP2C02_HEIGHT - 2 * l.top;
    l.pw = picture_width(w, l.W);
    l.x0 = ((l.W - l.pw) / 2) & ~3;
    l.y0 = (l.H - l.lines * l.sy) / 2;
    if (w->map_shape != (int)w->shape || w->map_w != l.pw) {
        for (int x = 0; x < l.pw; x++) w->xmap[x] = (uint16_t)(x * RP2C02_WIDTH / l.pw);
        w->map_shape = (int)w->shape;
        w->map_w = l.pw;
    }
    return l;
}

/* While the game runs: NES pixels straight to framebuffer words (two
 * screen pixels each), no canvas. */
static void draw_direct(NesWii *w)
{
    const Layout l = layout(w);
    const int wpr = l.W / 2;                 /* words per framebuffer row */
    uint32_t *fb = wii_video_direct();
    const bool claim = ((uintptr_t)fb & 31) == 0 && (wpr & 7) == 0;
    const uint32_t black = wii_video_yuyv(WII_COL_BLACK, WII_COL_BLACK);
    for (int y = 0; y < l.y0; y++) {
        uint32_t *r = fb + (size_t)y * wpr, *r2 = fb + (size_t)(l.H - 1 - y) * wpr;
        if (claim) { wii_video_claim(r, wpr); wii_video_claim(r2, wpr); }
        for (int x = 0; x < wpr; x++) r[x] = r2[x] = black;
    }
    const int bx = l.x0 / 2, pw2 = l.pw / 2;
    const uint16_t *m = w->xmap;
    for (int y = 0; y < l.lines; y++) {
        const uint8_t *src = w->s.ppu.pixels + (size_t)(l.top + y) * RP2C02_WIDTH;
        uint32_t *row = fb + (size_t)(l.y0 + y * l.sy) * wpr;
        uint32_t *row2 = l.sy > 1 ? row + wpr : row;     /* the line's second screen row */
        if (claim) wii_video_claim(row, (int)(row2 - row) + wpr);
        for (int x = 0; x < bx; x++) row[x] = row2[x] = black;
        uint32_t *d = row + bx, *d2 = row2 + bx;
        const uint32_t *yuyv = w->yuyv;
        for (int x = 0; x < pw2; x++)
            d[x] = d2[x] = yuyv[(src[m[2 * x]] & 0x3Fu) << 6 | (src[m[2 * x + 1]] & 0x3Fu)];
        for (int x = bx + pw2; x < wpr; x++) row[x] = row2[x] = black;
    }
}

#ifdef GEMU_WII_AUTOTEST
#define OVERLAY_Y 60          /* from the bottom */
static void draw_overlay(NesWii *w)
{
    NesState *s = &w->s;
    const int H = wii_video_height();
    wii_video_textf(16, H - 58, 1, WII_COL_WHITE,
                    "frame %llu  pc=%04X  emu %u + draw %u + convert %u us  mapper %u",
                    (unsigned long long)s->ppu.frame, s->cpu.PC, w->emu_us,
                    w->draw_us, wii_video_convert_us, s->cart.mapper);
    wii_video_textf(16, H - 46, 1, WII_COL_WHITE, "emu avg %u max %u us (60 frames)  %u fps  ppu %u apu %u us",
                    w->avg_us, w->max_us, w->fps, ppu_us, apu_us);
    wii_video_textf(16, H - 34, 1, WII_COL_WHITE,
                    "loop %u: present %u, %u frames, queue %u  %u loops/s  underruns %u since start",
                    w->shown_loop, w->shown_present, w->shown_run, w->shown_queue, w->ips,
                    w->shown_underruns);
}
#endif

static void draw_game(void *ud)
{
    NesWii *w = ud;
    NesState *s = &w->s;
    const Layout l = layout(w);
    const int W = l.W, H = l.H, sy = l.sy, top = l.top, lines = l.lines, pw = l.pw;
    const int x0 = l.x0, y0 = l.y0;
    /* Clear only what the picture doesn't cover. */
    if (y0 > 0) {
        wii_video_fill(0, 0, W, y0, WII_COL_BLACK);
        wii_video_fill(0, y0 + lines * sy, W, H - y0 - lines * sy, WII_COL_BLACK);
    }
    if (x0 > 0) {
        wii_video_fill(0, y0, x0, lines * sy, WII_COL_BLACK);
        wii_video_fill(x0 + pw, y0, W - x0 - pw, lines * sy, WII_COL_BLACK);
    }

    /* A line's colours, then the picture a word (4 canvas pixels) at a time:
     * x0 and pw are multiples of 4.  Big-endian: the first pixel is the top byte. */
    uint8_t *canvas = wii_video_canvas();
    uint8_t col[RP2C02_WIDTH];
    const uint16_t *m = w->xmap;
    for (int y = 0; y < lines; y++) {
        const uint8_t *src = s->ppu.pixels + (size_t)(top + y) * RP2C02_WIDTH;
        for (int x = 0; x < RP2C02_WIDTH; x++) col[x] = (uint8_t)(WII_COL_MACHINE + (src[x] & 0x3Fu));
        uint8_t *dst = canvas + (size_t)(y0 + y * sy) * W + x0;
        uint32_t *d4 = (uint32_t *)dst;
        for (int x = 0; x < pw; x += 4)
            *d4++ = (uint32_t)col[m[x]] << 24 | (uint32_t)col[m[x + 1]] << 16 |
                    (uint32_t)col[m[x + 2]] << 8 | col[m[x + 3]];
        for (int r = 1; r < sy; r++) memcpy(dst + (size_t)r * W, dst, (size_t)pw);
    }
    if (w->paused_view)
        wii_video_text(16, 16, 2, WII_COL_ACCENT, "PAUSED");
#ifdef GEMU_WII_AUTOTEST
    draw_overlay(w);
#endif
}

/* ── Menu bar: Video, Input ──────────────────────────────────────────────── */

static void radio(GemuMenu *m, const char *label, void (*set)(void *, int),
                  bool (*is)(void *, int), void *ud, int arg)
{
    GemuMenuItem *it = gemu_menu_add(m, GEMU_MENU_RADIO, label, set, ud, arg);
    it->checked = is;
}

static bool shape_is(void *ud, int arg)  { return ((NesWii *)ud)->shape == (Shape)arg; }
static void shape_set(void *ud, int arg) { ((NesWii *)ud)->shape = (Shape)arg; }
static bool overscan_on(void *ud, int arg) { (void)arg; return ((NesWii *)ud)->hide_overscan; }
static void overscan_toggle(void *ud, int arg)
{
    (void)arg;
    NesWii *w = ud;
    w->hide_overscan = !w->hide_overscan;
}

/* arg: port * 4 + NesDeviceType */
static bool port_is(void *ud, int arg)
{
    return ((NesWii *)ud)->cfg.ports[arg / 4] == (NesDeviceType)(arg % 4);
}
static void port_set(void *ud, int arg)
{
    NesWii *w = ud;
#ifdef GEMU_NETPLAY
    if (w->np_active) return;       /* every player's NES must stay the same */
#endif
    w->cfg.ports[arg / 4] = (NesDeviceType)(arg % 4);
}

static void nes_menus(void *ud, GemuMenu *bar)
{
    GemuMenu *vid = gemu_menu_add_submenu(bar, "Video");
    GemuMenu *shape = gemu_menu_add_submenu(vid, "Picture");
    radio(shape, "TV shape (8:7 pixels)", shape_set, shape_is, ud, SHAPE_TV);
    radio(shape, "Square pixels",         shape_set, shape_is, ud, SHAPE_SQUARE);
    radio(shape, "Stretch to fill",       shape_set, shape_is, ud, SHAPE_STRETCH);
    GemuMenuItem *it = gemu_menu_add(vid, GEMU_MENU_CHECK, "Hide overscan",
                                     overscan_toggle, ud, 0);
    it->checked = overscan_on;
#ifdef GEMU_NETPLAY
    wii_netplay_menu(bar);
#endif
}

static void nes_input_items(void *ud, GemuMenu *input)
{
    static const char *const ports[2] = { "Port 1", "Port 2" };
    for (int p = 0; p < 2; p++) {
        GemuMenu *m = gemu_menu_add_submenu(input, ports[p]);
        radio(m, "Controller", port_set, port_is, ud, p * 4 + NES_DEVICE_CONTROLLER);
        radio(m, "Nothing",    port_set, port_is, ud, p * 4 + NES_DEVICE_NONE);
    }
}

static void menu_reset(void *ud)
{
    NesState *s = ud;
#ifdef GEMU_NETPLAY
    NesWii *w = ud;
    if (w->np_active) { w->np_reset = true; return; }   /* everyone resets together */
#endif
    sav_save(s);
    nes_core_reset(s);
}

/* ── Run loop ────────────────────────────────────────────────────────────── */

/* Pacing as the Atari 2600's: one emulated frame per vblank (the NES's
 * 60.10 Hz is all but the Wii's 59.94), nudged by the audio queue - 2
 * frames when it is under one hardware buffer, none when it is more than
 * ~4 frames ahead, which also absorbs the 0.3% difference.  Without audio,
 * the frame's real duration paces it.
 *
 * When one frame and its picture don't fit in a vblank (heavy scenes on
 * the GameCube), passes run two frames and show the second - frame skip,
 * the game keeping its speed - until the slowest of the last 60 frames
 * plus the drawing would fit again, with SKIP_MARGIN_US to spare. */
#define FRAME_SAMPLES (APU2A03_SAMPLE_RATE / 60u)
#define QUEUE_LOW     SDL_AUDIO_WII_CHUNK
#define QUEUE_HIGH    (SDL_AUDIO_WII_CHUNK + 4u * FRAME_SAMPLES)
#define MAX_FRAMES_PER_PASS 3
#define SKIP_MARGIN_US 1500u


static uint32_t queued_samples(const NesState *s)
{
    return SDL_GetQueuedAudioSize(s->apu.audio_dev) / (uint32_t)sizeof(float);
}

/* vblanks: how many the previous pass (w->last_run frames) took. */
static int frames_wanted(NesWii *w, int vblanks)
{
    const uint32_t budget = (uint32_t)(1e6f / wii_video_refresh_hz());
    if (w->last_run == 1 && vblanks >= 2) {         /* a single frame didn't fit */
        w->skip = true;
        w->max_us = UINT32_MAX;                     /* until 60 more are measured */
        w->emu_sum = w->emu_max = w->emu_n = 0;
    } else if (w->skip && w->max_us < budget &&
               w->max_us + w->draw_us + SKIP_MARGIN_US < budget) {
        w->skip = false;
    }
    uint32_t q = queued_samples(&w->s);
    int n;
    if (w->skip) n = 2;
    else         n = q < QUEUE_LOW ? 2 : 1;
    if (q > QUEUE_HIGH) n--;
    return n < 0 ? 0 : n > MAX_FRAMES_PER_PASS ? MAX_FRAMES_PER_PASS : n;
}

static void run_frame(NesWii *w)
{
    u64 t0 = gettime();
#ifdef GEMU_WII_AUTOTEST
    nes_core_ppu_ticks = nes_core_apu_ticks = 0;
#endif
    nes_core_frame(&w->s);
#ifdef GEMU_WII_AUTOTEST
    ppu_us = (uint32_t)ticks_to_microsecs(nes_core_ppu_ticks);
    apu_us = (uint32_t)ticks_to_microsecs(nes_core_apu_ticks);
#endif
    w->emu_us = (uint32_t)ticks_to_microsecs(diff_ticks(t0, gettime()));
    w->emu_sum += w->emu_us;
    if (w->emu_us > w->emu_max) w->emu_max = w->emu_us;
    if (++w->emu_n == 60) {
        w->avg_us = w->emu_sum / 60; w->max_us = w->emu_max;
        w->emu_sum = w->emu_max = w->emu_n = 0;
    }
    w->fps_frames++;
    if (!w->fps_t0) w->fps_t0 = gettime();
    if (ticks_to_millisecs(diff_ticks(w->fps_t0, gettime())) >= 1000) {
        w->fps = w->fps_frames; w->fps_frames = 0; w->fps_t0 = gettime();
        w->ips = w->iters; w->iters = 0;
    }
}

void nes_run(NesState *s, const MosConfig *cfg)
{
    (void)cfg;
    NesWii *w = (NesWii *)s;
    WiiMenubar *menubar = wii_menubar_new(&(WiiMenubarHooks){
        .reset = menu_reset, .input_items = nes_input_items, .menus = nes_menus,
        .draw_frame = draw_game, .ud = w,
    });

    u64 t0 = gettime();
    uint64_t emu_cycles = 0;          /* emulated since t0, when there is no audio */
    u32 retrace = wii_video_retraces();

    for (;;) {
        u64 l0 = gettime();
        u32 now = wii_video_retraces();
        int vblanks = (int)(now - retrace);
        retrace = now;
        if (vblanks < 1) vblanks = 1;
        WiiInput in = wii_input_poll(true);
        WiiExit sys = wii_system_exit_requested();
        if (sys != WII_EXIT_NONE) { wii_request_exit(sys); break; }

        if (in.pressed & WII_BTN_PAUSE) {            /* HOME: menu bar */
            s->ctrl_state[0] = s->ctrl_state[1] = 0;
            WiiExit leave = wii_menubar_open(menubar);
            if (leave != WII_EXIT_NONE) { wii_request_exit(leave); break; }
            t0 = gettime(); emu_cycles = 0;
            retrace = wii_video_retraces();
            continue;
        }

        s->ctrl_state[0] = pad_from(in.port[0]);
        s->ctrl_state[1] = pad_from(in.port[1]);
#ifdef GEMU_NETPLAY
        /* A netplay game starting: power on as everyone else does. */
        int np_player, np_players;
        if (netplay_take_start(wii_netplay(), &np_player, &np_players))
            np_begin(w, np_player, np_players);
        w->np_pad = s->ctrl_state[0];   /* we play on our controller 1 */
#endif
        bool paused = wii_menubar_paused(menubar);
#ifdef GEMU_WII_AUTOTEST
        paused |= wii_autotest_frozen();
#endif
        if (!paused) {
            w->n_run = 0;
            int want = s->apu.audio_dev ? frames_wanted(w, vblanks) : MAX_FRAMES_PER_PASS;
            for (int n = 0; n < want; n++) {
                if (!s->apu.audio_dev) {
                    uint64_t real_us = ticks_to_microsecs(diff_ticks(t0, gettime()));
                    if (emu_cycles * 1000000.0 / NES_CPU_HZ >= (double)real_us) break;
                }
#ifdef GEMU_NETPLAY
                if (w->np_active) {             /* everyone's input, or wait for it */
                    int r = np_frame(w);
                    if (r < 0) np_end(w);
                    else if (r == 0) break;
                }
#endif
                uint64_t c0 = s->cpu.cycle_count;
                run_frame(w);
                w->n_run++;
                emu_cycles += s->cpu.cycle_count - c0;
#ifdef GEMU_NETPLAY
                if (w->np_active) np_after_frame(w);
#endif
            }
        }

        w->last_run = paused ? 0 : w->n_run;
        w->paused_view = wii_menubar_paused(menubar);
        u64 d0 = gettime();
        if (w->paused_view) draw_game(w);
        else                draw_direct(w);
        w->draw_us = (uint32_t)ticks_to_microsecs(diff_ticks(d0, gettime()));
        u64 p0 = gettime();
#ifdef GEMU_NETPLAY
        int ny0, ny1;                   /* chat, players joining or dropping... */
        wii_netplay_notices(&ny0, &ny1);
        if (!w->paused_view) wii_video_direct_rows(ny0, ny1);
#endif
        if (w->paused_view) {
            wii_video_present();
        } else {
#ifdef GEMU_WII_AUTOTEST
            const int H = wii_video_height();
            wii_video_fill(0, H - OVERLAY_Y, wii_video_width(), OVERLAY_Y - 20, WII_COL_BLACK);
            draw_overlay(w);
            wii_video_present_direct(H - OVERLAY_Y, H - 20);
#else
            wii_video_present_direct(0, 0);
#endif
        }
        w->present_us = (uint32_t)ticks_to_microsecs(diff_ticks(p0, gettime()));
        w->loop_us = (uint32_t)ticks_to_microsecs(diff_ticks(l0, gettime()));
        if (!paused) {
            w->shown_loop = w->loop_us; w->shown_present = w->present_us;
            w->shown_run = w->n_run;
            if (!w->underruns0) w->underruns0 = sdl_audio_wii_underruns + 1;
            w->shown_underruns = sdl_audio_wii_underruns + 1 - w->underruns0;
            w->shown_queue = s->apu.audio_dev ? queued_samples(s) : 0;
            w->iters++;
        }
    }
#ifdef GEMU_NETPLAY
    if (w->np_active) { netplay_leave_game(wii_netplay()); np_end(w); }   /* not into the save */
#endif
    sav_save(s);
    wii_menubar_free(menubar);
}
