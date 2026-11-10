/*
 * The Netplay screen and session for the Wii - see gemu/netplay_wii.h.
 *
 * Upstream's Netplay window (netplay_dialog_sdl.c) laid out for a TV and a
 * controller, one page per session state, in the 8x16 font:
 *
 *   offline    your name, connection type, a server address; below, the
 *              servers you've used and the public ones (users, games,
 *              ping) - A on one connects
 *   lobby      Create Game, then the server's games (A joins); the chat
 *   in a game  who's in it; Start (the owner) or Leave; the chat
 *   playing    who's playing; Leave; the chat
 *
 * Text (name, server, chat) comes from an on-screen keyboard.  The Wii's
 * network starts in the background the first time the screen opens.
 */
#include "gemu/netplay_wii.h"
#include "gemu/video_wii.h"
#include "gemu/input_wii.h"
#include "gemu/wii.h"

#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* network.h isn't included: its fd_set clashes with newlib's.  socInit
 * (no header) registers libogc's socket device, which socket(), sendto()...
 * go through; only if_config() calls it, net_init() doesn't. */
extern s32 net_init(void);
extern int socInit(void);

#define EMULATOR    "GEMU " GEMU_VERSION_STR   /* as upstream: players must match */
#define GW          WII_VIDEO_GLYPH16_W
#define GH          16
#define ROW         18
#define TITLE_H     44
#define FOOT_H      30
#define MARGIN      24
#define MAX_SV      400
#define MAX_GAMES   256
#define MAX_USERS   512
#define LOG_ROWS    40
#define PING_MS     5000
#define NOTICE_MS   6000

/* ── The session ─────────────────────────────────────────────────────────── */

static Netplay *np;           /* the session */
static char     cur_game[NETPLAY_GAME_LEN];

Netplay *wii_netplay(void)
{
    if (!np) np = netplay_create(EMULATOR, cur_game);
    return np;
}

void wii_netplay_set_game(const char *game)
{
    snprintf(cur_game, sizeof(cur_game), "%s", game ? game : "");
    if (np) netplay_set_game(np, cur_game);
}

void wii_netplay_shutdown(void)
{
    if (np) netplay_disconnect(np);    /* sends USER_QUIT, ends the session thread */
}

static uint32_t now_ms(void) { return (uint32_t)ticks_to_millisecs(gettime()); }

/* ── The network (net_init takes a few seconds: in the background) ─────── */

enum { NET_OFF, NET_STARTING, NET_UP, NET_FAILED };
static volatile int net_state = NET_OFF;
static volatile s32 net_err;
static pthread_t    net_thread;
static bool         net_thread_made;

static void *net_up(void *arg)
{
    (void)arg;
    s32 r = -EAGAIN;
    for (int i = 0; i < 100 && r == -EAGAIN; i++) {
        r = net_init();
        if (r == -EAGAIN) usleep(100000);
    }
    if (r >= 0) socInit();
    net_err = r;
    net_state = r >= 0 ? NET_UP : NET_FAILED;
    return NULL;
}

static void net_start(void)
{
    if (net_state == NET_STARTING || net_state == NET_UP) return;
    if (net_thread_made) { pthread_join(net_thread, NULL); net_thread_made = false; }
    net_state = NET_STARTING;
    if (pthread_create(&net_thread, NULL, net_up, NULL) == 0) net_thread_made = true;
    else { net_err = -1; net_state = NET_FAILED; }
}

/* ── Drawing helpers ─────────────────────────────────────────────────────── */

/* At most max characters of s at (x, y). */
static int text(int x, int y, uint8_t c, const char *s, int max)
{
    char buf[128];
    if (max > (int)sizeof(buf) - 1) max = (int)sizeof(buf) - 1;
    snprintf(buf, (size_t)max + 1, "%s", s);
    return wii_video_text16(x, y, c, buf);
}

static void title_bar(const char *right)
{
    const int W = wii_video_width();
    wii_video_fill(0, 0, W, TITLE_H, WII_COL_DARK);
    int x = wii_video_text(MARGIN, 14, 2, WII_COL_WHITE, "Netplay");
    if (right) text(x + 24, 18, WII_COL_GREY, right, (W - MARGIN - x - 24) / GW);
}

