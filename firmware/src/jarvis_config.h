/*
 * JARVIS satellite — compile-time configuration and tunables.
 * Runtime-overridable values live in NVS (see jarvis_settings.h); the values
 * here are the factory defaults used on first boot.
 */
#pragma once

/* --- Identity ----------------------------------------------------------- */
#define JARVIS_DEVICE_ID        "jarvis-panel"

/* --- Firmware version --------------------------------------------------- */
/* Compared against the release's version.txt by the OTA updater (jarvis_ota.c).
 * Stamp a release at build time, e.g.
 *   PLATFORMIO_BUILD_FLAGS='-D JARVIS_FW_VERSION=\"0.2.0\"' pio run -e jarvis-panel
 * (tools/release.sh does this for you). */
#ifndef JARVIS_FW_VERSION
#define JARVIS_FW_VERSION       "0.1.0"
#endif

/* --- JARVIS brain (the LXC bridge) -------------------------------------- */
/* Plain ws:// to the jarvis_ai voice bridge (no TLS configured server-side).
 *
 * The value below is an RFC-5737 documentation address (a placeholder, NOT a
 * real host). Point it at your own bridge without committing your private IP:
 *   - build time:  override via the PLATFORMIO_BUILD_FLAGS env var, e.g.
 *                    PLATFORMIO_BUILD_FLAGS='-D JARVIS_DEFAULT_SERVER=\"ws://192.0.2.10:8765/ws\"'
 *     (note the backslash-escaped inner quotes — PlatformIO parses the value
 *      with shlex, so plain quotes are stripped)
 *   - or edit this line locally and simply don't commit it.
 */
#ifndef JARVIS_DEFAULT_SERVER
#define JARVIS_DEFAULT_SERVER   "ws://192.0.2.10:8765/ws"
#endif
#define JARVIS_DEFAULT_CONVERSATION "jarvis-main"

/* --- Wi-Fi (blank => use provisioning / serial config) ------------------ */
#define JARVIS_DEFAULT_WIFI_SSID ""
#define JARVIS_DEFAULT_WIFI_PASS ""

/* --- Audio -------------------------------------------------------------- */
#define JARVIS_SAMPLE_RATE      16000          /* Hz, mono, matches bridge */
#define JARVIS_CHANNELS         1
#define JARVIS_BITS_PER_SAMPLE  16
#define JARVIS_FRAME_SAMPLES    480            /* 30 ms @ 16 kHz           */
#define JARVIS_FRAME_BYTES      (JARVIS_FRAME_SAMPLES * 2)

/* Capture gating */
#define JARVIS_MAX_LISTEN_MS    20000          /* safety cap per utterance */
#define JARVIS_SILENCE_STOP_MS  900            /* auto-stop after silence  */
#define JARVIS_SILENCE_ARM_MS   250            /* ignore initial silence   */
#define JARVIS_RMS_SPEECH       300            /* int16 RMS speech thresh  */
#define JARVIS_RMS_SILENCE      220            /* int16 RMS silence thresh */

/* Playback */
#define JARVIS_TTS_RING_BYTES   (JARVIS_SAMPLE_RATE * 2 * 4)  /* 4 s buffer  */

/* --- WebSocket ---------------------------------------------------------- */
#define JARVIS_WS_RX_BUF        4096
#define JARVIS_WS_TX_BUF        2048
#define JARVIS_WS_RECONNECT_MS  3000
#define JARVIS_WS_NETWORK_TIMEOUT_MS 8000

/* --- UI ----------------------------------------------------------------- */
#define JARVIS_UI_FPS           20
#define JARVIS_UI_LINES         5              /* rolling transcript lines  */

/* --- Wake / trigger ----------------------------------------------------- */
/* v1 trigger = tap the orb / serial console. ESP-SR WakeNet hooks live in
 * jarvis_wake.c behind JARVIS_ENABLE_WAKENET (needs a flashed model). */
#define JARVIS_TAP_DEBOUNCE_MS  350

/* --- OTA (over-the-air firmware updates) -------------------------------- */
/* Base URL of the release that publishes `version.txt` + `firmware.bin`; the
 * updater appends those two names, so this must point at a directory. Defaults
 * to this project's GitHub "latest release". Downloading release assets works
 * unauthenticated once the repo is public; a private repo needs a custom URL or
 * a token. Set to "" to disable updating entirely.
 *
 * Override for a self-hosted mirror, e.g.
 *   PLATFORMIO_BUILD_FLAGS='-D JARVIS_DEFAULT_OTA_URL=\"http://192.0.2.10:8000/fw/\"' */
#ifndef JARVIS_DEFAULT_OTA_URL
#define JARVIS_DEFAULT_OTA_URL \
    "https://github.com/ALeXXBody/jarvis-assistant/releases/latest/download/"
#endif
#define JARVIS_OTA_HTTP_TIMEOUT_MS  20000
#define JARVIS_OTA_USER_AGENT       "jarvis-panel-ota/1.0"

/* --- Feature gates ------------------------------------------------------ */
/* Set by sdkconfig once ESP-Hosted (ESP32-C6) is wired up. */
#if defined(CONFIG_ESP_WIFI_REMOTE_ENABLED) && CONFIG_ESP_WIFI_REMOTE_ENABLED
#define JARVIS_HAVE_WIFI 1
#else
#define JARVIS_HAVE_WIFI 0
#endif
