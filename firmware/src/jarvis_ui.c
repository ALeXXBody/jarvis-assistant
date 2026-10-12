#include "jarvis_ui.h"
#include "jarvis_config.h"
#include "jarvis_state.h"
#include "bsp_guition.h"
#include "font8x8_basic.h"

#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ui";

#define SCR_W BSP_LCD_H_RES
#define SCR_H BSP_LCD_V_RES

/* --- palette (RGB565) ---------------------------------------------------- */
#define C_BG        0x0861  /* very dark blue-grey  */
#define C_BG2       0x10A2
#define C_DIM       0x4208
#define C_TEXT      0xCE79
#define C_TEXT_DIM  0x6B4D
#define C_CYAN      0x07FF
#define C_CYAN_SOFT 0x05B6
#define C_AMBER     0xFD20
#define C_GREEN     0x07E6
#define C_RED       0xF800
#define C_WHITE     0xFFFF

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

typedef struct {
    uint16_t base;   /* ring colour   */
    uint16_t glow;   /* halo colour   */
    float    pulse;  /* 0..1 breathing amplitude */
    float    spin;   /* rad/s sweep speed */
} state_style_t;

static void style_for(jarvis_state_t s, state_style_t *o)
{
    switch (s) {
    case JVS_BOOTING:   *o = (state_style_t){ C_DIM,      C_DIM,   0.10f, 0.4f }; break;
    case JVS_IDLE:      *o = (state_style_t){ C_CYAN,     C_CYAN,  0.18f, 0.6f }; break;
    case JVS_LISTENING: *o = (state_style_t){ C_WHITE,    C_CYAN,  0.55f, 1.6f }; break;
    case JVS_THINKING:  *o = (state_style_t){ C_AMBER,    C_AMBER, 0.40f, 2.6f }; break;
    case JVS_SPEAKING:  *o = (state_style_t){ C_GREEN,    C_GREEN, 0.65f, 1.1f }; break;
    case JVS_ERROR:     *o = (state_style_t){ C_RED,      C_RED,   0.25f, 0.3f }; break;
    default:            *o = (state_style_t){ C_DIM,      C_DIM,   0.10f, 0.3f }; break;
    }
}

static inline void px(uint16_t *fb, int x, int y, uint16_t c)
{
    if (x >= 0 && x < SCR_W && y >= 0 && y < SCR_H) {
        fb[y * SCR_W + x] = c;
    }
}

static void fill_rect(uint16_t *fb, int x, int y, int w, int h, uint16_t c)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > SCR_W) w = SCR_W - x;
    if (y + h > SCR_H) h = SCR_H - y;
    for (int j = 0; j < h; j++) {
        uint16_t *row = fb + (y + j) * SCR_W + x;
        for (int i = 0; i < w; i++) {
            row[i] = c;
        }
    }
}

static void clear_bg(uint16_t *fb)
{
    /* Vertical gradient so the panel isn't a flat black slab. */
    for (int y = 0; y < SCR_H; y++) {
        uint16_t c = (y < SCR_H / 2) ? C_BG : C_BG2;
        uint16_t *row = fb + y * SCR_W;
        for (int x = 0; x < SCR_W; x++) {
            row[x] = c;
        }
    }
}

static int text_w(const char *s, int scale)
{
    return (int)strlen(s) * 8 * scale;
}

static void draw_text(uint16_t *fb, int x, int y, const char *s, uint16_t c, int scale)
{
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch < 128) {
            const uint8_t *g = font8x8_basic[ch];
            for (int gy = 0; gy < 8; gy++) {
                uint8_t bits = g[gy];
                for (int gx = 0; gx < 8; gx++) {
                    if (bits & (1 << gx)) {
                        for (int sy = 0; sy < scale; sy++) {
                            for (int sx = 0; sx < scale; sx++) {
                                px(fb, x + gx * scale + sx, y + gy * scale + sy, c);
                            }
                        }
                    }
                }
            }
        }
        x += 8 * scale;
    }
}

static void draw_text_centered(uint16_t *fb, int y, const char *s, uint16_t c, int scale)
{
    draw_text(fb, (SCR_W - text_w(s, scale)) / 2, y, s, c, scale);
}

