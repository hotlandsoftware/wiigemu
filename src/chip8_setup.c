/*
 * CHIP-8 family setup for the Wii.
 *
 * Upstream src/chip8_setup.c turns the command line into a Chip8Config; here
 * the ROM browser supplies the ROM and the defaults are the same.
 */
#include "chip8.h"
#include "gemu/wii.h"

#include <string.h>
#include <strings.h>

static const char *const chip8_extensions[] = {
    ".ch8", ".c8", ".chip8", ".8o", NULL,
};

static bool is_octo_source(const char *path)
{
    size_t n = path ? strlen(path) : 0;
    return !path /* built-in demo */ || (n >= 3 && !strcasecmp(path + n - 3, ".8o"));
}

static WiiExit chip8_run(const char *rom_path)
{
    const bool is_8o = is_octo_source(rom_path);
    Chip8Config cfg = {
        .rom_path = rom_path,
        .mem_size = CHIP8_MEM_SIZE,
        .cpu_hz   = is_8o ? CHIP8_DEFAULT_CPF_8O : CHIP8_DEFAULT_CPF,
        .machine  = CHIP8_MACHINE_GENERIC,
        .fg_argb  = is_8o ? 0xFFFFCC00u : 0xFFFFFFFFu,   /* Octo yellow / white */
        .bg_argb  = 0xFF000000u,
    };

    Chip8State *s = chip8_machine_create(&cfg);
    if (!s) return WII_EXIT_TO_MENU;          /* error already set */
    chip8_machine_run(s, &cfg);
    chip8_machine_destroy(s);
    return wii_take_exit();
}

const WiiMachine chip8_wii_machine = {
    .name          = "chip8",
    .description   = "CHIP-8",
    .extensions    = chip8_extensions,
    .builtin_label = "* Built-in demo *",
    .run           = chip8_run,
};