static void footer(const char *hint)
{
    const int W = wii_video_width(), H = wii_video_height();
    wii_video_fill(0, H - FOOT_H, W, FOOT_H, WII_COL_DARK);
    text(MARGIN, H - FOOT_H + 7, WII_COL_GREY, hint, (W - 2 * MARGIN) / GW);
}

/* One selectable row: the highlight bar when selected. */
static void row_bar(int y, bool sel)
{
    if (sel) wii_video_fill(MARGIN - 6, y - 1, wii_video_width() - 2 * MARGIN + 12, ROW, WII_COL_SELECT);
}

/* The newest log lines that fit in rows [y0, y1), wrapped, oldest at top. */
static void draw_log(int y0, int y1, uint8_t colour)
{
    static char lines[LOG_ROWS][NETPLAY_LINE_LEN];
    const int cols = (wii_video_width() - 2 * MARGIN) / GW;
    const int rows = (y1 - y0) / ROW;
    if (rows <= 0) return;
    int n = netplay_log(np, lines, LOG_ROWS);
    /* Lay out from the newest line up. */
    int y = y0 + (rows - 1) * ROW;
    for (int i = n - 1; i >= 0 && y >= y0; i--) {
        const char *s = lines[i];
        int len = (int)strlen(s), parts = len ? (len + cols - 1) / cols : 1;
        for (int p = parts - 1; p >= 0 && y >= y0; p--, y -= ROW) {
            uint8_t c = colour;
            if (!strncmp(s, "***", 3)) c = WII_COL_ERROR;
            else if (s[0] == '*') c = WII_COL_GREY;
            text(MARGIN + (p ? 2 * GW : 0), y, c, s + p * cols, p ? cols - 2 : cols);
        }
    }
}

/* ── On-screen keyboard ──────────────────────────────────────────────────── */

#define KB_COLS 12
static const char *const kb_keys[2][4] = {
    { "1234567890-=", "qwertyuiop[]", "asdfghjkl;'\\", "zxcvbnm,./`~" },
    { "!@#$%^&*()_+", "QWERTYUIOP{}", "ASDFGHJKL:\"|", "ZXCVBNM<>?`~" },
};
static const char *const kb_special[4] = { "Shift", "Space", "Del", "Done" };

static NetplayState session_state(void)
{
    NetplayInfo in;
    netplay_info(np, &in);
    return in.state;
}

/* Edit buf (max chars).  A types, B deletes, + done, - shift, HOME cancels.
 * False when cancelled (buf unchanged) - also when the session moves on
 * meanwhile (a game starting mustn't wait for someone's chat). */
