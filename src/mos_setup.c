/*
 * MOS family setup for the Wii (the 6502 machines).  Upstream
 * src/mos_setup.c turns the command line into a MosConfig; here the ROM
 * browser supplies the cartridge and the defaults are upstream's:
 *   -M atari2600   6507, TIA sound, NTSC
 *   -M nes         2A03, 2C02, 2A03 sound, NTSC
 * except that a joystick / controller is plugged into both ports, so a
 * second Wii controller just works.
 */
#include "atari2600.h"
#include "nes.h"
#include "gemu/wii.h"

#include <stddef.h>

/* Upstream detects the bank-switching scheme from the image, or takes it
 * from a scheme extension (".f8", ".e0", ...). */
static const char *const a2600_extensions[] = {
    ".a26", ".bin", ".rom",
    ".2k", ".4k", ".f8", ".f6", ".f4", ".f8sc", ".f6sc", ".f4sc", ".f8s", ".f6s", ".f4s",
    ".ef", ".efsc", ".df", ".dfsc", ".bf", ".bfsc", ".fe", ".e0", ".3f", ".3e", ".e7",
    ".fa", ".f0", ".mb", ".ua", ".cv", ".0840", ".084", ".sb", ".dpc", ".ar",
    NULL,
};

static WiiExit atari2600_wii_run(const char *rom_path)
{
    MosConfig cfg = {
        .machine        = MOS_MACHINE_ATARI2600,
        .cpu            = MOS_CPU_6507,
        .sound          = MOS_SOUND_TIA,
        .cart_path      = rom_path,          /* NULL: the built-in test cart */
        .a2600_ports    = { A2600_CTRL_CX40, A2600_CTRL_CX40 },
        .n_a2600_ports  = 2,
    };
    Atari2600State *s = atari2600_create(&cfg);
    if (!s) return WII_EXIT_TO_MENU;          /* error already set */
    atari2600_run(s, &cfg);
    atari2600_destroy(s);
    return wii_take_exit();
}

const WiiMachine atari2600_wii_machine = {
    .name          = "atari2600",
    .description   = "Atari 2600",
    .extensions    = a2600_extensions,
    .builtin_label = "* Built-in test cart *",
    .run           = atari2600_wii_run,
};

/* ── Nintendo Entertainment System ───────────────────────────────────────── */

static const char *const nes_extensions[] = { ".nes", NULL };

static WiiExit nes_wii_run(const char *rom_path)
{
    MosConfig cfg = {
        .machine   = MOS_MACHINE_NES,
        .cpu       = MOS_CPU_2A03,
        .vga       = MOS_VGA_RP2C02,
        .sound     = MOS_SOUND_2A03,
        .cart_path = rom_path,
        .ports     = { NES_DEVICE_CONTROLLER, NES_DEVICE_CONTROLLER },
        .n_ports   = 2,
    };
    NesState *s = nes_create(&cfg);
    if (!s) return WII_EXIT_TO_MENU;          /* error already set */
    nes_run(s, &cfg);
    nes_destroy(s);
    return wii_take_exit();
}

const WiiMachine nes_wii_machine = {
    .name        = "nes",
    .description = "Nintendo Entertainment System (NES)",
    .title       = "NES",
    .extensions  = nes_extensions,
    .run         = nes_wii_run,
};
