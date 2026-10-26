// Game Boy: a Game Boy (DMG) emulator for AuroraOS, built on Peanut-GB
// (peanut_gb.h, MIT, https://github.com/deltabeard/Peanut-GB), with sound
// from minigb_apu (sound.c).
//
// The top screen shows the game; the bottom screen is the app: the list of
// games, then, while one runs, its menu (pause, reset, save states, size,
// colours, border, frame skip, sound). Games are .gb (and dual-mode .gbc) files in
// the app's data folder, SD:/Aurora/Apps/C/GameBoy, or in SD:/roms/gb.
//
// A, B, START, SELECT and the D-pad (or the circle pad) are the Game Boy's;
// R held runs four times as fast. HOME saves and goes back to the Home Menu.
// A game's own save (its cartridge RAM) is written to saves/<game>.sav two
// seconds after the game last changed it, and on leaving.
#include <aurora_app.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "sound.h"

// Peanut-GB passes the APU's registers to these.
uint8_t audio_read(uint16_t addr);
void audio_write(uint16_t addr, uint8_t val);

#define ENABLE_SOUND 1
#define ENABLE_LCD 1
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#include "peanut_gb.h"
#pragma GCC diagnostic pop

uint8_t audio_read(uint16_t addr) { return sound_read(addr); }
void audio_write(uint16_t addr, uint8_t val) { sound_write(addr, val); }

#define GB_W LCD_WIDTH  // 160
#define GB_H LCD_HEIGHT // 144
#define FRAME_US 16743  // a Game Boy frame: 1 s / 59.7275
#define MAX_CATCHUP 3   // frames run in one pass to catch up, at most
#define FAST_FRAMES 4   // frames a pass with R held
#define SAVE_DELAY_MS 2000
#define CONFIRM_MS 3000
#define TOAST_MS 2500

#define BG_TOP RGB(16, 16, 20)
#define BG_BOTTOM RGB(24, 24, 30)
#define PANEL RGB(40, 40, 48)
#define PANEL_DOWN RGB(64, 64, 76)
#define TEXT_DIM RGB(150, 150, 160)

// --- The emulator ----------------------------------------------------------

static struct gb_s gb;
static uint8_t *rom;
static size_t rom_size;
static uint8_t *cart_ram;
static size_t cart_ram_size;
static bool ram_dirty;
static uint32_t ram_changed_ms;

static bool loaded, paused, crashed;
static char game_name[FS_NAME_MAX]; // the file name without its extension
static char game_path[FS_NAME_MAX + 64];

// The last frame drawn, as palette entries (0..11: layer * 4 + shade), stored
// by column so the blit to the top screen, which is stored by column too,
// reads it in order.
// As bytes, lcd[x] is column x from the top; it is written a word (four
// rows) at a time.
static uint32_t lcd[GB_W][GB_H / 4];
static bool frame_ready;

// Lines come one at a time but the frame is kept by column, so they are
// gathered four at a time and written a word per column: the ARM9's data
// cache does not take in writes that miss it, so each byte written straight
// to lcd would be a trip of its own to FCRAM.
static uint8_t grp[4][GB_W];
static int grp_at = -1;   // the group being gathered (line / 4), -1 none
static uint32_t grp_rows; // its rows gathered so far, a bit each

static void grp_flush(void) {
  uint32_t mask = 0;
  if (grp_at < 0)
    return;
  for (int k = 0; k < 4; k++)
    if (grp_rows & (1u << k))
      mask |= 0xFFu << (8 * k);
  for (int x = 0; x < GB_W; x++) {
    uint32_t w = (uint32_t)grp[0][x] | (uint32_t)grp[1][x] << 8 |
                 (uint32_t)grp[2][x] << 16 | (uint32_t)grp[3][x] << 24;
    uint32_t *d = &lcd[x][grp_at];
    // Rows the game did not draw (the LCD turned off mid-frame) keep theirs.
    *d = mask == 0xFFFFFFFFu ? w : (*d & ~mask) | (w & mask);
  }
  grp_at = -1;
  grp_rows = 0;
}

// Leaving gb_run_frame() on an error. newlib's setjmp() brings in libgcc's
// unwinder, which needs the .ARM.exidx section app.ld discards, so the ARM
// build uses GCC's own; a PC test build (clang has no __builtin_longjmp for
// every target) uses the library's.
#ifdef __arm__
static void *emu_jmp[5];
#define EMU_SETJMP() __builtin_setjmp(emu_jmp)
#define EMU_LONGJMP() __builtin_longjmp(emu_jmp, 1)
#else
#include <setjmp.h>
static jmp_buf emu_jmp;
#define EMU_SETJMP() setjmp(emu_jmp)
#define EMU_LONGJMP() longjmp(emu_jmp, 1)
#endif
static enum gb_error_e emu_err;
static uint16_t emu_err_addr;
static char crash_msg[80];

// Timing: emulated frames owed (in microseconds) and the speed shown.
static uint64_t last_us;
static uint32_t owed_us;
static int frames_this_second, speed_pct = 100;
static uint32_t second_start_ms;
#define SPEED_MS 2000 // how often the speed (and the timings) are shown

// The performance line (a tap on the menu's title shows it): microseconds
// spent since the last update emulating, in the APU, drawing the top screen,
// and outside the app's own work (presenting, waiting for the display).
static bool perf_on;
static uint32_t perf_gb_us, perf_apu_us, perf_draw_us, perf_wait_us;
static uint32_t perf_passes;
static char perf_line[80];

static uint8_t rom_read(struct gb_s *g, const uint_fast32_t addr) {
  (void)g;
  return addr < rom_size ? rom[addr] : 0xFF;
}

static uint8_t ram_read(struct gb_s *g, const uint_fast32_t addr) {
  (void)g;
  return addr < cart_ram_size ? cart_ram[addr] : 0xFF;
}

static void ram_write(struct gb_s *g, const uint_fast32_t addr,
                      const uint8_t val) {
  (void)g;
  if (addr < cart_ram_size && cart_ram[addr] != val) {
    cart_ram[addr] = val;
    ram_dirty = true;
    ram_changed_ms = sys_millis();
  }
}

// Peanut-GB must not get control back after an error, so this leaves
// gb_run_frame() for the EMU_SETJMP() in emulate().
static void emu_error(struct gb_s *g, const enum gb_error_e err,
                      const uint16_t addr) {
  (void)g;
  emu_err = err;
  emu_err_addr = addr;
  EMU_LONGJMP();
}

