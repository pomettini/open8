/** @file audio.c
 *
 *  See audio.h. PICO-8 sound data layout (in pico8_ram):
 *
 *    SFX:   0x3200, 64 slots x 68 bytes.
 *           bytes 0..63 = 32 notes, 2 bytes each (little-endian):
 *             pitch  = note & 0x3f          (0..63 semitones)
 *             wave   = (note >> 6) & 0x7    (0..7 instrument)
 *             volume = (note >> 9) & 0x7    (0 = silent .. 7 = max)
 *             effect = (note >> 12) & 0x7
 *           byte 65 = speed (note duration), 66 = loop start, 67 = loop end.
 *    Music: 0x3100, 64 patterns x 4 bytes (one per channel):
 *             low 6 bits = sfx index; bit 6 set = channel off;
 *             bit 7 of bytes 0/1/2 = loop-begin / loop-end / stop flags.
 *
 *  SPDX-License-Identifier: MIT
 **/
#include <stdint.h>
#include <math.h>

#include "audio.h"

extern uint8_t pico8_ram[]; /* defined in memory.c */

#define SR              44100
#define SFX_BASE        0x3200
#define SFX_BYTES       68
#define SFX_NOTES       32
#define MUSIC_BASE      0x3100
#define NUM_CH          4
/* 44100 / ~120.5 notes-per-second at speed 1 (PICO-8 reference tempo). */
#define SAMPLES_PER_SPEED 366

typedef struct
{
    int      active;
    int      sfx;          /* current sfx slot, or -1 */
    int      note;         /* current note index */
    int      last;         /* last note index to play (offset+length-1) */
    int      loop_s, loop_e;
    int      spn;          /* samples per note */
    int      samp;         /* samples elapsed in current note */
    float    phase;        /* oscillator phase [0,1) */
    uint32_t noise;        /* per-channel noise state */
    int      from_music;   /* triggered by music() */
} channel_t;

static channel_t ch[NUM_CH];

static int  mus_active;
static int  mus_pattern;
static int  mus_mask;

static float pitch_hz[64];
static int   tables_ready;

static void build_tables(void)
{
    if (tables_ready) return;
    for (int p = 0; p < 64; p++)
    {
        /* PICO-8: pitch 33 ~= 440 Hz; one semitone per step. */
        pitch_hz[p] = 440.0f * powf(2.0f, (float)(p - 33) / 12.0f);
    }
    tables_ready = 1;
}

void audio_reset(void)
{
    build_tables();
    for (int c = 0; c < NUM_CH; c++)
    {
        ch[c].active = 0;
        ch[c].sfx = -1;
        ch[c].noise = 0x1234u + (uint32_t)c * 0x9e37u;
    }
    mus_active = 0;
    mus_pattern = 0;
    mus_mask = 0;
}

static const uint8_t* sfx_ptr(int n) { return &pico8_ram[SFX_BASE + n * SFX_BYTES]; }

static void start_sfx_on(int c, int n, int offset, int length)
{
    const uint8_t* s = sfx_ptr(n);
    int speed = s[65];
    if (speed < 1) speed = 1;

    ch[c].active = 1;
    ch[c].sfx    = n;
    ch[c].note   = (offset > 0) ? offset : 0;
    ch[c].loop_s = s[66];
    ch[c].loop_e = s[67];
    ch[c].spn    = speed * SAMPLES_PER_SPEED;
    ch[c].samp   = 0;
    ch[c].phase  = 0.0f;
    if (length > 0)
        ch[c].last = ch[c].note + length - 1;
    else
        ch[c].last = SFX_NOTES - 1;
    if (ch[c].last > SFX_NOTES - 1) ch[c].last = SFX_NOTES - 1;
}

void audio_sfx(int n, int channel, int offset, int length)
{
    build_tables();

    if (n < 0)
    {
        /* -1: stop the given channel (or all if channel < 0). */
        if (channel < 0) { for (int c = 0; c < NUM_CH; c++) ch[c].active = 0; }
        else if (channel < NUM_CH) ch[channel].active = 0;
        return;
    }
    if (n >= 64) return;

    int c = channel;
    if (c < 0) /* auto: first free channel, else channel 0 */
    {
        c = 0;
        for (int i = 0; i < NUM_CH; i++) { if (!ch[i].active) { c = i; break; } }
    }
    if (c >= NUM_CH) return;

    start_sfx_on(c, n, offset, length);
    ch[c].from_music = 0;
}

/* Trigger the channels of the current music pattern. */
static void music_load_pattern(void)
{
    if (!mus_active) return;
    const uint8_t* pat = &pico8_ram[MUSIC_BASE + mus_pattern * 4];
    for (int c = 0; c < NUM_CH; c++)
    {
        uint8_t b = pat[c];
        if (b & 0x40) { ch[c].active = 0; continue; } /* channel off this pattern */
        if (mus_mask && !(mus_mask & (1 << c))) continue;
        start_sfx_on(c, b & 0x3f, 0, 0);
        ch[c].from_music = 1;
    }
}