static bool keyboard(const char *title, char *buf, size_t max)
{
    const NetplayState st = session_state();
    char edit[NETPLAY_LINE_LEN];
    snprintf(edit, sizeof(edit), "%s", buf);
    if (max >= sizeof(edit)) max = sizeof(edit) - 1;
    int r = 1, c = 0;
    bool shift = false;
    wii_input_flush();
    for (;;) {
        WiiInput in = wii_input_poll(false);
        if (wii_system_exit_requested() != WII_EXIT_NONE) return false;
        if (session_state() != st) { wii_input_flush(); return false; }
        size_t len = strlen(edit);
        bool press = false, done = false;
        if (in.repeat & WII_BTN_UP)    r = (r + 4) % 5;
        if (in.repeat & WII_BTN_DOWN)  r = (r + 1) % 5;
        if (in.repeat & WII_BTN_LEFT)  c = r == 4 ? (c / 3 * 3 + KB_COLS - 3) % KB_COLS : (c + KB_COLS - 1) % KB_COLS;
        if (in.repeat & WII_BTN_RIGHT) c = r == 4 ? (c / 3 * 3 + 3) % KB_COLS : (c + 1) % KB_COLS;
        if (in.pressed & WII_BTN_PAUSE) { wii_input_flush(); return false; }
        if (in.pressed & WII_BTN_MINUS) shift = !shift;
        if (in.pressed & WII_BTN_PLUS)  done = true;
        if (in.repeat & WII_BTN_BACK && len) edit[len - 1] = '\0';
        if (in.pressed & WII_BTN_ACCEPT) press = true;
        if (press && r < 4) {
            if (len < max) { edit[len] = kb_keys[shift][r][c]; edit[len + 1] = '\0'; }
        } else if (press) {
            switch (c / 3) {
            case 0: shift = !shift; break;
            case 1: if (len < max) { edit[len] = ' '; edit[len + 1] = '\0'; } break;
            case 2: if (len) edit[len - 1] = '\0'; break;
            case 3: done = true; break;
            }
        }
        if (done) {
            snprintf(buf, max + 1, "%s", edit);
            wii_input_flush();
            return true;
        }

        const int W = wii_video_width(), H = wii_video_height();
        const int cw = 44, ch = 40, kx = (W - KB_COLS * cw) / 2, ky = 150;
        wii_video_clear(WII_COL_BLACK);
        title_bar(title);
        wii_video_fill(kx - 4, 80, KB_COLS * cw + 8, 36, WII_COL_BTN_DARK);
        int cols = (KB_COLS * cw) / GW - 2;
        int from = (int)strlen(edit) > cols ? (int)strlen(edit) - cols : 0;
        int ex = text(kx + 4, 90, WII_COL_WHITE, edit + from, cols);
        if ((now_ms() / 400) & 1) wii_video_fill(ex + 1, 88, 2, GH + 2, WII_COL_ACCENT);
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < KB_COLS; x++) {
                bool sel = y == r && x == c;
                wii_video_fill(kx + x * cw + 2, ky + y * ch + 2, cw - 4, ch - 4,
                               sel ? WII_COL_SELECT : WII_COL_KEY);
                char k[2] = { kb_keys[shift][y][x], 0 };
                text(kx + x * cw + (cw - GW) / 2, ky + y * ch + (ch - GH) / 2, WII_COL_WHITE, k, 1);
            }
        for (int k = 0; k < 4; k++) {
            bool sel = r == 4 && c / 3 == k;
            int x = kx + k * 3 * cw, y = ky + 4 * ch;
            wii_video_fill(x + 2, y + 2, 3 * cw - 4, ch - 4,
                           sel ? WII_COL_SELECT : k == 0 && shift ? WII_COL_KEY_HELD : WII_COL_KEY);
            const char *l = kb_special[k];
            text(x + (3 * cw - (int)strlen(l) * GW) / 2, y + (ch - GH) / 2, WII_COL_WHITE, l, 8);
        }
        (void)H;
        footer("A: Type  B: Delete  " WII_KEY_MINUS ": Shift  " WII_KEY_PLUS ": Done  "
               WII_KEY_HOME ": Cancel");
        wii_video_present();
    }
}

/* ── Snapshots of the session, each frame ────────────────────────────────── */

static NetplayInfo   info;
static NetplayServer servers[MAX_SV];
static int           n_servers;
static NetplayGame   games[MAX_GAMES];
static int           n_games;
static NetplayUser   users[MAX_USERS];
static int           n_users;
static NetplayUser   room[NETPLAY_MAX_PLAYERS];
static int           n_room;
static uint32_t      seen_version = ~0u;

static void refresh(void)
{
    netplay_info(np, &info);
    uint32_t v = netplay_version(np);
    if (v == seen_version) return;
    seen_version = v;
    n_servers = netplay_servers(np, servers, MAX_SV);
    n_games = netplay_games(np, games, MAX_GAMES);
    n_users = netplay_users(np, users, MAX_USERS);
    n_room = netplay_room(np, room, NETPLAY_MAX_PLAYERS);
}

/* ── Offline: connect ────────────────────────────────────────────────────── */

typedef struct { char address[128]; const NetplayServer *sv; bool recent; } Entry;

