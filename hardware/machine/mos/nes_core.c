/*
 * NES core - see nes_core.h.  The code is upstream machine_nes.c's, cut down
 * to what a cartridge NES with standard controllers uses; comments marked
 * "Wii" say where it differs.
 */
#include "nes_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char nes_core_error[160];

#ifdef GEMU_WII_AUTOTEST
/* Where a frame's time goes (the autotest overlay). */
#include <ogc/lwp_watchdog.h>
uint64_t nes_core_ppu_ticks, nes_core_apu_ticks;
#  define TIMED(acc, stmt) do { u64 t_ = gettime(); stmt; acc += diff_ticks(t_, gettime()); } while (0)
#else
#  define TIMED(acc, stmt) stmt
#endif

/* ── iNES cartridge loading ──────────────────────────────────────────────── */

/* Wii: errors go to nes_core_error rather than stderr. */
static bool ines_load(NesState *s, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(nes_core_error, sizeof(nes_core_error), "Cannot open the file"); return false; }

    uint8_t hdr[16];
    if (fread(hdr, 1, 16, f) != 16 ||
        hdr[0] != 'N' || hdr[1] != 'E' || hdr[2] != 'S' || hdr[3] != 0x1A) {
        snprintf(nes_core_error, sizeof(nes_core_error), "Not an iNES (.nes) file");
        fclose(f); return false;
    }

    s->cart.prg_banks   = hdr[4];
    s->cart.chr_banks   = hdr[5];
    s->cart.mapper      = (hdr[7] & 0xF0) | (hdr[6] >> 4);
    s->cart.has_battery = (hdr[6] >> 1) & 1;

    bool four_screen = (hdr[6] >> 3) & 1;
    bool vertical    = hdr[6] & 1;
    if      (four_screen) s->cart.mirror = RP2C02_MIRROR_4SCREEN;
    else if (vertical)    s->cart.mirror = RP2C02_MIRROR_VERTICAL;
    else                  s->cart.mirror = RP2C02_MIRROR_HORIZONTAL;

    switch (s->cart.mapper) {
    case 0: case 1: case 2: case 3: case 4: case 5: case 7: case 9: case 24: case 26:
    case 66: case 119: case 178: case 228: break;
    default:
        if (nes_board_supported(s->cart.mapper)) break;
        snprintf(nes_core_error, sizeof(nes_core_error), "Mapper %u is not supported",
                 s->cart.mapper);
        fclose(f); return false;
    }
    if (s->cart.prg_banks == 0) {
        snprintf(nes_core_error, sizeof(nes_core_error), "Bad header: no PRG ROM");
        fclose(f); return false;
    }
    if ((s->cart.mapper == 0 || s->cart.mapper == 3) && s->cart.prg_banks > 2) {
        snprintf(nes_core_error, sizeof(nes_core_error), "Bad header: %s with %u x 16 KB PRG",
                 s->cart.mapper ? "CNROM" : "NROM", s->cart.prg_banks);
        fclose(f); return false;
    }

    /* Skip optional 512-byte trainer */
    if ((hdr[6] >> 2) & 1) fseek(f, 512, SEEK_CUR);

    /* Some dumps claim more PRG than the file holds; if what's there is a
     * whole number of 16 KB banks plus the stated CHR, believe the file. */
    long data_start = ftell(f);
    fseek(f, 0, SEEK_END);
    long data_len = ftell(f) - data_start;
    fseek(f, data_start, SEEK_SET);
    long chr_len  = (long)s->cart.chr_banks * 0x2000;
    long prg_have = data_len - chr_len;
    if (prg_have > 0 && prg_have < (long)s->cart.prg_banks * 0x4000 && prg_have % 0x4000 == 0)
        s->cart.prg_banks = (uint8_t)(prg_have / 0x4000);

    uint32_t prg_bytes = (uint32_t)s->cart.prg_banks * 0x4000;
    s->prg = malloc(prg_bytes);
    if (!s->prg || fread(s->prg, 1, prg_bytes, f) != prg_bytes) {
        snprintf(nes_core_error, sizeof(nes_core_error), "The file is cut short (PRG ROM)");
        fclose(f); free(s->prg); s->prg = NULL; return false;
    }

    if (s->cart.chr_banks > 0) {
        uint32_t chr_bytes = (uint32_t)s->cart.chr_banks * 0x2000;
        s->chr = malloc(chr_bytes);
        if (!s->chr || fread(s->chr, 1, chr_bytes, f) != chr_bytes) {
            snprintf(nes_core_error, sizeof(nes_core_error), "The file is cut short (CHR ROM)");
            fclose(f); free(s->chr); s->chr = NULL; free(s->prg); s->prg = NULL; return false;
        }
        s->chr_is_ram = false;
        if (s->cart.mapper == 119) {   /* TQROM: 8 KB CHR RAM after the ROM */
            uint8_t *chr = realloc(s->chr, chr_bytes + 0x2000u);
            if (!chr) { fclose(f); free(s->chr); s->chr = NULL; free(s->prg); s->prg = NULL; return false; }
            memset(chr + chr_bytes, 0, 0x2000u);
            s->chr = chr;
        }
    } else {
        /* CHR RAM: 8 KB writable by PPU */
        s->chr = calloc(1, 0x2000);
        if (!s->chr) { fclose(f); free(s->prg); s->prg = NULL; return false; }
        s->chr_is_ram = true;
        s->cart.chr_banks = 1;
    }

    fclose(f);
    s->has_board  = nes_board_supported(s->cart.mapper);
    s->fine_banks = s->has_board || s->cart.mapper == 4 || s->cart.mapper == 119 ||
                    s->cart.mapper == 24 || s->cart.mapper == 26;
    snprintf(s->cart_path_buf, sizeof(s->cart_path_buf), "%s", path);
    return true;
}

/* MMC3 and TQROM (MMC3 with 8 KB of CHR RAM beside the CHR ROM). */
static bool is_mmc3(const NesState *s) {
    return s->cart.mapper == 4 || s->cart.mapper == 119;
}

/* ── Mapper 1 (MMC1/SxROM) ───────────────────────────────────────────────── */

static void mmc1_update_banks(NesState *s) {
    uint8_t  prg_mode  = (s->mmc1_ctrl >> 2) & 3;
    uint8_t  chr_mode  = (s->mmc1_ctrl >> 4) & 1;
    uint8_t  prg_bank  =  s->mmc1_prg  & 0x0F;
    uint32_t prg_size  = (uint32_t)s->cart.prg_banks * 0x4000u;
    uint32_t prg_last  = prg_size - 0x4000u;
    uint32_t chr_size  = (uint32_t)s->cart.chr_banks * 0x2000u;

    switch (prg_mode) {
    case 0: case 1:   /* 32 KB: switch both halves together */
        s->prg_offsets[0] = ((uint32_t)(prg_bank & ~1u) * 0x4000u) % prg_size;
        s->prg_offsets[1] =  s->prg_offsets[0] + 0x4000u;
        break;
    case 2:           /* fix first bank at $8000, switch $C000 */
        s->prg_offsets[0] = 0;
        s->prg_offsets[1] = ((uint32_t)prg_bank * 0x4000u) % prg_size;
        break;
    case 3:           /* switch $8000, fix last bank at $C000 */
        s->prg_offsets[0] = ((uint32_t)prg_bank * 0x4000u) % prg_size;
        s->prg_offsets[1] = prg_last;
        break;
    }

    if (chr_mode == 0) {   /* 8 KB: one window */
        s->chr_offsets[0] = ((uint32_t)(s->mmc1_chr0 & ~1u) * 0x1000u) % chr_size;
        s->chr_offsets[1] =  s->chr_offsets[0] + 0x1000u;
    } else {               /* 4 KB x 2: independent windows */
        s->chr_offsets[0] = ((uint32_t)s->mmc1_chr0 * 0x1000u) % chr_size;
        s->chr_offsets[1] = ((uint32_t)s->mmc1_chr1 * 0x1000u) % chr_size;
    }

    static const uint8_t mirror_map[] = {
        RP2C02_MIRROR_SINGLE_A, RP2C02_MIRROR_SINGLE_B,
        RP2C02_MIRROR_VERTICAL, RP2C02_MIRROR_HORIZONTAL
    };
    s->ppu.mirror = mirror_map[s->mmc1_ctrl & 3];
}

static void mmc1_serial_write(NesState *s, uint16_t addr, uint8_t val) {
    if (val & 0x80) {          /* reset: force PRG mode 3 */
        s->mmc1_shift       = 0;
        s->mmc1_shift_count = 0;
        s->mmc1_ctrl       |= 0x0C;
        mmc1_update_banks(s);
        return;
    }
    /* Shift bit 0 in from the MSB side; after 5 writes the register is full. */
    s->mmc1_shift = (s->mmc1_shift >> 1) | ((val & 1u) << 4);
    if (++s->mmc1_shift_count < 5) return;

    uint8_t reg = (addr >> 13) & 3;
    switch (reg) {
    case 0: s->mmc1_ctrl = s->mmc1_shift; break;
    case 1: s->mmc1_chr0 = s->mmc1_shift; break;
    case 2: s->mmc1_chr1 = s->mmc1_shift; break;
    case 3: s->mmc1_prg  = s->mmc1_shift; break;
    }
    s->mmc1_shift       = 0;
    s->mmc1_shift_count = 0;
    mmc1_update_banks(s);
}

/* ── Mapper 4 (MMC3/TxROM) ───────────────────────────────────────────────── */

