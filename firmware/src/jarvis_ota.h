/*
 * OTA firmware updates — pull a release image over HTTPS.
 *
 * The panel fetches one asset from a base URL (`ota_url`, which must end in
 * '/') — by default the project's GitHub Pages mirror of the latest release:
 *
 *     <ota_url>firmware.bin    the application image
 *
 * The *incoming image* carries its own version in the app descriptor; the
 * updater reads it from the stream (esp_https_ota_get_img_desc) before
 * anything is written and compares it with the version of the image that is
 * actually running. Same or older: nothing is flashed. Newer: the inactive
 * OTA slot is written and the device reboots into it. One GET, one source
 * of truth — no auxiliary version file.
 *
 * Rollback safety: with CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE the freshly
 * flashed image boots in "pending verify" state and the bootloader reverts to
 * the previous slot unless jarvis_ota_mark_valid() is called after a healthy
 * boot. main.c calls it once bring-up has completed.
 *
 * Update checks are triggered manually (serial console 'u') and automatically
 * once after boot (jarvis_ota_autocheck_start()).
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* Logs the running partition and whether the image is pending verification.
 * Call early; does not touch the network. */
esp_err_t jarvis_ota_init(void);

/* Confirms the running image is healthy, cancelling any pending rollback.
 * No-op unless the running image is in "pending verify" state. */
void jarvis_ota_mark_valid(void);

/* Starts an update check in the background. Returns false if one is already
 * running (or the task could not be created). Safe to call from any task. */
bool jarvis_ota_check_async(void);

/* Waits for the network to come up, then runs a single check if `ota_url` is
 * configured. Returns immediately; work happens in a small task. */
void jarvis_ota_autocheck_start(void);

/* Version of the RUNNING image, read from the image's own app descriptor
 * (stamped at release time via firmware/version.txt -> PROJECT_VER; falls
 * back to the compile-time JARVIS_FW_VERSION macro on a bare build). This is
 * what the OTA check compares against version.txt, and what the UI reports -
 * the panel never needs to remember what it ran before. */
const char *jarvis_ota_running_version(void);
