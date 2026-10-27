// Commands to AuroraOS's ARM11 core.
//
// The core answers a command by copying cmd_seq into ack_seq once it has
// done it (src/os/Core11.c). The SDK's presents post a command without
// waiting for that, and the SDK's app_core_post() waits only for the GPU's
// work to be done, which the core marks a moment before it answers. A command
// posted in that moment has its cmd_seq copied into ack_seq with the
// present's, so it is answered without ever being run. That is the likeliest
// reason the first frame's ARM11 job came back unrun on a New 2DS XL (Beta 6,
// core 116, 2026-10-26), while the job posted before any present ran. So the
// core must be idle, its ack_seq equal to cmd_seq, before anything is posted,
// as cpu-stress does; nothing else on the ARM9 posts in between.
//
// app_core_post() is the SDK runtime's and AudioCtrl the OS's
// (include/audio.h); neither is part of aurora_app.h.
#include "core.h"

#include <aurora_app.h>

#include "../include/audio.h"

void app_core_post(uint32_t cmd, uint32_t a0, uint32_t a1, uint32_t a2,
                   uint32_t a3);

#define CORE_WAIT_US 1000000u

#ifdef __arm__
static bool core_idle(uint32_t us) {
  volatile AudioCtrl *ct = (volatile AudioCtrl *)AUDIO_CTRL_ADDR;
  uint64_t t0 = sys_micros();
  for (;;) {
    // The core writes ack_seq: the ARM9's cached copy of the line goes.
    __asm__ volatile("mcr p15, 0, %0, c7, c6, 1" ::"r"(&ct->ack_seq)
                     : "memory");
    if (ct->ack_seq == ct->cmd_seq)
      return true;
    if (sys_micros() - t0 > us)
      return false;
  }
}
#else
// A PC test build: the stand-in core answers at once.
static bool core_idle(uint32_t us) {
  (void)us;
  return true;
}
#endif

bool core_post(uint32_t cmd, uint32_t a0, uint32_t a1, uint32_t a2,
               uint32_t a3) {
  if (!core_idle(CORE_WAIT_US))
    return false;
  app_core_post(cmd, a0, a1, a2, a3);
  return core_idle(CORE_WAIT_US);
}
