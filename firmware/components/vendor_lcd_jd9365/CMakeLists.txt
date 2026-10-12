# Vendored JD9365 driver for the GUITION JC8012P4A1C panel — an earlier
# Espressif community version whose DPI timing (60 MHz pixel clock, narrower
# vsync porches) and vendor init table match this exact module, unlike the
# newer generic registry release. Source:
#   github.com/profi-max/JC8012P4A1_BSP_ESP32P4 (Demos_and_Docs …/jd9365)
idf_component_register(SRCS "esp_lcd_jd9365.c"
                       INCLUDE_DIRS "include"
                       REQUIRES "esp_lcd"
                       PRIV_REQUIRES "driver")

# The upstream copy got its version macros from the registry's
# cu_pkg_define_version(); we define them here instead.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    ESP_LCD_JD9365_VER_MAJOR=1
    ESP_LCD_JD9365_VER_MINOR=0
    ESP_LCD_JD9365_VER_PATCH=0)