static void mmc3_update_banks(NesState *s) {
    uint32_t prg_8k   = (uint32_t)s->cart.prg_banks * 2;   /* total 8 KB banks */
    uint32_t chr_1k   = (uint32_t)s->cart.chr_banks * 8;   /* total 1 KB banks */

    /* PRG: four 8 KB windows at $8000, $A000, $C000, $E000 */
    uint8_t  prg_mode = (s->mmc3_bank_sel >> 6) & 1;
    uint32_t r6 = (s->mmc3_regs[6] % prg_8k) * 0x2000u;
    uint32_t r7 = (s->mmc3_regs[7] % prg_8k) * 0x2000u;
    uint32_t f0 = (prg_8k - 2) * 0x2000u;   /* second-to-last fixed bank */
    uint32_t f1 = (prg_8k - 1) * 0x2000u;   /* last fixed bank */

    if (!prg_mode) {
        s->mmc3_prg_offsets[0] = r6;  s->mmc3_prg_offsets[1] = r7;
        s->mmc3_prg_offsets[2] = f0;  s->mmc3_prg_offsets[3] = f1;
    } else {
        s->mmc3_prg_offsets[0] = f0;  s->mmc3_prg_offsets[1] = r7;
        s->mmc3_prg_offsets[2] = r6;  s->mmc3_prg_offsets[3] = f1;
    }

    /* CHR: eight 1 KB windows at PPU $0000–$1FFF.
     * R0/R1 select 2 KB pairs (bit 0 ignored); R2–R5 select 1 KB banks.
     * CHR-invert bit swaps which half gets the 2 KB banks. */
    if (s->chr_is_ram || chr_1k == 0) return;

    uint8_t inv = (s->mmc3_bank_sel >> 7) & 1;
    uint8_t bank[8] = {
        (uint8_t)(s->mmc3_regs[0] & 0xFEu), (uint8_t)(s->mmc3_regs[0] | 1u),
        (uint8_t)(s->mmc3_regs[1] & 0xFEu), (uint8_t)(s->mmc3_regs[1] | 1u),
        s->mmc3_regs[2], s->mmc3_regs[3], s->mmc3_regs[4], s->mmc3_regs[5],
    };

    /* 2 KB at $0000, 2 KB at $0800, 1 KB×4 at $1000–$1FFF; the invert bit
     * swaps the halves. */
    s->tqrom_ram_windows = 0;
    for (int i = 0; i < 8; i++) {
        int w = inv ? i ^ 4 : i;
        if (s->cart.mapper == 119 && (bank[i] & 0x40u)) {   /* TQROM: CHR RAM */
            s->mmc3_chr_offsets[w] = chr_1k * 0x400u + (bank[i] & 7u) * 0x400u;
            s->tqrom_ram_windows |= (uint8_t)(1u << w);
        } else {
            s->mmc3_chr_offsets[w] = (bank[i] % chr_1k) * 0x400u;
        }
    }
}

static void mmc3_cpu_write(NesState *s, uint16_t addr, uint8_t val) {
    bool odd = (addr & 1) != 0;
    if (addr < 0xA000) {
        if (!odd) { s->mmc3_bank_sel = val; }
        else      { s->mmc3_regs[s->mmc3_bank_sel & 7] = val; mmc3_update_banks(s); }
    } else if (addr < 0xC000) {
        if (!odd && s->cart.mirror != RP2C02_MIRROR_4SCREEN)
            s->ppu.mirror = (val & 1) ? RP2C02_MIRROR_HORIZONTAL : RP2C02_MIRROR_VERTICAL;
        /* $A001 PRG-RAM protect: ignored */
    } else if (addr < 0xE000) {
        if (!odd) { s->mmc3_irq_latch = val; }
        else      { s->mmc3_irq_reload = true; s->mmc3_irq_counter = 0; }
    } else {
        if (!odd) { s->mmc3_irq_enabled = false; s->cart_irq = false; nes_update_irq(s); }
        else      { s->mmc3_irq_enabled = true; }
    }
}

static void mmc3_irq_scanline(void *ud) {
    NesState *s = ud;
    if (s->mmc3_irq_reload || s->mmc3_irq_counter == 0) {
        s->mmc3_irq_counter = s->mmc3_irq_latch;
        s->mmc3_irq_reload  = false;
    } else {
        s->mmc3_irq_counter--;
    }
    if (s->mmc3_irq_counter == 0 && s->mmc3_irq_enabled) {
        s->cart_irq = true;
        nes_update_irq(s);
    }
}

/* ── Mapper 178 (WaixingFS) ──────────────────────────────────────────────── */

static void m178_update_banks(NesState *s) {
    uint8_t  prg_mode = (s->m178_mode >> 1) & 3u;
    uint32_t prg_size = (uint32_t)s->cart.prg_banks * 0x4000u;
    uint32_t outer    = (uint32_t)s->m178_prg_hi * 0x20000u;
    uint32_t inner    = (uint32_t)(s->m178_prg_lo & 7u) * 0x4000u;

    switch (prg_mode) {
    case 0: /* NROM-256/BNROM: 32KB page, CPU A14 = PRG A14 */
        {
            uint32_t base = (outer + (uint32_t)(s->m178_prg_lo & 6u) * 0x4000u) % prg_size;
            s->prg_offsets[0] = base;
            s->prg_offsets[1] = (base + 0x4000u) % prg_size;
        }
        break;
    case 1: /* UNROM: switchable $8000–$BFFF, last inner bank fixed at $C000 */
        s->prg_offsets[0] = (outer + inner) % prg_size;
        s->prg_offsets[1] = (outer + 7u * 0x4000u) % prg_size;
        break;
    case 2: /* NROM-128: same 16KB bank mirrored at both windows */
        s->prg_offsets[0] = (outer + inner) % prg_size;
        s->prg_offsets[1] = (outer + inner) % prg_size;
        break;
    default: /* mode 3: UNROM variant - treat as mode 1 */
        s->prg_offsets[0] = (outer + inner) % prg_size;
        s->prg_offsets[1] = (outer + 7u * 0x4000u) % prg_size;
        break;
    }

    s->ppu.mirror = (s->m178_mode & 1u) ? RP2C02_MIRROR_HORIZONTAL : RP2C02_MIRROR_VERTICAL;
}

/* ── Mapper 5 (MMC5) ─────────────────────────────────────────────────────── */

static void mmc5_update_prg_banks(NesState *s) {
    uint32_t prg_size = (uint32_t)s->cart.prg_banks * 0x4000u;
    uint32_t unit     = 0x2000u; /* 8KB per slot */
    uint8_t  *r       = s->mmc5_prg_regs; /* r[0]=$5113 … r[4]=$5117 */

    for (int i = 0; i < 4; i++) s->mmc5_prg_is_ram[i] = false;

    switch (s->mmc5_prg_mode) {
    case 0: { /* 32KB at $8000-$FFFF from $5117 (always ROM) */
        uint32_t base = ((uint32_t)(r[4] & 0x7Cu) * unit) % prg_size;
        for (int i = 0; i < 4; i++) s->mmc5_prg_offsets[i] = base + (uint32_t)i * unit;
        break;
    }
    case 1: { /* 16KB from $5115 at $8000, 16KB from $5117 at $C000 */
        if (!(r[2] & 0x80u)) { s->mmc5_prg_is_ram[0] = s->mmc5_prg_is_ram[1] = true; }
        uint32_t b0 = ((uint32_t)(r[2] & 0x7Eu) * unit) % prg_size;
        uint32_t b1 = ((uint32_t)(r[4] & 0x7Eu) * unit) % prg_size;
        s->mmc5_prg_offsets[0] = b0;  s->mmc5_prg_offsets[1] = b0 + unit;
        s->mmc5_prg_offsets[2] = b1;  s->mmc5_prg_offsets[3] = b1 + unit;
        break;
    }
    case 2: { /* 16KB from $5115, 8KB from $5116, 8KB from $5117 */
        if (!(r[2] & 0x80u)) { s->mmc5_prg_is_ram[0] = s->mmc5_prg_is_ram[1] = true; }
        if (!(r[3] & 0x80u)) { s->mmc5_prg_is_ram[2] = true; }
        uint32_t b0 = ((uint32_t)(r[2] & 0x7Eu) * unit) % prg_size;
        s->mmc5_prg_offsets[0] = b0;  s->mmc5_prg_offsets[1] = b0 + unit;
        s->mmc5_prg_offsets[2] = ((uint32_t)(r[3] & 0x7Fu) * unit) % prg_size;
        s->mmc5_prg_offsets[3] = ((uint32_t)(r[4] & 0x7Fu) * unit) % prg_size;
        break;
    }
    case 3: /* Four 8KB banks from $5114-$5117 */
        for (int i = 0; i < 4; i++) {
            uint8_t reg = r[i + 1];
            if (i < 3 && !(reg & 0x80u)) s->mmc5_prg_is_ram[i] = true;
            s->mmc5_prg_offsets[i] = ((uint32_t)(reg & 0x7Fu) * unit) % prg_size;
        }
        break;
    }
}

/* Return 1KB CHR byte offset for a given register index + sub-slot offset. */
static uint32_t mmc5_chr1kb(const NesState *s, uint8_t reg_idx, uint8_t sub) {
    uint32_t chr_size = (uint32_t)s->cart.chr_banks * 0x2000u;
    uint16_t bank = (uint16_t)(((uint16_t)(s->mmc5_chr_hi & 3u) << 8) |
                                s->mmc5_chr_regs[reg_idx]);
    return ((uint32_t)(bank + sub) * 0x400u) % chr_size;
}

static uint8_t mmc5_chr_read(NesState *s, uint16_t addr) {
    if (s->chr_is_ram) return s->chr[addr & 0x1FFFu];

    uint32_t chr_size = (uint32_t)s->cart.chr_banks * 0x2000u;
    bool rendering = (s->ppu.ppumask & (PPUMASK_SHOW_BG | PPUMASK_SHOW_SPR)) &&
                     (s->ppu.scanline >= 0 && s->ppu.scanline < 240);

    /* Extended attribute mode: BG uses ExRAM bank bits instead of normal CHR banking */
    bool is_spr_fetch = rendering && (s->ppu.dot == 257) &&
                        (s->ppu.ppuctrl & PPUCTRL_SPR_8x16);
    if (!is_spr_fetch && rendering && s->mmc5_exram_mode == 1) {
        uint8_t bank8 = (uint8_t)(((s->mmc5_chr_hi & 3u) << 6) |
                                   (s->mmc5_exattr_latch & 0x3Fu));
        return s->chr[((uint32_t)bank8 * 0x1000u + (addr & 0x0FFFu)) % chr_size];
    }

    /* Determine whether this is a sprite or BG fetch */
    bool use_spr;
    if (s->ppu.ppuctrl & PPUCTRL_SPR_8x16)
        use_spr = is_spr_fetch;   /* 8x16: sprites at dot 257, BG elsewhere */
    else
        use_spr = !s->mmc5_last_chr_bg; /* 8x8: whichever set was last written */

    uint8_t mode = s->mmc5_chr_mode;

    if (use_spr) {
        /* Sprite banks: $5120-$5127 (indices 0-7) → 8 × 1KB windows */
        uint8_t s8 = (addr >> 10) & 7u;
        switch (mode) {
        case 0: return s->chr[(mmc5_chr1kb(s, 7, s8))         + (addr & 0x3FFu)];
        case 1: return s->chr[(mmc5_chr1kb(s, s8 >= 4 ? 7 : 3, s8 & 3u)) + (addr & 0x3FFu)];
        case 2: return s->chr[(mmc5_chr1kb(s, (s8 >> 1) * 2u + 1u, s8 & 1u)) + (addr & 0x3FFu)];
        case 3: return s->chr[(mmc5_chr1kb(s, s8, 0))          + (addr & 0x3FFu)];
        }
    } else {
        /* BG banks: $5128-$512B (indices 8-11) → 4 × 1KB, mirrored to $0000-$1FFF */
        uint8_t s4 = (addr >> 10) & 3u;
        switch (mode) {
        case 0: return s->chr[(mmc5_chr1kb(s, 11, s4))          + (addr & 0x3FFu)];
        case 1: return s->chr[(mmc5_chr1kb(s, 11, s4))          + (addr & 0x3FFu)];
        case 2: return s->chr[(mmc5_chr1kb(s, (s4 >> 1) * 2u + 9u, s4 & 1u)) + (addr & 0x3FFu)];
        case 3: return s->chr[(mmc5_chr1kb(s, 8u + s4, 0))      + (addr & 0x3FFu)];
        }
    }
    return 0;
}

