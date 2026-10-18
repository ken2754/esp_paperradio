#include "input_driver.h"
#include "board_pins.h"
#include "pmic_axp2101.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "INPUT";

#define POLL_INTERVAL_MS        20
#define DEBOUNCE_MS             40
#define REPEAT_INITIAL_MS       450
#define REPEAT_INTERVAL_MS      150
#define BOOT_LONG_PRESS_MS      2800

static QueueHandle_t s_queue = NULL;

static void emit(input_event_t ev)
{
    if (s_queue) {
        xQueueSend(s_queue, &ev, 0);
    }
}

typedef struct {
    gpio_num_t pin;
    bool pressed;
    bool repeat_active;
    TickType_t press_tick;
    TickType_t last_repeat_tick;
} repeat_btn_t;

static void poll_repeat_button(repeat_btn_t *b, input_event_t ev, TickType_t now)
{
    int level = gpio_get_level(b->pin);
    if (!b->pressed && level == 0) {
        b->pressed = true;
        b->repeat_active = false;
        b->press_tick = now;
        b->last_repeat_tick = now;
        emit(ev);
    } else if (b->pressed && level == 0) {
        uint32_t held_ms = pdTICKS_TO_MS(now - b->press_tick);
        uint32_t since_rep = pdTICKS_TO_MS(now - b->last_repeat_tick);
        if (!b->repeat_active && held_ms >= REPEAT_INITIAL_MS) {
            b->repeat_active = true;
            b->last_repeat_tick = now;
            emit(ev);
        } else if (b->repeat_active && since_rep >= REPEAT_INTERVAL_MS) {
            b->last_repeat_tick = now;
            emit(ev);
        }
    } else if (b->pressed && level == 1) {
        b->pressed = false;
        b->repeat_active = false;
    }
}

static void input_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(300)); // settle at boot

    repeat_btn_t up_btn = { .pin = GPIO_ROTARY_UP };
    repeat_btn_t down_btn = { .pin = GPIO_ROTARY_DOWN };

    bool push_pressed = false;
    TickType_t push_tick = 0;

    bool boot_pressed = false;
    bool boot_long_sent = false;
    TickType_t boot_tick = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
        TickType_t now = xTaskGetTickCount();

        // Rotary UP / DOWN (with key-repeat when held)
        poll_repeat_button(&up_btn, INPUT_EVT_ROTARY_UP, now);
        poll_repeat_button(&down_btn, INPUT_EVT_ROTARY_DOWN, now);

        // Rotary PUSH
        int push_lv = gpio_get_level(GPIO_ROTARY_PUSH);
        if (!push_pressed && push_lv == 0) {
            push_pressed = true;
            push_tick = now;
        } else if (push_pressed && push_lv == 1) {
            push_pressed = false;
            if (pdTICKS_TO_MS(now - push_tick) >= DEBOUNCE_MS) {
                emit(INPUT_EVT_ROTARY_PUSH);
            }
        }

        // BOOT button: short press = full EPD refresh, long press (2.8s) = start Setup AP
        int boot_lv = gpio_get_level(GPIO_BOOT_BUTTON);
        if (!boot_pressed && boot_lv == 0) {
            boot_pressed = true;
            boot_long_sent = false;
            boot_tick = now;
        } else if (boot_pressed && boot_lv == 0) {
            if (!boot_long_sent && pdTICKS_TO_MS(now - boot_tick) >= BOOT_LONG_PRESS_MS) {
                boot_long_sent = true;
                ESP_LOGI(TAG, "BOOT long press");
                emit(INPUT_EVT_BOOT_LONG);
            }
        } else if (boot_pressed && boot_lv == 1) {
            uint32_t dur = pdTICKS_TO_MS(now - boot_tick);
            boot_pressed = false;
            if (!boot_long_sent && dur >= DEBOUNCE_MS) {
                ESP_LOGI(TAG, "BOOT short press");
                emit(INPUT_EVT_BOOT_SHORT);
            }
        }

        // AXP2101 power key
        pmic_pek_event_t pek = pmic_poll_pek();
        if (pek == PMIC_PEK_SHORT) {
            ESP_LOGI(TAG, "PWR short press");
            emit(INPUT_EVT_PWR_SHORT);
        } else if (pek == PMIC_PEK_LONG) {
            ESP_LOGI(TAG, "PWR long press");
            emit(INPUT_EVT_PWR_LONG);
        }
    }
}

esp_err_t input_driver_init(QueueHandle_t evt_queue)
{
    s_queue = evt_queue;

    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << GPIO_ROTARY_UP) |
                        (1ULL << GPIO_ROTARY_PUSH) |
                        (1ULL << GPIO_ROTARY_DOWN) |
                        (1ULL << GPIO_BOOT_BUTTON) |
                        (1ULL << GPIO_PWR_IRQ),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        return err;
    }

    xTaskCreatePinnedToCore(input_task, "input_poll", 3072, NULL, 6, NULL, 1);
    return ESP_OK;
}
