# Game Boy

A Game Boy (DMG) emulator for AuroraOS, written as a C SDK app. The top
screen shows the game; the bottom screen lists the games and, while one runs,
holds its menu. The emulation is [Peanut-GB](https://github.com/deltabeard/Peanut-GB)
(`peanut_gb.h`, v1.3.0, MIT) and its sound is minigb_apu (`minigb_apu.h`
and `minigb_apu.inc`, from Peanut-GB's SDL example, MIT, see
`LICENSE.minigb_apu`); `main.c` and `sound.c` are the AuroraOS front end.

**Status:** on a console, without sound, it runs at full speed
(2026-10-22). With sound it played but ran at 80-100% while the picture moved
(GB 13-16 ms, APU 1.3, draw 6.5 a frame; 2026-10-23), so the sound broke up.
The word-at-a-time writes and Frame skip: Auto that followed have not been
tried on a console yet. Checked on a PC against a stand-in for the SDK:
Peanut-GB's test ROMs (dmg-acid2 draws correctly at every size, Blargg's
cpu_instrs passes), a cartridge-RAM test that keeps its save across runs, and
a 440 Hz tone played through a simulated voice with no gaps at 90% speed.

## Building

```
py sdk/aurcc.py build gameboy -o GameBoy.bin
```

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

The SDK plays WAV files only, so `sound.c` posts the ARM11 core's voice
commands itself, through the SDK runtime's `app_core_post()` and
`os_dcache_clean_range()` and the values from `include/audio.h`. None of these
is in `aurora_app.h`; if the SDK or the OS changes them, `sound.c` must follow.

Sound costs ARM9 time. If the speed in the menu drops below 100%, Sound: Off
gives it back.

## Timings

A tap on the menu's header (the game's name) shows, in place of the status
line, where the time goes, in milliseconds, updated every two seconds: `GB`
and `APU` per Game Boy frame, `draw` (the top screen and the menu) and `rest`
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