static uint8_t mmc5_nt_read(uint16_t addr, void *ud) {
    NesState *s = ud;
    uint8_t  nt  = (addr >> 10) & 3u;
    uint8_t  mode = (s->mmc5_nt_map >> (nt * 2u)) & 3u;
    uint16_t off  = addr & 0x3FFu;

    /* Save ExRAM latch on tile reads (used for extended attribute CHR banking) */
    if (off < 0x3C0u && s->mmc5_exram_mode == 1)
        s->mmc5_exattr_latch = s->mmc5_exram[off];

    /* Extended attribute: override attribute reads with ExRAM palette bits */
    if (off >= 0x3C0u && s->mmc5_exram_mode == 1) {
        uint8_t pal   = (s->mmc5_exattr_latch >> 6) & 3u;
        uint8_t shift = (uint8_t)(((s->ppu.v >> 4) & 4u) | (s->ppu.v & 2u));
        return (uint8_t)(pal << shift);
    }

    switch (mode) {
    case 0: return s->ppu.vram[off];
    case 1: return s->ppu.vram[0x400u + off];
    case 2: return (s->mmc5_exram_mode >= 2) ? s->mmc5_exram[off & 0x3FFu] : 0;
    case 3: /* fill mode */
        return (off >= 0x3C0u) ? (uint8_t)(s->mmc5_fill_attr & 3u) * 0x55u
                                : s->mmc5_fill_tile;
    }
    return 0;
}

static void mmc5_nt_write(uint16_t addr, uint8_t val, void *ud) {
    NesState *s = ud;
    uint8_t  nt  = (addr >> 10) & 3u;
    uint8_t  mode = (s->mmc5_nt_map >> (nt * 2u)) & 3u;
    uint16_t off  = addr & 0x3FFu;
    switch (mode) {
    case 0: s->ppu.vram[off]          = val; break;
    case 1: s->ppu.vram[0x400u + off] = val; break;
    case 2: s->mmc5_exram[off & 0x3FFu] = val; break;
    case 3: break; /* fill-mode: writes ignored */
    }
}

static void mmc5_irq_scanline(void *ud) {
    NesState *s = ud;
    int sl = s->ppu.scanline;
    if (sl >= 240) {
        s->mmc5_in_frame = false;
        return;
    }
    if (!s->mmc5_in_frame) {
        s->mmc5_in_frame    = true;
        s->mmc5_irq_counter = 0;
    } else {
        s->mmc5_irq_counter++;
    }
    if (s->mmc5_irq_target > 0 && s->mmc5_irq_counter == s->mmc5_irq_target) {
        s->mmc5_irq_pending = true;
        if (s->mmc5_irq_enabled) {
            s->cart_irq = true;
            nes_update_irq(s);
        }
    }
}

/* ── Mapper 24/26 (Konami VRC6a/VRC6b) ──────────────────────────────────── */

/* Advance all VRC6 state by one CPU cycle: handles IRQ and audio timers.
 * Returns current expansion audio output level (for apu.fds_in). */
static float vrc6_tick(NesState *s) {
    /* IRQ: prescaler counts CPU cycles; fires at 341 in scanline mode, each cycle in CPU mode */
    if (s->vrc6_irq_enabled) {
        bool fire = false;
        if (!s->vrc6_irq_mode) {
            if (++s->vrc6_prescaler >= 341) { s->vrc6_prescaler = 0; fire = true; }
        } else { fire = true; }
        if (fire) {
            if (s->vrc6_irq_counter == 0xFF) {
                s->vrc6_irq_counter = s->vrc6_irq_latch;
                s->cart_irq = true;
                nes_update_irq(s);
            } else { s->vrc6_irq_counter++; }
        }
    }

    if (s->vrc6_halt) return 0.0f;

    /* Pulse 1 */
    float p1 = 0.0f;
    if (s->vrc6_p1_en) {
        if (s->vrc6_p1_timer == 0) {
            s->vrc6_p1_timer = s->vrc6_p1_period;
            s->vrc6_p1_seq   = (s->vrc6_p1_seq + 1u) & 15u;
        } else { s->vrc6_p1_timer--; }
        uint8_t vol = s->vrc6_p1_ctrl & 0x0Fu;
        if ((s->vrc6_p1_ctrl & 0x80u) || s->vrc6_p1_seq <= ((s->vrc6_p1_ctrl >> 4) & 7u))
            p1 = (float)vol;
    }

    /* Pulse 2 */
    float p2 = 0.0f;
    if (s->vrc6_p2_en) {
        if (s->vrc6_p2_timer == 0) {
            s->vrc6_p2_timer = s->vrc6_p2_period;
            s->vrc6_p2_seq   = (s->vrc6_p2_seq + 1u) & 15u;
        } else { s->vrc6_p2_timer--; }
        uint8_t vol = s->vrc6_p2_ctrl & 0x0Fu;
        if ((s->vrc6_p2_ctrl & 0x80u) || s->vrc6_p2_seq <= ((s->vrc6_p2_ctrl >> 4) & 7u))
            p2 = (float)vol;
    }

    /* Sawtooth: 6 accumulator additions per 14-clock cycle (even steps 2,4,6,8,10,12) */
    float saw = 0.0f;
    if (s->vrc6_saw_en) {
        if (s->vrc6_saw_timer == 0) {
            s->vrc6_saw_timer = s->vrc6_saw_period;
            s->vrc6_saw_step++;
            if (s->vrc6_saw_step >= 14u) {
                s->vrc6_saw_step  = 0;
                s->vrc6_saw_accum = 0;
            } else if ((s->vrc6_saw_step & 1u) == 0u) {
                s->vrc6_saw_accum += s->vrc6_saw_rate;
            }
        } else { s->vrc6_saw_timer--; }
        saw = (float)(s->vrc6_saw_accum >> 3);
    }

    /* Mix: VRC6 pulses use same nonlinear formula as 2A03 (vol 0-15 = same units).
     * Sawtooth scaled to roughly match one 2A03 pulse at full volume. */
    float pulse = (p1 + p2 > 0.0f) ? 95.88f / (8128.0f / (p1 + p2) + 100.0f) : 0.0f;
    return pulse + saw * (0.15f / 31.0f);
}

static void vrc6_cpu_write(NesState *s, uint16_t addr, uint8_t val) {
    /* Mapper 26 (VRC6b) swaps address pins A0 and A1 */
    if (s->cart.mapper == 26) {
        uint8_t a01 = addr & 3u;
        addr = (uint16_t)((addr & ~3u) | ((a01 & 1u) << 1) | ((a01 >> 1) & 1u));
    }
    uint32_t prg_8k = (uint32_t)s->cart.prg_banks * 2u;
    uint32_t chr_1k = (uint32_t)s->cart.chr_banks * 8u;

    if (addr < 0x9000) {
        /* $8000: 16KB PRG bank at $8000-$BFFF (bits 3:0) */
        uint32_t bank = (uint32_t)(val & 0x0Fu) % s->cart.prg_banks;
        s->mmc3_prg_offsets[0] = bank * 0x4000u;
        s->mmc3_prg_offsets[1] = bank * 0x4000u + 0x2000u;
    } else if (addr < 0xA000) {
        switch (addr & 3u) {
        case 0: s->vrc6_p1_ctrl   = val; break;
        case 1: s->vrc6_p1_period = (s->vrc6_p1_period & 0xFF00u) | val; break;
        case 2:
            s->vrc6_p1_period = (s->vrc6_p1_period & 0x00FFu) | ((uint16_t)(val & 0x0Fu) << 8);
            s->vrc6_p1_en     = (val >> 7) & 1;
            break;
        case 3: s->vrc6_halt = val & 1; break;
        }
    } else if (addr < 0xB000) {
        switch (addr & 3u) {
        case 0: s->vrc6_p2_ctrl   = val; break;
        case 1: s->vrc6_p2_period = (s->vrc6_p2_period & 0xFF00u) | val; break;
        case 2:
            s->vrc6_p2_period = (s->vrc6_p2_period & 0x00FFu) | ((uint16_t)(val & 0x0Fu) << 8);
            s->vrc6_p2_en     = (val >> 7) & 1;
            break;
        }
    } else if (addr < 0xC000) {
        switch (addr & 3u) {
        case 0: s->vrc6_saw_rate   = val & 0x3Fu; break;
        case 1: s->vrc6_saw_period = (s->vrc6_saw_period & 0xFF00u) | val; break;
        case 2:
            s->vrc6_saw_period = (s->vrc6_saw_period & 0x00FFu) | ((uint16_t)(val & 0x0Fu) << 8);
            s->vrc6_saw_en     = (val >> 7) & 1;
            break;
        case 3:
            switch ((val >> 2) & 3u) {
            case 0: s->ppu.mirror = RP2C02_MIRROR_VERTICAL;   break;
            case 1: s->ppu.mirror = RP2C02_MIRROR_HORIZONTAL; break;
            case 2: s->ppu.mirror = RP2C02_MIRROR_SINGLE_A;   break;
            case 3: s->ppu.mirror = RP2C02_MIRROR_SINGLE_B;   break;
            }
            break;
        }
    } else if (addr < 0xD000) {
        /* $C000: 8KB PRG bank at $C000-$DFFF (bits 4:0) */
        if ((addr & 3u) == 0)
            s->mmc3_prg_offsets[2] = ((uint32_t)(val & 0x1Fu) % prg_8k) * 0x2000u;
    } else if (addr < 0xE000) {
        /* $D000-$D003: CHR 1KB banks for PPU $0000-$0FFF */
        if (chr_1k) s->mmc3_chr_offsets[addr & 3u] = ((uint32_t)val % chr_1k) * 0x400u;
    } else if (addr < 0xF000) {
        /* $E000-$E003: CHR 1KB banks for PPU $1000-$1FFF */
        if (chr_1k) s->mmc3_chr_offsets[4u + (addr & 3u)] = ((uint32_t)val % chr_1k) * 0x400u;
    } else {
        /* $F000-$F002: IRQ */
        switch (addr & 3u) {
        case 0: s->vrc6_irq_latch = val; break;
        case 1:
            s->vrc6_irq_after   = val & 1u;
            s->vrc6_irq_enabled = (val >> 1) & 1u;
            s->vrc6_irq_mode    = (val >> 2) & 1u;
            if (s->vrc6_irq_enabled) {
                s->vrc6_irq_counter = s->vrc6_irq_latch;
                s->vrc6_prescaler   = 0;
            } else { s->cart_irq = false; nes_update_irq(s); }
            break;
        case 2:
            s->cart_irq = false;
            nes_update_irq(s);
            s->vrc6_irq_counter = s->vrc6_irq_latch;
            s->vrc6_irq_enabled = s->vrc6_irq_after;
            s->vrc6_prescaler   = 0;
            break;
        }
    }
}

