/*
 * Atari 2600 bit-exactness test (host build; `make test-a26`).
 *
 * Built twice: with -DA26_REF against upstream GEMU's own sources and its
 * eager bus (the TIA run before every access, as machine_atari2600.c), and
 * without it against this repository's sources and hardware/machine/mos/
 * a2600_bus.c.  Each run feeds the same scripted inputs, changed at the same
 * CPU cycles, and writes every finished frame's hash and every audio sample:
 *
 *     a26_exact ROM CYCLES OUT     ->  OUT.frames (u64 per frame), OUT.audio (float)
 *
 * tools/a26_compare.py checks the two runs match (up to the shorter run,
 * since the lazy TIA finishes its last frame a little later).
 */
#include "atari2600.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE *audio_out;

/* ── SDL audio-queue stub: record the samples ────────────────────────────── */
int SDL_InitSubSystem(Uint32 f) { (void)f; return 0; }
void SDL_QuitSubSystem(Uint32 f) { (void)f; }
const char *SDL_GetError(void) { return ""; }
SDL_AudioDeviceID SDL_OpenAudioDevice(const char *d, int c, const SDL_AudioSpec *w,
                                      SDL_AudioSpec *h, int a)
{ (void)d; (void)c; (void)a; if (h) *h = *w; return 1; }
void SDL_PauseAudioDevice(SDL_AudioDeviceID d, int p) { (void)d; (void)p; }
void SDL_CloseAudioDevice(SDL_AudioDeviceID d) { (void)d; }
int SDL_QueueAudio(SDL_AudioDeviceID d, const void *x, Uint32 l)
{ (void)d; fwrite(x, 1, l, audio_out); return 0; }
Uint32 SDL_GetQueuedAudioSize(SDL_AudioDeviceID d) { (void)d; return 0; }
void SDL_ClearQueuedAudio(SDL_AudioDeviceID d) { (void)d; }

static Atari2600State S;

/* ── The bus under test ──────────────────────────────────────────────────── */
#ifdef A26_REF
/* Upstream machine_atari2600.c's bus, verbatim apart from the monitor. */
static inline void bus_access(Atari2600State *s, uint16_t addr) {
    if (addr != s->last_addr) { s->cart.distinct++; s->last_addr = addr; }
    tia_run(&s->tia, s->cpu.cycle_count * 3u);
}
static uint8_t a26_read(uint16_t addr, void *ud) {
    Atari2600State *s = ud;
    bus_access(s, addr);
    uint8_t v;
    if (addr & 0x1000u)        v = a26_cart_read(&s->cart, addr, s->bus, s->cpu.cycle_count);
    else if (!(addr & 0x80u))  v = tia_read(&s->tia, addr, s->bus);
    else if (!(addr & 0x200u)) v = s->riot.ram[addr & 0x7Fu];
    else                       v = m6532_read(&s->riot, addr, s->cpu.cycle_count);
    a26_cart_snoop(&s->cart, addr, v, false);
    s->bus = v;
    return v;
}
static void a26_write(uint16_t addr, uint8_t val, void *ud) {
    Atari2600State *s = ud;
    bus_access(s, addr);
    if (addr & 0x1000u)        a26_cart_write(&s->cart, addr, val, s->cpu.cycle_count);
    else if (!(addr & 0x80u))  s->wsync |= tia_write(&s->tia, addr, val);
    else if (!(addr & 0x200u)) s->riot.ram[addr & 0x7Fu] = val;
    else                       m6532_write(&s->riot, addr, val, s->cpu.cycle_count);
    a26_cart_snoop(&s->cart, addr, val, true);
    s->bus = val;
}
static uint8_t ram_peek(void *ud, uint8_t a) { return ((Atari2600State *)ud)->riot.ram[a & 0x7Fu]; }
static void ram_poke(void *ud, uint8_t a, uint8_t v) { ((Atari2600State *)ud)->riot.ram[a & 0x7Fu] = v; }
static void bus_attach(Atari2600State *s) {
    s->cpu.mem_read = a26_read; s->cpu.mem_write = a26_write; s->cpu.mem_ud = s;
    a26_cart_set_ram_access(ram_peek, ram_poke, s);
}
static void bus_step(Atari2600State *s) {
    mos6502_step(&s->cpu);
    if (s->wsync) {
        s->wsync = false;
        int left = (TIA_LINE_CLOCKS - s->tia.hctr) % TIA_LINE_CLOCKS;
        s->cpu.cycle_count += (uint64_t)(left + 2) / 3u;
    }
}
static void bus_power_on(Atari2600State *s) {
    tia_reset(&s->tia);
    m6532_reset(&s->riot, s->cpu.cycle_count);
    a26_cart_reset(&s->cart);
    s->wsync = false;
    mos6502_reset(&s->cpu);
}
static void bus_sync(Atari2600State *s) { (void)s; }
#else
#include "a2600_bus.h"
#define bus_attach   a26_bus_attach
#define bus_step     a26_bus_step
#define bus_power_on a26_bus_power_on
#define bus_sync     a26_bus_sync
#endif

