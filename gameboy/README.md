# Game Boy

A Game Boy (DMG) emulator for AuroraOS, written as a C SDK app. The top
screen shows the game; the bottom screen lists the games and, while one runs,
holds its menu. The emulation is [Peanut-GB](https://github.com/deltabeard/Peanut-GB)
(`peanut_gb.h`, v1.3.0, MIT) and its sound is minigb_apu (`minigb_apu.h`
and `minigb_apu.inc`, from Peanut-GB's SDL example, MIT, see
`LICENSE.minigb_apu`); `main.c`, `sound.c`, `core.c` and `arm11/` are the
AuroraOS front end.

**Status:** on a New 2DS XL (OS Beta 6, core 116; 2026-10-27) it runs at
99-100% with sound, emulating on the ARM11 at 804 MHz (see *The ARM11*), and
the buttons answer at once; the sound still drops in pitch now and then. On
the ARM9 alone it ran at 80-100% with sound (GB 13-16 ms, APU 1.3, draw 6.5
a frame; 2026-10-23). Checked on a PC against a stand-in for the SDK:
Peanut-GB's test ROMs (dmg-acid2 draws correctly at every size, Blargg's
cpu_instrs passes), a cartridge-RAM test that keeps its save across runs, and
a 440 Hz tone played through a simulated voice with no gaps at 90% speed.

## Building

```
py gameboy/build.py -o GameBoy.bin
```

`build.py` builds the ARM11 job first (see *The ARM11*), then the app with
`sdk/aurcc.py`. `py sdk/aurcc.py build gameboy -o GameBoy.bin` still builds
the app, without the job: it then emulates on the ARM9 only.

Copy `GameBoy.bin` to `SD:/Aurora/Apps/C/`, and games (`.gb` files) to
`SD:/Aurora/Apps/C/GameBoy/` or `SD:/roms/gb/`.

## Controls

| | |
|--|--|
| D-pad or circle pad | the Game Boy's D-pad |
| A, B, START, SELECT | the same buttons |
| R (held) | fast forward, four frames a frame |
| Touch screen | the menu |
| HOME | saves and goes back to the Home Menu |

In the list of games: Up/Down (or a drag) to choose, A (or a tap) to play,
L/R a page at a time, Y to look for games again, B back to the game.

The menu: Pause, Reset, Save state, Load state, Size (1x, 1.5x, Fit, Full),
Colors (Green, Pocket, Gray, Aurora, Color), Border (Plain, Frame, Image),
Frame skip (Off, Auto, On), Sound (On, Off), Screen (Auto off, On), and
Games. Reset and Load state take a second tap within three seconds.