/* ── CHR bus callbacks (PPU address space 0x0000–0x1FFF) ─────────────────── */

static uint8_t nes_chr_read(uint16_t addr, void *ud) {
    NesState *s = ud;
    if (!s->chr) return 0;
    if (s->cart.mapper == 5) return mmc5_chr_read(s, addr);
    if (is_mmc3(s) && s->chr_is_ram) return s->chr[addr & 0x1FFF];
    if (s->fine_banks) {
        if (s->board.chr_disabled) return 0xFF;
        return s->chr[s->mmc3_chr_offsets[addr >> 10] + (addr & 0x3FFu)];
    }
    if (s->cart.mapper == 9) {
        /* MMC2: 4 KB CHR windows with latch-based bank selection.
         * Latch updates AFTER the fetch - the triggering tile ($FD/$FE)
         * renders with the old bank; the new bank takes effect next fetch. */
        if (s->chr_is_ram) return s->chr[addr & 0x1FFF];
        bool     is_right = (addr & 0x1000u) != 0;
        uint16_t tile     = addr & 0x0FF8u;
        uint8_t  bank;
        if (is_right)
            bank = s->m9_latch_r ? s->m9_chr_rfe : s->m9_chr_rfd;
        else
            bank = s->m9_latch_l ? s->m9_chr_lfe : s->m9_chr_lfd;
        uint32_t chr_size = (uint32_t)s->cart.chr_banks * 0x2000u;
        uint8_t  data = s->chr[((uint32_t)bank * 0x1000u + (addr & 0x0FFFu)) % chr_size];
        /* Latch fires only on the high bitplane fetch ($xFD8-$xFDF / $xFE8-$xFEF) */
        if (tile == 0x0FD8u) {
            if (is_right) s->m9_latch_r = false; else s->m9_latch_l = false;
        } else if (tile == 0x0FE8u) {
            if (is_right) s->m9_latch_r = true;  else s->m9_latch_l = true;
        }
        return data;
    }
    if (s->cart.mapper >= 1 && s->cart.mapper != 7) {
        uint8_t slot = addr >= 0x1000 ? 1 : 0;
        return s->chr[s->chr_offsets[slot] + (addr & 0x0FFF)];
    }
    return s->chr[addr & 0x1FFF];
}

static void nes_chr_write(uint16_t addr, uint8_t val, void *ud) {
    NesState *s = ud;
    if (s->cart.mapper == 119 && (s->tqrom_ram_windows >> (addr >> 10) & 1u)) {
        s->chr[s->mmc3_chr_offsets[addr >> 10] + (addr & 0x3FFu)] = val;
        return;
    }
    if (!s->chr_is_ram) return;
    if (s->cart.mapper == 4) { s->chr[addr & 0x1FFF] = val; return; }
    if (s->fine_banks) {
        s->chr[s->mmc3_chr_offsets[addr >> 10] + (addr & 0x3FFu)] = val; return;
    }
    if (s->cart.mapper >= 1 && s->cart.mapper != 7) {
        uint8_t slot = addr >= 0x1000 ? 1 : 0;
        s->chr[s->chr_offsets[slot] + (addr & 0x0FFF)] = val;
    } else {
        s->chr[addr & 0x1FFF] = val;
    }
}

static uint8_t nes_prg_direct(const NesState *s, uint16_t addr) {
    if (addr < 0x8000 || !s->prg) return 0;
    if (s->cart.mapper == 9) {
        uint8_t slot   = (uint8_t)((addr - 0x8000u) >> 13);
        uint32_t prg8k = (uint32_t)s->cart.prg_banks * 2u;
        uint32_t bank8;
        if      (slot == 0) bank8 = s->m9_prg_reg & 0x0Fu;
        else if (slot == 1) bank8 = prg8k - 3u;
        else if (slot == 2) bank8 = prg8k - 2u;
        else                bank8 = prg8k - 1u;
        return s->prg[bank8 * 0x2000u + (addr & 0x1FFFu)];
    }
    if (s->fine_banks) {
        uint8_t slot = (uint8_t)((addr - 0x8000u) >> 13);
        return s->prg[s->mmc3_prg_offsets[slot] + (addr & 0x1FFFu)];
    }
    if (s->cart.mapper >= 1) {
        uint8_t slot = addr >= 0xC000 ? 1 : 0;
        return s->prg[s->prg_offsets[slot] + (addr & 0x3FFF)];
    }
    uint32_t off = addr - 0x8000u;
    if (s->cart.prg_banks == 1) off &= 0x3FFF;
    return s->prg[off];
}

/* Cartridge byte the CPU sees at $8000-$FFFF under the current banking,
 * before Game Genie patches.  No side effects; requires s->prg. */
static uint8_t nes_prg_mapped(const NesState *s, uint16_t addr) {
    if (s->cart.mapper == 9) {
        uint8_t slot   = (uint8_t)((addr - 0x8000u) >> 13);  /* 0..3 */
        uint32_t prg8k = (uint32_t)s->cart.prg_banks * 2u;
        uint32_t bank8;
        if      (slot == 0) bank8 = s->m9_prg_reg & 0x0Fu;
        else if (slot == 1) bank8 = prg8k - 3u;
        else if (slot == 2) bank8 = prg8k - 2u;
        else                bank8 = prg8k - 1u;  /* slot 3 */
        return s->prg[bank8 * 0x2000u + (addr & 0x1FFFu)];
    } else if (s->cart.mapper == 5) {
        uint8_t slot = (uint8_t)((addr - 0x8000u) >> 13);
        if (s->mmc5_prg_is_ram[slot])
            return s->prg_ram[(s->mmc5_prg_offsets[slot] + (addr & 0x1FFFu)) & 0x1FFFu];
        else
            return s->prg[s->mmc5_prg_offsets[slot] + (addr & 0x1FFFu)];
    } else if (s->fine_banks) {
        uint8_t slot = (uint8_t)((addr - 0x8000u) >> 13);
        return s->prg[s->mmc3_prg_offsets[slot] + (addr & 0x1FFFu)];
    } else if (s->cart.mapper >= 1) {
        uint8_t slot = addr >= 0xC000 ? 1 : 0;
        return s->prg[s->prg_offsets[slot] + (addr & 0x3FFF)];
    } else {
        /* NROM: 1 bank → mirrored; 2 banks → direct 32 KB */
        uint32_t off = addr - 0x8000u;
        if (s->cart.prg_banks == 1) off &= 0x3FFF;
        return s->prg[off];
    }
}

static uint8_t nes_cpu_read(uint16_t addr, void *ud);
static uint8_t nes_cpu_read_bus(uint16_t addr, void *ud);
static void    nes_cpu_write_bus(uint16_t addr, uint8_t val, void *ud);

/* The CPU's /IRQ: the APU's frame and DMC flags, and the cartridge's line.
 * Wii: no connector faults, so the cartridge's line is just cart_irq. */
void nes_update_irq(NesState *s) {
    s->cpu.irq = s->apu.fc_irq || s->apu.dmc.irq_flag || s->cart_irq;
}

static void nes_sync_ppu_to_cpu(NesState *s, uint64_t cpu_cycle);

/* Wii: the PPU runs lazily.  Upstream brings it up to the CPU before every
 * bus access; here that happens only when the CPU touches a PPU register or
 * the cartridge (whose banking the PPU reads through), or reaches the cycle
 * where the PPU could next raise NMI or a mapper IRQ, or finish the frame
 * (rp2c02_ticks_to_event).  Between those nothing the CPU can see changes,
 * so the result is the same, cycle for cycle.  One NES at a time. */
static uint64_t ppu_deadline;
static bool apu_eager;     /* the APU side ticks every cycle (see prg_windows_update) */

/* The first cycle at which nes_sync_all() has anything to do: the PPU's
 * deadline, or the APU's (it syncs up to the cycle before the access). */
static uint64_t sync_at;

static void sync_at_update(const NesState *s) {
    uint64_t a = apu_eager ? 0 : s->apu_deadline + 1u;
    sync_at = a < ppu_deadline ? a : ppu_deadline;
}

static void ppu_deadline_update(NesState *s) {
    if (s->ppu.nmi_pending)             /* a register access raised it: hand it over */
        ppu_deadline = s->ppu_synced_cpu_cycle + 1;
    else
        ppu_deadline = s->ppu_synced_cpu_cycle + (rp2c02_ticks_to_event(&s->ppu) + 2u) / 3u;
    sync_at_update(s);
}