static char name[NETPLAY_NAME_LEN];
static char address[128];
static int  conn = NETPLAY_CONN_DEFAULT;
static bool settings_loaded, lists_asked;
static uint32_t last_ping;

/* The Wii's nickname (Wii Settings), as plain ASCII; "" if none. */
static void wii_nickname(char *out, size_t len)
{
    out[0] = '\0';
#ifdef HW_RVL
    u8 raw[64] = {0};
    if (CONF_GetNickName(raw) <= 0) return;
    size_t o = 0;
    bool utf16 = raw[0] == 0 && raw[1] != 0;
    for (size_t i = 0; i < sizeof(raw) && o + 1 < len; i += utf16 ? 2 : 1) {
        unsigned ch = utf16 ? (unsigned)(raw[i] << 8 | raw[i + 1]) : raw[i];
        if (!ch) break;
        if (ch >= 0x20 && ch < 0x7F) out[o++] = (char)ch;
    }
    out[o] = '\0';
#endif
}

static void load_settings(void)
{
    if (settings_loaded) return;
    settings_loaded = true;
    netplay_last_settings(name, sizeof(name), &conn);
    char recent[1][128];
    if (netplay_recent_servers(recent, 1) == 0) {     /* first time: the Wii's own name */
        char nick[NETPLAY_NAME_LEN];
        wii_nickname(nick, sizeof(nick));
        if (nick[0]) snprintf(name, sizeof(name), "%s", nick);
    } else {
        snprintf(address, sizeof(address), "%s", recent[0]);
    }
}

/* Busiest first: users online ("3/100"), then games. */
static int by_users(const void *a, const void *b)
{
    const NetplayServer *x = ((const Entry *)a)->sv, *y = ((const Entry *)b)->sv;
    int ux = atoi(x->users), uy = atoi(y->users);
    if (ux != uy) return uy - ux;
    return y->games - x->games;
}

/* Recent servers first, most recent at the top (with the public list's
 * details when it has them), then the public ones, busiest first. */
static int connect_entries(Entry *out, int max)
{
    char recent[8][128];
    int nr = netplay_recent_servers(recent, 8), n = 0;
    for (int i = 0; i < nr && n < max; i++) {
        Entry *e = &out[n++];
        snprintf(e->address, sizeof(e->address), "%s", recent[i]);
        e->sv = NULL;
        e->recent = true;
        for (int j = 0; j < n_servers; j++)
            if (!strcmp(servers[j].address, recent[i])) e->sv = &servers[j];
    }
    for (int j = 0; j < n_servers && n < max; j++) {
        bool dup = false;
        for (int i = 0; i < nr && !dup; i++) dup = !strcmp(servers[j].address, recent[i]);
        if (dup) continue;
        Entry *e = &out[n++];
        snprintf(e->address, sizeof(e->address), "%s", servers[j].address);
        e->sv = &servers[j];
        e->recent = false;
    }
    int first_public = nr < n ? nr : n;
    qsort(out + first_public, (size_t)(n - first_public), sizeof(*out), by_users);
    return n;
}

static void do_connect(const char *addr)
{
    char err[160];
    if (net_state != NET_UP) {
        netplay_note(np, net_state == NET_STARTING ? "The network is still starting - a moment..."
                                                  : "No network - check the Wii's Internet settings");
        return;
    }
    snprintf(address, sizeof(address), "%s", addr);
    if (!netplay_connect(np, NETPLAY_MODE_KAILLERA, addr, name, conn, err, sizeof(err)))
        netplay_note(np, "%s", err);
}

