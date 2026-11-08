#pragma once
/*
 * NES core for the Wii machine (and the bit-exact test, tests/nes_exact.c):
 * upstream machine_nes.c's emulation - iNES loading, the mappers it
 * implements itself (0-5, 7, 9, 24/26, 66, 119, 178, 228; the rest are in
 * nes_boards.c), the CPU memory map, OAM/DMC DMA, controllers and the
 * PPU/APU sync - without its desktop extras (FDS, VS. System, Zapper,
 * R.O.B., Lua, Game Genie, cartridge-pin faults, monitor, snapshots).
 * The PPU, APU, 2A03 core and boards are upstream's files.
 */
#include "nes.h"
#include <stdbool.h>

/* Why the last nes_core_load failed. */
extern char nes_core_error[160];

/* Load an iNES file into a zeroed NesState.  False (and nes_core_error) if
 * it isn't one, or uses a mapper we lack. */
bool nes_core_load(NesState *s, const char *path);
/* Wire up the CPU, PPU and APU (opening audio for MOS_SOUND_2A03), then
 * power on.  cfg must outlive s. */
void nes_core_init(NesState *s, const MosConfig *cfg);
void nes_core_reset(NesState *s);         /* the console's Reset button */
/* After NesState was overwritten wholesale (pointers already fixed up). */
void nes_core_resync(NesState *s);
/* Run until the PPU finishes a frame (ppu.dirty), then queue its audio.
 * Controller input comes from ctrl_state[]. */
void nes_core_frame(NesState *s);
void nes_core_free(NesState *s);          /* ROM, CHR, audio (not s itself) */