/* --- reactor ------------------------------------------------------------- */

#define ORB_N 1024

static void build_orb_lut(uint16_t *lut, const state_style_t *st,
                          float R, float t)
{
    /* Ring radii breathe outward over time. */
    float breath = 1.0f + st->pulse * 0.12f * sinf(t * 2.2f);
    float ring[3] = { 0.62f * R * breath,
                      0.80f * R * breath,
                      0.95f * R * breath };
    float ring_w = R * 0.035f;
    float ring_amp[3] = { 1.0f, 0.75f, 0.55f };

    for (int i = 0; i < ORB_N; i++) {
        /* normalised squared radius -> linear radius */
        float nd = (float)i / (float)(ORB_N - 1);      /* d2/R2 */
        float d = sqrtf(nd) * R;

        /* halo: bright core fading out */
        float halo = 1.0f - nd;
        halo = halo * halo * 0.55f;

        /* core spark */
        float core = (d < R * 0.16f) ? 1.0f : 0.0f;

        float ring_i = 0.0f;
        for (int k = 0; k < 3; k++) {
            float dd = d - ring[k];
            ring_i += ring_amp[k] * expf(-(dd * dd) / (2.0f * ring_w * ring_w));
        }

        float intensity = halo + ring_i + core;
        if (intensity > 1.0f) intensity = 1.0f;

        /* blend base ring colour with white at the very core */
        uint16_t col = st->base;
        if (core > 0.0f) {
            col = C_WHITE;
        }
        /* scale colour channels by intensity (approx, per 565 nibble) */
        int r = ((col >> 11) & 0x1F);
        int g = ((col >> 5) & 0x3F);
        int b = (col & 0x1F);
        r = (int)(r * intensity);
        g = (int)(g * intensity);
        b = (int)(b * intensity);
        lut[i] = (uint16_t)((r << 11) | (g << 5) | b);
    }
}

static void draw_orb(uint16_t *fb, int cx, int cy, float R, uint16_t *lut)
{
    int R2 = (int)(R * R);
    int i0 = cx - (int)R, i1 = cx + (int)R;
    int j0 = cy - (int)R, j1 = cy + (int)R;
    if (i0 < 0) i0 = 0;
    if (j0 < 0) j0 = 0;
    if (i1 > SCR_W) i1 = SCR_W;
    if (j1 > SCR_H) j1 = SCR_H;

    for (int y = j0; y < j1; y++) {
        int dy = y - cy;
        int dy2 = dy * dy;
        uint16_t *row = fb + y * SCR_W;
        for (int x = i0; x < i1; x++) {
            int dx = x - cx;
            int d2 = dx * dx + dy2;
            if (d2 < R2) {
                row[x] = lut[(d2 * (ORB_N - 1)) / R2];
            }
        }
    }
}

static void draw_spokes(uint16_t *fb, int cx, int cy, float R, float t, uint16_t c)
{
    for (int k = 0; k < 24; k++) {
        float a = t * 0.9f + (float)k * (2.0f * (float)M_PI / 24.0f);
        float r0 = R * 1.02f;
        float r1 = R * (1.06f + 0.02f * sinf(t * 3.0f + k));
        int x0 = cx + (int)(cosf(a) * r0), y0 = cy + (int)(sinf(a) * r0);
        int x1 = cx + (int)(cosf(a) * r1), y1 = cy + (int)(sinf(a) * r1);
        int steps = (int)(R * 0.06f) + 1;
        for (int s = 0; s < steps; s++) {
            int x = x0 + (x1 - x0) * s / steps;
            int y = y0 + (y1 - y0) * s / steps;
            px(fb, x, y, c);
        }
    }
}

/* --- chrome -------------------------------------------------------------- */

