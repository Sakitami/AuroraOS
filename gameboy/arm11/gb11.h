// A Game Boy frame on the ARM11: the block the app (on the ARM9) and the job
// (job.c11, run by AuroraOS's ARM11 core with AUDIO_CMD_RUN11) share. The app
// copies the job's code to GB11_CODE and posts one job a frame, with this
// block as its argument; the emulator's state, the ROM, the cartridge RAM,
// the APU's state and the frame are the app's, in FCRAM both CPUs reach.
#ifndef GAMEBOY_GB11_H
#define GAMEBOY_GB11_H

#include <stdint.h>

// The SDK's sound pool, which this app leaves alone (it plays no WAV files).
// Not the image scratch at 0x25100000 that cpu-stress uses: a border picture
// loads there.
#define GB11_CODE     0x23400000u
#define GB11_CODE_MAX 0x40000u

struct gb_s;

typedef struct {
  // Set by the app.
  uint32_t probe; // only report the sizes below, run nothing
  struct gb_s *gb;
  const uint8_t *rom;
  uint32_t rom_size;
  uint8_t *cart_ram;
  uint32_t cart_ram_size;
  void *apu;    // sound.c's struct minigb_apu_ctx
  uint8_t *lcd; // GB_W columns of GB_H palette entries, as main.c keeps it
  uint32_t draw; // draw this frame's lines (0: Frame skip: Auto skipped it)

  // Set by the job.
  uint32_t ran;
  uint32_t frame_ready; // the last line was drawn
  uint32_t ram_written; // the game changed its cartridge RAM
  uint32_t error;       // 1 + Peanut-GB's gb_error_e, 0 if none
  uint32_t error_addr;
  uint32_t gb_size, apu_size; // as the job was built, for the app to check
} Gb11Job;

#endif
