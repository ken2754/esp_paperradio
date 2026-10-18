#ifndef INPUT_DRIVER_H
#define INPUT_DRIVER_H

#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

typedef enum {
    INPUT_EVT_ROTARY_UP = 0,
    INPUT_EVT_ROTARY_DOWN,
    INPUT_EVT_ROTARY_PUSH,
    INPUT_EVT_BOOT_SHORT,
    INPUT_EVT_BOOT_LONG,
    INPUT_EVT_PWR_SHORT,
    INPUT_EVT_PWR_LONG,
} input_event_t;

esp_err_t input_driver_init(QueueHandle_t evt_queue);

#endif // INPUT_DRIVER_H