/* Wii: PRG ROM reads go through four 8 KB window pointers, the same bytes
 * nes_prg_mapped() picks, recomputed whenever the banking may have changed
 * (a write to $4020-$FFFF, a board register read, reset).  Not for MMC5,
 * whose windows can be RAM.  And whether the APU side ticks every cycle,
 * worked out once. */
static const uint8_t *prg_win[4];
static bool prg_fast;

static bool nes_apu_eager(const NesState *s);

static void chr_windows_update(NesState *s);

static void prg_windows_update(NesState *s) {
    apu_eager = nes_apu_eager(s);
    sync_at_update(s);
    chr_windows_update(s);
    prg_fast = s->prg && s->cart.mapper != 5;
    if (!prg_fast) return;
    for (int w = 0; w < 4; w++) {
        uint16_t a = (uint16_t)(0x8000u + w * 0x2000u);
        uint32_t off;
        if (s->cart.mapper == 9) {
            uint32_t prg8k = (uint32_t)s->cart.prg_banks * 2u;
            uint32_t bank8 = w == 0 ? (s->m9_prg_reg & 0x0Fu) : prg8k - 4u + (uint32_t)w;
            off = bank8 * 0x2000u;
        } else if (s->fine_banks) {
            off = s->mmc3_prg_offsets[w];
        } else if (s->cart.mapper >= 1) {
            off = s->prg_offsets[a >= 0xC000 ? 1 : 0] + (a & 0x3FFFu);
        } else {
            off = a - 0x8000u;
            if (s->cart.prg_banks == 1) off &= 0x3FFF;
        }
        prg_win[w] = s->prg + off;
    }
}

/* Wii: the PPU's direct CHR windows (rp2c02.h), as nes_chr_read() maps
 * them.  MMC2 and MMC5 bank by what the PPU fetches: they keep the callback. */
static void chr_windows_update(NesState *s) {
    unsigned m = s->cart.mapper;
    bool direct = s->chr && m != 5 && m != 9 && !(s->fine_banks && s->board.chr_disabled);
    s->ppu.chr_direct = direct;
    if (!direct) return;
    for (int w = 0; w < 8; w++) {
        uint32_t off;
        if (is_mmc3(s) && s->chr_is_ram)  off = (uint32_t)w * 0x400u;
        else if (s->fine_banks)           off = s->mmc3_chr_offsets[w];
        else if (m >= 1 && m != 7)        off = s->chr_offsets[w >> 2] + (uint32_t)(w & 3) * 0x400u;
        else                              off = (uint32_t)w * 0x400u;
        s->ppu.chr_win[w] = s->chr + off;
    }
}

/* The DMC asking for a byte, or its channel disabled while it waited. */
static void nes_dmc_dma_check(NesState *s) {
    if (s->apu.dmc.dma_pending && !s->dmc_dma) {
        s->dmc_dma = s->dmc_dummy = s->dma_halt = true;
    }
    if (s->apu.dmc.dma_abort) {
        s->apu.dmc.dma_abort = false;
        if (s->dmc_dma) {
            if (s->dma_halt && !s->in_dma) s->dmc_dma = s->dmc_dummy = s->dma_halt = false;
            else s->dmc_abort = true;   /* already under way: dropped at the next cycle */
        }
    }
}

/* Whether the cartridge side must tick with every CPU cycle: chips that
 * count M2 (VRC IRQs and audio, FME-7 and friends).  Otherwise the APU
 * alone runs in batches. */
static bool nes_apu_eager(const NesState *s) {
    return s->board.ticks || s->cart.mapper == 24 || s->cart.mapper == 26;
}

/* The APU, the cartridge's expansion audio and its mapper's cycle counters,
 * one tick per CPU cycle up to `cpu_cycle`. */
static void nes_sync_apu_to_cpu(NesState *s, uint64_t cpu_cycle) {
    if (s->apu_synced_cpu_cycle >= cpu_cycle || s->apu_syncing) return;
    s->apu_syncing = true;   /* the DMC fetches through nes_cpu_read */
    if (!nes_apu_eager(s)) {
        while (s->apu_synced_cpu_cycle < cpu_cycle) {
            uint64_t to = cpu_cycle - s->apu_synced_cpu_cycle > 256u ? s->apu_synced_cpu_cycle + 256u
                                                                     : cpu_cycle;
            s->apu.cycle = s->apu_synced_cpu_cycle;
            TIMED(nes_core_apu_ticks, apu2a03_run(&s->apu, to));
            s->apu_synced_cpu_cycle = to;
            if (s->apu.audio_dev && s->apu.frame_n > 900) apu2a03_flush(&s->apu);
        }
        nes_dmc_dma_check(s);
        s->apu_deadline = s->apu_synced_cpu_cycle + apu2a03_until_event(&s->apu);
        sync_at_update(s);
        s->apu_syncing = false;
        nes_update_irq(s);
        return;
    }
    bool is_vrc6 = s->cart.mapper == 24 || s->cart.mapper == 26;
    while (s->apu_synced_cpu_cycle < cpu_cycle) {
        s->apu_synced_cpu_cycle++;
        if (is_vrc6)
            s->apu.fds_in = vrc6_tick(s);
        s->apu.cycle = s->apu_synced_cpu_cycle - 1u;   /* tick numbers the cycle */
        apu2a03_tick(&s->apu);
        if (s->board.ticks)
            nes_board_tick(s, 1);
        nes_dmc_dma_check(s);
    }
    if (s->apu.audio_dev && s->apu.frame_n > 900) apu2a03_flush(&s->apu);
    s->apu_syncing = false;
    nes_update_irq(s);
}

/* Before each bus access the rest of the console catches up with the CPU,
 * so registers, IRQ and NMI are exact to the cycle: the PPU through this
 * cycle, the APU side up to its start. */
static void nes_sync_all(NesState *s) {
    if (s->cpu.cycle_count < sync_at) return;   /* Wii: neither side due */
    uint64_t c = s->cpu.cycle_count - 1;
    if (c >= s->apu_deadline || apu_eager) nes_sync_apu_to_cpu(s, c);
    if (s->cpu.cycle_count >= ppu_deadline) nes_sync_ppu_to_cpu(s, s->cpu.cycle_count);
    sync_at_update(s);
}

/* ── DMA ─────────────────────────────────────────────────────────────────────
 * A DMA halts the CPU (RDY) on its next read cycle - a write can't be
 * halted - and the halted CPU keeps repeating that read.  The DMC's fetch
 * needs a halt and a dummy cycle, then a get (even) cycle; OAM DMA reads on
 * get cycles and writes $2004 on put cycles, 256 times.  Running together,
 * any DMA cycle counts as the DMC's halt or dummy cycle.
 *
 * The 2A03 reads its own registers ($4015-$4017, mirrored every $20) only
 * while the 6502's address bus is in $4000-$401F: a DMA read there sees
 * open bus unless the CPU was halted on such an address, and then reads the
 * registers too, whatever the DMA's own address (bus conflicts). */

static uint8_t dma_bus_read(NesState *s, uint16_t a) {
    uint8_t v = nes_cpu_read_bus(a, s);
    if (a != 0x4015) s->cpu_bus = v;
    return v;
}

static uint8_t dma_read(NesState *s, uint16_t a, bool regs, uint16_t *prev) {
    uint8_t v;
    if (!regs) {
        v = (a & 0xFFE0u) == 0x4000u ? s->cpu_bus : dma_bus_read(s, a);
        *prev = a;
        return v;
    }
    uint16_t reg = (uint16_t)(0x4000u | (a & 0x1Fu));
    if (reg == 0x4015) {
        v = dma_bus_read(s, 0x4015);
        if (a != reg) (void)dma_bus_read(s, a);
    } else if (reg == 0x4016 || reg == 0x4017) {
        /* the controller sees back-to-back reads as one */
        v = *prev == reg ? s->cpu_bus : dma_bus_read(s, reg);
        if (a != reg) {
            uint8_t ext = dma_bus_read(s, a);
            v = (uint8_t)((ext & 0xE0u) | (v & ext & 0x1Fu));
        }
    } else {
        v = dma_bus_read(s, a);
    }
    *prev = reg;
    return v;
}

static void dma_cycle(NesState *s) {
    s->cpu.cycle_count++;
    nes_sync_all(s);
}

static bool loop_touched;

static void nes_run_dma(NesState *s, uint16_t cpu_addr) {
    loop_touched = true;           /* Wii: not an idle loop's pass (skip_idle_loop) */
    s->in_dma = true;
    s->cpu.cycle_count--;          /* the DMA's cycles come before this read */
    bool regs = (cpu_addr & 0xFFE0u) == 0x4000u;
    bool skip_dummy = cpu_addr == 0x4016 || cpu_addr == 0x4017;
    uint16_t prev = cpu_addr;
    uint8_t oam_val = 0;
    unsigned oam_n = 0;

    /* halt: the CPU's read happens and is repeated later */
    dma_cycle(s);
    if (s->dmc_abort) s->dmc_dma = s->dmc_dummy = s->dmc_abort = false;
    else (void)dma_bus_read(s, cpu_addr);
    s->dma_halt = false;

    while (s->dmc_dma || s->oam_dma) {
        if (s->dmc_abort) s->dmc_dma = s->dmc_dummy = s->dmc_abort = s->dma_halt = false;
        bool get = ((s->cpu.cycle_count + 1u) & 1u) == 0;
        bool dmc_ready = s->dmc_dma && !s->dma_halt && !s->dmc_dummy;
        if (s->dma_halt) s->dma_halt = false;
        else if (s->dmc_dummy) s->dmc_dummy = false;
        dma_cycle(s);
        if (get && dmc_ready) {
            uint8_t v = dma_read(s, s->apu.dmc.cur_addr, regs, &prev);
            s->dmc_dma = false;
            apu2a03_dmc_fill(&s->apu, v);
            s->apu_deadline = 0; sync_at = 0;   /* its state moved on: look again */
        } else if (get && s->oam_dma && !(oam_n & 1u)) {
            oam_val = dma_read(s, (uint16_t)(s->oam_page << 8 | oam_n >> 1), regs, &prev);
            oam_n++;
        } else if (!get && s->oam_dma && (oam_n & 1u)) {
            nes_cpu_write_bus(0x2004, oam_val, s);
            s->cpu_bus = oam_val;
            if (++oam_n == 512) s->oam_dma = false;
        } else if (!skip_dummy) {
            (void)dma_bus_read(s, cpu_addr);   /* halted: the read repeats */
        }
    }
    s->cpu.cycle_count++;          /* back to the CPU's own read */
    s->cpu.rdy_halted = true;
    s->in_dma = false;
}