static void draw_line(struct gb_s *g, const uint8_t *px,
                      const uint_fast8_t line) {
  (void)g;
  if (line >= GB_H)
    return;
  if ((int)(line >> 2) != grp_at) {
    grp_flush();
    grp_at = (int)(line >> 2);
  }
  // Bits 1-0 are the shade, bits 5-4 the layer (OBJ0, OBJ1, BG).
  uint8_t *row = grp[line & 3];
  for (int x = 0; x < GB_W; x++)
    row[x] = (uint8_t)(((px[x] & 0x30) >> 2) | (px[x] & 3));
  grp_rows |= 1u << (line & 3);
  if (line == GB_H - 1) {
    grp_flush();
    frame_ready = true;
  }
}

// --- Settings ----------------------------------------------------------------

typedef struct {
  const char *name;
  int w, h;
} Size;

static const Size sizes[] = {
    {"1x", 160, 144},
    {"1.5x", 240, 216},
    {"Fit", 266, 240},
    {"Full", 400, 240},
};
#define SIZES ((int)(sizeof(sizes) / sizeof(sizes[0])))

// Each palette is BG, OBJ0 and OBJ1, lightest shade first.
typedef struct {
  const char *name;
  uint32_t c[3][4];
} Palette;

#define SAME3(a, b, c, d) {{a, b, c, d}, {a, b, c, d}, {a, b, c, d}}
static const Palette palettes[] = {
    {"Green", SAME3(0x9BBC0F, 0x8BAC0F, 0x306230, 0x0F380F)},
    {"Pocket", SAME3(0xC4CFA1, 0x8B956D, 0x4D533C, 0x1F1F1F)},
    {"Gray", SAME3(0xFFFFFF, 0xAAAAAA, 0x555555, 0x000000)},
    {"Aurora", SAME3(0xE8FFF8, 0x64E8C8, 0x2C7A6E, 0x0A1F1C)},
    // As a Game Boy Color shows a game it has no colours for.
    {"Color",
     {{0xFFFFFF, 0x7BFF31, 0x0063C5, 0x000000},
      {0xFFFFFF, 0xFF8484, 0x943A3A, 0x000000},
      {0xFFFFFF, 0xFF8484, 0x943A3A, 0x000000}}},
};
#define PALETTES ((int)(sizeof(palettes) / sizeof(palettes[0])))

// The border round the game: plain, a frame drawn in the palette's colours,
// or a picture from the card.
enum { BORDER_PLAIN, BORDER_FRAME, BORDER_IMAGE, BORDERS };
static const char *const border_names[BORDERS] = {"Plain", "Frame", "Image"};

static int size_mode = 2, palette = 0, border = BORDER_FRAME;
// Frame skip: never; when behind (a pass that runs several frames to catch
// up draws only the last); or every other frame (Peanut-GB's own).
enum { SKIP_OFF, SKIP_AUTO, SKIP_ON, SKIPS };
static const char *const skip_names[SKIPS] = {"Off", "Auto", "On"};
static int frame_skip = SKIP_AUTO;
static bool sound_on = true;
// The menu goes dark after a while without a touch, and a tap brings it back.
static bool screen_auto_off = true;

// Palette entry -> 0xRRGGBB, which as a little-endian word is a framebuffer
// pixel's B, G, R bytes.
static uint32_t pal32[12];
static int out_x, out_y;       // where the game sits on the top screen
static uint8_t xmap[SCREEN_TOP_WIDTH]; // output column -> source column
// Each output row of a column, in memory order (the bottom row first): its
// source row.
static uint8_t rowsrc[SCREEN_HEIGHT];
static bool top_clear = true; // the border needs drawing

static void settings_path(char *out, size_t max) {
  snprintf(out, max, "%s/settings.txt", app_dir());
}

static void load_settings(void) {
  char path[300], line[64];
  int v;
  FILE *f;
  settings_path(path, sizeof(path));
  f = fopen(path, "r");
  if (!f)
    return;
  while (fgets(line, sizeof(line), f)) {
    if (sscanf(line, "size=%d", &v) == 1 && v >= 0 && v < SIZES)
      size_mode = v;
    else if (sscanf(line, "palette=%d", &v) == 1 && v >= 0 && v < PALETTES)
      palette = v;
    else if (sscanf(line, "frameskip=%d", &v) == 1)
      frame_skip = v >= 0 && v < SKIPS ? v : SKIP_AUTO;
    else if (sscanf(line, "sound=%d", &v) == 1)
      sound_on = v != 0;
    else if (sscanf(line, "screenoff=%d", &v) == 1)
      screen_auto_off = v != 0;
    else if (sscanf(line, "border=%d", &v) == 1 && v >= 0 && v < BORDERS)
      border = v;
  }
  fclose(f);
}

static void save_settings(void) {
  char path[300];
  FILE *f;
  settings_path(path, sizeof(path));
  f = fopen(path, "w");
  if (!f)
    return;
  fprintf(f,
          "size=%d\npalette=%d\nframeskip=%d\nborder=%d\nsound=%d\n"
          "screenoff=%d\n",
          size_mode, palette, frame_skip, border, sound_on ? 1 : 0,
          screen_auto_off ? 1 : 0);
  fclose(f);
}

static void apply_palette(void) {
  for (int layer = 0; layer < 3; layer++)
    for (int shade = 0; shade < 4; shade++) {
      uint32_t c = palettes[palette].c[layer][shade];
      // Entries are layer * 4 + shade with OBJ0 = 0, OBJ1 = 1, BG = 2;
      // palettes list BG first.
      int e = (layer == 0 ? 2 : layer - 1) * 4 + shade;
      pal32[e] = c;
    }
}

static void apply_size(void) {
  const Size *s = &sizes[size_mode];
  out_x = (SCREEN_TOP_WIDTH - s->w) / 2;
  out_y = (SCREEN_HEIGHT - s->h) / 2;
  for (int x = 0; x < s->w; x++)
    xmap[x] = (uint8_t)(x * GB_W / s->w);
  for (int i = 0; i < s->h; i++)
    rowsrc[i] = (uint8_t)((s->h - 1 - i) * GB_H / s->h);
  top_clear = true;
}

// --- The top screen ----------------------------------------------------------

// `t`/256 of the way from a to b.
static uint32_t mix(uint32_t a, uint32_t b, int t) {
  uint32_t c = 0;
  for (int sh = 0; sh <= 16; sh += 8) {
    int x = (int)(a >> sh & 255), y = (int)(b >> sh & 255);
    c |= (uint32_t)(x + (y - x) * t / 256) << sh;
  }
  return c;
}

