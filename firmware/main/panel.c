/* Front panel: settings screen, encoder, buttons, saved settings (see panel.h). */
#include "panel.h"
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "beacon_cfg.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "nvs.h"
#include "oled.h"

#define FMIN_KHZ    2300000u                                     /* the firmware's band (TX_FMIN_KHZ, TX_FMAX_KHZ in main.c) */
#define FMAX_KHZ    2450000u
#define NAME_X      8                                            /* service name / provider: 20 characters up to the right edge */
#define LEVEL_MIN   (-40)                                        /* dB relative to amplitude 300 */
#define LEVEL_MAX   4                                            /* amplitude 480, the firmware's maximum */
#define IDLE_US     2000000                                      /* the beacon starts again this long after the last input */
#define LONG_US     800000                                       /* K2 held: cancel */
#define DEBOUNCE_US 20000
#define ENC_STEPS   4                                            /* quadrature steps per detent: 4 (most EC11) or 2 */
#define ENC_DIR     1                                            /* -1 if the encoder turns the wrong way */

/* 8 MS/s: 200, 160, 120, 80, 64, 32, 24, 20, 16 samples per symbol (160 MHz / baud must round down to 20 x S) */
const uint32_t panel_sr_baud[] = {40000, 50000, 66666, 100000, 125000, 250000, 333000, 400000, 500000};
#define NSR ((int)(sizeof(panel_sr_baud) / sizeof(panel_sr_baud[0])))
const uint8_t panel_nsr = NSR;

