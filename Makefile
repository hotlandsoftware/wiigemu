# GEMU for Wii - build with devkitPPC + libogc (devkitPro "wii-dev").
#
#   make                  -> gemu.dol
#   make PLATFORM=gc      -> gemu-gc.dol, the GameCube build (same code; ROMs
#                            from an SD Gecko / SD2SP2; EMBED=dir bakes a
#                            <machine>/<rom> tree into it, for Dolphin)
#   make TARGETS="chip8"  -> choose machine families (default: all)
#   make package          -> dist/apps/gemu/ for the Homebrew Channel
#   make run              -> build and start in Dolphin (tools/run-dolphin.sh)
#   make AUTOTEST=1       -> gemu-autotest.dol, scripted input (tests/autotest_wii.c)
#   make check-upstream   -> diff the verbatim GEMU files against $(GEMU)
#   make test-a26 / test-nes -> 2600 / NES output bit-identical to upstream (host gcc)

ifeq ($(strip $(DEVKITPRO)),)
$(error DEVKITPRO is not set: source /etc/profile.d/devkit-env.sh (see tools/install-devkitpro.sh))
endif
ifeq ($(strip $(DEVKITPPC)),)
DEVKITPPC := $(DEVKITPRO)/devkitPPC
endif

PLATFORM ?= wii
ifeq ($(PLATFORM),wii)
include $(DEVKITPPC)/wii_rules
SUFFIX   :=
PLATLIBS := -lwiiuse -lbte
else ifeq ($(PLATFORM),gc)
include $(DEVKITPPC)/gamecube_rules
SUFFIX   := -gc
PLATLIBS :=
else
$(error PLATFORM must be wii or gc)
endif

TARGET  := gemu$(SUFFIX)
BUILD   := build$(SUFFIX)
.DEFAULT_GOAL := all
TARGETS ?= chip8 mos
GEMU    ?= gemu
DEFINES :=

ifneq ($(AUTOTEST),)
DEFINES += -DGEMU_WII_AUTOTEST=$(AUTOTEST)
BUILD   := build$(SUFFIX)-autotest
TARGET  := gemu$(SUFFIX)-autotest
endif

# ── Sources ──────────────────────────────────────────────────────────────────
# UPSTREAM files are verbatim copies from GEMU; keep them that way so they
# can be re-synced (make check-upstream GEMU=/path/to/gemu).

UPSTREAM := core/include/gemu/tcg.h core/include/gemu/display.h \
            core/include/gemu/monitor.h core/include/gemu/menu.h \
            core/include/gemu/gemu.h \
            core/src/tcg.c core/src/menu.c \
            vga/x86/cgafont8x8.c vga/x86/cgafont8x8.h \
            vga/multi/vgafont16.c vga/multi/vgafont16.h

SRCS := src/main.c \
        core/src/tcg.c core/src/menu.c \
        core/src/wii.c core/src/video_wii.c core/src/input_wii.c \
        core/src/audio_wii.c core/src/menu_wii.c core/src/menubar_wii.c \
        core/src/browser_wii.c \
        vga/x86/cgafont8x8.c vga/multi/vgafont16.c
DATA :=

ifneq ($(AUTOTEST),)
SRCS += tests/autotest_wii.c
endif

# Files baked into the DOL as the read-only embed: device (core/src/embed_fs.c):
# EMBED=dir holding <machine>/<rom>, listed as embed:/gemu/<machine>/.
# Dolphin has no GameCube SD adapter, so that's how its GC runs get ROMs.
ifneq ($(EMBED),)
DEFINES += -DGEMU_EMBED
SRCS    += core/src/embed_fs.c
DATA    += $(BUILD)/embed.bin
$(BUILD)/embed.bin: FORCE
	@mkdir -p $(BUILD)
	@python3 -I tools/pack_embed.py "$(EMBED)" $@
endif

# One block per machine family.
ifneq ($(filter chip8,$(TARGETS)),)
DEFINES  += -DHAVE_CHIP8
SRCS     += src/chip8_setup.c \
            cpu/chip8/chip8_cpu.c cpu/chip8/octo.c \
            hardware/machine/chip8/machine_chip8.c