/* sel: 0 name, 1 connection, 2 server address, 3.. the list */
static void page_connect(const WiiInput *in, int *sel, int *top)
{
    static Entry entries[8 + MAX_SV];
    const int W = wii_video_width(), H = wii_video_height();
    int n = connect_entries(entries, 8 + MAX_SV);
    const int list_y = TITLE_H + 3 * ROW + 46, log_rows = 4;
    const int rows = (H - FOOT_H - log_rows * ROW - 8 - list_y) / ROW;
    const int total = 3 + n;

    if (in->repeat & WII_BTN_UP)   *sel = (*sel + total - 1) % total;
    if (in->repeat & WII_BTN_DOWN) *sel = (*sel + 1) % total;
    if (*sel == 1 && (in->repeat & WII_BTN_LEFT)  && conn > NETPLAY_CONN_MIN) conn--;
    if (*sel == 1 && (in->repeat & WII_BTN_RIGHT) && conn < NETPLAY_CONN_MAX) conn++;
    if (*sel >= 3) {
        if (in->repeat & WII_BTN_LEFT)  { *sel -= rows; if (*sel < 3) *sel = 3; }
        if (in->repeat & WII_BTN_RIGHT) { *sel += rows; if (*sel >= total) *sel = total - 1; }
    }
    if (in->pressed & WII_BTN_PLUS) { netplay_refresh_servers(np); last_ping = 0; }
    if (in->pressed & WII_BTN_ACCEPT) {
        if (*sel == 0) keyboard("Your name", name, NETPLAY_NAME_LEN - 1);
        else if (*sel == 1) conn = conn % NETPLAY_CONN_MAX + 1;
        else if (*sel == 2) { if (keyboard("Server (host or host:port)", address, 127) && address[0]) do_connect(address); }
        else do_connect(entries[*sel - 3].address);
    }
    int li = *sel - 3;
    if (li >= 0) { if (li < *top) *top = li; if (li >= *top + rows) *top = li - rows + 1; }
    if (*top > n - rows) *top = n - rows > 0 ? n - rows : 0;

    /* Pings, every few seconds while the list shows (as upstream's window). */
    if (net_state == NET_UP && n_servers && now_ms() - last_ping > PING_MS) {
        netplay_ping_servers(np);
        last_ping = now_ms();
    }

    wii_video_clear(WII_COL_BLACK);
    const char *ns = net_state == NET_UP ? "Kaillera - not connected"
                   : net_state == NET_STARTING ? "Starting the network..."
                   : net_state == NET_FAILED ? "No network" : "";
    title_bar(ns);
    int y = TITLE_H + 12;
    const char *labels[3] = { "Name", "Connection", "Server" };
    char vals[3][160];
    snprintf(vals[0], sizeof(vals[0]), "%s", name[0] ? name : "(none)");
    snprintf(vals[1], sizeof(vals[1]), "< %s >  %d frame%s a packet (the same for everyone in a game)",
             netplay_conn_name(conn), conn, conn == 1 ? "" : "s");
    snprintf(vals[2], sizeof(vals[2]), "%s", address[0] ? address : "(type one, or pick below)");
    for (int i = 0; i < 3; i++, y += ROW) {
        row_bar(y, *sel == i);
        text(MARGIN, y, WII_COL_GREY, labels[i], 12);
        text(MARGIN + 12 * GW, y, WII_COL_WHITE, vals[i], (W - 2 * MARGIN) / GW - 12);
    }

    y = list_y - ROW - 4;
    char head[128];
    snprintf(head, sizeof(head), "%-34s %-9s %5s %5s  %s", "Servers", "Users", "Games", "Ping", "Location");
    text(MARGIN, y, WII_COL_ACCENT, head, 78);
    if (!n) {
        const char *why = net_state != NET_UP ? "" : netplay_servers_busy(np)
                        ? "Fetching the public server lists..." : "No servers listed - " WII_KEY_PLUS " fetches the lists again";
        text(MARGIN, list_y, WII_COL_GREY, why, 76);
    }
    for (int r = 0; r < rows && *top + r < n; r++) {
        const Entry *e = &entries[*top + r];
        int yy = list_y + r * ROW;
        row_bar(yy, *sel == 3 + *top + r);
        char line[160], ping[8] = "";
        if (e->sv && e->sv->ping >= 0) snprintf(ping, sizeof(ping), "%d", e->sv->ping);
        if (e->sv)
            snprintf(line, sizeof(line), "%-34.34s %-9.9s %5d %5s  %s", e->sv->name, e->sv->users,
                     e->sv->games, ping, e->sv->location);
        else
            snprintf(line, sizeof(line), "%-34.34s", e->address);
        text(MARGIN, yy, e->recent ? WII_COL_ACCENT : WII_COL_WHITE, line, 78);
    }
    draw_log(H - FOOT_H - log_rows * ROW - 4, H - FOOT_H - 4, WII_COL_GREY);
    footer("A: Connect / Edit  " WII_KEY_PLUS ": Refresh list  B: Close");
}