// A dark backdrop tinted with the palette's darkest shade and, where there is
// room round the game, a bezel with a power light.
static void draw_frame(void) {
  const Size *s = &sizes[size_mode];
  uint32_t dark = palettes[palette].c[0][3];
  int p = out_y < 14 ? out_y : 14; // the bezel, as wide as the room allows
  gfx_gradient(SCREEN_TOP, 0, 0, SCREEN_TOP_WIDTH, SCREEN_HEIGHT,
               mix(RGB(36, 38, 50), dark, 96), mix(RGB(8, 8, 12), dark, 64));
  if (p >= 8) {
    gfx_round_rect(SCREEN_TOP, out_x - p, out_y - p, s->w + 2 * p,
                   s->h + 2 * p, 10, mix(RGB(58, 60, 72), dark, 48));
    gfx_circle(SCREEN_TOP, out_x - (p + 2) / 2, out_y + 26, 2,
               RGB(232, 64, 64));
  }
  if (out_x >= 2) // the edge of the screen
    gfx_rect(SCREEN_TOP, out_x - 2, out_y - (p >= 8 ? 2 : 0), s->w + 4,
             s->h + (p >= 8 ? 4 : 0), RGB(14, 14, 18));
}

// The picture for BORDER_IMAGE: borders/<game>.png (or .jpg, .bmp) for this
// game, else border.png, looked up once per game. Stretched to the screen
// when it is not 400 x 240; its clear pixels show the frame.
static GfxImage *border_img;
static char border_img_game[FS_NAME_MAX];
static bool border_img_looked;

static GfxImage *find_border(void) {
  static const char *const exts[] = {"png", "jpg", "bmp"};
  char path[FS_NAME_MAX + 64];
  if (border_img_looked && !strcmp(border_img_game, game_name))
    return border_img;
  if (border_img)
    gfx_image_free(border_img);
  border_img = NULL;
  for (int i = 0; i < 6 && !border_img; i++) {
    if (i < 3 && !loaded)
      continue;
    if (i < 3)
      snprintf(path, sizeof(path), "%s/borders/%s.%s", app_dir(), game_name,
               exts[i]);
    else
      snprintf(path, sizeof(path), "%s/border.%s", app_dir(), exts[i - 3]);
    if (fs_exists(path))
      border_img = gfx_image_load(path);
  }
  snprintf(border_img_game, sizeof(border_img_game), "%s", game_name);
  border_img_looked = true;
  return border_img;
}

static void draw_border(void) {
  GfxImage *img;
  if (border == BORDER_PLAIN) {
    gfx_clear(SCREEN_TOP, BG_TOP);
    return;
  }
  draw_frame();
  if (border == BORDER_IMAGE && (img = find_border())) {
    if (img->w == SCREEN_TOP_WIDTH && img->h == SCREEN_HEIGHT)
      gfx_image(SCREEN_TOP, img, 0, 0);
    else
      gfx_image_scaled(SCREEN_TOP, img, 0, 0, SCREEN_TOP_WIDTH,
                       SCREEN_HEIGHT);
  }
}

// The frame, scaled to the chosen size by the nearest pixel. In the
// framebuffer pixel (x, y) is at ((x * 240) + (239 - y)) * 3, so a column is
// one run that goes up the screen. Every size's columns start on a word and
// hold a multiple of four pixels (out_y * 3 and h * 3 are multiples of 4), so
// four pixels go out as three words: a quarter of the writes of doing it a
// byte at a time, which mattered, as they all miss the cache.
static void blit(void) {
  const Size *s = &sizes[size_mode];
  const size_t col_bytes = SCREEN_HEIGHT * 3;
  uint8_t *fb = gfx_framebuffer(SCREEN_TOP);
  if (top_clear) {
    draw_border();
    top_clear = false;
  }
  for (int x = 0; x < s->w; x++) {
    uint32_t *d = (uint32_t *)(fb + (size_t)(out_x + x) * col_bytes +
                               (size_t)(SCREEN_HEIGHT - out_y - s->h) * 3);
    const uint8_t *col = (const uint8_t *)lcd[xmap[x]];
    for (int i = 0; i < s->h; i += 4) {
      uint32_t c0 = pal32[col[rowsrc[i]]], c1 = pal32[col[rowsrc[i + 1]]];
      uint32_t c2 = pal32[col[rowsrc[i + 2]]], c3 = pal32[col[rowsrc[i + 3]]];
      d[0] = c0 | c1 << 24;
      d[1] = c1 >> 8 | c2 << 16;
      d[2] = c2 >> 16 | c3 << 8;
      d += 3;
    }
  }
  gfx_mark(SCREEN_TOP);
}

static void center(Screen scr, int y, GfxFont font, uint32_t color,
                   const char *text) {
  int w = gfx_text_width(font, text);
  gfx_print(scr, (gfx_width(scr) - w) / 2, y, font, color, "%s", text);
}

static void draw_top_idle(void) {
  gfx_gradient(SCREEN_TOP, 0, 0, SCREEN_TOP_WIDTH, SCREEN_HEIGHT,
               RGB(26, 30, 44), RGB(8, 8, 12));
  // A small Game Boy.
  gfx_round_rect(SCREEN_TOP, 170, 40, 60, 96, 8, RGB(196, 196, 188));
  gfx_rect(SCREEN_TOP, 178, 48, 44, 38, RGB(80, 80, 96));
  gfx_rect(SCREEN_TOP, 182, 52, 36, 30, 0x9BBC0F);
  gfx_rect(SCREEN_TOP, 180, 100, 14, 4, RGB(60, 60, 70));
  gfx_rect(SCREEN_TOP, 185, 95, 4, 14, RGB(60, 60, 70));
  gfx_circle(SCREEN_TOP, 211, 104, 4, RGB(150, 40, 90));
  gfx_circle(SCREEN_TOP, 220, 98, 4, RGB(150, 40, 90));
  center(SCREEN_TOP, 150, FONT_TITLE, COLOR_WHITE, "Game Boy");
  center(SCREEN_TOP, 180, FONT_REGULAR, TEXT_DIM,
         "Pick a game on the touch screen");
  top_clear = true;
}

// --- Saves -------------------------------------------------------------------

static void save_path(char *out, size_t max, const char *ext) {
  snprintf(out, max, "%s/saves/%s.%s", app_dir(), game_name, ext);
}