static void draw_top_bar(uint16_t *fb, const jarvis_snapshot_t *s)
{
    fill_rect(fb, 0, 0, SCR_W, 2, C_DIM);
    draw_text(fb, 40, 44, "J.A.R.V.I.S.", C_CYAN, 3);

    /* link dots */
    int x = SCR_W - 60;
    px(fb, x, 52, s->ws_connected ? C_GREEN : C_RED);
    fill_rect(fb, x - 2, 50, 5, 5, s->ws_connected ? C_GREEN : C_RED);
    fill_rect(fb, x - 34, 50, 5, 5, s->wifi_connected ? C_GREEN : C_DIM);

    char buf[32];
    if (s->battery_pct >= 0) {
        snprintf(buf, sizeof(buf), "%d%%", s->battery_pct);
    } else {
        snprintf(buf, sizeof(buf), "--");
    }
    draw_text(fb, SCR_W - 150, 88, buf, C_TEXT_DIM, 2);
}

static void draw_bottom(uint16_t *fb, const jarvis_snapshot_t *s)
{
    int y = 980;
    const char *name = jarvis_state_name(s->state);
    char up[24];
    /* uppercase the state label */
    size_t n = strlen(name);
    for (size_t i = 0; i < n && i < sizeof(up) - 1; i++) {
        up[i] = (name[i] >= 'a' && name[i] <= 'z') ? (char)(name[i] - 32) : name[i];
    }
    up[n < sizeof(up) ? n : sizeof(up) - 1] = '\0';
    draw_text_centered(fb, y, up, C_TEXT, 4);

    if (s->approval_pending) {
        draw_text_centered(fb, y + 48, "APPROVE?  tap = allow", C_AMBER, 2);
    } else if (s->tool[0]) {
        char line[80];
        snprintf(line, sizeof(line), "using %s", s->tool);
        draw_text_centered(fb, y + 48, line, C_AMBER, 2);
    } else if (s->status[0]) {
        draw_text_centered(fb, y + 48, s->status, C_TEXT_DIM, 2);
    }

    if (s->last_total_seconds > 0.1f) {
        char lat[48];
        snprintf(lat, sizeof(lat), "last turn %.1fs", s->last_total_seconds);
        draw_text(fb, 40, SCR_H - 34, lat, C_TEXT_DIM, 2);
    }
}

static void draw_transcript(uint16_t *fb, const jarvis_snapshot_t *s)
{
    int y = 1090;
    for (int i = 0; i < JVS_LINES; i++) {
        const char *line = s->lines[i];
        if (!line[0]) {
            y += 30;
            continue;
        }
        bool is_reply = (strncmp(line, "JARVIS:", 7) == 0);
        uint16_t c = is_reply ? C_CYAN_SOFT : C_TEXT_DIM;
        draw_text(fb, 40, y, line, c, 2);
        y += 30;
    }
}

/* --- task ---------------------------------------------------------------- */

static void ui_task(void *arg)
{
    (void)arg;
    uint16_t *lut = malloc(ORB_N * sizeof(uint16_t));
    if (!lut) {
        ESP_LOGE(TAG, "no mem for orb LUT");
        vTaskDelete(NULL);
        return;
    }

    const int cx = SCR_W / 2;
    const int cy = 560;
    const float R = 300.0f;
    float t = 0.0f;

    jarvis_snapshot_t snap;
    while (true) {
        uint16_t *fb = bsp_display_framebuffer();
        if (fb) {
            jarvis_state_copy(&snap);
            state_style_t st;
            style_for(snap.state, &st);

            clear_bg(fb);
            draw_top_bar(fb, &snap);
            build_orb_lut(lut, &st, R, t);
            draw_orb(fb, cx, cy, R, lut);
            draw_spokes(fb, cx, cy, R, t, st.glow);
            draw_bottom(fb, &snap);
            draw_transcript(fb, &snap);
            /* single-framebuffer direct render: punch the dirty region's
             * cache write-back so the panel DMA sees the new pixels */
            bsp_display_flush(0, 0, SCR_W, SCR_H);
        }

        t += 1.0f / JARVIS_UI_FPS;
        vTaskDelay(pdMS_TO_TICKS(1000 / JARVIS_UI_FPS));
    }
}

esp_err_t jarvis_ui_init(void)
{
    BaseType_t ok = xTaskCreatePinnedToCore(ui_task, "jarvis_ui", 8192, NULL, 4,
                                            NULL, 1);
    return (ok == pdPASS) ? ESP_OK : ESP_FAIL;
}
