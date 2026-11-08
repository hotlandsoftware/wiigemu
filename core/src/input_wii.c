#include "gemu/input_wii.h"

#include <gccore.h>
#ifdef HW_RVL
#include <wiiuse/wpad.h>
#endif

volatile bool wii_input_power_requested;
volatile bool wii_input_reset_requested;

static uint32_t prev_held;
static int      hold_frames;
static bool     prev_game;
static uint32_t suppressed;
static bool     flush_pending;

static void on_reset(u32 irq, void *ctx) { (void)irq; (void)ctx; wii_input_reset_requested = true; }

#ifdef HW_RVL
static void on_power(void)            { wii_input_power_requested = true; }
static void on_wpad_power(s32 chan)   { (void)chan; wii_input_power_requested = true; }
#endif

void wii_input_init(void)
{
    PAD_Init();
    SYS_SetResetCallback(on_reset);
#ifdef HW_RVL
    WPAD_Init();
    SYS_SetPowerCallback(on_power);
    WPAD_SetPowerButtonCallback(on_wpad_power);
#endif
}

#ifdef HW_RVL

static uint32_t map_wiimote(uint32_t w, bool game)
{
    uint32_t b = 0;

    /* Always held sideways, D-pad on the left (NES style), in menus and in
     * game alike, so the D-pad is rotated. */
    if (w & WPAD_BUTTON_RIGHT) b |= WII_BTN_UP;
    if (w & WPAD_BUTTON_LEFT)  b |= WII_BTN_DOWN;
    if (w & WPAD_BUTTON_UP)    b |= WII_BTN_LEFT;
    if (w & WPAD_BUTTON_DOWN)  b |= WII_BTN_RIGHT;

    if (game) {             /* 2/1 are the face buttons, A/B extra keys */
        if (w & WPAD_BUTTON_2)     b |= WII_BTN_ACCEPT;
        if (w & WPAD_BUTTON_1)     b |= WII_BTN_BACK;
        if (w & WPAD_BUTTON_A)     b |= WII_BTN_X;
        if (w & WPAD_BUTTON_B)     b |= WII_BTN_Y;
    } else {                /* menus: either button works */
        if (w & (WPAD_BUTTON_A | WPAD_BUTTON_2)) b |= WII_BTN_ACCEPT;
        if (w & (WPAD_BUTTON_B | WPAD_BUTTON_1)) b |= WII_BTN_BACK;
    }
    if (w & (WPAD_BUTTON_MINUS | WPAD_BUTTON_PLUS)) b |= WII_BTN_KEYPAD;
    if (w & WPAD_BUTTON_MINUS)                      b |= WII_BTN_MINUS;
    if (w & WPAD_BUTTON_PLUS)                       b |= WII_BTN_PLUS;
    if (w & WPAD_BUTTON_HOME)                       b |= WII_BTN_PAUSE;

    /* Classic Controller (reported through the same bitmask). */
    if (w & WPAD_CLASSIC_BUTTON_UP)    b |= WII_BTN_UP;
    if (w & WPAD_CLASSIC_BUTTON_DOWN)  b |= WII_BTN_DOWN;
    if (w & WPAD_CLASSIC_BUTTON_LEFT)  b |= WII_BTN_LEFT;
    if (w & WPAD_CLASSIC_BUTTON_RIGHT) b |= WII_BTN_RIGHT;
    if (w & WPAD_CLASSIC_BUTTON_A)     b |= WII_BTN_ACCEPT;
    if (w & WPAD_CLASSIC_BUTTON_B)     b |= WII_BTN_BACK;
    if (w & WPAD_CLASSIC_BUTTON_X)     b |= WII_BTN_X;
    if (w & WPAD_CLASSIC_BUTTON_Y)     b |= WII_BTN_Y;
    if (w & (WPAD_CLASSIC_BUTTON_MINUS | WPAD_CLASSIC_BUTTON_PLUS)) b |= WII_BTN_KEYPAD;
    if (w & WPAD_CLASSIC_BUTTON_MINUS) b |= WII_BTN_MINUS;
    if (w & WPAD_CLASSIC_BUTTON_PLUS)  b |= WII_BTN_PLUS;
    if (w & WPAD_CLASSIC_BUTTON_HOME)  b |= WII_BTN_PAUSE;
    return b;
}
#endif

static uint32_t map_gcpad(uint32_t p, int sx, int sy)
{
    uint32_t b = 0;

    if ((p & PAD_BUTTON_UP)    || sy >  48) b |= WII_BTN_UP;
    if ((p & PAD_BUTTON_DOWN)  || sy < -48) b |= WII_BTN_DOWN;
    if ((p & PAD_BUTTON_LEFT)  || sx < -48) b |= WII_BTN_LEFT;
    if ((p & PAD_BUTTON_RIGHT) || sx >  48) b |= WII_BTN_RIGHT;
    if (p & PAD_BUTTON_A)     b |= WII_BTN_ACCEPT;
    if (p & PAD_BUTTON_B)     b |= WII_BTN_BACK;
    if (p & PAD_BUTTON_X)     b |= WII_BTN_X | WII_BTN_PLUS;
    if (p & PAD_BUTTON_Y)     b |= WII_BTN_Y | WII_BTN_MINUS;
    if (p & PAD_TRIGGER_Z)    b |= WII_BTN_KEYPAD;
    if (p & PAD_BUTTON_START) b |= WII_BTN_PAUSE;
    return b;
}

void wii_input_flush(void) { flush_pending = true; }

WiiInput wii_input_poll(bool game)
{
    PAD_ScanPads();
#ifdef HW_RVL
    WPAD_ScanPads();
#endif

    uint32_t held = 0, port[WII_INPUT_PORTS];
    for (int c = 0; c < WII_INPUT_PORTS; c++) {
        uint32_t pad = PAD_ButtonsHeld(c);
#ifdef HW_RVL
        uint32_t w = WPAD_ButtonsHeld(c);
#  ifdef GEMU_WII_AUTOTEST
        if (c == 0) w |= wii_autotest_pad();
#  endif
        port[c] = map_wiimote(w, game);
#else
#  ifdef GEMU_WII_AUTOTEST
        if (c == 0) pad |= wii_autotest_pad();
#  endif
        port[c] = 0;
#endif
        port[c] |= map_gcpad(pad, PAD_StickX(c), PAD_StickY(c));
        held |= port[c];
    }

    /* Switching layouts (menu <-> game): buttons already down, e.g. the A
     * press that launched a ROM, are ignored until they are released. */
    if (game != prev_game || flush_pending) {
        suppressed = held; prev_game = game; flush_pending = false;
    }
    suppressed &= held;
    held &= ~suppressed;

    WiiInput in = { .held = held, .pressed = held & ~prev_held };
    for (int c = 0; c < WII_INPUT_PORTS; c++) in.port[c] = port[c] & ~suppressed;

    const uint32_t dirs = WII_BTN_UP | WII_BTN_DOWN | WII_BTN_LEFT | WII_BTN_RIGHT;
    if (held & dirs) {
        if (in.pressed & dirs) hold_frames = 0;
        hold_frames++;
    } else {
        hold_frames = 0;
    }
    in.repeat = in.pressed;
    if (hold_frames > 20 && (hold_frames % 4) == 0)
        in.repeat |= held & dirs;

    prev_held = held;
    return in;
}
