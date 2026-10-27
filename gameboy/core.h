// Commands to AuroraOS's ARM11 core (the APU's voice, the ARM11 job), posted
// so that none is lost.
#ifndef GAMEBOY_CORE_H
#define GAMEBOY_CORE_H

#include <stdbool.h>
#include <stdint.h>

// Waits for the core to be idle, posts the command and waits for it to be
// done. False if the core did not answer within a second, either time.
bool core_post(uint32_t cmd, uint32_t a0, uint32_t a1, uint32_t a2,
               uint32_t a3);

#endif
