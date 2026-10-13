/* Front panel: SSD1306 / SH1106 OLED (oled.c), EC11 rotary encoder with push button and three buttons.
 *
 * Every field on the screen can be changed: beacon on / off, PA, frequency, source (test card or the stand-alone video in the flash),
 * level, symbol rate, pilots, service name and provider (symbol rate, pilots and names: the test card's; the video brings its own).
 *   browse   encoder or K1 / K2: next / previous field (underlined); K3 or encoder push: edit it
 *   edit     encoder: change the value (inverse video); K1 / K2: cursor right / left (frequency digit, character);
 *            K3 or encoder push: confirm - the beacon restarts with the new value at once and the settings are saved (NVS);
 *            K2 held, or K2 on a field without a cursor: cancel
 * The transmit loop runs with interrupts masked, so it cannot follow the encoder: it only looks at the GPIO interrupt status
 * (edges latched in hardware, no interrupt is taken) every half loop. The first touch stops the beacon; it starts again with the
 * confirmed values, or 2 s after the last input when nothing is being edited. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "beacon_enc.h"
#include "soc/gpio_reg.h"
#include "soc/soc.h"

#define PANEL_ENC_A    5
#define PANEL_ENC_B    6
#define PANEL_ENC_SW   7
#define PANEL_K1       10                                        /* next / cursor right */
#define PANEL_K2       20                                        /* previous / cursor left / cancel */
#define PANEL_K3       21                                        /* edit / confirm */
#define PANEL_PINS     ((1u << PANEL_ENC_A) | (1u << PANEL_ENC_B) | (1u << PANEL_ENC_SW) | (1u << PANEL_K1) | (1u << PANEL_K2) | (1u << PANEL_K3))
#define PANEL_NAME_MAX 20                                        /* what fits behind the "S" / "P" label on the screen */
_Static_assert(PANEL_NAME_MAX <= BENC_NAME_MAX, "name length");

typedef struct {
    uint8_t tx;                                                  /* beacon on the air */
    uint8_t pa;                                                  /* GPIO3 high while it transmits */
    uint8_t pilots;
    uint8_t sr;                                                  /* index into panel_sr_baud */
    int8_t level_db;                                             /* DAC amplitude relative to 300 */
    uint32_t fkhz;
    char service[PANEL_NAME_MAX + 1];
    char provider[PANEL_NAME_MAX + 1];
    uint8_t video;                                               /* source: 1 the video in the flash (when there is one), 0 the test card */
} panel_cfg_t;                                                   /* new fields go last: a shorter saved blob still loads (see load) */

extern panel_cfg_t panel_cfg;                                    /* the confirmed settings */
extern const uint32_t panel_sr_baud[];
extern const uint8_t panel_nsr;                                 /* entries of panel_sr_baud */

enum { PANEL_NONE, PANEL_APPLY, PANEL_LOCAL };

void panel_init(const panel_cfg_t *defaults);                    /* after nvs_flash_init: pins, display, saved settings */
int32_t panel_amp(void);                                         /* DAC amplitude of panel_cfg.level_db */
int panel_poll(bool pc);                                         /* about every ms while not transmitting; pc: the host has the radio */
bool panel_busy(void);                                           /* editing, or input less than 2 s ago: keep the beacon off */
void panel_draw(void);                                           /* the settings screen */
void panel_error(const char *msg);                               /* a line on the settings screen until the next confirm */
void panel_video(const char *line);                              /* the flash holds a video: its mode line (at start-up, NULL: none) */
void panel_show(const char *state, bool pa, uint32_t fkhz, const char *l4, const char *l5, const char *l6, const char *l7);  /* host mode */
void panel_wake(void);                                           /* the beacon stopped for panel input: take the latched buttons */

static inline void panel_latch_clear(void) { REG_WRITE(GPIO_STATUS_W1TC_REG, PANEL_PINS); }
static inline bool panel_latched(void) { return REG_READ(GPIO_STATUS_REG) & PANEL_PINS; }   /* cheap: runs between beacon chunks */
