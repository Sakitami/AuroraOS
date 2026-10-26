// Game Boy sound: the APU (minigb_apu) and a stream to the speakers.
#ifndef GAMEBOY_SOUND_H
#define GAMEBOY_SOUND_H

#include <stdbool.h>
#include <stdint.h>

// The APU's registers, 0xFF10..0xFF3F, for Peanut-GB's audio_read() and
// audio_write().
uint8_t sound_read(uint16_t addr);
void sound_write(uint16_t addr, uint8_t val);

// The APU as at power-on.
void sound_reset(void);

// Starts and stops the stream. sound_start() is false when the console cannot
// play it (no ARM11 core behind the app, or one too old for voices).
bool sound_start(void);
void sound_stop(void);
bool sound_playing(void);

// One Game Boy frame of sound, after each gb_run_frame().
void sound_frame(void);

#endif
