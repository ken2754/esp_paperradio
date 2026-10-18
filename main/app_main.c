#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board_pins.h"
#include "bsp_i2c.h"
#include "pmic_axp2101.h"
#include "settings_manager.h"
#include "ui_display.h"
#include "radio_app.h"
#include "wifi_manager.h"
#include "web_portal.h"

static const char *TAG = "MAIN";

void app_main(void)
{
    ESP_LOGI(TAG, "=== PaperRadio booting on Waveshare ESP32-S3 e-Paper 3.97 ===");

    ESP_ERROR_CHECK(settings_init());
    ESP_ERROR_CHECK(bsp_i2c_init());
    pmic_init();

    ESP_ERROR_CHECK(ui_display_init());
    ESP_ERROR_CHECK(radio_app_init());

    // Hold BOOT at startup to force Setup SoftAP mode
    bool force_ap = (gpio_get_level(GPIO_BOOT_BUTTON) == 0);
    if (force_ap) {
        ESP_LOGI(TAG, "BOOT held at startup -> forcing Setup SoftAP mode");
        wifi_manager_enable_ap();
    } else {
        wifi_manager_start();
    }

    ESP_ERROR_CHECK(web_portal_start());
    radio_app_request_ui_refresh(true);
}
