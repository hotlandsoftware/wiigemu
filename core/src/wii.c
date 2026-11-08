#include "gemu/wii.h"
#include "gemu/video_wii.h"
#include "gemu/input_wii.h"
#include "gemu/audio_wii.h"

#include <gccore.h>
#include <fat.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

static bool storage_ok;
static char app_dir[256];
static char last_error[160];
static WiiExit exit_request;

void wii_init(int argc, char **argv)
{
    wii_video_init();
    wii_input_init();
    wii_audio_init();
    storage_ok = fatInitDefault();

    /* argv[0] from the Homebrew Channel: "sd:/apps/gemu/boot.dol". */
    const char *argv0 = argc > 0 ? argv[0] : NULL;
    if (argv0 && strchr(argv0, ':')) {
        snprintf(app_dir, sizeof(app_dir), "%s", argv0);
        char *slash = strrchr(app_dir, '/');
        if (slash) *slash = '\0'; else app_dir[0] = '\0';
    }
}

void wii_shutdown(WiiExit why)
{
    wii_audio_shutdown();
    if (why == WII_EXIT_POWEROFF)
        SYS_ResetSystem(SYS_POWEROFF, 0, 0);
    /* Returning from main() goes back to the Homebrew Channel. */
}

bool        wii_storage_ok(void) { return storage_ok; }
const char *wii_app_dir(void)    { return app_dir; }

void    wii_request_exit(WiiExit why) { exit_request = why; }
WiiExit wii_take_exit(void)
{
    WiiExit why = exit_request;
    exit_request = WII_EXIT_NONE;
    return why;
}

WiiExit wii_system_exit_requested(void)
{
    if (wii_input_power_requested) return WII_EXIT_POWEROFF;
    if (wii_input_reset_requested) return WII_EXIT_TO_LOADER;
    return WII_EXIT_NONE;
}

void wii_set_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(last_error, sizeof(last_error), fmt, ap);
    va_end(ap);
}

const char *wii_error(void)       { return last_error; }
void        wii_clear_error(void) { last_error[0] = '\0'; }

bool wii_machine_handles(const WiiMachine *m, const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot) return false;
    for (const char *const *e = m->extensions; *e; e++)
        if (!strcasecmp(dot, *e)) return true;
    return false;
}
