/** @file audio.h
 *
 *  PICO-8 audio synthesiser for open8 (platform-independent).
 *
 *  Renders the PICO-8 SFX (0x3200) and music (0x3100) data from pico8_ram into
 *  44.1 kHz mono int16 samples. The platform layer pulls audio_render() from its
 *  output callback (Playdate: a sound AudioSource). sfx()/music() in api.c drive
 *  the channel state.
 *
 *  Scope: a working core — 4 channels, the standard waveforms, pitch/volume,
 *  per-SFX speed/loop, and basic music pattern sequencing. Some instruments
 *  (tilted-saw / organ / phaser) and note effects (slide/vibrato/arp) are
 *  approximated or stubbed; see audio.c. Constants (tempo, mix level) may want
 *  on-device tuning.
 *
 *  SPDX-License-Identifier: MIT
 **/
#ifndef OPEN8_AUDIO_H
#define OPEN8_AUDIO_H

#include <stdint.h>

/* Stop all channels and music. Call on cart (re)load. */
void audio_reset(void);

/* PICO-8 sfx(n, channel, offset, length). channel < 0 = auto; n < 0 = stop. */
void audio_sfx(int n, int channel, int offset, int length);

/* PICO-8 music(n, fade_ms, channel_mask). n < 0 = stop. */
void audio_music(int n, int fade_ms, int channel_mask);

/* Render `frames` mono samples at 44100 Hz. Safe to call with audio stopped
 * (produces silence). Returns 1 if any channel was active. */
int audio_render(int16_t* out, int frames);

#endif /* OPEN8_AUDIO_H */
