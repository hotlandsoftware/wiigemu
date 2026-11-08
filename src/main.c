/*
 * GEMU for Wii - Homebrew Channel entry point.
 *
 * Like upstream src/main.c this only dispatches to machine families, which
 * are compiled in by `make TARGETS="..."` (HAVE_<FAMILY>).  The home screen
 * ("Welcome to GEMU!") lists them; picking one opens its ROM browser, and B
 * there comes back here.
 *
 * A ROM path passed as the first argument (HBC meta.xml <arguments>, or a
 * forwarder) boots straight into the family that handles its extension.
 */
#include "gemu/wii.h"
#include "gemu/menu_wii.h"
#include "gemu/input_wii.h"

#include <stddef.h>
#include <stdlib.h>
#include <strings.h>

#ifdef HAVE_CHIP8
extern const WiiMachine chip8_wii_machine;
#endif
#ifdef HAVE_MOS
extern const WiiMachine atari2600_wii_machine;
extern const WiiMachine nes_wii_machine;
#endif

static const WiiMachine *const registry[] = {
#ifdef HAVE_CHIP8
    &chip8_wii_machine,
#endif
#ifdef HAVE_MOS
    &atari2600_wii_machine,
    &nes_wii_machine,
#endif
};
#define N_MACHINES ((int)(sizeof(registry) / sizeof(registry[0])))

_Static_assert(sizeof(registry) > 0, "no machine families enabled (TARGETS)");

/* The home screen lists machines alphabetically, whatever the build order. */
static const WiiMachine *machines[N_MACHINES];

static int by_description(const void *a, const void *b)
{
    return strcasecmp((*(const WiiMachine *const *)a)->description,
                      (*(const WiiMachine *const *)b)->description);
}

static void sort_machines(void)
{
    for (int i = 0; i < N_MACHINES; i++) machines[i] = registry[i];
    qsort(machines, N_MACHINES, sizeof(machines[0]), by_description);
}

static const WiiMachine *machine_for_path(const char *path)
{
    for (int i = 0; i < N_MACHINES; i++)
        if (wii_machine_handles(machines[i], path)) return machines[i];
    return NULL;
}

/* Home screen: the machine list.  NULL with *why set when the user leaves. */
static const WiiMachine *pick_machine(WiiExit *why)
{
    static int sel, top;
    const char *labels[N_MACHINES];
    for (int i = 0; i < N_MACHINES; i++) labels[i] = machines[i]->description;

    for (;;) {
        WiiInput in = wii_input_poll(false);
        WiiExit sys = wii_system_exit_requested();
        if (sys != WII_EXIT_NONE)        { *why = sys; return NULL; }
        if (in.pressed & WII_BTN_PAUSE)  { *why = WII_EXIT_TO_LOADER; return NULL; }
        wii_list_navigate(N_MACHINES, in.repeat, &sel, &top);
        if (in.pressed & WII_BTN_ACCEPT) { wii_clear_error(); return machines[sel]; }

        wii_list_draw(&(WiiList){
            .pre = "Welcome to ", .post = "!",
            .labels = labels, .n = N_MACHINES,
            .status = wii_error(), .status_error = true,
            .footer = "A:Select  HOME:Exit",
        }, sel, top);
    }
}

int main(int argc, char **argv)
{
    wii_init(argc, argv);
    sort_machines();
    WiiExit why = WII_EXIT_NONE;
    const WiiMachine *m = NULL;         /* NULL = home screen */

    if (argc > 1 && argv[1] && argv[1][0]) {
        const WiiMachine *direct = machine_for_path(argv[1]);
        if (direct) {
            m = direct;
            why = direct->run(argv[1]);
        } else {
            wii_set_error("No machine handles %s", argv[1]);
        }
    }

    while (why == WII_EXIT_NONE || why == WII_EXIT_TO_MENU) {
        why = WII_EXIT_NONE;
        if (!m && !(m = pick_machine(&why))) break;

        const char *rom = NULL;
        why = wii_browser_run(m, true, &rom);
        if (why == WII_EXIT_TO_MENU) { m = NULL; continue; }   /* back to list */
        if (why != WII_EXIT_NONE) break;

        why = m->run(rom);
    }

    wii_shutdown(why);
    return 0;   /* back to the Homebrew Channel */
}
