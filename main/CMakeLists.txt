idf_component_register(
    SRCS
        "app_main.c"
        "radio_app.c"
        "bsp_i2c.c"
        "pmic_axp2101.c"
        "epd_ssd1677.c"
        "gfx.c"
        "fonts/font_terminus_16.c"
        "fonts/font_terminus_24.c"
        "fonts/font_terminus_32.c"
        "ui_display.c"
        "input_driver.c"
        "audio_player.c"
        "stream_client.c"
        "artwork_manager.c"
        "settings_manager.c"
        "wifi_manager.c"
        "web_portal.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_driver_gpio esp_driver_i2c esp_driver_i2s esp_driver_spi
        esp_wifi esp_netif esp_event nvs_flash esp_timer esp_psram
        esp_http_client esp_http_server esp-tls mbedtls json log heap
    EMBED_TXTFILES "web/index.html"
)