static void save_ram(void) {
  char path[FS_NAME_MAX + 64], dir[300];
  if (!loaded || !cart_ram_size || !ram_dirty)
    return;
  snprintf(dir, sizeof(dir), "%s/saves", app_dir());
  save_path(path, sizeof(path), "sav");
  if (fs_mkdir(dir) && fs_write_file(path, cart_ram, cart_ram_size))
    ram_dirty = false;
}

static void load_ram(void) {
  char path[FS_NAME_MAX + 64];
  size_t n = 0;
  uint8_t *data;
  save_path(path, sizeof(path), "sav");
  data = fs_read_file(path, &n);
  if (!data)
    return;
  memcpy(cart_ram, data, n < cart_ram_size ? n : cart_ram_size);
  free(data);
}

// A save state is this header, the emulator's context and the cartridge RAM.
// The context holds pointers (the callbacks), which are set again on loading.
typedef struct {
  char magic[4]; // "AGBS"
  uint32_t version;
  uint32_t context_size;
  uint32_t ram_size;
  uint32_t rom_id;
} StateHeader;

#define STATE_VERSION 1

// Tells one game from another: the header (title, licensee, checksums) and
// the size.
static uint32_t rom_id(void) {
  uint32_t h = 2166136261u ^ (uint32_t)rom_size;
  for (uint32_t a = 0x134; a < 0x150; a++)
    h = (h ^ rom_read(&gb, a)) * 16777619u;
  return h;
}

static void set_callbacks(void) {
  gb.gb_rom_read = rom_read;
  gb.gb_cart_ram_read = ram_read;
  gb.gb_cart_ram_write = ram_write;
  gb.gb_error = emu_error;
  gb.gb_serial_tx = NULL;
  gb.gb_serial_rx = NULL;
  gb.gb_bootrom_read = NULL;
  gb.display.lcd_draw_line = draw_line;
  gb.direct.priv = NULL;
}

static bool save_state(void) {
  char path[FS_NAME_MAX + 64], dir[300];
  StateHeader h = {{'A', 'G', 'B', 'S'}, STATE_VERSION, sizeof(gb),
                   (uint32_t)cart_ram_size, rom_id()};
  FILE *f;
  bool ok;
  snprintf(dir, sizeof(dir), "%s/saves", app_dir());
  save_path(path, sizeof(path), "state");
  if (!fs_mkdir(dir) || !(f = fopen(path, "wb")))
    return false;
  ok = fwrite(&h, sizeof(h), 1, f) == 1 && fwrite(&gb, sizeof(gb), 1, f) == 1 &&
       (!cart_ram_size || fwrite(cart_ram, cart_ram_size, 1, f) == 1);
  return fclose(f) == 0 && ok;
}

static const char *load_state(void) {
  static struct gb_s s;
  char path[FS_NAME_MAX + 64];
  uint8_t *ram = NULL;
  StateHeader h;
  FILE *f;
  bool ok;
  save_path(path, sizeof(path), "state");
  f = fopen(path, "rb");
  if (!f)
    return "No save state yet";
  ok = fread(&h, sizeof(h), 1, f) == 1 && !memcmp(h.magic, "AGBS", 4) &&
       h.version == STATE_VERSION && h.context_size == sizeof(gb) &&
       h.ram_size == cart_ram_size && h.rom_id == rom_id() &&
       fread(&s, sizeof(s), 1, f) == 1;
  // Nothing changes until all of it has been read.
  if (ok && cart_ram_size)
    ok = (ram = malloc(cart_ram_size)) && fread(ram, cart_ram_size, 1, f) == 1;
  fclose(f);
  if (!ok) {
    free(ram);
    return "The save state does not fit this game";
  }
  gb = s;
  if (ram) {
    memcpy(cart_ram, ram, cart_ram_size);
    free(ram);
  }
  set_callbacks();
  ram_dirty = cart_ram_size != 0;
  ram_changed_ms = sys_millis();
  return NULL;
}

// --- The list of games -------------------------------------------------------

typedef struct {
  char name[FS_NAME_MAX]; // shown, without the extension
  char path[FS_NAME_MAX + 64];
  bool color;             // .gbc
} Game;

#define MAX_GAMES 512
static Game *games;
static int game_count;

static bool has_ext(const char *name, const char *ext) {
  size_t n = strlen(name), e = strlen(ext);
  return n > e && !strcasecmp(name + n - e, ext);
}

static void scan_dir(const char *dir) {
  FsDir *d = fs_dir_open(dir);
  FsEntry e;
  if (!d)
    return;
  while (game_count < MAX_GAMES && fs_dir_read(d, &e)) {
    Game *g = &games[game_count];
    bool gbc = has_ext(e.name, ".gbc");
    if (e.is_dir || (!gbc && !has_ext(e.name, ".gb")))
      continue;
    snprintf(g->path, sizeof(g->path), "%s/%s", dir, e.name);
    snprintf(g->name, sizeof(g->name), "%s", e.name);
    g->name[strlen(g->name) - (gbc ? 4 : 3)] = 0;
    g->color = gbc;
    game_count++;
  }
  fs_dir_close(d);
}

static int by_name(const void *a, const void *b) {
  return strcasecmp(((const Game *)a)->name, ((const Game *)b)->name);
}

static void scan_games(void) {
  if (!games)
    games = malloc(sizeof(Game) * MAX_GAMES);
  game_count = 0;
  if (!games)
    return;
  scan_dir(app_dir());
  scan_dir("/roms/gb");
  qsort(games, (size_t)game_count, sizeof(Game), by_name);
}

// --- The bottom screen -------------------------------------------------------

enum { VIEW_GAMES, VIEW_MENU };
static int view = VIEW_GAMES;
static bool ui_dirty = true;
static char toast[96];
static uint32_t toast_until;

static void say(const char *msg) {
  snprintf(toast, sizeof(toast), "%s", msg);
  toast_until = sys_millis() + TOAST_MS;
  ui_dirty = true;
}

// The list: rows under a header, scrolled by the pixel.
#define LIST_Y 44
#define ROW_H 30
#define LIST_ROWS 6
#define LIST_H (ROW_H * LIST_ROWS)
static int sel, scroll_px;
static bool dragging;
static int drag_y0, drag_scroll0;

static int max_scroll(void) {
  int m = game_count * ROW_H - LIST_H;
  return m > 0 ? m : 0;
}

