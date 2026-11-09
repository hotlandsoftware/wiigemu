# GEMU for Wii - summary

Homebrew Channel port of GEMU. One `gemu.dol`; machine families are compiled
in with `make TARGETS="..."` (-> `HAVE_<FAMILY>`), like upstream's
`configure --target-list`. Currently: `chip8`, `mos` (Atari 2600, NES).

---

## Build / run

```
source /etc/profile.d/devkit-env.sh
make                       # gemu.dol      make package -> dist/apps/gemu/
tools/run-dolphin.sh       # build + start Dolphin (repo-local profile .dolphin/, SD from sd/)
tools/run-dolphin.sh --autotest[=N]   # scripted run, frame -> build-autotest/dolphin/last-frame.png
make check-upstream GEMU=gemu         # verbatim files still identical to upstream?
```

Verify UI/emulation changes with `--autotest=N` and read the saved frame:
the script (tests/autotest_wii.c) is in input polls, one per video frame;
with N the machine freezes at poll N and CPU registers are drawn on screen.

---

## GameCube build

`make PLATFORM=gc` -> `gemu-gc.dol` from the same sources (`HW_DOL`; Wii-only
code is under `#ifdef HW_RVL`): GC pads only, no power-off, "Exit (reset)",
4:3 always.  ROMs from `sp2:` (SD2SP2), `carda:`/`cardb:` (SD Gecko), in
`gemu/<machine>/` or `roms/<machine>/` (`wii_rom_dirs` in core/src/wii.c).
`make PLATFORM=gc package` -> `dist/gamecube/gemu.dol` (boot it from Swiss).
Dolphin has no GC SD adapter, so `tools/run-dolphin.sh --gc` bakes the staged
SD tree into the DOL (`EMBED=dir`, read-only `embed:` device,
core/src/embed_fs.c + tools/pack_embed.py).  Dolphin runs GC code at the
GC's 486 MHz, so the overlay timings are the real GC's.  NES: heavy scenes
can exceed a vblank there; machine_nes.c then runs two frames per pass and
shows one (frame skip), back to 1:1 once the 60-frame max fits.
Note: an AUTOTEST=N frame can look identical to the boot screen (e.g. 1100) -
check the script before suspecting a hang.

---

## Upstream files - do not edit

Copied verbatim from GEMU, listed in `UPSTREAM` in the Makefile (files in
`PATCHED` carry marked "GEMU for Wii" speed-ups; `make check-upstream`
counts their changed lines):
`core/include/gemu/{tcg,display,monitor,menu}.h`, `core/src/{tcg,menu}.c`,
`cpu/chip8/*`, `vga/x86/cgafont8x8.*`, `vga/multi/vgafont16.*`, and for the
2600 `cpu/mos/mos6502.*`, `vga/mos/tia.*`, `audio/tia_sound.*`,
`hardware/device/m6532.*`, `hardware/machine/mos/a2600_cart.c`, `hardware/mos/*.h`;
for the NES `vga/mos/rp2c02.*`, `audio/apu2a03.*`, `hardware/machine/mos/nes_{boards,pins}.c`,
`hardware/device/fds.h`; and the headers they include. Fix bugs upstream and re-copy.
`compat/SDL2/SDL.h` + `core/src/sdl_audio_wii.c` give upstream sound code the
SDL audio queue it uses (SDL_OpenAudioDevice / SDL_QueueAudio) on ASND.
Wii-specific code goes in `*_wii.c` / `*_wii.h`, `hardware/machine/`, `src/`.

---

## Layers

- `core/src/*_wii.c` - platform: `wii_video_*` (8-bit palette canvas ->
  YUYV XFB, CGA-font text, vsync flip), `wii_input_*` (logical `WII_BTN_*`
  merged from Wii Remote / Classic / GC; `game` = sideways layout; buttons
  held across a menu<->game switch are masked until released),
  `wii_audio_beep()`.
- `core/src/wii.c`, `gemu/wii.h` - `WiiMachine` descriptor, `wii_init()`,
  `wii_set_error()` (shown by the browser), `wii_request_exit()` /
  `wii_take_exit()` (how a void run loop reports why it stopped),
  `wii_system_exit_requested()` (power/reset buttons; check in every loop).
- `core/src/menu_wii.c` - `wii_list_*` full-screen lists (home screen, ROM browser);
  header is `pre` + gold "GEMU" + `post`.
- `core/src/menubar_wii.c` - HOME menu bar: upstream's `gemu/menu.h` model
  drawn and navigated like upstream `gemu_menubar_sdl.c` (8x16 VGA font,
  same colours/metrics, About box). Standard bar Machine/Input/Help, built
  fresh on each open; machines add items through `WiiMenubarHooks`
  (`machine_items`, `input_items`, `reset`, `draw_frame`).
- `core/src/browser_wii.c` - ROM browser per machine: `sd|usb:/gemu|roms/<name>/`
  + app dir, filtered by `WiiMachine.extensions`, built-in entry pinned.
- `src/main.c` - registry of `WiiMachine`s; home screen ("Welcome to GEMU!",
  the machine list, sorted by description, always shown); B in a ROM browser returns to it; argv[1] direct boot.

---

## Adding a machine family

1. Copy the upstream CPU / device sources verbatim to the same paths; add
   them to the family block and `UPSTREAM` in the Makefile.
2. `hardware/machine/<fam>/machine_<name>.c`: implement the upstream
   machine API (`<fam>_machine_create/_run/_destroy` from the CPU header)
   on the Wii backends. Run loop: `wii_input_poll(true)`, check
   `wii_system_exit_requested()`, HOME -> `wii_menubar_open()` (skip guest
   frames while `wii_menubar_paused()`), pace guest
   frames with `gettime()` (video may be 50 Hz), draw, `wii_video_present()`.
   Leave with `wii_request_exit()`; report load errors with `wii_set_error()`.