/* Wii: loop_touched is set by any access an idle loop can't make, and by
 * a DMA stealing cycles (see skip_idle_loop). */

static uint8_t nes_cpu_read(uint16_t addr, void *ud) {
    NesState *s = ud;
    if (addr >= 0x2000 && (addr < 0x8000 || !prg_fast)) loop_touched = true;
    nes_sync_all(s);
    if (s->dma_halt && !s->in_dma) {
        nes_run_dma(s, addr);
        nes_sync_all(s);
    }
    uint8_t v;
    if (addr >= 0x8000 && prg_fast)  v = prg_win[(addr >> 13) & 3u][addr & 0x1FFFu];   /* Wii */
    else if (addr < 0x2000)          v = s->ram[addr & 0x07FF];
    else                             v = nes_cpu_read_bus(addr, ud);
    if (addr != 0x4015) s->cpu_bus = v;   /* $4015 is read over the internal bus only */
    return v;
}

static void nes_cpu_write(uint16_t addr, uint8_t val, void *ud) {
    NesState *s = ud;
    loop_touched = true;
    nes_sync_all(s);
    s->cpu_bus = val;
    nes_cpu_write_bus(addr, val, ud);
    if (addr >= 0x4020) prg_windows_update(s);
}

/* Wii: in one rp2c02_run, not tick by tick; NMI is taken at the end, which
 * the deadline keeps within a CPU cycle of where it was raised. */
static void nes_sync_ppu_to_cpu(NesState *s, uint64_t cpu_cycle) {
    if (s->ppu_synced_cpu_cycle < cpu_cycle) {
        uint32_t n = (uint32_t)(cpu_cycle - s->ppu_synced_cpu_cycle) * 3u;
        s->ppu_synced_cpu_cycle = cpu_cycle;
        if (s->ppu.nmi_pending) {
            /* raised by a register access: upstream takes it after the
             * next tick, which could otherwise be cleared by a later one
             * (pre-render dot 1) */
            rp2c02_tick(&s->ppu);
            n--;
            if (s->ppu.nmi_pending) {
                s->cpu.nmi = true;
                s->ppu.nmi_pending = false;
            }
        }
        TIMED(nes_core_ppu_ticks, rp2c02_run(&s->ppu, n));
        if (s->ppu.nmi_pending) {
            s->cpu.nmi = true;
            s->ppu.nmi_pending = false;
        }
    }
    ppu_deadline_update(s);
}

/* ── CPU memory map ──────────────────────────────────────────────────────── */

static bool prg_ram_here(const NesState *s) {
    return (s->cart.mapper == 1 && !(s->mmc1_prg & 0x10)) ||
           is_mmc3(s) || s->cart.mapper == 24 || s->cart.mapper == 26 ||
           s->cart.mapper == 178 ||
           (s->has_board && nes_board_has_prg_ram(s));
}

static uint8_t nes_cpu_read_bus(uint16_t addr, void *ud) {
    NesState *s = ud;

    if (addr < 0x2000) return s->ram[addr & 0x07FF];

    if (addr < 0x4000) {
        nes_sync_ppu_to_cpu(s, s->cpu.cycle_count);
        uint8_t v = rp2c02_read(&s->ppu, (uint8_t)(addr & 7));
        ppu_deadline_update(s);
        return v;
    }

    if (addr == 0x4015) {
        nes_sync_apu_to_cpu(s, s->cpu.cycle_count - 1);
        s->apu_deadline = 0; sync_at = 0;
        /* bit 5 isn't driven: it reads the CPU's internal bus (and, unlike
         * every other read, this one doesn't reach the external bus) */
        uint8_t v = (uint8_t)((apu2a03_read(&s->apu, 0x4015) & ~0x20u) | (s->cpu_bus & 0x20u));
        nes_update_irq(s);
        return v;
    }

    /* The controller ports drive D0-D4 (whatever is plugged in uses D0);
     * D5-D7 are open bus.  Wii: standard controllers only. */
    uint8_t ctrl_open = s->cpu_bus & 0xE0u;
    if (addr == 0x4016 || addr == 0x4017) {
        int p = addr & 1;
        if (s->cfg->ports[p] != NES_DEVICE_CONTROLLER) return ctrl_open;
        if (s->ctrl_strobe) return ctrl_open | ((s->ctrl_state[p] & NES_BTN_A) ? 1u : 0u);
        uint8_t bit = s->ctrl_shift[p] & 1;
        s->ctrl_shift[p] = (s->ctrl_shift[p] >> 1) | 0x80u;
        return ctrl_open | bit;
    }

    /* Wii: mapper registers may depend on where the PPU is (MMC5's) */
    if ((s->has_board || s->cart.mapper == 5) && addr >= 0x4020 && addr < 0x8000)
        nes_sync_ppu_to_cpu(s, s->cpu.cycle_count);

    if (s->has_board && addr >= 0x4020 && addr < 0x8000) {
        uint8_t v;
        bool hit = nes_board_read(s, addr, &v);
        prg_windows_update(s);
        if (hit) return v;
    }

    /* MMC5: readable registers at $5100-$5BFF and ExRAM at $5C00-$5FFF */
    if (s->cart.mapper == 5 && addr >= 0x5000 && addr < 0x6000) {
        if (addr == 0x5204) {
            uint8_t v = (s->mmc5_irq_pending ? 0x80u : 0u) |
                        (s->mmc5_in_frame    ? 0x40u : 0u);
            s->mmc5_irq_pending = false;
            s->cart_irq = false;
            nes_update_irq(s);
            return v;
        }
        if (addr == 0x5205) return (uint8_t)((uint16_t)s->mmc5_mul[0] * s->mmc5_mul[1]);
        if (addr == 0x5206) return (uint8_t)(((uint16_t)s->mmc5_mul[0] * s->mmc5_mul[1]) >> 8);
        if (addr >= 0x5C00)
            return (s->mmc5_exram_mode >= 2) ? s->mmc5_exram[addr - 0x5C00u] : s->cpu_bus;
        return s->cpu_bus;
    }

    if (addr >= 0x6000 && addr < 0x8000) {
        if (s->cart.mapper == 5)
            return s->prg_ram[addr & 0x1FFFu]; /* $5113 always maps RAM here */
        if (prg_ram_here(s))
            return s->prg_ram[addr & 0x1FFF];
        return s->cpu_bus;   /* no PRG-RAM: open bus */
    }

    if (addr >= 0x8000) {
        if (!s->prg) return s->cpu_bus;
        return nes_prg_mapped(s, addr);
    }

    return s->cpu_bus;  /* open bus: the last value on the data bus */
}