DATA     += data/machine/chip8/demo.8o
UPSTREAM += cpu/chip8/chip8.h cpu/chip8/chip8_cpu.c cpu/chip8/octo.c cpu/chip8/octo.h
endif

ifneq ($(filter mos,$(TARGETS)),)
DEFINES  += -DHAVE_MOS
SRCS     += src/mos_setup.c \
            cpu/mos/mos6502.c vga/mos/tia.c audio/tia_sound.c \
            hardware/device/m6532.c hardware/machine/mos/a2600_cart.c \
            hardware/machine/mos/a2600_bus.c hardware/machine/mos/machine_atari2600.c \
            vga/mos/rp2c02.c audio/apu2a03.c \
            hardware/machine/mos/nes_core.c hardware/machine/mos/nes_boards.c \
            hardware/machine/mos/nes_pins.c hardware/machine/mos/machine_nes.c \
            core/src/sdl_audio_wii.c
DATA     += data/machine/atari2600/testcart.a26
UPSTREAM += cpu/mos/mos6502.c cpu/mos/mos6502.h \
            hardware/device/m6532.c hardware/device/m6532.h hardware/device/hex_editor.h \
            hardware/machine/mos/a2600_cart.c hardware/mos/a2600_cart.h \
            hardware/mos/atari2600.h hardware/mos/mos6502cfg.h \
            core/include/gemu/gemu_display.h core/include/gemu/serial.h \
            hardware/mos/nes.h hardware/mos/nes_boards.h hardware/mos/nes_pins.h \
            hardware/machine/mos/nes_boards.c hardware/machine/mos/nes_pins.c \
            hardware/device/fds.h
# Upstream files carrying Wii speed-ups (lazy batched sound, TIA idle spans,
# batched PPU and APU runs), output bit-identical to upstream's (make test-a26,
# make test-nes).  Marked "GEMU for Wii".
PATCHED  += vga/mos/tia.c vga/mos/tia.h audio/tia_sound.c audio/tia_sound.h \
            vga/mos/rp2c02.c vga/mos/rp2c02.h audio/apu2a03.c audio/apu2a03.h
endif

# Hot emulation cores (run per colour clock / per cycle): inline aggressively.
# On the 2600 this is ~25% faster than plain -O2.
HOT_SRCS := cpu/mos/mos6502.c vga/mos/tia.c audio/tia_sound.c \
            vga/mos/rp2c02.c audio/apu2a03.c hardware/machine/mos/nes_core.c
$(addprefix $(BUILD)/,$(HOT_SRCS:.c=.o)): CFLAGS += -O3 -finline-limit=1000

# Embedded data: data/.../foo.8o -> foo_8o[] / foo_8o_size, header foo_8o.h
data_base  = $(BUILD)/data/$(subst .,_,$(notdir $(1)))
DATA_OBJS := $(foreach f,$(DATA),$(call data_base,$(f)).o)
DATA_HDRS := $(DATA_OBJS:.o=.h)
OBJS := $(addprefix $(BUILD)/,$(SRCS:.c=.o)) $(DATA_OBJS)
DEPS := $(addprefix $(BUILD)/,$(SRCS:.c=.d))

# Unified include path, as in upstream's configure.
INCLUDES := -Icore/include -Icpu -Icpu/chip8 -Icpu/mos -Ivga -Ivga/x86 -Ivga/multi -Ivga/mos \
            -Iaudio -Ihardware -Ihardware/mos -Ihardware/device \
            -Ihardware/machine -Ihardware/machine/chip8 -Ihardware/machine/mos -Isrc \
            -Icompat \
            -I$(BUILD)/data \
            -I$(LIBOGC_INC) $(foreach d,$(PORTLIBS),-I$(d)/include)
CFLAGS   := -O2 -g -Wall -Wextra -Wno-unused-parameter -std=gnu11 \
            $(MACHDEP) $(DEFINES) $(INCLUDES) -MMD -MP