void audio_music(int n, int fade_ms, int channel_mask)
{
    (void)fade_ms; /* fades not implemented */
    build_tables();

    if (n < 0)
    {
        mus_active = 0;
        for (int c = 0; c < NUM_CH; c++) if (ch[c].from_music) ch[c].active = 0;
        return;
    }
    if (n >= 64) return;

    mus_active  = 1;
    mus_pattern = n;
    mus_mask    = channel_mask;
    music_load_pattern();
}

/* When all music-driven channels have finished, advance the pattern. */
static void music_advance_if_done(void)
{
    if (!mus_active) return;
    for (int c = 0; c < NUM_CH; c++)
        if (ch[c].from_music && ch[c].active) return; /* still playing */

    const uint8_t* pat = &pico8_ram[MUSIC_BASE + mus_pattern * 4];
    if (pat[2] & 0x80) { mus_active = 0; return; }        /* stop flag */
    if (pat[1] & 0x80)                                    /* loop-end: find loop start */
    {
        int p = mus_pattern;
        while (p > 0 && !(pico8_ram[MUSIC_BASE + p * 4] & 0x80)) p--;
        mus_pattern = p;
    }
    else
    {
        mus_pattern = (mus_pattern + 1) & 0x3f;
    }
    music_load_pattern();
}

static float osc(int wave, float p, channel_t* c)
{
    switch (wave)
    {
        case 0: return 4.0f * fabsf(p - 0.5f) - 1.0f;                 /* triangle */
        case 1: return (p < 0.875f) ? (p / 0.875f) * 2.0f - 1.0f      /* tilted saw (approx) */
                                    : 1.0f - (p - 0.875f) / 0.125f * 2.0f;
        case 2: return 2.0f * p - 1.0f;                              /* saw */
        case 3: return (p < 0.5f) ? 1.0f : -1.0f;                    /* square */
        case 4: return (p < 0.3125f) ? 1.0f : -1.0f;                 /* pulse (~1/3) */
        case 5: return (4.0f * fabsf(p - 0.5f) - 1.0f) * 0.6f        /* organ (approx) */
                     + (4.0f * fabsf(fmodf(p * 2.0f, 1.0f) - 0.5f) - 1.0f) * 0.4f;
        case 6: { /* noise */
            c->noise = c->noise * 1103515245u + 12345u;
            return ((float)((c->noise >> 9) & 0xffff) / 32768.0f) - 1.0f;
        }
        case 7: { /* phaser (approx: two detuned saws) */
            float a = 2.0f * p - 1.0f;
            float q = fmodf(p * 1.005f, 1.0f);
            return (a + (2.0f * q - 1.0f)) * 0.5f;
        }
        default: return 0.0f;
    }
}

int audio_render(int16_t* out, int frames)
{
    int any = 0;

    for (int i = 0; i < frames; i++)
    {
        float mix = 0.0f;

        for (int c = 0; c < NUM_CH; c++)
        {
            if (!ch[c].active) continue;
            any = 1;

            const uint8_t* s = sfx_ptr(ch[c].sfx);
            const uint8_t* np = s + ch[c].note * 2;
            unsigned n16   = (unsigned)np[0] | ((unsigned)np[1] << 8);
            int pitch  = n16 & 0x3f;
            int wave   = (n16 >> 6) & 0x7;
            int vol    = (n16 >> 9) & 0x7;
            int effect = (n16 >> 12) & 0x7;

            if (vol > 0)
            {
                float amp = (float)vol / 7.0f;
                /* effects 4/5 = fade in/out across the note (others not yet impl). */
                if (effect == 4) amp *= (float)ch[c].samp / (float)ch[c].spn;
                else if (effect == 5) amp *= 1.0f - (float)ch[c].samp / (float)ch[c].spn;

                mix += osc(wave, ch[c].phase, &ch[c]) * amp;

                float step = pitch_hz[pitch] / (float)SR;
                ch[c].phase += step;
                if (ch[c].phase >= 1.0f) ch[c].phase -= 1.0f;
            }

            /* advance note timing */
            if (++ch[c].samp >= ch[c].spn)
            {
                ch[c].samp = 0;
                ch[c].phase = 0.0f;
                int next = ch[c].note + 1;
                if (ch[c].loop_e > ch[c].loop_s && next >= ch[c].loop_e)
                    next = ch[c].loop_s;            /* sfx internal loop */
                if (next > ch[c].last) { ch[c].active = 0; }
                else ch[c].note = next;
            }
        }

        /* 0.25 headroom for 4 channels, soft clamp to int16. */
        int v = (int)(mix * 0.25f * 32767.0f);
        if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
        out[i] = (int16_t)v;
    }

    music_advance_if_done();
    return any;
}