static void clamp_scroll(void) {
  if (scroll_px > max_scroll())
    scroll_px = max_scroll();
  if (scroll_px < 0)
    scroll_px = 0;
}

static void show_sel(void) {
  if (sel * ROW_H < scroll_px)
    scroll_px = sel * ROW_H;
  if ((sel + 1) * ROW_H > scroll_px + LIST_H)
    scroll_px = (sel + 1) * ROW_H - LIST_H;
  clamp_scroll();
}

static void draw_games(void) {
  uint32_t accent = sys_accent();
  gfx_clear(SCREEN_BOTTOM, BG_BOTTOM);
  if (!game_count) {
    gfx_print(SCREEN_BOTTOM, 16, 60, FONT_REGULAR, COLOR_WHITE,
              "No games found.");
    gfx_print(SCREEN_BOTTOM, 16, 90, FONT_SMALL, TEXT_DIM,
              "Copy .gb files to\n%s\nor to /roms/gb on the SD card,\n"
              "then press Y.",
              app_dir());
  }
  for (int i = scroll_px / ROW_H; i < game_count; i++) {
    int y = LIST_Y + i * ROW_H - scroll_px;
    if (y >= LIST_Y + LIST_H)
      break;
    bool on = i == sel;
    // Rows are drawn whole; the header and footer cover what pokes out.
    gfx_round_rect(SCREEN_BOTTOM, 10, y + 2, 300, ROW_H - 4, 6,
                   on ? PANEL_DOWN : PANEL);
    if (on)
      gfx_rect(SCREEN_BOTTOM, 10, y + 6, 3, ROW_H - 12, accent);
    gfx_print(SCREEN_BOTTOM, 20, y + 8, FONT_REGULAR,
              on ? COLOR_WHITE : RGB(210, 210, 216), "%.40s", games[i].name);
    if (games[i].color)
      gfx_print(SCREEN_BOTTOM, 272, y + 10, FONT_SMALL, TEXT_DIM, "GBC");
    if (loaded && !strcmp(games[i].path, game_path))
      gfx_circle(SCREEN_BOTTOM, 262, y + ROW_H / 2, 3, accent);
  }
  gfx_rect(SCREEN_BOTTOM, 0, 0, SCREEN_BOTTOM_WIDTH, LIST_Y, BG_BOTTOM);
  gfx_print(SCREEN_BOTTOM, 16, 10, FONT_TITLE, COLOR_WHITE, "Games");
  if (game_count) {
    char n[24];
    snprintf(n, sizeof(n), "%d", game_count);
    gfx_print(SCREEN_BOTTOM, 304 - gfx_text_width(FONT_SMALL, n), 18,
              FONT_SMALL, TEXT_DIM, "%s", n);
  }
  if (game_count > LIST_ROWS) {
    int h = LIST_H * LIST_H / (game_count * ROW_H);
    int y = LIST_Y + (LIST_H - h) * scroll_px / max_scroll();
    gfx_round_rect(SCREEN_BOTTOM, 313, y, 4, h, 2, TEXT_DIM);
  }
  gfx_rect(SCREEN_BOTTOM, 0, LIST_Y + LIST_H, SCREEN_BOTTOM_WIDTH,
           SCREEN_HEIGHT - LIST_Y - LIST_H, BG_BOTTOM);
  gfx_print(SCREEN_BOTTOM, 16, 226, FONT_SMALL, TEXT_DIM,
            loaded ? "A: play   B: back to game   Y: refresh"
                   : "A: play    Y: refresh    HOME: quit");
}

// The menu while a game runs: two columns of buttons.
enum {
  BTN_PAUSE,
  BTN_RESET,
  BTN_SAVE,
  BTN_LOAD,
  BTN_SIZE,
  BTN_COLORS,
  BTN_BORDER,
  BTN_SKIP,
  BTN_SOUND,
  BTN_SCREEN,
  BTN_GAMES,
  BUTTONS
};
#define BTN_W 140
#define BTN_H 28
#define BTN_GAP 5
#define BTN_TOP 46
static int pressed = -1;        // the button under the stylus
static int armed = -1;          // a button tapped once, waiting for a second
static uint32_t armed_until;

static void btn_rect(int b, int *x, int *y, int *w) {
  *x = 16 + (b % 2) * (BTN_W + 8);
  *y = BTN_TOP + (b / 2) * (BTN_H + BTN_GAP);
  *w = b == BTN_GAMES ? 2 * BTN_W + 8 : BTN_W; // the last row, alone
}

static int btn_at(int tx, int ty) {
  for (int b = 0; b < BUTTONS; b++) {
    int x, y, w;
    btn_rect(b, &x, &y, &w);
    if (tx >= x && tx < x + w && ty >= y && ty < y + BTN_H)
      return b;
  }
  return -1;
}

static void btn_label(int b, char *out, size_t max) {
  switch (b) {
  case BTN_PAUSE:
    snprintf(out, max, "%s", paused ? "Resume" : "Pause");
    break;
  case BTN_RESET:
    snprintf(out, max, "%s", armed == b ? "Tap to reset" : "Reset");
    break;
  case BTN_SAVE:
    snprintf(out, max, "Save state");
    break;
  case BTN_LOAD:
    snprintf(out, max, "%s", armed == b ? "Tap to load" : "Load state");
    break;
  case BTN_SIZE:
    snprintf(out, max, "Size: %s", sizes[size_mode].name);
    break;
  case BTN_COLORS:
    snprintf(out, max, "Colors: %s", palettes[palette].name);
    break;
  case BTN_BORDER:
    snprintf(out, max, "Border: %s", border_names[border]);
    break;
  case BTN_SKIP:
    snprintf(out, max, "Frame skip: %s", skip_names[frame_skip]);
    break;
  case BTN_SOUND:
    snprintf(out, max, "Sound: %s", sound_on ? "On" : "Off");
    break;
  case BTN_SCREEN:
    snprintf(out, max, "Screen: %s", screen_auto_off ? "Auto off" : "On");
    break;
  default:
    snprintf(out, max, "Games");
    break;
  }
}

#define HEADER_H 44

