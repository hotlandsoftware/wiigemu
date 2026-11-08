/*
 * SDL audio-queue API on ASND (see compat/SDL2/SDL.h), so upstream sound
 * code that queues samples with SDL_QueueAudio runs unchanged.
 *
 * Queued float samples go into a ring of int16; an ASND voice streams it
 * through a chain of DMA buffers, refilled from its callback (which runs in
 * the audio interrupt, once per mixing pass) whenever the voice's next-buffer
 * slot is free.  SDL_AUDIO_WII_CHUNK samples go out at a time, so callers
 * should keep at least that much queued.  The ring has one producer (the emulator)
 * and one consumer (the callback), so plain volatile indices are enough.
 * When the ring runs dry the voice plays silence, so a paused machine just
 * goes quiet.
 */
#include <SDL2/SDL.h>

#include <gccore.h>
#include <asndlib.h>
#include <string.h>

#define VOICE      1                 /* voice 0 is the CHIP-8 beeper */
#define RING       16384             /* samples (power of two) */
/* Samples per DMA buffer: longer than one ASND mixing pass (~21 ms), which
 * is how often the callback runs - with shorter buffers the voice would
 * starve between passes and play slow, with gaps. */
#define CHUNK      SDL_AUDIO_WII_CHUNK
#define N_CHUNKS   4

static int16_t           ring[RING];
static volatile uint32_t ring_r, ring_w;      /* free-running indices */
static int16_t           dma[N_CHUNKS][CHUNK] __attribute__((aligned(32)));
static int               dma_next;
static int               rate;
static float             speed = 1.0f;
static bool              open_dev, playing;
static int16_t           last;               /* held on underrun to avoid clicks */
volatile uint32_t        sdl_audio_wii_underruns;   /* buffers the ring couldn't fill */

int         SDL_InitSubSystem(Uint32 flags) { (void)flags; return 0; }
void        SDL_QuitSubSystem(Uint32 flags) { (void)flags; }
const char *SDL_GetError(void)              { return "unsupported audio format"; }

static void fill(int16_t *buf)
{
    if (ring_w - ring_r < CHUNK && playing) sdl_audio_wii_underruns++;
    for (unsigned i = 0; i < CHUNK; i++) {
        if (ring_r != ring_w) last = ring[ring_r++ & (RING - 1)];
        else if (last) last -= last / 16 + (last > 0 ? 1 : -1);   /* fade to 0 */
        buf[i] = last;
    }
    DCFlushRange(buf, sizeof(dma[0]));
}

static void voice_cb(s32 voice)
{
    if (!ASND_TestVoiceBufferReady(voice)) return;   /* next slot still queued */
    int16_t *buf = dma[dma_next];
    dma_next = (dma_next + 1) % N_CHUNKS;
    fill(buf);
    ASND_AddVoice(voice, buf, sizeof(dma[0]));
}

SDL_AudioDeviceID SDL_OpenAudioDevice(const char *device, int iscapture,
                                      const SDL_AudioSpec *want, SDL_AudioSpec *have,
                                      int allowed_changes)
{
    (void)device; (void)allowed_changes;
    if (iscapture || open_dev || !want || want->format != AUDIO_F32SYS ||
        want->channels != 1 || want->callback)
        return 0;
    rate = want->freq;
    speed = 1.0f;
    if (have) *have = *want;
    ring_r = ring_w = 0;
    last = 0;
    open_dev = true;
    playing = false;
    return 1;
}

void sdl_audio_wii_set_speed(float s)
{
    speed = s;
    if (playing) ASND_ChangePitchVoice(VOICE, (s32)(rate * speed + 0.5f));
}

void SDL_PauseAudioDevice(SDL_AudioDeviceID dev, int pause_on)
{
    if (dev != 1 || !open_dev) return;
    if (!pause_on && !playing) {
        ASND_Pause(0);
        dma_next = 1;
        fill(dma[0]);
        ASND_SetVoice(VOICE, VOICE_MONO_16BIT, (s32)(rate * speed + 0.5f), 0, dma[0], sizeof(dma[0]),
                      200, 200, voice_cb);         /* of 255: headroom vs. clipping */
        playing = true;
    } else if (pause_on && playing) {
        ASND_StopVoice(VOICE);
        playing = false;
    }
}

void SDL_CloseAudioDevice(SDL_AudioDeviceID dev)
{
    if (dev != 1 || !open_dev) return;
    SDL_PauseAudioDevice(dev, 1);
    open_dev = false;
}

int SDL_QueueAudio(SDL_AudioDeviceID dev, const void *data, Uint32 len)
{
    if (dev != 1 || !open_dev) return -1;
    const float *f = data;
    for (Uint32 i = 0; i < len / sizeof(float); i++) {
        if (ring_w - ring_r >= RING) break;          /* full: drop the rest */
        float v = f[i] * 32767.0f;
        ring[ring_w & (RING - 1)] = (int16_t)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : v);
        ring_w++;
    }
    return 0;
}

/* In the units the caller queued: float bytes. */
Uint32 SDL_GetQueuedAudioSize(SDL_AudioDeviceID dev)
{
    if (dev != 1 || !open_dev) return 0;
    return (ring_w - ring_r) * (Uint32)sizeof(float);
}

void SDL_ClearQueuedAudio(SDL_AudioDeviceID dev)
{
    if (dev == 1) ring_r = ring_w;
}
