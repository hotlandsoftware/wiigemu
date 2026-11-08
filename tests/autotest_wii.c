/*
 * Scripted input for unattended runs in Dolphin (`make AUTOTEST=1`, or
 * `AUTOTEST=N` to stop the script and freeze the machine at poll N).
 *
 * The script is in logical buttons, by input poll (one per video frame), and
 * walks the CHIP-8 flow: pick CHIP-8 on the home screen, run the built-in demo, toggle the keypad, steer,
 * pick a speed from the HOME menu bar, go back to the ROM list, run the 3rd
 * entry, open About and Pause, load the 4th (broken) entry, go back home;
 * then Atari 2600: run the built-in test cart, scroll, fire, Game Reset,
 * Console > B&W, Video > Palette, back to the ROM list, broken.a26, home;
 * then the NES: run the 2nd ROM (smb.nes, when staged), Start, run right
 * and jump, Video > Picture > Square pixels, ROM list, broken.nes, home.
 *
 * Presses are fed in as a sideways Wii Remote's raw buttons (GameCube: pad
 * 1's; see wii_autotest_pad), so they go through the real controller mapping.
 *
 * Useful freeze points: 20 home screen, 45 ROM list, 140 game, 200 keypad focused, 330 key 6 held, 440 Machine menu, 515 Speed
 * submenu, 850 About box, 890 paused game, 1040 load error, 1070 home.
 * 2600: 1150 test cart, 1280 scrolled, 1340 fire held, 1480 Console menu,
 * 1560 B&W, 1650 Video > Palette, 1690 phosphor on, 1800 load error, 1840 home.
 * NES: 1900 ROM list, 2000 title, 2500 running, 2895 Picture menu,
 * 2960 square pixels, 3100 load error, end: home.
 */
#include "gemu/input_wii.h"

#include <stddef.h>
#include <gccore.h>
#ifdef HW_RVL
#include <wiiuse/wpad.h>
#endif