// The game's name, the speed and the status line (or the timings).
static void draw_header(void) {
  uint32_t now = sys_millis();
  const char *status;
  char line[96];
  gfx_rect(SCREEN_BOTTOM, 0, 0, SCREEN_BOTTOM_WIDTH, HEADER_H, BG_BOTTOM);
  gfx_print(SCREEN_BOTTOM, 16, 8, FONT_BOLD, COLOR_WHITE, "%.30s",
            game_name);
  if (!paused) {
    snprintf(line, sizeof(line), "%d%%", speed_pct);
    gfx_print(SCREEN_BOTTOM, 304 - gfx_text_width(FONT_SMALL, line), 10,
              FONT_SMALL, speed_pct >= 95 ? TEXT_DIM : COLOR_ORANGE, "%s",
              line);
  }
  if (toast_until && now < toast_until)
    status = toast;
  else if (crashed)
    status = crash_msg;
  else if (paused)
    status = "Paused";
  else if (hid_keys_held() & KEY_R)
    status = "Fast forward";
  else
    status = "Playing";
  if (perf_on && !(toast_until && now < toast_until) && !crashed)
    status = perf_line[0] ? perf_line : "Timing...";
  gfx_print(SCREEN_BOTTOM, 16, 28, FONT_SMALL,
            crashed ? COLOR_ORANGE : TEXT_DIM, "%s", status);
}

static void draw_menu(void) {
  uint32_t accent = sys_accent();
  gfx_clear(SCREEN_BOTTOM, BG_BOTTOM);
  draw_header();
  for (int b = 0; b < BUTTONS; b++) {
    int x, y, w, tw;
    char label[40];
    bool off = crashed && (b == BTN_PAUSE || b == BTN_SAVE);
    btn_rect(b, &x, &y, &w);
    btn_label(b, label, sizeof(label));
    gfx_round_rect(SCREEN_BOTTOM, x, y, w, BTN_H, 8,
                   b == pressed ? PANEL_DOWN : PANEL);
    if (armed == b)
      gfx_rect_outline(SCREEN_BOTTOM, x, y, w, BTN_H, 2, accent);
    tw = gfx_text_width(FONT_REGULAR, label);
    gfx_print(SCREEN_BOTTOM, x + (w - tw) / 2, y + 7, FONT_REGULAR,
              off ? RGB(90, 90, 100) : COLOR_WHITE, "%s", label);
  }
}

// The menu, dark: nothing is drawn or presented on the bottom screen until a
// tap, which also spares the ARM9. The backlight stays on (the SDK cannot turn
// one screen's off), so it saves little power.
#define SCREEN_OFF_MS 10000
static bool menu_dark, dark_drawn;
static uint32_t last_touch_ms;

static void menu_wake(void) {
  menu_dark = false;
  last_touch_ms = sys_millis();
  ui_dirty = true;
}

// As a game starts or comes back from the list: dark at once when the menu
// turns itself off.
static void menu_enter(void) {
  menu_dark = screen_auto_off;
  dark_drawn = false;
  last_touch_ms = sys_millis();
  ui_dirty = true;
}

static void draw_bottom(void) {
  if (view == VIEW_GAMES) {
    draw_games();
  } else if (menu_dark) {
    if (!dark_drawn) {
      gfx_clear(SCREEN_BOTTOM, COLOR_BLACK);
      center(SCREEN_BOTTOM, 112, FONT_SMALL, RGB(48, 48, 56),
             "Tap to show the menu");
      dark_drawn = true;
    }
  } else {
    draw_menu();
    dark_drawn = false;
  }
  ui_dirty = false;
}

// --- Running a game ----------------------------------------------------------

static void close_game(void) {
  save_ram();
  free(rom);
  free(cart_ram);
  rom = cart_ram = NULL;
  rom_size = cart_ram_size = 0;
  loaded = false;
}

static void start_timing(void) {
  last_us = sys_micros();
  owed_us = 0;
  frames_this_second = 0;
  second_start_ms = sys_millis();
  speed_pct = 100;
  perf_gb_us = perf_apu_us = perf_draw_us = perf_wait_us = perf_passes = 0;
}

static bool open_game(const Game *g) {
  enum gb_init_error_e err;
  size_t n = 0;
  uint8_t *data;
  time_t now = time(NULL);

  if (loaded && !strcmp(g->path, game_path))
    return true; // already running
  data = fs_read_file(g->path, &n);
  if (!data || n < 0x150) {
    free(data);
    say("The file cannot be read");
    return false;
  }
  close_game();
  rom = data;
  rom_size = n;
  err = gb_init(&gb, rom_read, ram_read, ram_write, emu_error, NULL);
  if (err != GB_INIT_NO_ERROR) {
    say(err == GB_INIT_CARTRIDGE_UNSUPPORTED
            ? "This cartridge type is not supported"
            : "Not a Game Boy game (bad header checksum)");
    free(rom);
    rom = NULL;
    rom_size = 0;
    draw_top_idle();
    return false;
  }
  if (gb_get_save_size_s(&gb, &cart_ram_size) != 0)
    cart_ram_size = 0;
  if (cart_ram_size) {
    cart_ram = malloc(cart_ram_size);
    if (!cart_ram) {
      say("Out of memory");
      free(rom);
      rom = NULL;
      rom_size = cart_ram_size = 0;
      draw_top_idle();
      return false;
    }
    memset(cart_ram, 0xFF, cart_ram_size);
  }
  snprintf(game_name, sizeof(game_name), "%s", g->name);
  snprintf(game_path, sizeof(game_path), "%s", g->path);
  loaded = true;
  load_ram();
  ram_dirty = false;
  gb_init_lcd(&gb, draw_line);
  sound_reset();
  gb.direct.frame_skip = frame_skip == SKIP_ON;
  gb_set_rtc(&gb, localtime(&now));
  memset(lcd, 0, sizeof(lcd));
  grp_at = -1;
  grp_rows = 0;
  paused = crashed = false;
  armed = -1;
  top_clear = true;
  start_timing();
  // A game that needs a Game Boy Color says so at 0x143.
  if (rom[0x143] == 0xC0)
    say("Made for the Game Boy Color only: it may not run");
  return true;
}

static uint8_t joypad(uint32_t held) {
  uint8_t j = 0xFF; // a pressed button is a 0 bit
  if (held & KEY_A)
    j &= (uint8_t)~JOYPAD_A;
  if (held & KEY_B)
    j &= (uint8_t)~JOYPAD_B;
  if (held & KEY_SELECT)
    j &= (uint8_t)~JOYPAD_SELECT;
  if (held & KEY_START)
    j &= (uint8_t)~JOYPAD_START;
  if (held & (KEY_RIGHT | KEY_CPAD_RIGHT))
    j &= (uint8_t)~JOYPAD_RIGHT;
  if (held & (KEY_LEFT | KEY_CPAD_LEFT))
    j &= (uint8_t)~JOYPAD_LEFT;
  if (held & (KEY_UP | KEY_CPAD_UP))
    j &= (uint8_t)~JOYPAD_UP;
  if (held & (KEY_DOWN | KEY_CPAD_DOWN))
    j &= (uint8_t)~JOYPAD_DOWN;
  return j;
}

