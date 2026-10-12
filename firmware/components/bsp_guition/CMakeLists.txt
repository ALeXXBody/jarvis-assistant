# BSP for the GUITION JC8012P4A1C_I_W_Y (ESP32-P4 satellite).
#
# Hardware bring-up is split one driver per file so pieces can be disabled
# individually while integrating on real hardware.
set(BSP_SRCS
    "bsp_guition.c"
    "bsp_display.c"
    "bsp_touch.c"
    "bsp_audio.c"
    "bsp_led.c"
    "bsp_sys.c"
    "gsl3680_fw.c"
)

idf_component_register(
    SRCS ${BSP_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES
        driver
        esp_driver_i2c
        esp_driver_i2s
        esp_driver_gpio
        esp_driver_ledc
        esp_driver_rmt
        esp_adc
        esp_lcd
        esp_hw_support
        vendor_lcd_jd9365
        espressif__esp_codec_dev
)
