/* donut.c on AuroraOS: Andy Sloane's spinning ASCII donut, as in
 * https://github.com/akhileshthite/3d-donut. This is that repository's
 * "Donut.c without a math library.c", the fixed-point version: the ARM9 has
 * no FPU, and the floating-point Donut.c would take seconds over each frame.
 *
 * Changed from the original: the console is 50 x 30 square cells rather
 * than 80 x 22 cells twice as tall as wide, so the donut is centred on it
 * and scaled the same both ways; app_loop() shows each frame and paces it
 * in place of usleep(); ESC[H goes back to the top, since the console has
 * no ESC[23A. START or HOME goes back to the Home Menu. */
#include <aurora_app.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The original's expressions are kept as written, unparenthesised. */
#pragma GCC diagnostic ignored "-Wparentheses"

#define W 50
#define H 30
#define K 19 /* the largest scale that keeps every frame on the console */

#define R(mul,shift,x,y) \
  _=x; \
  x -= mul*y>>shift; \
  y += mul*_>>shift; \
  _ = 3145728-x*x-y*y>>11; \
  x = x*_>>10; \
  y = y*_>>10;

static int8_t b[W * H], z[W * H];

static void info(int fps) {
  gfx_clear(SCREEN_BOTTOM, COLOR_BLACK);

  gfx_print(SCREEN_BOTTOM, 16, 16, FONT_TITLE, COLOR_WHITE, "donut.c");
  gfx_print(SCREEN_BOTTOM, 16, 50, FONT_REGULAR, COLOR_GRAY,
            "Andy Sloane's spinning donut,\nin fixed point on the ARM9");

  if (fps)
    gfx_print(SCREEN_BOTTOM, 16, 110, FONT_REGULAR, COLOR_WHITE, "%d fps",
              fps);
  gfx_print(SCREEN_BOTTOM, 16, 212, FONT_SMALL, COLOR_GRAY,
            "START: back to the Home Menu");
}

int main(void) {
  int sA=1024,cA=0,sB=1024,cB=0,_;
  int shown = -1;

  while (app_loop()) {
    if (hid_keys_down() & KEY_START)
      break;

    if (app_fps() != shown)
      info(shown = app_fps());

    memset(b, 32, sizeof(b));  // text buffer
    memset(z, 127, sizeof(z));   // z buffer

    int sj=0, cj=1024;

    for (int j = 0; j < 90; j++) {
      int si = 0, ci = 1024;  // sine and cosine of angle i

      for (int i = 0; i < 324; i++) {
        int R1 = 1, R2 = 2048, K2 = 5120*1024;

        int x0 = R1*cj + R2,
            x1 = ci*x0 >> 10,
            x2 = cA*sj >> 10,
            x3 = si*x0 >> 10,
            x4 = R1*x2 - (sA*x3 >> 10),
            x5 = sA*sj >> 10,
            x6 = K2 + R1*1024*x5 + cA*x3,
            x7 = cj*si >> 10,
            x = W/2 + K*(cB*x1 - sB*x4)/x6,
            y = H/2 + K*(cB*x4 + sB*x1)/x6,
            N = (-cA*x7 - cB*((-sA*x7>>10) + x2) - ci*(cj*sB >> 10) >> 10) - x5 >> 7;

        int o = x + W * y;
        int8_t zz = (x6-K2)>>15;

        if (H > y && y >= 0 && x >= 0 && W > x && zz < z[o]) {
          z[o] = zz;
          b[o] = ".,-~:;=!*#$@"[N > 0 ? N : 0];
        }

        R(5, 8, ci, si)  // rotate i
      }

      R(9, 7, cj, sj)  // rotate j
    }

    printf("\x1b[H");

    for (int k = 0; k < W * H; k++) {
      if (k && k % W == 0)
        putchar('\n'); /* a full row does not wrap by itself; a newline
                          after the last one would scroll the console */
      putchar(b[k]);
    }

    R(5, 7, cA, sA);
    R(5, 8, cB, sB);
  }

  return 0;
}