static void set_crash_msg(void) {
  snprintf(crash_msg, sizeof(crash_msg), "Stopped: %s at $%04X. Reset to go on.",
           emu_err == GB_INVALID_OPCODE  ? "bad instruction"
           : emu_err == GB_INVALID_READ  ? "bad read"
           : emu_err == GB_INVALID_WRITE ? "bad write"
                                         : "error",
           emu_err_addr);
}

// Runs as many Game Boy frames as the time since the last pass owes (the
// display runs at 59.8 Hz, the Game Boy at 59.7), up to MAX_CATCHUP when the
// ARM9 falls behind, and shows the newest.
static void emulate(uint32_t held) {
  uint64_t now = sys_micros();
  uint32_t elapsed = (uint32_t)(now - last_us);
  int n;

  last_us = now;
  if (elapsed > 100000u) // back from a pause or a long save
    elapsed = FRAME_US;
  owed_us += elapsed;
  n = (int)(owed_us / FRAME_US);
  owed_us -= (uint32_t)n * FRAME_US;
  if (n > MAX_CATCHUP) {
    n = MAX_CATCHUP;
    owed_us = 0;
  }
  if (held & KEY_R)
    n = FAST_FRAMES;

  gb.direct.joypad = joypad(held);
  if (EMU_SETJMP()) {
    crashed = paused = true;
    set_crash_msg();
    menu_wake(); // the error has to be seen
    ui_dirty = true;
    return;
  }
  for (int i = 0; i < n; i++) {
    uint64_t t0 = sys_micros(), t1;
    // Peanut-GB skips a line's drawing when there is no callback for it, so
    // a frame only caught up with costs the CPU and the APU alone.
    gb.display.lcd_draw_line =
        frame_skip == SKIP_AUTO && i < n - 1 ? NULL : draw_line;
    gb_run_frame(&gb);
    t1 = sys_micros();
    perf_gb_us += (uint32_t)(t1 - t0);
    if (sound_on) {
      sound_frame();
      perf_apu_us += (uint32_t)(sys_micros() - t1);
    }
    frames_this_second++;
  }
  if (frame_ready) {
    uint64_t t0 = sys_micros();
    frame_ready = false;
    blit();
    perf_draw_us += (uint32_t)(sys_micros() - t0);
  }
}

// Milliseconds with one decimal.
static void ms1(char *out, size_t max, uint32_t us) {
  snprintf(out, max, "%u.%u", (unsigned)(us / 1000u),
           (unsigned)(us / 100u % 10u));
}

static void draw_header(void);

static void update_speed(void) {
  uint32_t now = sys_millis(), span = now - second_start_ms;
  int frames = frames_this_second;
  char gb_ms[12], apu_ms[12], draw_ms[12], wait_ms[12];
  if (span < SPEED_MS)
    return;
  // Frames per second (in hundredths) against the Game Boy's 59.73. In 32
  // bits: libgcc's 64-bit division needs the unwinder, which app.ld drops.
  speed_pct = (int)((uint32_t)frames * 100000u / span * 100u / 5973u);
  // Per Game Boy frame for the emulation and the APU, per pass for the rest.
  ms1(gb_ms, sizeof(gb_ms), frames ? perf_gb_us / (uint32_t)frames : 0);
  ms1(apu_ms, sizeof(apu_ms), frames ? perf_apu_us / (uint32_t)frames : 0);
  ms1(draw_ms, sizeof(draw_ms), perf_passes ? perf_draw_us / perf_passes : 0);
  ms1(wait_ms, sizeof(wait_ms), perf_passes ? perf_wait_us / perf_passes : 0);
  snprintf(perf_line, sizeof(perf_line), "GB %s  APU %s  draw %s  rest %s",
           gb_ms, apu_ms, draw_ms, wait_ms);
  frames_this_second = 0;
  perf_gb_us = perf_apu_us = perf_draw_us = perf_wait_us = perf_passes = 0;
  second_start_ms = now;
  // Only the header: a whole bottom screen costs a frame's worth of time.
  if (view == VIEW_MENU && !ui_dirty && !menu_dark)
    draw_header();
}

// --- Input -------------------------------------------------------------------

// A held direction repeats after 300 ms, every 80 ms.
static uint32_t repeat_keys(uint32_t down, uint32_t held) {
  static uint32_t since, next;
  uint32_t now = sys_millis(), dirs = held & (KEY_DPAD | KEY_CPAD);
  if (down & (KEY_DPAD | KEY_CPAD)) {
    since = now;
    next = now + 300;
    return down;
  }
  if (dirs && now - since >= 300 && (int32_t)(now - next) >= 0) {
    next = now + 80;
    return down | dirs;
  }
  return down;
}

static void play_selected(void) {
  if (sel < game_count && open_game(&games[sel])) {
    view = VIEW_MENU;
    menu_enter();
    start_timing();
  }
  ui_dirty = true;
}

static void games_input(uint32_t down, uint32_t held) {
  uint32_t keys = repeat_keys(down, held);
  if (keys & (KEY_UP | KEY_CPAD_UP) && sel > 0)
    sel--, show_sel(), ui_dirty = true;
  if (keys & (KEY_DOWN | KEY_CPAD_DOWN) && sel < game_count - 1)
    sel++, show_sel(), ui_dirty = true;
  if (keys & KEY_L && game_count)
    sel = sel > LIST_ROWS ? sel - LIST_ROWS : 0, show_sel(), ui_dirty = true;
  if (keys & KEY_R && game_count) {
    sel = sel + LIST_ROWS < game_count ? sel + LIST_ROWS : game_count - 1;
    show_sel();
    ui_dirty = true;
  }
  if (down & KEY_A && game_count)
    play_selected();
  if (down & KEY_B && loaded) {
    view = VIEW_MENU;
    menu_enter();
    start_timing();
    ui_dirty = true;
  }
  if (down & KEY_Y) {
    scan_games();
    sel = 0;
    scroll_px = 0;
    ui_dirty = true;
  }

  // The stylus: a drag scrolls, a tap on a row plays it.
  if (hid_touch_down()) {
    drag_y0 = hid_touch_y();
    drag_scroll0 = scroll_px;
    dragging = false;
  } else if (hid_touch_held()) {
    int dy = hid_touch_y() - drag_y0;
    if (dy > 8 || dy < -8)
      dragging = true;
    if (dragging) {
      scroll_px = drag_scroll0 - dy;
      clamp_scroll();
      ui_dirty = true;
    }
  } else if (hid_touch_up() && !dragging) {
    int y = drag_y0 - LIST_Y;
    if (y >= 0 && y < LIST_H) {
      int row = (y + scroll_px) / ROW_H;
      if (row < game_count) {
        sel = row;
        play_selected();
      }
    }
  }
}