static void nes_cpu_write_bus(uint16_t addr, uint8_t val, void *ud) {
    NesState *s = ud;

    if (addr < 0x2000) { s->ram[addr & 0x07FF] = val; return; }

    if (addr < 0x4000) {
        nes_sync_ppu_to_cpu(s, s->cpu.cycle_count);
        rp2c02_write(&s->ppu, (uint8_t)(addr & 7), val);
        ppu_deadline_update(s);
        return;
    }

    if (addr == 0x4014) {
        /* OAM DMA: starts on the CPU's next read (nes_run_dma) */
        s->oam_page = val;
        s->oam_dma = s->dma_halt = true;
        return;
    }

    if (addr == 0x4016) {
        bool new_strobe = (val & 1) != 0;
        if (s->ctrl_strobe && !new_strobe) {
            s->ctrl_shift[0] = s->ctrl_state[0];
            s->ctrl_shift[1] = s->ctrl_state[1];
        }
        s->ctrl_strobe = new_strobe;
        return;
    }

    /* APU registers */
    if ((addr >= 0x4000 && addr <= 0x4013) || addr == 0x4015 || addr == 0x4017) {
        nes_sync_apu_to_cpu(s, s->cpu.cycle_count - 1);
        apu2a03_write(&s->apu, addr, val);
        nes_dmc_dma_check(s);
        s->apu_deadline = 0; sync_at = 0;   /* the write may bring the next event closer */
        /* $4010/$4017 can clear APU IRQ flags */
        nes_update_irq(s);
        return;
    }

    /* Wii: the cartridge's banking, mirroring and IRQ counter are the PPU's
     * view; bring it up to now before they change */
    if (addr >= 0x4020) nes_sync_ppu_to_cpu(s, s->cpu.cycle_count);

    if (s->has_board && addr >= 0x4020 && nes_board_write(s, addr, val))
        return;

    if (s->cart.mapper == 178 && addr >= 0x4800 && addr <= 0x4803) {
        switch (addr) {
        case 0x4800: s->m178_mode   = val; break;
        case 0x4801: s->m178_prg_lo = val; break;
        case 0x4802: s->m178_prg_hi = val; break;
        default: break; /* $4803: PRG-RAM bank - stored implicitly, single 8KB bank only */
        }
        m178_update_banks(s);
        return;
    }

    /* MMC5 registers at $5000-$5BFF and ExRAM at $5C00-$5FFF */
    if (s->cart.mapper == 5 && addr >= 0x5000 && addr < 0x6000) {
        if (addr == 0x5100) { s->mmc5_prg_mode = val & 3u; mmc5_update_prg_banks(s); return; }
        if (addr == 0x5101) { s->mmc5_chr_mode = val & 3u; return; }
        /* $5102/$5103: PRG-RAM protect - always allow writes (not enforced) */
        if (addr == 0x5104) { s->mmc5_exram_mode = val & 3u; return; }
        if (addr == 0x5105) { s->mmc5_nt_map = val; return; }
        if (addr == 0x5106) { s->mmc5_fill_tile = val; return; }
        if (addr == 0x5107) { s->mmc5_fill_attr = val & 3u; return; }
        if (addr >= 0x5113 && addr <= 0x5117) {
            s->mmc5_prg_regs[addr - 0x5113u] = val;
            mmc5_update_prg_banks(s);
            return;
        }
        if (addr >= 0x5120 && addr <= 0x5127) {
            s->mmc5_chr_regs[addr - 0x5120u] = val;
            s->mmc5_last_chr_bg = false;
            return;
        }
        if (addr >= 0x5128 && addr <= 0x512B) {
            s->mmc5_chr_regs[8u + (addr - 0x5128u)] = val;
            s->mmc5_last_chr_bg = true;
            return;
        }
        if (addr == 0x5130) { s->mmc5_chr_hi = val & 3u; return; }
        if (addr == 0x5203) { s->mmc5_irq_target = val; return; }
        if (addr == 0x5204) {
            s->mmc5_irq_enabled = (val & 0x80u) != 0;
            if (!s->mmc5_irq_enabled) { s->mmc5_irq_pending = false; s->cart_irq = false; nes_update_irq(s); }
            return;
        }
        if (addr == 0x5205) { s->mmc5_mul[0] = val; return; }
        if (addr == 0x5206) { s->mmc5_mul[1] = val; return; }
        if (addr >= 0x5C00 && s->mmc5_exram_mode <= 2) {
            s->mmc5_exram[addr - 0x5C00u] = val;
            return;
        }
        return; /* ignore other MMC5 writes */
    }

    if (addr >= 0x6000 && addr < 0x8000) {
        if (s->cart.mapper == 5) { s->prg_ram[addr & 0x1FFFu] = val; return; }
        if (prg_ram_here(s))
            s->prg_ram[addr & 0x1FFF] = val;
        return;
    }

    if (addr >= 0x8000) {
        if      (s->cart.mapper == 1) mmc1_serial_write(s, addr, val);
        else if (s->cart.mapper == 2) s->prg_offsets[0] = ((uint32_t)(val % s->cart.prg_banks)) * 0x4000u;
        else if (s->cart.mapper == 3) {
            /* CNROM: 8KB CHR bank select, with bus conflicts like GxROM */
            val &= nes_prg_direct(s, addr);
            s->chr_offsets[0] = ((uint32_t)val % s->cart.chr_banks) * 0x2000u;
            s->chr_offsets[1] = s->chr_offsets[0] + 0x1000u;
        }
        else if (s->cart.mapper == 66) {
            /* GxROM has bus conflicts: the CPU write value is ANDed with
             * the ROM byte currently driving the data bus. */
            val &= nes_prg_direct(s, addr);
            uint32_t prg_size = (uint32_t)s->cart.prg_banks * 0x4000u;
            uint32_t chr_size = (uint32_t)s->cart.chr_banks * 0x2000u;
            s->prg_offsets[0] = (((uint32_t)(val >> 4) & 3u) * 0x8000u) % prg_size;
            s->prg_offsets[1] = s->prg_offsets[0] + 0x4000u;
            if (chr_size > 0) {
                s->chr_offsets[0] = (((uint32_t)val & 3u) * 0x2000u) % chr_size;
                s->chr_offsets[1] = s->chr_offsets[0] + 0x1000u;
            }
        }
        else if (is_mmc3(s)) mmc3_cpu_write(s, addr, val);
        else if (s->cart.mapper == 9) {
            /* MMC2 register map (decoded from CPU A14..A12):
             *   $A000-$AFFF  PRG bank (bits 3:0)
             *   $B000-$BFFF  left CHR  FD-latch bank (bits 4:0)
             *   $C000-$CFFF  left CHR  FE-latch bank
             *   $D000-$DFFF  right CHR FD-latch bank
             *   $E000-$EFFF  right CHR FE-latch bank
             *   $F000-$FFFF  mirroring (bit 0: 0=vertical, 1=horizontal) */
            if (addr < 0xB000)
                s->m9_prg_reg = val & 0x0Fu;
            else if (addr < 0xC000)
                s->m9_chr_lfd = val & 0x1Fu;
            else if (addr < 0xD000)
                s->m9_chr_lfe = val & 0x1Fu;
            else if (addr < 0xE000)
                s->m9_chr_rfd = val & 0x1Fu;
            else if (addr < 0xF000)
                s->m9_chr_rfe = val & 0x1Fu;
            else
                s->ppu.mirror = (val & 1u) ? RP2C02_MIRROR_HORIZONTAL : RP2C02_MIRROR_VERTICAL;
        }
        else if (s->cart.mapper == 7) {
            /* AxROM: bits 2:0 = 32KB PRG bank, bit 4 = mirror (0=single-A, 1=single-B) */
            uint32_t prg_size = (uint32_t)s->cart.prg_banks * 0x4000u;
            uint8_t  bank     = val & 0x07u;
            s->prg_offsets[0] = ((uint32_t)bank * 0x8000u) % prg_size;
            s->prg_offsets[1] = s->prg_offsets[0] + 0x4000u;
            s->ppu.mirror     = (val & 0x10u) ? RP2C02_MIRROR_SINGLE_B
                                               : RP2C02_MIRROR_SINGLE_A;
        }
        else if (s->cart.mapper == 228) {
            /* Action 52 - NESdev register layout (address + data bus):
             *   FEDCBA98 76543210  (full 16-bit address)
             *   1.MHHPPP PPS.CCCC
             * D(13)=mirror  C:B(12:11)=chip-sel  A→6(10:6)=page(5-bit)
             * 5=PRG-mode  3:0=chr_hi   data[1:0]=chr_lo
             * Three 512KB chips at chip-sel 0,1,3; chip-sel 2 = open bus. */
            uint8_t chip     = (uint8_t)((addr >> 11) & 3u);
            uint8_t page     = (uint8_t)((addr >> 6) & 0x1Fu);
            uint8_t prg_mode = (uint8_t)((addr >> 5) & 1u);
            uint8_t chr_hi   = (uint8_t)(addr & 0xFu);
            uint8_t mirror   = (uint8_t)((addr >> 13) & 1u);
            uint8_t chr_bank = (uint8_t)((chr_hi << 2) | (val & 3u));

            if (chip == 2u) return; /* open bus - no PRG chip, ignore */

            uint32_t global_bank = (chip == 3u) ? (64u + page)
                                                 : ((uint32_t)chip * 32u + page);
            uint32_t prg_size = (uint32_t)s->cart.prg_banks * 0x4000u;
            if (!prg_mode) {
                /* 32KB split: round to even/odd pair */
                uint32_t base = (global_bank & ~1u) * 0x4000u;
                s->prg_offsets[0] = base % prg_size;
                s->prg_offsets[1] = (base + 0x4000u) % prg_size;
            } else {
                /* 16KB: same bank mirrored in both $8000 and $C000 windows */
                uint32_t base = (global_bank * 0x4000u) % prg_size;
                s->prg_offsets[0] = base;
                s->prg_offsets[1] = base;
            }

            uint32_t chr_size = (uint32_t)s->cart.chr_banks * 0x2000u;
            if (chr_size > 0) {
                s->chr_offsets[0] = ((uint32_t)chr_bank * 0x2000u) % chr_size;
                s->chr_offsets[1] = s->chr_offsets[0] + 0x1000u;
            }

            s->ppu.mirror = mirror ? RP2C02_MIRROR_HORIZONTAL : RP2C02_MIRROR_VERTICAL;
        }
        else if (s->cart.mapper == 24 || s->cart.mapper == 26)
            vrc6_cpu_write(s, addr, val);
    }
}

/* PPU hooks for the cartridge's mapper: scanline IRQ counters and
 * nametable mapping.  Called again when the cartridge changes. */
static void nes_install_mapper_hooks(NesState *s) {
    unsigned m = s->cart.mapper;
    s->ppu.irq_scanline = NULL;
    s->ppu.nt_read      = NULL;
    s->ppu.nt_write     = NULL;
    s->ppu.irq_ud = s->ppu.nt_ud = s;
    if (m == 4 || m == 119) s->ppu.irq_scanline = mmc3_irq_scanline;
    if (m == 5) {
        s->ppu.irq_scanline = mmc5_irq_scanline;
        s->ppu.nt_read      = mmc5_nt_read;
        s->ppu.nt_write     = mmc5_nt_write;
    }
    if (s->has_board && nes_board_wants_scanline(m)) s->ppu.irq_scanline = nes_board_scanline;
    if (s->has_board && nes_board_wants_nt(m)) {
        s->ppu.nt_read  = nes_board_nt_read;
        s->ppu.nt_write = nes_board_nt_write;
    }
}