static const char CHARSET[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_./+()";   /* space first: deletes */
static const uint32_t FSTEP[] = {10000, 1000, 100, 10, 1};       /* kHz per frequency cursor position */
static const uint8_t FCHAR[] = {2, 3, 5, 6, 7};                  /* ... and the character it underlines in "2300.500" */

enum { F_TX, F_PA, F_FREQ, F_SRC, F_LEVEL, F_SR, F_PILOTS, F_SVC, F_PRV, F_N };
enum { B_K1, B_K2, B_K3, B_SW, B_N };
enum { E_NONE, E_NEXT, E_PREV, E_CANCEL, E_OK };
static const uint8_t BTN_PIN[B_N] = {PANEL_K1, PANEL_K2, PANEL_K3, PANEL_ENC_SW};

panel_cfg_t panel_cfg;

static struct {
    bool ok, pc, edit, dirty;
    uint8_t sel, cur;
    uint8_t qstate;                                              /* encoder: last A | B << 1 */
    int8_t qacc;
    int16_t steps;                                               /* detents not handled yet */
    int64_t last;                                                /* last input; 0: none pending */
    const char *msg;
    const char *video;                                           /* panel_video: the video's mode line, NULL: no video in the flash */
    panel_cfg_t ed;                                              /* the value being edited */
    struct { bool raw, down, used; int64_t traw, tdown; } b[B_N];
} ui;

/* ------------------------------------------------------------ input */

static void enc_sample(void) {                                   /* also called by the display driver during a frame */
    static const int8_t tab[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};
    const uint32_t in = REG_READ(GPIO_IN_REG);
    const uint8_t s = (uint8_t)((in >> PANEL_ENC_A & 1u) | (in >> PANEL_ENC_B & 1u) << 1);
    ui.qacc += tab[ui.qstate << 2 | s];
    ui.qstate = s;
    if (s == 3 || (ENC_STEPS == 2 && s == 0)) {                  /* a detent: count it when the steps on the way mostly agree */
        if (ui.qacc >= 2) ui.steps += ENC_DIR;
        else if (ui.qacc <= -2) ui.steps -= ENC_DIR;
        ui.qacc = 0;
    }
}

static int press_event(int i) { return i == B_K1 ? E_NEXT : i == B_K2 ? E_PREV : E_OK; }

static int buttons(int64_t now) {                                /* one event per call; K2 acts on release (held: cancel) */
    const uint32_t in = REG_READ(GPIO_IN_REG);
    for (int i = 0; i < B_N; ++i) {
        const bool p = !(in >> BTN_PIN[i] & 1u);                 /* to GND when pressed */
        if (p != ui.b[i].raw) { ui.b[i].raw = p; ui.b[i].traw = now; }
        if (ui.b[i].raw != ui.b[i].down && now - ui.b[i].traw >= DEBOUNCE_US) {
            ui.b[i].down = ui.b[i].raw;
            if (ui.b[i].down) {
                ui.b[i].tdown = now;
                ui.b[i].used = i != B_K2;
                if (i != B_K2) return press_event(i);
            } else if (i == B_K2 && !ui.b[i].used) {
                return E_PREV;
            }
        }
        if (i == B_K2 && ui.b[i].down && !ui.b[i].used && now - ui.b[i].tdown >= LONG_US) {
            ui.b[i].used = true;
            return E_CANCEL;
        }
    }
    return E_NONE;
}

static void input_sync(int64_t now) {                            /* take the pins as they are: nothing held counts as a new press */
    const uint32_t in = REG_READ(GPIO_IN_REG);
    ui.qstate = (uint8_t)((in >> PANEL_ENC_A & 1u) | (in >> PANEL_ENC_B & 1u) << 1);
    ui.qacc = 0;
    ui.steps = 0;
    for (int i = 0; i < B_N; ++i) {
        ui.b[i].raw = ui.b[i].down = !(in >> BTN_PIN[i] & 1u);
        ui.b[i].traw = ui.b[i].tdown = now;
        ui.b[i].used = true;
    }
}

/* ------------------------------------------------------------ settings */

static uint32_t fmin_khz(uint8_t sr) { return FMIN_KHZ + (panel_sr_baud[sr] * 135u / 200u + 999u) / 1000u; }   /* the signal stays in the band */
static uint32_t fmax_khz(uint8_t sr) { return FMAX_KHZ - (panel_sr_baud[sr] * 135u / 200u + 999u) / 1000u; }
static uint32_t clamp_f(uint32_t f, uint8_t sr) { return f < fmin_khz(sr) ? fmin_khz(sr) : f > fmax_khz(sr) ? fmax_khz(sr) : f; }

int32_t panel_amp(void) {
    const long a = lroundf(300.0f * powf(10.0f, panel_cfg.level_db / 20.0f));
    return a < 1 ? 1 : a > 480 ? 480 : (int32_t)a;
}

static bool valid(const panel_cfg_t *c) {
    return c->tx <= 1 && c->pa <= 1 && c->pilots <= 1 && c->video <= 1 && c->sr < NSR && c->level_db >= LEVEL_MIN && c->level_db <= LEVEL_MAX &&
           c->fkhz == clamp_f(c->fkhz, c->sr) && memchr(c->service, 0, sizeof(c->service)) && memchr(c->provider, 0, sizeof(c->provider)) &&
           c->service[0] && c->provider[0];
}

static void save(void) {
    nvs_handle_t h;
    if (nvs_open("panel", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "cfg", &panel_cfg, sizeof(panel_cfg));
    nvs_commit(h);
    nvs_close(h);
}

static void load(void) {
    nvs_handle_t h;
    panel_cfg_t c;
    size_t n = sizeof(c);
    if (nvs_open("panel", NVS_READONLY, &h) != ESP_OK) return;
    if (nvs_get_blob(h, "cfg", &c, &n) == ESP_OK && n <= sizeof(c) && n >= offsetof(panel_cfg_t, video)) {
        memcpy((uint8_t *)&c + n, (const uint8_t *)&panel_cfg + n, sizeof(c) - n);   /* fields an older firmware did not save: defaults */
        if (valid(&c)) panel_cfg = c;
    }
    nvs_close(h);
}

/* ------------------------------------------------------------ editing */

static char *text_field(void) { return ui.sel == F_SVC ? ui.ed.service : ui.sel == F_PRV ? ui.ed.provider : NULL; }
static bool has_cursor(void) { return ui.sel == F_FREQ || text_field(); }

static void edit_value(int d) {
    panel_cfg_t *e = &ui.ed;
    char *s = text_field();
    switch (ui.sel) {
    case F_TX: e->tx ^= d & 1; break;
    case F_PA: e->pa ^= d & 1; break;
    case F_PILOTS: e->pilots ^= d & 1; break;
    case F_SRC: e->video ^= d & 1; break;
    case F_FREQ: {
        const int64_t f = (int64_t)e->fkhz + (int64_t)d * FSTEP[ui.cur];
        e->fkhz = clamp_f(f < 0 ? 0 : (uint32_t)f, e->sr);
        break;
    }
    case F_LEVEL: {
        const int l = e->level_db + d;
        e->level_db = (int8_t)(l < LEVEL_MIN ? LEVEL_MIN : l > LEVEL_MAX ? LEVEL_MAX : l);
        break;
    }
    case F_SR: {
        const int r = e->sr + d;
        e->sr = (uint8_t)(r < 0 ? 0 : r >= NSR ? NSR - 1 : r);
        e->fkhz = clamp_f(e->fkhz, e->sr);
        break;
    }
    default: {                                                   /* the character under the cursor; past the end: a new one */
        const size_t n = strlen(s);
        if (ui.cur >= n) { s[n] = ' '; s[n + 1] = 0; }
        const char *p = strchr(CHARSET, s[ui.cur]);
        const int len = (int)sizeof(CHARSET) - 1;
        const int k = (int)(p && *p ? p - CHARSET : 0);
        s[ui.cur] = CHARSET[((k + d) % len + len) % len];
        break;
    }
    }
}

static void move_cursor(int d) {
    const int max = ui.sel == F_FREQ ? 4 : (int)strlen(text_field()) < PANEL_NAME_MAX - 1 ? (int)strlen(text_field()) : PANEL_NAME_MAX - 1;
    const int c = ui.cur + d;
    ui.cur = (uint8_t)(c < 0 ? 0 : c > max ? max : c);
}

static void trim(char *s, const char *old) {
    size_t n = strlen(s);
    while (n && s[n - 1] == ' ') s[--n] = 0;
    if (!n) strcpy(s, old);                                      /* an empty name is not taken */
}

static int dispatch(int e) {
    switch (e) {
    case E_OK:
        if (!ui.edit) {
            ui.ed = panel_cfg;
            ui.edit = true;
            ui.cur = ui.sel == F_FREQ ? 1 : 0;
            return PANEL_NONE;
        }
        trim(ui.ed.service, panel_cfg.service);
        trim(ui.ed.provider, panel_cfg.provider);
        panel_cfg = ui.ed;
        ui.edit = false;
        ui.msg = NULL;
        ui.last = 0;                                             /* no waiting: the beacon takes the new value now */
        save();
        return PANEL_APPLY;
    case E_NEXT:
        if (!ui.edit) ui.sel = (uint8_t)((ui.sel + 1) % F_N);
        else if (has_cursor()) move_cursor(1);
        return PANEL_NONE;
    case E_PREV:
        if (!ui.edit) ui.sel = (uint8_t)((ui.sel + F_N - 1) % F_N);
        else if (has_cursor()) move_cursor(-1);
        else ui.edit = false;
        return PANEL_NONE;
    case E_CANCEL:
        ui.edit = false;
        return PANEL_NONE;
    }
    return PANEL_NONE;
}

/* ------------------------------------------------------------ screen */

void panel_draw(void) {
    if (!ui.ok) return;
    ui.pc = false;
    ui.dirty = false;
    const panel_cfg_t *c = ui.edit ? &ui.ed : &panel_cfg;
    oled_fb_t f;
    char b[24];
    oled_clear(&f);
    const char *txs = c->tx ? "BEACON TX" : "BEACON noTX", *pas = c->pa ? "PA ON" : "PA OFF", *pls = c->pilots ? "pilots" : "noPilots";
    const int pax = OLED_W - 6 * (int)strlen(pas);
    oled_text(&f, 0, 0, 1, txs);
    oled_text(&f, pax, 0, 1, pas);
    oled_hline(&f, 10);
    snprintf(b, sizeof(b), "%4lu.%03lu", (unsigned long)(c->fkhz / 1000), (unsigned long)(c->fkhz % 1000));
    oled_text(&f, 0, 2, 2, b);
    oled_text(&f, OLED_W - 18, 3, 1, "MHz");
    const char *src = c->video ? (ui.video ? ui.video : "VIDEO none") : "TEST CARD " BEACON_FEC;
    oled_text(&f, 0, 4, 1, ui.msg ? ui.msg : src);
    snprintf(b, sizeof(b), "%+ddB", c->level_db);
    oled_text(&f, 96, 4, 1, b);
    snprintf(b, sizeof(b), "SR %lu kS/s", (unsigned long)((panel_sr_baud[c->sr] + 500) / 1000));
    oled_text(&f, 0, 5, 1, b);
    oled_text(&f, 72, 5, 1, pls);
    oled_text(&f, 0, 6, 1, "S");
    oled_text(&f, NAME_X, 6, 1, c->service);
    oled_text(&f, 0, 7, 1, "P");
    oled_text(&f, NAME_X, 7, 1, c->provider);
    const struct { int x0, x1, page; } r[F_N] = {
        {0, 6 * (int)strlen(txs), 0}, {pax, OLED_W, 0}, {0, 96, 2}, {0, 6 * (int)strlen(src), 4}, {96, OLED_W, 4}, {0, 66, 5}, {72, 72 + 6 * (int)strlen(pls), 5},
        {NAME_X, NAME_X + 6 * (int)strlen(c->service), 6}, {NAME_X, NAME_X + 6 * (int)strlen(c->provider), 7},
    };
    const int s = ui.sel, y = r[s].page == 2 ? 31 : 8 * r[s].page + 7;
    if (!ui.edit) {
        oled_hseg(&f, r[s].x0, r[s].x1, y);                     /* browse: the selected field underlined */
    } else if (s == F_FREQ) {                                    /* edit: inverse video, the digit or character under the cursor */
        oled_invert(&f, 12 * FCHAR[ui.cur], 12 * FCHAR[ui.cur] + 12, 2);
        oled_invert(&f, 12 * FCHAR[ui.cur], 12 * FCHAR[ui.cur] + 12, 3);
        oled_hseg(&f, r[s].x0, r[s].x1, y);
    } else if (s == F_SVC || s == F_PRV) {
        oled_invert(&f, NAME_X + 6 * ui.cur, NAME_X + 6 + 6 * ui.cur, r[s].page);
        oled_hseg(&f, r[s].x0, r[s].x1, y);
    } else {
        oled_invert(&f, r[s].x0, r[s].x1, r[s].page);
    }
    oled_flush(&f);
}

void panel_show(const char *state, bool pa, uint32_t fkhz, const char *l4, const char *l5, const char *l6, const char *l7) {
    if (!ui.ok) return;
    ui.pc = true;
    char b[24];
    oled_fb_t f;
    oled_clear(&f);
    oled_text(&f, 1, 0, 1, state);
    oled_invert(&f, 0, 6 * (int)strlen(state) + 1, 0);
    const char *pas = pa ? "PA ON" : "PA OFF";
    oled_text(&f, OLED_W - 6 * (int)strlen(pas), 0, 1, pas);
    oled_hline(&f, 10);
    if (fkhz) {
        snprintf(b, sizeof(b), "%4lu.%03lu", (unsigned long)(fkhz / 1000), (unsigned long)(fkhz % 1000));
        oled_text(&f, 0, 2, 2, b);
        oled_text(&f, OLED_W - 18, 3, 1, "MHz");
    }
    const char *l[4] = {l4, l5, l6, l7};
    for (int i = 0; i < 4; ++i)
        if (l[i]) oled_text(&f, 0, 4 + i, 1, l[i]);
    oled_flush(&f);
}

void panel_error(const char *msg) {
    ui.msg = msg;
    panel_draw();
}

void panel_video(const char *line) {
    ui.video = line;
    panel_draw();
}

/* ------------------------------------------------------------ main loop interface */

void panel_init(const panel_cfg_t *defaults) {
    const gpio_config_t config = {
        .pin_bit_mask = PANEL_PINS,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,                          /* only for the status latch: no handler, the CPU never takes it */
    };
    gpio_config(&config);
    panel_cfg = *defaults;
    load();
    ui.ok = oled_init();
    oled_byte_hook = enc_sample;
    input_sync(esp_timer_get_time());
    panel_latch_clear();
    panel_draw();
}

bool panel_busy(void) { return ui.edit || (ui.last && esp_timer_get_time() - ui.last < IDLE_US); }

void panel_wake(void) {
    const uint32_t st = REG_READ(GPIO_STATUS_REG) & PANEL_PINS;
    panel_latch_clear();
    const int64_t now = esp_timer_get_time();
    input_sync(now);
    ui.last = now;
    for (int i = 0; i < B_N; ++i)                                /* a button touched during the transmission counts as a press; */
        if (st >> BTN_PIN[i] & 1u) { dispatch(press_event(i)); break; }   /* the encoder only wakes the panel up */
    panel_draw();
}

int panel_poll(bool pc) {
    const int64_t now = esp_timer_get_time();
    enc_sample();
    const int e = buttons(now);
    const int st = ui.steps;
    ui.steps = 0;
    int r = PANEL_NONE;
    if (e != E_NONE || st) {
        ui.last = now;
        if (pc) return PANEL_LOCAL;                              /* any key takes the radio back from the host */
        if (st) {
            if (ui.edit) edit_value(st);
            else ui.sel = (uint8_t)(((ui.sel + st) % F_N + F_N) % F_N);
        }
        r = dispatch(e);
        ui.dirty = true;
    }
    if (ui.dirty) panel_draw();                                  /* also replaces a host mode screen once the panel is used */
    return r;
}
