/*
 * NES bit-exactness test (host build; `make test-nes`).
 *
 * Built twice: with -DNES_REF against upstream GEMU's machine_nes.c and
 * its PPU, APU and boards (desktop parts stubbed out by
 * tests/nes_ref_stubs.c, the run loop driven through the display and
 * monitor stubs), and without it against this repository's
 * hardware/machine/mos/nes_core.c and sources.  Both get the same
 * controller input, frame by frame, and write each frame's hash and every
 * audio sample:
 *
 *     nes_exact ROM FRAMES OUT   ->  OUT.frames (u64 per frame), OUT.audio (float)
 *
 * tools/a26_compare.py checks the two runs match.
 */
#include <SDL2/SDL.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE *audio_out, *frames_out;
static unsigned n_frames, frame_limit;
static uint32_t lcg = 12345, held;

/* ── SDL stub: record the samples ────────────────────────────────────────── */
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
#ifdef NES_REF
Uint32 SDL_GetTicks(void) { return 0; }
void SDL_Delay(Uint32 ms) { (void)ms; }
int SDL_ShowMessageBox(const SDL_MessageBoxData *d, int *b) { (void)d; *b = 0; return 0; }
#endif

/* ── Input script ────────────────────────────────────────────────────────── */
/* Controller bits as NES_BTN_*: A B Select Start Up Down Left Right; player
 * 2 in bits 8-15.  Start is pressed now and then (title screens), the rest
 * changes every 8 frames from a fixed LCG. */
static uint32_t script(unsigned n)
{
    if (n % 8 == 0) {
        lcg = lcg * 1664525u + 1013904223u;
        held = (lcg >> 8) & 0xF3F3u;          /* no Select/Start from the LCG */
        if ((held & 0x30u) == 0x30u) held &= ~0x20u;   /* not Up and Down together */
        if ((held & 0xC0u) == 0xC0u) held &= ~0x40u;
        if (n < 60) held = 0;
    }
    uint32_t h = held;
    if (n % 300 >= 90 && n % 300 < 96) h |= 0x08u;   /* Start */
    return h;
}

static uint64_t fnv(const void *p, size_t n)
{
    const uint8_t *b = p;
    uint64_t h = 1469598103934665603ull;
    while (n--) { h ^= *b++; h *= 1099511628211ull; }
    return h;
}

/* Frames are compared as ARGB.  Upstream's pixels_argb starts out zero
 * where ours (converted from palette index 0) is that colour: the same. */
static const uint32_t index0_argb = 0xFF666666u;

static void frame_done(const uint32_t *argb)
{
    static uint32_t norm[256 * 240];
    for (int i = 0; i < 256 * 240; i++) norm[i] = argb[i] ? argb[i] : index0_argb;
    argb = norm;
    uint64_t h = fnv(argb, 256u * 240u * 4u);
    fwrite(&h, sizeof(h), 1, frames_out);
    n_frames++;
}

static bool open_outputs(const char *prefix)
{
    char path[512];
    snprintf(path, sizeof(path), "%s.audio", prefix);
    audio_out = fopen(path, "wb");
    snprintf(path, sizeof(path), "%s.frames", prefix);
    frames_out = fopen(path, "wb");
    return audio_out && frames_out;
}

#ifdef NES_REF
/* ── Upstream: nes_create + nes_run, steered by the stubs ────────────────── */
#include "nes.h"

uint32_t nesx_poll(void)        { return script(n_frames); }
void     nesx_render(const uint32_t *argb) { frame_done(argb); }
bool     nesx_done(void)        { return n_frames >= frame_limit; }

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: %s ROM FRAMES OUT\n", argv[0]); return 2; }
    frame_limit = (unsigned)strtoul(argv[2], NULL, 0);
    if (!open_outputs(argv[3])) return 2;
    MosConfig cfg = {
        .machine = MOS_MACHINE_NES, .cpu = MOS_CPU_2A03, .vga = MOS_VGA_RP2C02,
        .sound = MOS_SOUND_2A03, .cart_path = argv[1],
        .ports = { NES_DEVICE_CONTROLLER, NES_DEVICE_CONTROLLER }, .n_ports = 2,
        .display_type = GEMU_DISPLAY_SDL,
    };
    NesState *s = nes_create(&cfg);
    if (!s) return 1;
    nes_run(s, &cfg);
    fclose(frames_out); fclose(audio_out);
    printf("%u frames\n", n_frames);
    return 0;
}
#else
/* ── Ours: nes_core ──────────────────────────────────────────────────────── */
#include "nes_core.h"

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: %s ROM FRAMES OUT\n", argv[0]); return 2; }
    frame_limit = (unsigned)strtoul(argv[2], NULL, 0);
    if (!open_outputs(argv[3])) return 2;
    static MosConfig cfg = {
        .machine = MOS_MACHINE_NES, .cpu = MOS_CPU_2A03, .vga = MOS_VGA_RP2C02,
        .sound = MOS_SOUND_2A03,
        .ports = { NES_DEVICE_CONTROLLER, NES_DEVICE_CONTROLLER }, .n_ports = 2,
    };
    NesState *s = calloc(1, sizeof(*s));
    if (!s || !nes_core_load(s, argv[1])) { fprintf(stderr, "%s\n", nes_core_error); return 1; }
    nes_core_init(s, &cfg);
    while (n_frames < frame_limit) {
        uint32_t h = script(n_frames);
        s->ctrl_state[0] = (uint8_t)h;
        s->ctrl_state[1] = (uint8_t)(h >> 8);
        nes_core_frame(s);
        static uint32_t argb[256 * 240];      /* the core only keeps palette indices */
        for (int i = 0; i < 256 * 240; i++) argb[i] = rp2c02_palette_rgb[s->ppu.pixels[i] & 0x3Fu];
        frame_done(argb);
    }
    nes_core_free(s);
    free(s);
    fclose(frames_out); fclose(audio_out);
    printf("%u frames\n", n_frames);
    return 0;
}
#endif
