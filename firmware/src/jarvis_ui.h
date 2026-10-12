/*
 * JARVIS satellite UI — a direct-to-framebuffer arc-reactor renderer.
 *
 * No LVGL: the app owns a single RGB565 framebuffer supplied by the BSP and
 * paints the reactor + status + transcript straight into it. Rendering uses a
 * squared-distance LUT so the per-pixel cost is one array lookup.
 *
 * The ui_* API is intentionally small so a richer (LVGL) renderer can replace
 * this module later without touching the rest of the firmware.
 */
#pragma once

#include "esp_err.h"

esp_err_t jarvis_ui_init(void);
