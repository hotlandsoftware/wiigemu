#pragma once
/*
 * Atari 2600 bus for the Wii machine (and the bit-exact test,
 * tests/a26_exact.c): upstream's memory map, WSYNC hold and power-on, with
 * the TIA run lazily.
 *
 * Upstream runs the TIA up to the CPU's cycle before every bus access.  Its
 * state only matters to the CPU when the CPU touches a TIA register (and to
 * the machine at a frame boundary, which is itself a TIA write: VSYNC), so
 * here it is caught up only then.  Everything the TIA computes is the same;
 * it just happens in fewer, longer runs - which is what makes the TIA's
 * idle-span fast path worthwhile.  Cartridge reads of the common schemes
 * (2K/4K, F8/F6/F4) take a direct path.  tests/a26_exact.c checks the whole
 * thing against upstream, frame by frame and sample by sample.
 */
#include "atari2600.h"

void a26_bus_attach(Atari2600State *s);   /* CPU read/write callbacks, RAM access */
void a26_bus_step(Atari2600State *s);     /* one instruction + WSYNC hold */
void a26_bus_power_on(Atari2600State *s);
void a26_bus_sync(Atari2600State *s);     /* run the TIA up to the CPU */