LDFLAGS  := -g $(MACHDEP) -Wl,-Map,$(BUILD)/$(TARGET).map
LIBDIRS  := -L$(LIBOGC_LIB) $(foreach d,$(PORTLIBS),-L$(d)/lib)
LIBS     := -lfat -lasnd $(PLATLIBS) -logc -lm

.PHONY: all package run clean check-upstream test-a26 test-nes FORCE

all: $(TARGET).dol

$(TARGET).elf: $(OBJS)
	@echo linking $@
	@$(CC) $(LDFLAGS) $(OBJS) $(LIBDIRS) $(LIBS) -o $@

# Flag changes rebuild everything: edits to this file, and DEFINES changing
# between runs (e.g. EMBED=... on or off), recorded in $(BUILD)/defines.
DEFS_STAMP := $(BUILD)/defines
$(shell mkdir -p $(BUILD); echo '$(DEFINES)' | cmp -s - $(DEFS_STAMP) || echo '$(DEFINES)' > $(DEFS_STAMP))
$(OBJS): Makefile $(DEFS_STAMP)

# Sources may include generated data headers.
$(addprefix $(BUILD)/,$(SRCS:.c=.o)): | $(DATA_HDRS)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	@echo $<
	@$(CC) $(CFLAGS) -c $< -o $@

define data_rule
$(call data_base,$(1)).o $(call data_base,$(1)).h &: $(1)
	@mkdir -p $(BUILD)/data
	@echo $(1)
	@bin2s -a 32 -H $(call data_base,$(1)).h $(1) > $(call data_base,$(1)).s
	@$(CC) -x assembler-with-cpp $(MACHDEP) -c $(call data_base,$(1)).s -o $(call data_base,$(1)).o
endef
$(foreach f,$(DATA),$(eval $(call data_rule,$(f))))

ifeq ($(PLATFORM),gc)
package: $(TARGET).dol
	@mkdir -p dist/gamecube
	cp $(TARGET).dol dist/gamecube/gemu.dol
	@echo "Copy dist/gamecube/gemu.dol to your SD card and start it from Swiss;"
	@echo "ROMs go in gemu/<machine>/ on the same card."
else
package: $(TARGET).dol
	@mkdir -p dist/apps/gemu
	cp $(TARGET).dol dist/apps/gemu/boot.dol
	cp packaging/hbc/meta.xml packaging/hbc/icon.png dist/apps/gemu/
	@echo "Copy dist/apps/ to the root of your SD card."
endif

run:
	tools/run-dolphin.sh

check-upstream:
	@test -d "$(GEMU)" || { echo "no GEMU checkout at '$(GEMU)' (set GEMU=...)"; exit 1; }
	@rc=0; for f in $(UPSTREAM); do \
	    cmp -s "$$f" "$(GEMU)/$$f" || { echo "differs: $$f"; rc=1; }; \
	done; [ $$rc = 0 ] && echo "all $(words $(UPSTREAM)) upstream files match $(GEMU)"; \
	for f in $(PATCHED); do \
	    echo "patched: $$f ($$(diff -u "$(GEMU)/$$f" "$$f" | grep -c '^[-+][^-+]') lines changed)"; \
	done; exit $$rc