static void press(int b) {
  const char *err;
  uint32_t now = sys_millis();
  // Reset and Load state undo play, so they take a second tap.
  if ((b == BTN_RESET || b == BTN_LOAD) && armed != b) {
    armed = b;
    armed_until = now + CONFIRM_MS;
    return;
  }
  armed = -1;
  switch (b) {
  case BTN_PAUSE:
    if (!crashed) {
      paused = !paused;
      start_timing();
    }
    break;
  case BTN_RESET:
    gb_reset(&gb);
    sound_reset();
    paused = crashed = false;
    toast_until = 0;
    start_timing();
    say("Reset");
    break;
  case BTN_SAVE:
    if (!crashed)
      say(save_state() ? "State saved" : "The state could not be saved");
    break;
  case BTN_LOAD:
    err = load_state();
    if (!err) {
      crashed = false;
      start_timing();
    }
    say(err ? err : "State loaded");
    break;
  case BTN_SIZE:
    size_mode = (size_mode + 1) % SIZES;
    apply_size();
    blit();
    save_settings();
    break;
  case BTN_COLORS:
    palette = (palette + 1) % PALETTES;
    apply_palette();
    top_clear = true; // the frame takes the palette's colours
    blit();
    save_settings();
    break;
  case BTN_BORDER:
    border = (border + 1) % BORDERS;
    border_img_looked = false; // look on the card again
    if (border == BORDER_IMAGE && !find_border())
      say("No border.png in the GameBoy folder");
    top_clear = true;
    blit();
    save_settings();
    break;
  case BTN_SKIP:
    frame_skip = (frame_skip + 1) % SKIPS;
    gb.direct.frame_skip = frame_skip == SKIP_ON;
    save_settings();
    break;
  case BTN_SOUND:
    sound_on = !sound_on;
    save_settings();
    break;
  case BTN_SCREEN:
    screen_auto_off = !screen_auto_off;
    save_settings();
    break;
  default:
    save_ram();
    view = VIEW_GAMES;
    for (int i = 0; i < game_count; i++)
      if (!strcmp(games[i].path, game_path))
        sel = i;
    show_sel();
    break;
  }
}

static void menu_input(void) {
  static bool waking; // the touch that woke the menu, until it lifts
  int b;
  if (hid_touch_held())
    last_touch_ms = sys_millis();
  if (menu_dark) {
    if (hid_touch_down()) {
      menu_wake();
      waking = true;
    }
    return;
  }
  if (waking) {
    if (!hid_touch_held())
      waking = false;
    return;
  }
  if (armed >= 0 && (int32_t)(sys_millis() - armed_until) >= 0) {
    armed = -1;
    ui_dirty = true;
  }
  // A button acts when the stylus lifts on it; sliding off cancels. A tap on
  // the header shows or hides the timings.
  static bool header_tap;
  if (hid_touch_down()) {
    pressed = btn_at(hid_touch_x(), hid_touch_y());
    header_tap = hid_touch_y() < HEADER_H;
    ui_dirty = true;
  } else if (hid_touch_held()) {
    if (pressed >= 0 && btn_at(hid_touch_x(), hid_touch_y()) != pressed) {
      pressed = -1;
      ui_dirty = true;
    }
  } else if (hid_touch_up()) {
    b = btn_at(hid_touch_x(), hid_touch_y());
    if (b >= 0 && b == pressed)
      press(b);
    else if (header_tap && hid_touch_y() < HEADER_H)
      perf_on = !perf_on;
    pressed = -1;
    header_tap = false;
    ui_dirty = true;
  }
}

// --- Main ----------------------------------------------------------------------

// The stream plays only while a game runs on screen, so pausing, the list of
// games and leaving are silent.
static void sync_sound(void) {
  static bool unavailable;
  bool want = sound_on && loaded && view == VIEW_MENU && !paused && !crashed;
  if (want && !sound_playing() && !unavailable && !sound_start()) {
    unavailable = true;
    say("Sound is not available here");
  } else if (!want && sound_playing()) {
    sound_stop();
  }
}

static void save_on_exit(void) {
  sound_stop(); // the voice would go on looping in the Home Menu
  save_ram();
  save_settings();
}

int main(void) {
  bool was_fast = false;

  load_settings();
  apply_palette();
  apply_size();
  atexit(save_on_exit);
  scan_games();
  draw_top_idle();

  uint64_t pass_end = sys_micros();
  while (app_loop()) {
    uint32_t down = hid_keys_down(), held = hid_keys_held();
    bool fast = (held & KEY_R) != 0;
    bool running = view == VIEW_MENU && !paused;

    if (running) {
      perf_wait_us += (uint32_t)(sys_micros() - pass_end);
      perf_passes++;
    }

    if (view == VIEW_GAMES) {
      games_input(down, held);
    } else {
      menu_input();
      if (fast != was_fast)
        ui_dirty = true;
    }
    if (view == VIEW_MENU && screen_auto_off && !menu_dark && !paused &&
        pressed < 0 && armed < 0 &&
        sys_millis() - last_touch_ms >= SCREEN_OFF_MS) {
      menu_dark = true;
      ui_dirty = true;
    }
    sync_sound();
    if (view == VIEW_MENU) {
      if (!paused)
        emulate(held);
      update_speed();
    }
    was_fast = fast;

    if (ram_dirty && sys_millis() - ram_changed_ms >= SAVE_DELAY_MS)
      save_ram();
    if (toast_until && (int32_t)(sys_millis() - toast_until) >= 0) {
      toast_until = 0;
      ui_dirty = true;
    }
    if (ui_dirty) {
      uint64_t t0 = sys_micros();
      draw_bottom();
      perf_draw_us += (uint32_t)(sys_micros() - t0);
    }
    pass_end = sys_micros();
  }
  return 0;
}
