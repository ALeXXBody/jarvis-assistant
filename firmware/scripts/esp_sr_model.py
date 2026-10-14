"""PlatformIO extra script for the `jarvis-panel-sr` environment.

Why this exists
---------------
esp-sr ships its AFE models as a single packed blob (`srmodels.bin`, produced
by `model/movemodel.py`) that must be written to the `model` flash partition
and is read back at runtime by `esp_srmodel_init("model")`.

ESP-IDF normally arranges that itself: the esp-sr component's CMakeLists adds

    add_custom_target(srmodels_bin ALL DEPENDS .../srmodels/srmodels.bin)
    esptool_py_flash_to_partition(flash "model" ".../srmodels.bin")

Under PlatformIO neither half happens. PlatformIO does not run the CMake `all`
target (it compiles each component through SCons and links the ELF itself), so
the `srmodels_bin` target never fires; and it uploads with its own esptool
invocation, so IDF's `flash` target arguments are ignored.

So this hook does both jobs explicitly:

  1. run `movemodel.py` to pack the models selected in sdkconfig into
     `$BUILD_DIR/srmodels/srmodels.bin`;
  2. teach PlatformIO to flash that file at the `model` partition offset.

Timing notes
------------
Plain (post) `extra_scripts` are loaded *after* the platform builder has
already expanded the load-time `FLASH_EXTRA_IMAGES` loop in
espressif32/builder/main.py, so appending there only affects late consumers
(the merged factory image). The upload command is the subst string
``$UPLOADER $UPLOADERFLAGS $ESP32_APP_OFFSET $SOURCE``, and SCons expands
`$UPLOADERFLAGS` when the target runs, so appending the model image to
`UPLOADERFLAGS` is what actually gets it onto the device. We do both.
"""

import csv
import os
import subprocess
import sys

Import("env")  # noqa: F821  (provided by SCons)

PROJECT_DIR = env.subst("$PROJECT_DIR")
BUILD_DIR = env.subst("$BUILD_DIR")
PIOENV = env.subst("$PIOENV")

PARTITION_CSV = os.path.join(PROJECT_DIR, "partitions.sr.csv")
SDKCONFIG = os.path.join(PROJECT_DIR, "sdkconfig.%s" % PIOENV)
ESP_SR_DIR = os.path.join(PROJECT_DIR, "managed_components", "espressif__esp-sr")
MOVEMODEL = os.path.join(ESP_SR_DIR, "model", "movemodel.py")
MODEL_IMAGE = os.path.join(BUILD_DIR, "srmodels", "srmodels.bin")


def _model_partition_offset():
    """Return the `model` partition offset (as '0x...') from the SR table."""
    with open(PARTITION_CSV, newline="") as fh:
        for row in csv.reader(fh):
            if not row or row[0].strip().startswith("#"):
                continue
            if row[0].strip() == "model":
                return row[3].strip()
    raise RuntimeError("no `model` partition in %s" % PARTITION_CSV)


def _generate_model_image():
    """Pack the sdkconfig-selected AFE models into $BUILD_DIR/srmodels/."""
    if not os.path.isfile(MOVEMODEL):
        raise RuntimeError(
            "esp-sr is not present at %s - is CONFIG_JARVIS_ENABLE_ESP_SR set "
            "in sdkconfig.%s?" % (ESP_SR_DIR, PIOENV)
        )
    if not os.path.isfile(SDKCONFIG):
        raise RuntimeError("missing %s (run a build first)" % SDKCONFIG)

    os.makedirs(os.path.dirname(MODEL_IMAGE), exist_ok=True)
    cmd = [
        sys.executable,
        MOVEMODEL,
        "-d1", SDKCONFIG,   # sdkconfig, to read the model selection
        "-d2", ESP_SR_DIR,  # component root, which holds model/
        "-d3", BUILD_DIR,   # output root -> BUILD_DIR/srmodels/srmodels.bin
    ]
    print("esp-sr: packing AFE models -> %s" % MODEL_IMAGE)
    subprocess.check_call(cmd)

    if not os.path.isfile(MODEL_IMAGE) or os.path.getsize(MODEL_IMAGE) <= 4:
        sys.stderr.write(
            "esp-sr: warning: %s is empty - no AFE models were selected in "
            "sdkconfig.%s (check CONFIG_SR_NSN_* / CONFIG_SR_VADN_*)\n"
            % (MODEL_IMAGE, PIOENV)
        )


offset = _model_partition_offset()
_generate_model_image()

# The upload command (write-flash) gets the model image appended...
env.Append(UPLOADERFLAGS=[offset, MODEL_IMAGE])
# ...and anything that walks FLASH_EXTRA_IMAGES late (e.g. the merged
# `firmware.factory.bin`) picks it up too.
env.Append(FLASH_EXTRA_IMAGES=[(offset, MODEL_IMAGE)])
print("esp-sr: model image will be flashed at %s" % offset)
