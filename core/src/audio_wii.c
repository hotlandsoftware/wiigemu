#include "gemu/audio_wii.h"

#include <gccore.h>
#include <asndlib.h>
#include <stdint.h>

#define BEEP_RATE    48000
#define BEEP_HZ      480                       /* 100 samples per period */
#define BEEP_PERIOD  (BEEP_RATE / BEEP_HZ)
#define BEEP_SAMPLES (BEEP_PERIOD * 16)        /* 3200 bytes, 32-byte multiple */
#define BEEP_VOICE   0
#define BEEP_VOLUME  96                        /* of 255 */

static int16_t beep_buf[BEEP_SAMPLES] __attribute__((aligned(32)));
static bool    beeping;

void wii_audio_init(void)
{
    for (int i = 0; i < BEEP_SAMPLES; i++)
        beep_buf[i] = (i % BEEP_PERIOD) < BEEP_PERIOD / 2 ? 6000 : -6000;
    DCFlushRange(beep_buf, sizeof(beep_buf));

    ASND_Init();
    ASND_Pause(0);
}

void wii_audio_beep(bool on)
{
    if (on == beeping) return;
    beeping = on;
    if (on)
        ASND_SetInfiniteVoice(BEEP_VOICE, VOICE_MONO_16BIT, BEEP_RATE, 0,
                              beep_buf, sizeof(beep_buf),
                              BEEP_VOLUME, BEEP_VOLUME);
    else
        ASND_StopVoice(BEEP_VOICE);
}

void wii_audio_shutdown(void)
{
    wii_audio_beep(false);
    ASND_End();
}