Frame skip: Auto (the default) draws every frame while the emulation keeps
up. When it falls behind, a pass runs two or three frames to catch up and
draws only the last: the others cost the CPU and the APU but not Peanut-GB's
line drawing or the top screen, so the game and its sound stay at full speed
and only the picture drops towards 30 frames a second. On draws every other
frame always (Peanut-GB's own frame skip), Off never skips.

With Screen: Auto off (the default) the menu is dark while a game runs: a tap
anywhere on the bottom screen shows it (that tap only wakes it, it presses
nothing), and it goes dark again after ten seconds without a touch, never
while paused. Nothing is drawn on the bottom screen meanwhile, which spares
the ARM9 a little. The backlight stays on, since the SDK cannot turn off one
screen's, so it saves little power.

## Borders

Round a game smaller than the screen (every size but Full; Fit leaves only
the sides) the top screen shows a border:

* **Plain**: a dark colour.
* **Frame** (the default): a backdrop tinted with the palette's darkest shade
  and, at 1x and 1.5x, a bezel with a power light.
* **Image**: a picture of your own, over the frame, so its clear pixels show
  the frame. `borders/<game>.png` is used for that game, otherwise
  `border.png`; `.jpg` and `.bmp` work too. Make it 400 x 240 (other sizes
  are stretched to fit). The game covers its middle: 160 x 144 at (120, 48)
  for 1x, 240 x 216 at (80, 12) for 1.5x.

## Sound

The APU makes 275 samples a frame (16426 Hz, mono; half the usual rate, for
the ARM9's sake), written into a ring buffer of a quarter of a second that
CSND voice 0 loops over at 16384.25 Hz. The ARM9 writes 62 ms ahead of where
the voice is (worked out from the time since it started) and stretches or
shrinks a frame by up to 12% to stay there, so a game running at 90% plays a
whole, lower sound rather than breaking up; below about 88% it breaks up
anyway. After a stall it starts again from silence, and in fast forward it
drops what does not fit. It plays only while a game runs on screen: pausing,
the list of games and HOME stop it.

Closing the lid (OS Beta 7, SDK 1.3) darkens the screens and holds the app at
its next button read, but the voice would play on, looping what is in the
ring. So the app reads the lid sensor itself (GPIO1 bit 2, as the OS's
`include/lid.h` does) before each wait and stops the voice; it starts again,
from silence, when the lid opens. The voice's position is worked out from the
wall clock (`gettimeofday()`), not `sys_micros()`, which SDK 1.3 stops while
the lid is closed.

The SDK plays WAV files only, so `sound.c` posts the ARM11 core's voice
commands itself, through `core_post()` (`core.c`, over the SDK runtime's
`app_core_post()`), `os_dcache_clean_range()` and the values from
`include/audio.h`. None of these
is in `aurora_app.h`; if the SDK or the OS changes them, `sound.c` must follow.

Sound costs ARM9 time. If the speed in the menu drops below 100%, Sound: Off
gives it back.

## The ARM11

On the ARM9 the emulation took 13-16 ms of a 16.7 ms frame (New 2DS XL,
2026-10-23). So each Game Boy frame is handed to the ARM11 as a job
(`AUDIO_CMD_RUN11`, AuroraOS core 116 on, as `cpu-stress/` does): Peanut-GB's
CPU and drawing and the APU's register writes run there, with the ARM11's
caches on (16 KB each, which take writes) and, on a New 3DS, at 804 MHz
(back to what it was on leaving). The ARM9 waits for the job, then makes the
frame's sound and draws the top screen.

* `arm11/job.c11` is the job: Peanut-GB and minigb_apu built for the ARM11
  (`-march=armv6k`), with no C library and no `.bss`, linked at `0x23400000`
  (the SDK's sound pool, which this app does not use) by `arm11/job11.ld`.
  It is `.c11` so that aurcc, which builds every `.c` in a folder for the
  ARM9, leaves it alone. `arm11/gb11.h` is the block the two CPUs share.
* The emulator's state, the ROM, the cartridge RAM, the APU's state and the
  frame stay in the app's memory. Around each job the ARM9 cleans and
  invalidates its data cache, and sets its own callbacks back afterwards.
* At start a first job only reports how the job was built; if its
  `struct gb_s` or APU state differ in size from the app's, or the console
  cannot run jobs (an old core, or the app not started from the Home Menu),
  the emulation stays on the ARM9. A line `arm11=0` in `settings.txt` keeps it
  there too, to compare.
* While a job runs the core does nothing else (no presents, touch or new
  sound); a frame's job takes a few milliseconds. An exception in the job
  hangs the core.
* Every command to the core (jobs, the voice) goes through `core.c`, which
  waits for the core to be idle first. The core answers a command by copying
  `cmd_seq` into `ack_seq` once it is done, and the SDK's presents do not wait
  for that: a command posted just after a present can be answered unrun. The
  first try on a New 2DS XL (Beta 6, core 116, 2026-10-26) fell back to the
  ARM9 with "did not answer" on its first frame; with `core.c` it runs on the
  ARM11 (2026-10-27).

This uses `AUDIO_CMD_RUN11`, `AUDIO_CMD_N3DS` and `include/audio.h`, the OS's
internals, besides the SDK runtime's `app_core_post()`.

On a New 2DS XL at 804 MHz (2026-10-27) the timings read GB 4.7-12.9 ms,
APU 0.9, draw 1.2-1.9, at 99-100%; GB there includes waiting for the core to
finish the last present. Checked on a PC too: the job's code, built for the
PC, draws dmg-acid2 exactly as the ARM9 build does, reports cartridge RAM
writes, and comes back with the error and address of a bad instruction.

## Timings

A tap on the menu's header (the game's name) shows, in place of the status
line, where the time goes, in milliseconds, updated every two seconds: which
CPU emulates (`ARM9`, or `ARM11` and its clock), then `GB` (on the ARM11, the
whole job, waiting included) and `APU` per Game Boy frame, `draw` (the top screen and the menu) and `rest`
(presenting and waiting for the display) per pass. A frame has 16.7 ms; when
the first three add up to more, the game slows down.

## Files

Everything is in the app's data folder, `SD:/Aurora/Apps/C/GameBoy/`:

| File | |
|--|--|
| `saves/<game>.sav` | the cartridge's RAM (the game's own save), written two seconds after the game last changed it and on leaving |
| `saves/<game>.state` | the save state: the emulator's context and the cartridge RAM, for this build of the app |
| `border.png`, `borders/<game>.png` | border pictures (see *Borders*) |
| `settings.txt` | size, colours, border, frame skip, sound and screen |

## Limits

* Sound is mono, and a save state does not keep the APU's state (sound
  catches up with the next notes the game plays).
* Game Boy games only. Peanut-GB has no Game Boy Color mode, so `.gbc` games
  made for both run in black and white, and Game Boy Color-only games do not
  run.
* Cartridges: no MBC, MBC1, MBC2, MBC3 (with its clock) and MBC5. MBC7, the
  Pocket Camera, HuC1/HuC3 and TAMA5 are not supported.
* A save state from another build of the app may be refused (the context's
  size is checked).
* Emulation runs on the ARM9 (FCRAM, behind a 4 KB data cache that takes in
  no writes that miss it, which is why the frame is written a word at a time).
  If it cannot keep up even drawing one frame in three (Frame skip: Auto),
  the game slows down.
