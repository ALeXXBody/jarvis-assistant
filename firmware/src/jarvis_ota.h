/*
 * OTA firmware updates — pull a release image over HTTPS.
 *
 * The panel fetches two assets from a base URL (`ota_url`, which must end in
 * '/') — by default the project's GitHub "latest release":
 *
 *     <ota_url>version.txt     plain text, e.g. "0.1.1"
 *     <ota_url>firmware.bin    the application image
 *
 * `<ota_url>version.txt` is compared with the version compiled into the running
 * image (`JARVIS_FW_VERSION`). If the remote is newer, `firmware.bin` is written
 * to the *inactive* OTA slot and the device reboots into it.
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

/* The version string compiled into this image (JARVIS_FW_VERSION). */
const char *jarvis_ota_running_version(void);