static const struct { int frame, len; uint32_t btn; } autotest_script[] = {
    {  24, 2, WII_BTN_DOWN },                               /* home: -> CHIP-8       */
    {  30, 2, WII_BTN_ACCEPT },                             /* pick CHIP-8           */
    {  60, 2, WII_BTN_ACCEPT },                             /* run built-in demo     */
    { 150, 2, WII_BTN_KEYPAD }, { 250, 2, WII_BTN_KEYPAD }, /* keypad focus on / off */
    { 300, 60, WII_BTN_RIGHT },                             /* steer right (key 6)   */
    { 420, 2, WII_BTN_PAUSE },                              /* HOME: Machine menu    */
    { 450, 2, WII_BTN_DOWN }, { 470, 2, WII_BTN_DOWN },     /* -> Speed              */
    { 490, 2, WII_BTN_RIGHT },                              /* open submenu (on 30)  */
    { 505, 2, WII_BTN_DOWN },                               /* -> 40                 */
    { 525, 2, WII_BTN_ACCEPT },                             /* choose, menu closes   */
    { 600, 2, WII_BTN_PAUSE }, { 630, 2, WII_BTN_UP },      /* wrap to Exit...       */
    { 650, 2, WII_BTN_UP },                                 /* -> Back to ROM list   */
    { 680, 2, WII_BTN_ACCEPT },
    { 720, 2, WII_BTN_DOWN }, { 740, 2, WII_BTN_DOWN },
    { 760, 2, WII_BTN_ACCEPT },                             /* run bounce.ch8        */
    { 820, 2, WII_BTN_PAUSE }, { 830, 2, WII_BTN_LEFT },    /* wrap to Help          */
    { 840, 2, WII_BTN_ACCEPT },                             /* About GEMU            */
    { 860, 2, WII_BTN_ACCEPT },                             /* Close                 */
    { 870, 2, WII_BTN_PAUSE }, { 880, 2, WII_BTN_ACCEPT },  /* Machine > Pause       */
    { 900, 2, WII_BTN_PAUSE }, { 930, 2, WII_BTN_UP }, { 950, 2, WII_BTN_UP },
    { 970, 2, WII_BTN_ACCEPT },                             /* back to ROM list      */
    { 1000, 2, WII_BTN_DOWN }, { 1020, 2, WII_BTN_ACCEPT }, /* broken.8o -> error    */
    { 1060, 2, WII_BTN_BACK },                              /* B: back to home       */
    /* Atari 2600 */
    { 1080, 2, WII_BTN_UP }, { 1100, 2, WII_BTN_ACCEPT },   /* home: Atari 2600      */
    { 1130, 2, WII_BTN_ACCEPT },                            /* built-in test cart    */
    { 1220, 60, WII_BTN_RIGHT },                            /* joystick: scroll      */
    { 1300, 60, WII_BTN_ACCEPT },                           /* fire: tone            */
    { 1400, 4, WII_BTN_PLUS },                              /* + : Game Reset        */
    { 1450, 2, WII_BTN_PAUSE },                             /* HOME                  */
    { 1460, 2, WII_BTN_LEFT }, { 1465, 2, WII_BTN_LEFT },   /* wrap: Help, Video...  */
    { 1470, 2, WII_BTN_LEFT },                              /* ... -> Console        */
    { 1490, 2, WII_BTN_DOWN }, { 1500, 2, WII_BTN_DOWN }, { 1510, 2, WII_BTN_DOWN },
    { 1520, 2, WII_BTN_ACCEPT },                            /* B&W                   */
    { 1600, 2, WII_BTN_PAUSE }, { 1610, 2, WII_BTN_LEFT },  /* HOME, wrap to Help... */
    { 1620, 2, WII_BTN_LEFT },                              /* ... -> Video          */
    { 1640, 2, WII_BTN_RIGHT },                             /* Palette submenu       */
    { 1660, 2, WII_BTN_BACK },                              /* back to Video         */
    { 1665, 2, WII_BTN_DOWN }, { 1670, 2, WII_BTN_ACCEPT }, /* Phosphor on           */
    { 1700, 2, WII_BTN_PAUSE }, { 1710, 2, WII_BTN_UP }, { 1720, 2, WII_BTN_UP },
    { 1730, 2, WII_BTN_ACCEPT },                            /* back to ROM list      */
    { 1760, 2, WII_BTN_DOWN }, { 1770, 2, WII_BTN_ACCEPT }, /* broken.a26 -> error   */
    { 1820, 2, WII_BTN_BACK },                              /* home                  */
    /* NES (smb.nes if staged: run-dolphin.sh --autotest roms/nes/smb.nes) */
    { 1850, 2, WII_BTN_DOWN }, { 1860, 2, WII_BTN_DOWN },   /* home: -> NES          */
    { 1880, 2, WII_BTN_ACCEPT },                            /* ROM list              */
    { 1910, 2, WII_BTN_DOWN }, { 1920, 2, WII_BTN_ACCEPT }, /* 2nd entry: smb.nes    */
    { 2050, 4, WII_BTN_PLUS },                              /* + : Start             */
    { 2300, 500, WII_BTN_RIGHT | WII_BTN_BACK },            /* run right (B held)... */
    { 2340, 20, WII_BTN_ACCEPT }, { 2420, 20, WII_BTN_ACCEPT }, /* ...jumping (A)   */
    { 2500, 20, WII_BTN_ACCEPT }, { 2580, 20, WII_BTN_ACCEPT },
    { 2660, 20, WII_BTN_ACCEPT }, { 2740, 20, WII_BTN_ACCEPT },
    { 2850, 2, WII_BTN_PAUSE },                             /* HOME                  */
    { 2860, 2, WII_BTN_LEFT }, { 2870, 2, WII_BTN_LEFT },   /* wrap: Help, Video     */
    { 2890, 2, WII_BTN_RIGHT },                             /* Picture (on TV shape) */
    { 2900, 2, WII_BTN_DOWN }, { 2910, 2, WII_BTN_ACCEPT }, /* Square pixels         */
    { 3000, 2, WII_BTN_PAUSE }, { 3010, 2, WII_BTN_UP }, { 3020, 2, WII_BTN_UP },
    { 3030, 2, WII_BTN_ACCEPT },                            /* back to ROM list      */
    { 3060, 2, WII_BTN_UP }, { 3070, 2, WII_BTN_ACCEPT },   /* broken.nes -> error   */
    { 3110, 2, WII_BTN_BACK },                              /* home                  */
};