/* ── Connected: the lobby, the game we're in ─────────────────────────────── */

static bool game_matches(const NetplayGame *g)
{
    return !strcmp(g->emulator, EMULATOR) && !strcmp(g->name, cur_game);
}

static int chat_rows(void) { return 9; }

static void chat_prompt(void)
{
    char line[128] = "";
    if (keyboard(info.state >= NETPLAY_ROOM ? "Say (to the game)" : "Say (to the server)", line, 120) && line[0])
        netplay_chat(np, line);
}

static void page_lobby(const WiiInput *in, int *sel, int *top, bool *close)
{
    static int order[MAX_GAMES];
    const int W = wii_video_width(), H = wii_video_height();
    /* Games you can join first (same game, waiting), then the rest. */
    int n = 0;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < n_games; i++)
            if ((game_matches(&games[i]) && games[i].status == 0) == (pass == 0)) order[n++] = i;
    const int list_y = TITLE_H + 12 + 2 * ROW, chat_y = H - FOOT_H - chat_rows() * ROW - 6;
    const int rows = (chat_y - 8 - list_y) / ROW, total = 1 + n;

    if (in->repeat & WII_BTN_UP)   *sel = (*sel + total - 1) % total;
    if (in->repeat & WII_BTN_DOWN) *sel = (*sel + 1) % total;
    if (in->repeat & WII_BTN_LEFT)  { *sel -= rows; if (*sel < 0) *sel = 0; }
    if (in->repeat & WII_BTN_RIGHT) { *sel += rows; if (*sel >= total) *sel = total - 1; }
    if (*sel >= total) *sel = total - 1;
    if (*sel < *top) *top = *sel;
    if (*sel >= *top + rows) *top = *sel - rows + 1;
    if (in->pressed & WII_BTN_ACCEPT) {
        if (*sel == 0) netplay_create_game(np);
        else netplay_join_game(np, games[order[*sel - 1]].id);
    }
    if (in->pressed & WII_BTN_PLUS)  chat_prompt();
    if (in->pressed & WII_BTN_MINUS) { netplay_disconnect(np); *sel = *top = 0; }
    (void)close;

    wii_video_clear(WII_COL_BLACK);
    char t[200];
    snprintf(t, sizeof(t), "%s on %s - %s connection - %d user%s", info.user, info.server,
             netplay_conn_name(info.conn), n_users, n_users == 1 ? "" : "s");
    title_bar(t);
    int y = TITLE_H + 12;
    char head[128];
    snprintf(head, sizeof(head), "%-44s %-7s %-8s %s", "Games", "Players", "Status", "Owner / emulator");
    text(MARGIN, y + ROW, WII_COL_ACCENT, head, 78);

    for (int r = 0; r < rows && *top + r < total; r++) {
        int i = *top + r, yy = list_y + r * ROW;
        row_bar(yy, *sel == i);
        char line[200];
        if (i == 0) {
            if (cur_game[0]) snprintf(line, sizeof(line), "Create Game: %s", cur_game);
            else snprintf(line, sizeof(line), "Create Game (load a game first)");
            text(MARGIN, yy, cur_game[0] ? WII_COL_ACCENT : WII_COL_GREY, line, 78);
            continue;
        }
        const NetplayGame *g = &games[order[i - 1]];
        static const char *const st[] = { "waiting", "playing", "syncing" };
        bool ours = !strcmp(g->emulator, EMULATOR);
        snprintf(line, sizeof(line), "%-44.44s %-7.7s %-8s %s%s%s", g->name, g->players,
                 g->status < 3 ? st[g->status] : "?", g->owner, ours ? "" : " / ", ours ? "" : g->emulator);
        text(MARGIN, yy, game_matches(g) && g->status == 0 ? WII_COL_WHITE : WII_COL_GREY, line, 78);
    }
    wii_video_fill(MARGIN - 6, chat_y - 4, W - 2 * MARGIN + 12, 1, WII_COL_BORDER);
    draw_log(chat_y, H - FOOT_H - 4, WII_COL_WHITE);
    footer("A: Join / Create  " WII_KEY_PLUS ": Chat  " WII_KEY_MINUS ": Disconnect  B: Close");
}

