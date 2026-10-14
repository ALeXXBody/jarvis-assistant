# PlatformIO / ESP-IDF component registration for the JARVIS app.

set(jarvis_srcs
    "main.c"
    "jarvis_settings.c"
    "jarvis_state.c"
    "jarvis_ws.c"
    "jarvis_audio.c"
    "jarvis_afe.c"
    "jarvis_turn.c"
    "jarvis_ui.c"
    "jarvis_wifi.c"
    "jarvis_wake.c"
    "jarvis_ota.c"
)

set(jarvis_requires
    nvs_flash
    esp_wifi
    esp_event
    esp_netif
    esp_timer
    driver
    esp_driver_i2s
    esp_driver_i2c
    esp_driver_gpio
    esp_driver_ledc
    esp_driver_rmt
    esp_adc
    esp_lcd
    json
    esp_https_ota
    esp_http_client
    app_update
    esp_app_format
    mbedtls
    bsp_guition
    espressif__esp_websocket_client
)

# The ESP-SR audio front-end is only pulled in for the `jarvis-panel-sr` build
# (CONFIG_JARVIS_ENABLE_ESP_SR). The matching dependency entry in
# src/idf_component.yml is gated on the same symbol, so the default build never
# downloads esp-sr/esp-dl and links none of this.
if(CONFIG_JARVIS_ENABLE_ESP_SR)
    list(APPEND jarvis_requires espressif__esp-sr)
endif()

idf_component_register(
    SRCS ${jarvis_srcs}
    INCLUDE_DIRS
        "."
    REQUIRES ${jarvis_requires}
)

# WebSocket client + display/audio drivers are pulled from the ESP component
# registry (see idf_component.yml) and become normal IDF components.
