# PlatformIO / ESP-IDF component registration for the JARVIS app.
idf_component_register(
    SRCS
        "main.c"
        "jarvis_settings.c"
        "jarvis_state.c"
        "jarvis_ws.c"
        "jarvis_audio.c"
        "jarvis_turn.c"
        "jarvis_ui.c"
        "jarvis_wifi.c"
        "jarvis_wake.c"
        "jarvis_ota.c"
    INCLUDE_DIRS
        "."
    REQUIRES
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

# WebSocket client + display/audio drivers are pulled from the ESP component
# registry (see idf_component.yml) and become normal IDF components.
