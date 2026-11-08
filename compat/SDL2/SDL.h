#pragma once
/*
 * The slice of SDL2 that upstream GEMU sources compiled unchanged on the Wii
 * use: the audio queue (SDL_OpenAudioDevice + SDL_QueueAudio), as in
 * audio/tia_sound.c.  Implemented on ASND by core/src/sdl_audio_wii.c.
 *
 * Only mono AUDIO_F32SYS output is supported; anything else fails to open.
 */
#include <stdint.h>

typedef uint8_t  Uint8;
typedef uint16_t Uint16;
typedef uint32_t Uint32;
typedef int32_t  Sint32;
typedef uint32_t SDL_AudioDeviceID;
typedef uint16_t SDL_AudioFormat;

#define SDL_INIT_AUDIO 0x00000010u
/* Wii: samples handed to the hardware per refill (see sdl_audio_wii.c). */
#define SDL_AUDIO_WII_CHUNK 1024u
extern volatile Uint32 sdl_audio_wii_underruns;   /* Wii: refills that ran dry */
/* Wii: play the queue at speed x the rate it was opened with (e.g. to make
 * one emulated frame per vblank exactly what the hardware consumes). */
void sdl_audio_wii_set_speed(float speed);
#define AUDIO_F32SYS   0x8120u      /* 32-bit float, native byte order */

typedef void (*SDL_AudioCallback)(void *userdata, Uint8 *stream, int len);

typedef struct SDL_AudioSpec {
    int               freq;
    SDL_AudioFormat   format;
    Uint8             channels;
    Uint8             silence;
    Uint16            samples;
    Uint16            padding;
    Uint32            size;
    SDL_AudioCallback callback;     /* must be NULL: queue mode only */
    void             *userdata;
} SDL_AudioSpec;

int               SDL_InitSubSystem(Uint32 flags);
void              SDL_QuitSubSystem(Uint32 flags);
const char       *SDL_GetError(void);
SDL_AudioDeviceID SDL_OpenAudioDevice(const char *device, int iscapture,
                                      const SDL_AudioSpec *desired,
                                      SDL_AudioSpec *obtained, int allowed_changes);
void              SDL_PauseAudioDevice(SDL_AudioDeviceID dev, int pause_on);
void              SDL_CloseAudioDevice(SDL_AudioDeviceID dev);
int               SDL_QueueAudio(SDL_AudioDeviceID dev, const void *data, Uint32 len);
Uint32            SDL_GetQueuedAudioSize(SDL_AudioDeviceID dev);
void              SDL_ClearQueuedAudio(SDL_AudioDeviceID dev);