# ── Atari 2600 bit-exactness: our TIA/bus vs upstream GEMU's (host build) ──
HOSTCC      ?= gcc
A26_ROMS    ?= $(wildcard roms/atari2600/*.a26 roms/atari2600/*.bin)
A26_CYCLES  ?= 12000000
A26_TEST    := build-host/a26
A26_CORE     = cpu/mos/mos6502.c vga/mos/tia.c audio/tia_sound.c \
               hardware/device/m6532.c hardware/machine/mos/a2600_cart.c
A26_INC      = -Icore/include -Icpu/mos -Ivga/mos -Iaudio -Ihardware/mos -Ihardware/device

test-a26:
	@test -d "$(GEMU)" || { echo "needs the upstream GEMU checkout in '$(GEMU)' (set GEMU=...)"; exit 1; }
	@mkdir -p $(A26_TEST)
	@$(HOSTCC) -O2 -DA26_REF -Icompat $(subst -I,-I$(GEMU)/,$(A26_INC)) tests/a26_exact.c \
	    $(addprefix $(GEMU)/,$(A26_CORE)) -o $(A26_TEST)/ref
	@$(HOSTCC) -O2 -Icompat $(A26_INC) -Ihardware/machine/mos tests/a26_exact.c \
	    $(A26_CORE) hardware/machine/mos/a2600_bus.c -o $(A26_TEST)/ours
	@rc=0; for rom in data/machine/atari2600/testcart.a26 $(A26_ROMS); do \
	    n=$$(basename "$$rom"); \
	    t0=$$(date +%s%N); $(A26_TEST)/ref  "$$rom" $(A26_CYCLES) $(A26_TEST)/ref  >/dev/null; \
	    t1=$$(date +%s%N); $(A26_TEST)/ours "$$rom" $(A26_CYCLES) $(A26_TEST)/ours >/dev/null; \
	    t2=$$(date +%s%N); \
	    python3 -I tools/a26_compare.py $(A26_TEST)/ref $(A26_TEST)/ours "$$n" || rc=1; \
	    echo "     upstream $$(( (t1-t0)/1000000 )) ms, ours $$(( (t2-t1)/1000000 )) ms"; \
	done; exit $$rc

# ── NES bit-exactness: our nes_core.c vs upstream's machine_nes.c (host build) ──
NES_ROMS    ?= $(wildcard roms/nes/*.nes)
NES_FRAMES  ?= 1800
NES_TEST    := build-host/nes
NES_CORE     = vga/mos/rp2c02.c audio/apu2a03.c cpu/mos/mos6502.c \
               hardware/machine/mos/nes_boards.c hardware/machine/mos/nes_pins.c
NES_INC      = -Icore/include -Icpu/mos -Ivga/mos -Iaudio -Ihardware -Ihardware/mos -Ihardware/device

test-nes:
	@test -d "$(GEMU)" || { echo "needs the upstream GEMU checkout in '$(GEMU)' (set GEMU=...)"; exit 1; }
	@test -n "$(NES_ROMS)" || { echo "no ROMs: put .nes files in roms/nes/ or set NES_ROMS=..."; exit 1; }
	@mkdir -p $(NES_TEST)
	@$(HOSTCC) -O2 -w -DNES_REF $$(pkg-config --cflags sdl2) $(subst -I,-I$(GEMU)/,$(NES_INC)) -I$(GEMU)/crt \
	    tests/nes_exact.c tests/nes_ref_stubs.c $(GEMU)/hardware/machine/mos/machine_nes.c \
	    $(addprefix $(GEMU)/,$(NES_CORE)) -lm -o $(NES_TEST)/ref
	@$(HOSTCC) -O2 -Icompat $(NES_INC) -Ihardware/machine/mos tests/nes_exact.c \
	    hardware/machine/mos/nes_core.c $(NES_CORE) -lm -o $(NES_TEST)/ours
	@rc=0; for rom in $(NES_ROMS); do \
	    n=$$(basename "$$rom"); \
	    t0=$$(date +%s%N); $(NES_TEST)/ref  "$$rom" $(NES_FRAMES) $(NES_TEST)/ref  >/dev/null 2>&1; \
	    t1=$$(date +%s%N); $(NES_TEST)/ours "$$rom" $(NES_FRAMES) $(NES_TEST)/ours >/dev/null; \
	    t2=$$(date +%s%N); \
	    python3 -I tools/a26_compare.py $(NES_TEST)/ref $(NES_TEST)/ours "$$n" || rc=1; \
	    echo "     upstream $$(( (t1-t0)/1000000 )) ms, ours $$(( (t2-t1)/1000000 )) ms"; \
	done; exit $$rc

clean:
	rm -rf build build-autotest build-gc build-gc-autotest build-host dist \
	    gemu.elf gemu.dol gemu-autotest.elf gemu-autotest.dol \
	    gemu-gc.elf gemu-gc.dol gemu-gc-autotest.elf gemu-gc-autotest.dol

-include $(DEPS)
