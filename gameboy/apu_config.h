// minigb_apu's settings, the same for every file that includes it: the
// register writes work out values from the sample rate, and the ARM11 (which
// takes the writes) and the ARM9 (which makes the samples) share the APU's
// state.
#ifndef GAMEBOY_APU_CONFIG_H
#define GAMEBOY_APU_CONFIG_H

// 275 samples a Game Boy frame, a little more than the voice plays (see
// sound.c); half the usual 32 kHz, to halve what the APU costs the ARM9.
#define AUDIO_SAMPLE_RATE 16426
#define MINIGB_APU_AUDIO_FORMAT_S16SYS

#endif