/* ── Controllers (as machine_atari2600.c: two joysticks) ─────────────────── */
static uint8_t read_pa(void *ud) {
    const Atari2600State *s = ud;
    uint8_t v = 0xFF;
    for (int port = 0; port < 2; port++) {
        int base = port ? A26_ACT_P2_UP : A26_ACT_P1_UP, shift = port ? 0 : 4;
        for (int k = 0; k < 4; k++)
            if (s->held & GEMU_ACTION(base + k)) v &= (uint8_t)~((1u << k) << shift);
    }
    return v;
}
static uint8_t read_pb(void *ud) {
    const Atari2600State *s = ud;
    uint8_t v = 0x3F;
    if (s->held & GEMU_ACTION(A26_ACT_RESET))  v &= (uint8_t)~0x01u;
    if (s->held & GEMU_ACTION(A26_ACT_SELECT)) v &= (uint8_t)~0x02u;
    if (!s->color)    v &= (uint8_t)~0x08u;
    if (s->diff_a[0]) v |= 0x40u;
    if (s->diff_a[1]) v |= 0x80u;
    return v;
}

/* Inputs change every 20000 cycles from a fixed LCG; Game Reset is held
 * for the first stretch (most games start on it), the switches flip now
 * and then. */
static void script_inputs(Atari2600State *s, uint32_t *lcg, uint64_t n) {
    *lcg = *lcg * 1664525u + 1013904223u;
    uint32_t r = *lcg >> 8;
    uint32_t h = r & (GEMU_ACTION(A26_ACT_P1_UP) | GEMU_ACTION(A26_ACT_P1_DOWN) |
                      GEMU_ACTION(A26_ACT_P1_LEFT) | GEMU_ACTION(A26_ACT_P1_RIGHT) |
                      GEMU_ACTION(A26_ACT_P1_FIRE) | GEMU_ACTION(A26_ACT_P2_FIRE) |
                      GEMU_ACTION(A26_ACT_P2_LEFT));
    if (n >= 10 && n < 25) h |= GEMU_ACTION(A26_ACT_RESET);
    if (n % 97 == 50) h |= GEMU_ACTION(A26_ACT_SELECT);
    if (n % 211 == 100) s->color = !s->color;
    if (n % 157 == 70) s->diff_a[0] = !s->diff_a[0];
    s->held = h;
    s->tia.fire[0] = h & GEMU_ACTION(A26_ACT_P1_FIRE);
    s->tia.fire[1] = h & GEMU_ACTION(A26_ACT_P2_FIRE);
}

static uint64_t fnv(const void *p, size_t n) {
    const uint8_t *b = p;
    uint64_t h = 1469598103934665603ull;
    while (n--) { h ^= *b++; h *= 1099511628211ull; }
    return h;
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s ROM CYCLES OUT\n", argv[0]); return 2; }
    char path[512];
    snprintf(path, sizeof(path), "%s.audio", argv[3]);
    audio_out = fopen(path, "wb");
    snprintf(path, sizeof(path), "%s.frames", argv[3]);
    FILE *frames = fopen(path, "wb");
    if (!audio_out || !frames) return 2;

    Atari2600State *s = &S;
    s->color = true;
    s->cart.cpu_hz = A2600_COLOR_HZ_NTSC / 3.0;
    if (!a26_cart_load(&s->cart, argv[1])) return 1;
    tia_init(&s->tia);
    tia_sound_init(&s->tia.sound, A2600_COLOR_HZ_NTSC / 114.0, true);
    m6532_init(&s->riot);
    s->riot.read_pa = read_pa; s->riot.read_pb = read_pb; s->riot.ud = s;
    mos6502_init(&s->cpu);
    bus_attach(s);
    s->cpu.addr_mask = 0x1FFFu; s->cpu.no_int_pins = true; s->cpu.dummy_cycles = true;
    bus_power_on(s);

    uint64_t limit = strtoull(argv[2], NULL, 0), next = 0, n_inputs = 0, n_frames = 0;
    uint32_t lcg = 12345;
    while (s->cpu.cycle_count < limit) {
        if (s->cpu.cycle_count >= next) { script_inputs(s, &lcg, n_inputs++); next += 20000; }
        bus_step(s);
        if (s->tia.frame_ready) {
            s->tia.frame_ready = false;
            uint64_t h = fnv(tia_frame(&s->tia), sizeof(s->tia.fb[0]));
            fwrite(&h, sizeof(h), 1, frames);
            n_frames++;
            tia_sound_flush(&s->tia.sound);
        }
    }
    bus_sync(s);
    tia_sound_flush(&s->tia.sound);
    fclose(frames); fclose(audio_out);
    printf("%llu frames, %llu cycles\n", (unsigned long long)n_frames,
           (unsigned long long)s->cpu.cycle_count);
    return 0;
}