/* In a game: who's in it; Start (the owner) or Leave. */
static void page_room(const WiiInput *in, int *sel, bool *close)
{
    const int W = wii_video_width(), H = wii_video_height();
    const bool playing = info.state >= NETPLAY_STARTING;
    const bool can_start = !playing && info.owner;
    const char *items[2];
    int n = 0;
    if (can_start) items[n++] = "Start the game";
    items[n++] = "Leave the game";
    if (*sel >= n) *sel = n - 1;
    if (in->repeat & WII_BTN_UP)   *sel = (*sel + n - 1) % n;
    if (in->repeat & WII_BTN_DOWN) *sel = (*sel + 1) % n;
    if (in->pressed & WII_BTN_ACCEPT) {
        if (can_start && *sel == 0) netplay_start_game(np);
        else { netplay_leave_game(np); *sel = 0; if (playing) *close = true; }
    }
    if (in->pressed & WII_BTN_PLUS) chat_prompt();

    wii_video_clear(WII_COL_BLACK);
    const NetplayGame *g = NULL;
    for (int i = 0; i < n_games; i++) if (games[i].id == info.game_id) g = &games[i];
    char t[200];
    snprintf(t, sizeof(t), "%s on %s", info.user, info.server);
    title_bar(t);
    int y = TITLE_H + 12;
    char line[200];
    snprintf(line, sizeof(line), "%s%s", g ? g->name : "Game", info.owner ? " (yours)" : "");
    text(MARGIN, y, WII_COL_ACCENT, line, 78);
    y += ROW;
    if (playing)
        snprintf(line, sizeof(line), "Playing: you are player %d of %d, input delay %d frame%s",
                 info.player, info.players, info.delay, info.delay == 1 ? "" : "s");
    else if (info.owner)
        snprintf(line, sizeof(line), "Start when everyone's in (they load the same game, then Join).");
    else
        snprintf(line, sizeof(line), "Waiting for the owner to start the game.");
    text(MARGIN, y, WII_COL_GREY, line, 78);
    y += ROW;
    if (playing) text(MARGIN, y, WII_COL_GREY, "(everyone's game waits while this is open)", 78);
    y += ROW + 6;
    for (int i = 0; i < n_room; i++, y += ROW) {
        snprintf(line, sizeof(line), "%d. %-24s %4u ms  %s%s", i + 1, room[i].name, room[i].ping,
                 netplay_conn_name(room[i].conn), strcmp(room[i].name, info.user) ? "" : "  (you)");
        text(MARGIN + 2 * GW, y, WII_COL_WHITE, line, 74);
    }
    y = TITLE_H + 12 + 3 * ROW + 6 + NETPLAY_MAX_PLAYERS * ROW;
    for (int i = 0; i < n; i++, y += ROW) {
        row_bar(y, *sel == i);
        text(MARGIN, y, WII_COL_WHITE, items[i], 40);
    }
    const int chat_y = H - FOOT_H - chat_rows() * ROW - 6;
    wii_video_fill(MARGIN - 6, chat_y - 4, W - 2 * MARGIN + 12, 1, WII_COL_BORDER);
    draw_log(chat_y, H - FOOT_H - 4, WII_COL_WHITE);
    footer(playing ? "A: Select  " WII_KEY_PLUS ": Chat  B: Back to the game"
                   : "A: Select  " WII_KEY_PLUS ": Chat  B: Close");
}

/* ── The screen ──────────────────────────────────────────────────────────── */

