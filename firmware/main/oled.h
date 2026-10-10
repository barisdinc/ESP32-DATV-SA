/* SSD1306 / SH1106 128x64 OLED on a bit-banged I2C bus (front panel).
 * No I2C driver, no interrupts, no heap: the transmit loops run with interrupts masked and need every free byte of heap, so the
 * display is only drawn between transmissions. The frame buffer is the caller's (on the stack): static data must stay below the
 * RF bank (rf_bank_guard.ld), which leaves no room for 1 KB. oled_flush is a no-op when no display answered at power-up. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define OLED_SDA_GPIO 0
#define OLED_SCL_GPIO 1
#define OLED_W 128
#define OLED_H 64
#define OLED_COL_OFFSET 2                                        /* SH1106 (most 1.3" modules): 132-column RAM, picture from column 2; real SSD1306: 0 */

typedef struct { uint8_t px[OLED_W * OLED_H / 8]; } oled_fb_t;   /* one byte = 8 vertical pixels, SSD1306 page layout */

bool oled_init(void);                                            /* probes 0x3C and 0x3D, blanks the screen; false: no display */
void oled_clear(oled_fb_t *f);
void oled_text(oled_fb_t *f, int x, int page, int scale, const char *s);      /* 5x7 font, scale 1 or 2; page = 8-pixel row */
void oled_invert(oled_fb_t *f, int x0, int x1, int page);                      /* inverse video for a part of one page */
void oled_hline(oled_fb_t *f, int y);
void oled_hseg(oled_fb_t *f, int x0, int x1, int y);                     /* x0 <= x < x1 */
extern void (*oled_byte_hook)(void);                             /* called after every byte sent: the panel samples its encoder */
void oled_flush(const oled_fb_t *f);                                         /* about 50 ms for the whole frame */
