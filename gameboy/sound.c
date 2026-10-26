// Game Boy sound: minigb_apu (minigb_apu.inc, MIT) makes each frame's samples,
// and a CSND voice on the ARM11 core plays them from a ring buffer it loops
// over, while the ARM9 keeps writing ahead of it.
//
// The SDK plays whole WAV files only, so this posts the core's voice commands
// itself, as the SDK's runtime does for snd_music(): through app_core_post()
// and os_dcache_clean_range(), on voice 0 (the music voice, which this app
// does not otherwise use). Those two functions and the command values below
// (from include/audio.h) are the SDK's and the OS's internals, not part of
// aurora_app.h: if they change, this file has to follow.
#include "sound.h"

#include <aurora_app.h>
#include <string.h>

// 275 samples a Game Boy frame, a little more than the voice plays (see
// OUT_PER_US_Q32); the surplus is dropped as it builds up. The pitch is off by
// 0.25%. Half the usual 32 kHz, to halve what the APU costs the ARM9.
#define AUDIO_SAMPLE_RATE 16426
#define MINIGB_APU_AUDIO_FORMAT_S16SYS
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "minigb_apu.inc"
#pragma GCC diagnostic pop

void app_core_post(uint32_t cmd, uint32_t a0, uint32_t a1, uint32_t a2,
                   uint32_t a3);
void os_dcache_clean_range(const void *addr, uint32_t len);

#define CMD_VOICE      9u          // AUDIO_CMD_VOICE
#define CMD_VOICE_STOP 10u         // AUDIO_CMD_VOICE_STOP
#define VOICE_LOOP     (1u << 8)   // AUDIO_VOICE_LOOP
#define VOICE_PCM16    (1u << 9)   // AUDIO_VOICE_PCM16
#define VOICE_VOL(v)   ((uint32_t)(v) << 16)
#define VOICE          0u

// The rate asked of the core, which divides 0x3FEC3FC by it (4091), and the
// rate that gives, 16384.25 Hz, in samples a microsecond as 32.32 fixed point.
#define OUT_RATE       16384u
#define OUT_PER_US_Q32 70369815ull

#define RING  4096u // samples (a power of two): a quarter of a second
#define LEAD  1024  // how far ahead of the voice to write: 62 ms
#define GUARD 512u  // silence kept after the newest samples
// AUDIO_SAMPLES, which minigb_apu works out with doubles: that is folded at
// compile time but is no constant for an array size.
#define FRAME 275u
// Samples a frame may be stretched or shrunk by, 12%: when the emulation runs
// slow, the sound plays lower rather than breaking up.
#define MAX_ADJUST 34

// Not static: the PC test build reads it as the voice would.
int16_t sound_ring[RING] __attribute__((aligned(32)));

static struct minigb_apu_ctx apu;
static bool playing;
static uint64_t start_us;
static uint32_t wpos; // samples written since the start; ring index & (RING-1)
// The lead, smoothed over about sixteen frames, in sixteenths of a sample.
// The stretch follows it rather than each frame's lead, which jumps about
// (a pass that catches up writes two or three frames at once), so the pitch
// moves slowly instead of wobbling.
static int32_t lead_q4;

uint8_t sound_read(uint16_t addr) { return minigb_apu_audio_read(&apu, addr); }

void sound_write(uint16_t addr, uint8_t val) {
  minigb_apu_audio_write(&apu, addr, val);
}

void sound_reset(void) { minigb_apu_audio_init(&apu); }

bool sound_playing(void) { return playing; }

// Where the voice is, in samples since it started.
static uint32_t play_pos(void) {
  return (uint32_t)(((sys_micros() - start_us) * OUT_PER_US_Q32) >> 32);
}

// The ring from sample `from` on, `n` samples, as two runs where it wraps.
static void ring_runs(uint32_t from, uint32_t n, uint32_t *at, uint32_t *first) {
  *at = from & (RING - 1u);
  *first = n < RING - *at ? n : RING - *at;
}

static void ring_clean(uint32_t from, uint32_t n) {
  uint32_t at, first;
  ring_runs(from, n, &at, &first);
  os_dcache_clean_range(&sound_ring[at], first * 2u);
  if (n > first)
    os_dcache_clean_range(sound_ring, (n - first) * 2u);
}

static void ring_zero(uint32_t from, uint32_t n) {
  uint32_t at, first;
  ring_runs(from, n, &at, &first);
  memset(&sound_ring[at], 0, first * 2u);
  if (n > first)
    memset(sound_ring, 0, (n - first) * 2u);
}

static void ring_put(uint32_t from, const int16_t *s, uint32_t n) {
  uint32_t at, first;
  ring_runs(from, n, &at, &first);
  memcpy(&sound_ring[at], s, first * 2u);
  if (n > first)
    memcpy(sound_ring, s + first, (n - first) * 2u);
}

bool sound_start(void) {
  if (playing)
    return true;
  if (!snd_available())
    return false;
  memset(sound_ring, 0, sizeof(sound_ring));
  os_dcache_clean_range(sound_ring, sizeof(sound_ring));
  wpos = LEAD; // what comes before is the silence just cleared
  lead_q4 = LEAD << 4;
  app_core_post(CMD_VOICE,
                VOICE | VOICE_LOOP | VOICE_PCM16 | VOICE_VOL(0x8000),
                (uint32_t)(uintptr_t)sound_ring, sizeof(sound_ring), OUT_RATE);
  start_us = sys_micros(); // the core has taken it: the voice is running
  playing = true;
  return true;
}

void sound_stop(void) {
  if (!playing)
    return;
  app_core_post(CMD_VOICE_STOP, 1u << VOICE, 0, 0, 0);
  playing = false;
}

// The APU runs every frame, so its envelopes and lengths keep time, but its
// samples go out only while the stream plays. They are written LEAD samples
// ahead of the voice: a frame is stretched or shrunk to stay there (by up to
// 12%, so a game running at 90% keeps a whole, lower sound), one that fell
// behind anyway (after a stall) starts again from silence, and one far ahead
// (fast forward) is dropped.
void sound_frame(void) {
  static audio_sample_t stereo[2 * FRAME];
  static int16_t mono[FRAME], out[FRAME + MAX_ADJUST];
  uint32_t play, n, step, acc = 0;
  int32_t lead, adjust;

  minigb_apu_audio_callback(&apu, stereo);
  if (!playing)
    return;
  for (uint32_t i = 0; i < FRAME; i++)
    mono[i] = (int16_t)((stereo[2 * i] + stereo[2 * i + 1]) >> 1);

  play = play_pos();
  lead = (int32_t)(wpos - play);
  if (lead < 0) { // the voice caught up: start again ahead of it
    ring_zero(play, LEAD);
    ring_clean(play, LEAD);
    wpos = play + LEAD;
    lead = LEAD;
    lead_q4 = LEAD << 4;
  }
  if (lead > 2 * LEAD)
    return;

  lead_q4 += lead - (lead_q4 >> 4);
  adjust = ((lead_q4 >> 4) - LEAD) / 32;
  if (adjust > MAX_ADJUST)
    adjust = MAX_ADJUST;
  if (adjust < -MAX_ADJUST)
    adjust = -MAX_ADJUST;
  n = (uint32_t)((int32_t)FRAME - adjust);
  step = (FRAME << 16) / n; // nearest sample: drop or repeat a few
  for (uint32_t i = 0; i < n; i++, acc += step)
    out[i] = mono[acc >> 16];

  ring_put(wpos, out, n);
  ring_zero(wpos + n, GUARD);
  ring_clean(wpos, n + GUARD);
  wpos += n;
}
