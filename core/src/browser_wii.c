/*
 * ROM browser: lists a machine's ROMs from the standard folders, sorted,
 * with the machine's built-in program (if any) pinned to the top.
 */
#include "gemu/wii.h"
#include "gemu/menu_wii.h"
#include "gemu/input_wii.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define MAX_ROMS 512

/* Searched in order for every machine; "%s" is the machine name. */
static const char *const rom_dir_fmts[] = {
    "sd:/gemu/%s", "usb:/gemu/%s", "sd:/roms/%s", "usb:/roms/%s",
};

typedef struct {
    char *path;     /* NULL = built-in program */
    char *label;
} RomEntry;

static RomEntry roms[MAX_ROMS];
static int      n_roms;
static char     picked[512];

static void clear_roms(void)
{
    for (int i = 0; i < n_roms; i++) { free(roms[i].path); free(roms[i].label); }
    n_roms = 0;
}

static void add_rom(const char *path, const char *label)
{
    if (n_roms >= MAX_ROMS) return;
    for (int i = 0; i < n_roms; i++)              /* same folder reachable twice */
        if (roms[i].path && path && !strcasecmp(roms[i].path, path)) return;
    roms[n_roms].path  = path ? strdup(path) : NULL;
    roms[n_roms].label = strdup(label);
    n_roms++;
}

static void scan_dir(const WiiMachine *m, const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.' || !wii_machine_handles(m, e->d_name)) continue;
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode))
            add_rom(path, e->d_name);
    }
    closedir(d);
}

static int cmp_entry(const void *a, const void *b)
{
    const RomEntry *x = a, *y = b;
    if (!x->path) return -1;
    if (!y->path) return 1;
    return strcasecmp(x->label, y->label);
}

static void scan(const WiiMachine *m)
{
    clear_roms();
    if (m->builtin_label) add_rom(NULL, m->builtin_label);
    for (size_t i = 0; i < sizeof(rom_dir_fmts) / sizeof(rom_dir_fmts[0]); i++) {
        char dir[128];
        snprintf(dir, sizeof(dir), rom_dir_fmts[i], m->name);
        scan_dir(m, dir);
    }
    if (wii_app_dir()[0]) scan_dir(m, wii_app_dir());
    qsort(roms, (size_t)n_roms, sizeof(roms[0]), cmp_entry);
}

/* Selection is remembered per machine by path, across runs. */
static const WiiMachine *last_machine;
static char              last_path[512];

static int find_last(void)
{
    for (int i = 0; i < n_roms; i++)
        if (roms[i].path ? !strcmp(roms[i].path, last_path) : !last_path[0])
            return i;
    return 0;
}

WiiExit wii_browser_run(const WiiMachine *m, bool can_go_back, const char **rom)
{
    if (m != last_machine) { last_machine = m; last_path[0] = '\0'; }
    scan(m);

    int sel = find_last(), top = 0;
    wii_list_navigate(n_roms, 0, &sel, &top);

    char status[160];
    bool status_err = wii_error()[0] != '\0';
    snprintf(status, sizeof(status), "%s",
             status_err ? wii_error() : wii_storage_ok() ? "" : "No SD/USB storage found");
    status_err = status_err || !wii_storage_ok();
    wii_clear_error();

    char hint[64];
    snprintf(hint, sizeof(hint), "Put ROMs in sd:/gemu/%s/", m->name);
    const bool only_builtin = n_roms == (m->builtin_label ? 1 : 0);
    const char *footer = can_go_back ? "A:Run  B:Back  -:Rescan  HOME:Exit"
                                     : "A:Run  -:Rescan  HOME:Exit";

    for (;;) {
        WiiInput in = wii_input_poll(false);
        WiiExit sys = wii_system_exit_requested();
        if (sys != WII_EXIT_NONE)          return sys;
        if (in.pressed & WII_BTN_PAUSE)   return WII_EXIT_TO_LOADER;
        if (can_go_back && (in.pressed & WII_BTN_BACK)) return WII_EXIT_TO_MENU;

        wii_list_navigate(n_roms, in.repeat, &sel, &top);

        if (in.pressed & WII_BTN_KEYPAD) {     /* -/+ : rescan */
            scan(m);
            sel = top = 0;
            snprintf(status, sizeof(status), "Found %d ROM%s",
                     n_roms - (m->builtin_label ? 1 : 0),
                     n_roms - (m->builtin_label ? 1 : 0) == 1 ? "" : "s");
            status_err = false;
        }
        if ((in.pressed & WII_BTN_ACCEPT) && n_roms > 0) {
            snprintf(last_path, sizeof(last_path), "%s", roms[sel].path ? roms[sel].path : "");
            if (roms[sel].path) {
                snprintf(picked, sizeof(picked), "%s", roms[sel].path);
                *rom = picked;
            } else {
                *rom = NULL;
            }
            return WII_EXIT_NONE;
        }

        const char *labels[MAX_ROMS];
        bool        hl[MAX_ROMS];
        for (int i = 0; i < n_roms; i++) { labels[i] = roms[i].label; hl[i] = !roms[i].path; }
        char title[48];
        snprintf(title, sizeof(title), " %s", m->title ? m->title : m->description);
        wii_list_draw(&(WiiList){
            .post = title, .labels = labels, .highlight = hl, .n = n_roms,
            .empty_hint = only_builtin ? hint : NULL,
            .status = status, .status_error = status_err, .footer = footer,
        }, sel, top);
    }
}