/* AUTOTEST=N stops the script at poll N, freezing the app in that state. */
#if GEMU_WII_AUTOTEST > 1
#  define AUTOTEST_STOP GEMU_WII_AUTOTEST
#else
#  define AUTOTEST_STOP 0x7FFFFFFF
#endif

static int autotest_frame;

bool wii_autotest_frozen(void) { return autotest_frame >= AUTOTEST_STOP; }

/* Logical button -> what a sideways Wii Remote (GameCube: a GC pad) sends. */
static uint32_t to_raw(uint32_t b)
{
    static const struct { uint32_t btn, raw; } map[] = {
#ifdef HW_RVL
        { WII_BTN_UP,     WPAD_BUTTON_RIGHT }, { WII_BTN_DOWN,  WPAD_BUTTON_LEFT },
        { WII_BTN_LEFT,   WPAD_BUTTON_UP },    { WII_BTN_RIGHT, WPAD_BUTTON_DOWN },
        { WII_BTN_ACCEPT, WPAD_BUTTON_2 },     { WII_BTN_BACK,  WPAD_BUTTON_1 },
        { WII_BTN_X,      WPAD_BUTTON_A },     { WII_BTN_Y,     WPAD_BUTTON_B },
        { WII_BTN_KEYPAD, WPAD_BUTTON_MINUS }, { WII_BTN_PAUSE, WPAD_BUTTON_HOME },
        { WII_BTN_MINUS,  WPAD_BUTTON_MINUS }, { WII_BTN_PLUS,  WPAD_BUTTON_PLUS },
#else
        { WII_BTN_UP,     PAD_BUTTON_UP },     { WII_BTN_DOWN,  PAD_BUTTON_DOWN },
        { WII_BTN_LEFT,   PAD_BUTTON_LEFT },   { WII_BTN_RIGHT, PAD_BUTTON_RIGHT },
        { WII_BTN_ACCEPT, PAD_BUTTON_A },      { WII_BTN_BACK,  PAD_BUTTON_B },
        { WII_BTN_X,      PAD_BUTTON_X },      { WII_BTN_Y,     PAD_BUTTON_Y },
        { WII_BTN_KEYPAD, PAD_TRIGGER_Z },     { WII_BTN_PAUSE, PAD_BUTTON_START },
        { WII_BTN_MINUS,  PAD_BUTTON_Y },      { WII_BTN_PLUS,  PAD_BUTTON_X },
#endif
    };
    uint32_t w = 0;
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (b & map[i].btn) w |= map[i].raw;
    return w;
}

static uint32_t script_buttons(void)
{
    /* Once stopped, keep holding what was held at the stop frame, so the
     * frozen picture shows it (held buttons never make new presses). */
    static uint32_t last;
    int frame = autotest_frame;
    if (frame >= AUTOTEST_STOP) return last;
    uint32_t b = 0;
    for (size_t i = 0; i < sizeof(autotest_script) / sizeof(autotest_script[0]); i++)
        if (frame >= autotest_script[i].frame &&
            frame <  autotest_script[i].frame + autotest_script[i].len)
            b |= autotest_script[i].btn;
    autotest_frame++;
    return last = b;
}

uint32_t wii_autotest_pad(void) { return to_raw(script_buttons()); }