3. `src/<fam>_setup.c`: build the upstream config struct with upstream's
   defaults, call the machine API, `return wii_take_exit();`, and export
   `const WiiMachine <fam>_wii_machine`.
4. Register it in `src/main.c` under `#ifdef HAVE_<FAM>`; add a
   `machine_for()` case in `tools/run-dolphin.sh`.
5. Machine data in `data/machine/<fam>/` is embedded via the Makefile's
   `DATA` list (`foo.8o` -> `foo_8o[]`, `foo_8o_size`, `#include "foo_8o.h"`).

---

## Machine notes

- Atari 2600 (`hardware/machine/mos/machine_atari2600.c`, setup in
  `src/mos_setup.c`): upstream's memory map / WSYNC / ports / window /
  palettes; built-in test cart from `tools/make_a26_testcart.py`. Paced by
  the audio queue (`AUDIO_TARGET`), up to 3 frames per pass. Menus via the
  `menus` hook: Console, Video; jacks under Input. Machine palette lives at
  128..255 in the video layer (`WII_COL_MACHINE`); phosphor is a video-layer
  blend. Input: `WiiInput.port[0/1]` = left/right joystick,
  `WII_BTN_PLUS/MINUS` = Game Reset / Select.
- Performance (Dolphin, Wii time; budget 16.7 ms/frame): Ms. Pac-Man in play
  ~9.0 ms emulation + 1.2 draw + 1.8 convert, 1 frame per vblank, 60 fps,
  ~6.5 ms idle (was 23.5 ms emulation and 20-34 fps). Exact speed-ups:
  lazy TIA + cart fast paths (2K/4K, F8/F6/F4 w/o SC) in `a2600_bus.c`;
  batched sound (`tia_sound_run`), quiet-HBLANK skip (`hblank_span`) and
  idle spans (`idle_span`/`run_idle`) in tia.c, which also cover objects
  drawing nothing (blank GRPx, missile/ball off: `drawing_shows`,
  `*_skip`) and HBLANK during HMOVE (jumps between movement clocks); 4-pixel
  paths in `wii_video_present` and the blit; -O3 inlining for `HOT_SRCS`.
  While the game runs it draws straight to the XFB (`draw_direct`, one YUYV
  word pair per TIA pixel; canvas path for menu / pause / phosphor), and
  paces with the NES's frame skip. GameCube (Dolphin) Pitfall: 15.9 ms
  emulation + 4.2 draw/convert, 40 fps -> 12.0 + 1.0 ms, 60 fps. The TIA /
  sound changes are `PATCHED`; `make test-a26` must stay identical to upstream
  (add ROMs to roms/atari2600/ or A26_ROMS=...).
- Pacing (`frames_wanted`): 1 frame per vblank; 2 when the audio queue is
  under one ASND buffer, 0 when > ~4 frames ahead. The ASND shim refills
  1024-sample buffers (longer than ASND's ~21 ms mixing pass) only when
  `ASND_TestVoiceBufferReady`; shorter buffers made the voice starve, which
  throttled the audio-paced emulation to ~34 fps with gaps in the sound.
- The autotest overlay (2600, NES) shows emu/draw/convert, 60-frame avg/max,
  fps, loop/present time, frames per loop, audio queue and underruns (NES:
  also PPU and APU time).
- NES (`hardware/machine/mos/machine_nes.c`, UI; `nes_core.c`, emulation):
  `nes_core.c` is upstream machine_nes.c's emulation minus FDS / VS. System /
  Zapper / R.O.B. / Lua / Game Genie / cartridge-pin faults / monitor.
  `make test-nes` builds upstream's machine_nes.c on the host (desktop parts
  stubbed by tests/nes_ref_stubs.c) and checks frames + audio identical
  (ROMs from roms/nes/ or NES_ROMS=..., no spaces in paths). Exact speed-ups:
  lazy PPU sync with deadlines (`ppu_deadline`, `rp2c02_ticks_to_event`: VBlank,
  frame end at dot 339 and 340, mapper dot 260), batched `rp2c02_run` with
  specialized spans, direct CHR windows (`chr_direct`, not MMC2/MMC5), cached
  PRG windows, batched `apu2a03_run`, idle-loop skipping (`skip_idle_loop`:
  a DMA in a pass disqualifies it; never past the frame end). A pending NMI
  from a register write is handed over after exactly one PPU tick, as
  upstream does (AccuracyCoin catches it otherwise). Drawing goes straight
  to the XFB (`wii_video_direct`) unless paused / menu open. Dolphin: SMB
  ~11.3 ms emulation, SMB3 ~12.2 ms, 60 fps (was 35 ms). Battery saves:
  `<rom>.sav` next to the ROM.  Sprites of a span are precomputed into a
  line buffer (`sp[]` in `visible_span`); nametable/attribute/CHR fetches go
  direct when no hook needs them.  Measuring a slow scene: the user's
  Dolphin save state -> `tools/nes_state_from_dolphin.py STATE ROM OUT.nesstate
  [DOL]`, then `tools/run-dolphin.sh --autotest=2040 ROM OUT.nesstate` (the
  autotest build resumes from `<rom>.nesstate`, `load_test_state`). SMB
  World -1 underwater (Bloopers): 15.7 -> 11.8 ms. The user tests the other
  machines themselves: don't rerun 2600 checks for NES-only changes.

## Known upstream CHIP-8 issues (also affect desktop GEMU)

- TB cache is not invalidated on guest stores (`Fx55`, `Fx33`), so
  self-modifying code runs stale translations.
- CALL/RET don't check stack over/underflow.