static void nes_reset(NesState *s) {
    rp2c02_reset(&s->ppu);
    s->ppu.mirror = s->cart.mirror;
    s->ppu_synced_cpu_cycle = s->apu_synced_cpu_cycle = s->cpu.cycle_count;
    s->apu_deadline = 0; sync_at = 0;

    /* Mapper bank state must be set before mos6502_reset reads the reset
       vector, otherwise prg_offsets[1] is 0 and the vector is fetched from
       the wrong physical bank. */
    if (s->cart.mapper == 1) {
        s->mmc1_shift       = 0;
        s->mmc1_shift_count = 0;
        s->mmc1_ctrl        = 0x0C;   /* PRG mode 3: fix last bank at $C000 */
        s->mmc1_chr0        = 0;
        s->mmc1_chr1        = 0;
        s->mmc1_prg         = 0;
        mmc1_update_banks(s);
    } else if (s->cart.mapper == 2) {
        s->prg_offsets[0] = 0;
        s->prg_offsets[1] = (uint32_t)(s->cart.prg_banks - 1) * 0x4000u;
        s->chr_offsets[0] = 0;
        s->chr_offsets[1] = 0x1000u;
    } else if (s->cart.mapper == 3) {
        /* NROM-style fixed PRG: 16KB mirrored, or 32KB */
        s->prg_offsets[0] = 0;
        s->prg_offsets[1] = s->cart.prg_banks > 1 ? 0x4000u : 0;
        s->chr_offsets[0] = 0;
        s->chr_offsets[1] = 0x1000u;
    } else if (s->cart.mapper == 66) {
        s->prg_offsets[0] = 0;
        s->prg_offsets[1] = 0x4000u;
        s->chr_offsets[0] = 0;
        s->chr_offsets[1] = 0x1000u;
    } else if (s->cart.mapper == 228) {
        /* Reset maps the last two 16KB banks so the reset vector is readable */
        uint32_t last = (uint32_t)(s->cart.prg_banks - 1) * 0x4000u;
        s->prg_offsets[0] = last >= 0x4000u ? last - 0x4000u : 0;
        s->prg_offsets[1] = last;
        s->chr_offsets[0] = 0;
        s->chr_offsets[1] = 0x1000u;
    } else if (s->cart.mapper == 5) {
        s->mmc5_prg_mode    = 3;    /* four 8KB banks */
        s->mmc5_chr_mode    = 3;    /* 1KB CHR banks */
        s->mmc5_exram_mode  = 3;    /* read-only at reset */
        s->mmc5_nt_map      = 0;    /* all CIRAM page 0 */
        s->mmc5_irq_enabled = false;
        s->mmc5_irq_pending = false;
        s->mmc5_in_frame    = false;
        s->mmc5_irq_counter = 0;
        s->mmc5_last_chr_bg = false;
        s->mmc5_exattr_latch = 0;
        memset(s->mmc5_prg_regs,  0xFF, sizeof(s->mmc5_prg_regs));
        memset(s->mmc5_chr_regs,  0,    sizeof(s->mmc5_chr_regs));
        s->mmc5_chr_hi = 0;
        /* $5117 = 0xFF → last 8KB ROM bank at $E000-$FFFF */
        mmc5_update_prg_banks(s);
    } else if (s->cart.mapper == 178) {
        s->m178_mode   = 0;
        s->m178_prg_lo = 0;
        s->m178_prg_hi = 0;
        m178_update_banks(s);
        s->chr_offsets[0] = 0;
        s->chr_offsets[1] = 0x1000u;
    } else if (s->cart.mapper == 9) {
        s->m9_prg_reg  = 0;
        s->m9_chr_lfd  = 0;
        s->m9_chr_lfe  = 0;
        s->m9_chr_rfd  = 0;
        s->m9_chr_rfe  = 0;
        s->m9_latch_l  = false;
        s->m9_latch_r  = false;
    } else if (s->cart.mapper == 7) {
        s->prg_offsets[0] = 0;
        s->prg_offsets[1] = 0x4000u;
        s->ppu.mirror     = RP2C02_MIRROR_SINGLE_A;
    } else if (is_mmc3(s)) {
        memset(s->mmc3_regs, 0, sizeof(s->mmc3_regs));
        s->mmc3_bank_sel    = 0;
        s->mmc3_irq_latch   = 0;
        s->mmc3_irq_counter = 0;
        s->mmc3_irq_reload  = false;
        s->mmc3_irq_enabled = false;
        s->cart_irq         = false;
        mmc3_update_banks(s);
    } else if (s->cart.mapper == 24 || s->cart.mapper == 26) {
        uint32_t n8k = (uint32_t)s->cart.prg_banks * 2u;
        s->mmc3_prg_offsets[0] = 0;
        s->mmc3_prg_offsets[1] = 0x2000u;
        s->mmc3_prg_offsets[2] = 0;
        s->mmc3_prg_offsets[3] = (n8k - 1u) * 0x2000u;  /* $E000: fixed last 8KB */
        memset(s->mmc3_chr_offsets, 0, sizeof(s->mmc3_chr_offsets));
        s->vrc6_irq_latch   = 0;
        s->vrc6_irq_counter = 0;
        s->vrc6_irq_enabled = false;
        s->vrc6_irq_after   = false;
        s->vrc6_irq_mode    = false;
        s->vrc6_prescaler   = 0;
        s->vrc6_p1_ctrl  = 0; s->vrc6_p1_period = 0; s->vrc6_p1_en = false;
        s->vrc6_p1_timer = 0; s->vrc6_p1_seq    = 0;
        s->vrc6_p2_ctrl  = 0; s->vrc6_p2_period = 0; s->vrc6_p2_en = false;
        s->vrc6_p2_timer = 0; s->vrc6_p2_seq    = 0;
        s->vrc6_saw_rate = 0; s->vrc6_saw_period = 0; s->vrc6_saw_en = false;
        s->vrc6_saw_timer = 0; s->vrc6_saw_accum = 0; s->vrc6_saw_step = 0;
        s->vrc6_halt    = false;
        s->cart_irq     = false;
    }
    memset(&s->board, 0, sizeof(s->board));
    if (s->has_board) {
        nes_board_reset(s);
        s->cart_irq = false;
    }
    nes_update_irq(s);

    mos6502_reset(&s->cpu);
    apu2a03_reset(&s->apu);
}

/* ── API (nes_core.h) ────────────────────────────────────────────────────── */

bool nes_core_load(NesState *s, const char *path) {
    nes_core_error[0] = '\0';
    return ines_load(s, path);
}

/* As upstream nes_create, from rp2c02_init on, for a cartridge machine. */
void nes_core_init(NesState *s, const MosConfig *cfg) {
    s->cfg = cfg;

    rp2c02_init(&s->ppu, cfg->is_pal);
    if (cfg->is_dendy) s->ppu.vblank_line = 291;  /* Dendy: 50 extra post-render lines */
    if (cfg->disabled_features & MOS_FEATURE_ODD_FRAME_SKIP)
        s->ppu.no_odd_skip = true;
    s->ppu.chr_read  = nes_chr_read;
    s->ppu.chr_write = nes_chr_write;
    s->ppu.chr_ud    = s;
    s->ppu.mirror    = s->cart.mirror;

    mos6502_init(&s->cpu);
    s->cpu.mem_read        = nes_cpu_read;
    s->cpu.mem_write       = nes_cpu_write;
    s->cpu.mem_ud          = s;
    mos_cpu_apply(&s->cpu, cfg->cpu);
    s->cpu.dummy_cycles = true;   /* page-crossing and branch dummy reads hit registers */
    s->cpu.cycle_polling = true;  /* interrupts polled on each instruction's penultimate cycle */

    s->apu.sweep_enabled = true;
    s->apu.dmc_enabled = true;
    s->apu.frame_irq_enabled = true;
    if (cfg->sound == MOS_SOUND_2A03) {
        uint32_t apu_clock = cfg->cpu == MOS_CPU_2A07 ? 1662607u : 1789773u;
        apu2a03_init(&s->apu, apu_clock);   /* silent if audio won't open */
        s->apu.mem_read = nes_cpu_read;
        s->apu.mem_ud   = s;
    }
    s->apu.sweep_enabled = (cfg->disabled_features & MOS_FEATURE_SWEEP_REGISTERS) == 0;
    s->apu.dmc_enabled = (cfg->disabled_features & MOS_FEATURE_DMC_CHANNEL) == 0;
    s->apu.dma_external = true;   /* the DMC's fetches are DMAs that halt the CPU */
    s->apu.frame_irq_enabled = (cfg->disabled_features & MOS_FEATURE_FRAME_IRQ) == 0;

    s->ppu.no_argb = true;        /* Wii: we draw from ppu.pixels */

    nes_install_mapper_hooks(s);
    nes_reset(s);
    ppu_deadline_update(s);
    prg_windows_update(s);
}

void nes_core_reset(NesState *s) { nes_reset(s); ppu_deadline_update(s); prg_windows_update(s); }

/* As one pass of upstream nes_run's loop, without the display. */
/* Wii: idle loops.  Games wait for the NMI in a loop like "jmp *" or
 * "lda flag / beq": when a pass of a short backward loop starts where the
 * last one did, with the same registers, having written nothing and read
 * only RAM and ROM, every further pass is the same until something outside
 * the CPU changes - which happens no sooner than the PPU's or APU's next
 * event.  So the passes up to just before that are skipped, cycles only:
 * the PPU and APU then catch up as they would have anyway. */
typedef struct {
    uint16_t pc;            /* loop head, 0 = none */
    uint8_t  a, x, y, sp, p;
    bool     poll[4];
    uint64_t cycle;
} LoopMark;
static LoopMark loop;

static void loop_mark(NesState *s, uint16_t pc) {
    const Mos6502 *c = &s->cpu;
    loop = (LoopMark){ pc, c->A, c->X, c->Y, c->SP, c->P,
                       { c->poll_nmi[0], c->poll_nmi[1], c->poll_irq[0], c->poll_irq[1] },
                       c->cycle_count };
    loop_touched = false;
}

static void skip_idle_loop(NesState *s) {
    const Mos6502 *c = &s->cpu;
    uint64_t len = c->cycle_count - loop.cycle;
    if (!loop_touched && !c->nmi && !s->dma_halt && !apu_eager && len && len <= 32 &&
        c->A == loop.a && c->X == loop.x && c->Y == loop.y && c->SP == loop.sp &&
        c->P == loop.p && c->poll_nmi[0] == loop.poll[0] && c->poll_nmi[1] == loop.poll[1] &&
        c->poll_irq[0] == loop.poll[2] && c->poll_irq[1] == loop.poll[3]) {
        uint64_t limit = ppu_deadline < s->apu_deadline + 1u ? ppu_deadline : s->apu_deadline + 1u;
        uint64_t now = c->cycle_count;
        if (limit > now + 2u * len) s->cpu.cycle_count += (limit - now - 2u * len) / len * len;
    }
    loop_mark(s, c->PC);
}

void nes_core_frame(NesState *s) {
    s->ppu.dirty = false;
    while (!s->ppu.dirty) {
        uint16_t pc0 = s->cpu.PC;
        mos6502_step(&s->cpu);
        if (s->cpu.cycle_count >= ppu_deadline) nes_sync_ppu_to_cpu(s, s->cpu.cycle_count);
        if (s->ppu.dirty) break;
        uint16_t pc = s->cpu.PC;
        if (pc == loop.pc)                     skip_idle_loop(s);
        else if (pc <= pc0 && pc0 - pc <= 16) loop_mark(s, pc);   /* a short loop's head */
    }
    /* as upstream, which syncs after every instruction: to the end of the
     * one that finished the frame */
    nes_sync_ppu_to_cpu(s, s->cpu.cycle_count);
    nes_sync_apu_to_cpu(s, s->cpu.cycle_count);
    apu2a03_flush(&s->apu);
}

void nes_core_free(NesState *s) {
    apu2a03_destroy(&s->apu);
    free(s->prg); s->prg = NULL;
    free(s->chr); s->chr = NULL;
}

/* After the whole NesState was replaced (a state from another run). */
void nes_core_resync(NesState *s) {
    ppu_deadline_update(s);
    prg_windows_update(s);
    loop.pc = 0;
}