void wii_netplay_screen(void)
{
    if (!wii_netplay()) return;
    load_settings();
    net_start();
    seen_version = ~0u;
    int sel = 0, top = 0;
    NetplayState shown = (NetplayState)-1;
    wii_input_flush();
    for (;;) {
        WiiInput in = wii_input_poll(false);
        if (wii_system_exit_requested() != WII_EXIT_NONE) break;
#ifndef GEMU_WII_AUTOTEST          /* tests stay off the internet: local servers only */
        if (net_state == NET_UP && !lists_asked) { lists_asked = true; netplay_refresh_servers(np); }
#else
        (void)lists_asked;
#endif
        refresh();
        if (info.state != shown) {               /* a new page: start at its top */
            if (info.state == NETPLAY_STARTING && shown != (NetplayState)-1) break;   /* game on */
            shown = info.state;
            sel = top = 0;
        }
        bool close = (in.pressed & (WII_BTN_BACK | WII_BTN_PAUSE)) != 0;
        WiiInput act = in;
        if (close) act.pressed = act.repeat = 0;
        switch (info.state) {
        case NETPLAY_OFFLINE:
        case NETPLAY_CONNECTING: page_connect(&act, &sel, &top); break;
        case NETPLAY_LOBBY:      page_lobby(&act, &sel, &top, &close); break;
        default:                 page_room(&act, &sel, &close); break;
        }
        wii_video_present();
        if (close) break;
    }
    wii_input_flush();
}

/* ── Menu ────────────────────────────────────────────────────────────────── */

static bool is_offline(void *ud, int arg)
{
    (void)ud; (void)arg;
    if (!np) return true;
    NetplayInfo in;
    netplay_info(np, &in);
    return in.state == NETPLAY_OFFLINE;
}
static bool is_online(void *ud, int arg) { return !is_offline(ud, arg); }
static void open_screen(void *ud, int arg) { (void)ud; (void)arg; wii_netplay_screen(); }
static void disconnect(void *ud, int arg) { (void)ud; (void)arg; if (np) netplay_disconnect(np); }

void wii_netplay_menu(GemuMenu *bar)
{
    GemuMenu *m = gemu_menu_add_submenu(bar, "Netplay");
    GemuMenuItem *it = gemu_menu_add(m, GEMU_MENU_ACTION, "Connect...", open_screen, NULL, 0);
    it->enabled = is_offline;
    it = gemu_menu_add(m, GEMU_MENU_ACTION, "Lobby...", open_screen, NULL, 0);
    it->enabled = is_online;
    it = gemu_menu_add(m, GEMU_MENU_ACTION, "Disconnect", disconnect, NULL, 0);
    it->enabled = is_online;
}

/* ── In-game notices ─────────────────────────────────────────────────────── */

void wii_netplay_notices(int *y0, int *y1)
{
    *y0 = *y1 = 0;
    if (!np) return;
    static char last[NETPLAY_LINE_LEN];
    static uint32_t since;
    static char lines[3][NETPLAY_LINE_LEN];
    NetplayInfo in;
    netplay_info(np, &in);
    if (in.state < NETPLAY_ROOM) { last[0] = '\0'; since = 0; return; }
    int n = netplay_log(np, lines, 3);
    if (!n) return;
    if (strcmp(lines[n - 1], last) != 0) {
        snprintf(last, sizeof(last), "%s", lines[n - 1]);
        since = now_ms();
    }
    if (!since || now_ms() - since > NOTICE_MS) return;
    const int W = wii_video_width();
    *y0 = 8;
    *y1 = 8 + n * ROW + 6;
    wii_video_fill(0, *y0, W, *y1 - *y0, WII_COL_BLACK);
    for (int i = 0; i < n; i++) {
        uint8_t c = !strncmp(lines[i], "***", 3) ? WII_COL_ERROR
                  : lines[i][0] == '*' ? WII_COL_GREY : WII_COL_WHITE;
        text(MARGIN, *y0 + 3 + i * ROW, c, lines[i], (W - 2 * MARGIN) / GW);
    }
}
