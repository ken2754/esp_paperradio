#ifndef BSP_I2C_H
#define BSP_I2C_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/**
 * Shared I2C bus (GPIO41/42) for ES8311, AXP2101 and PCF85063.
 * Every transaction from any task MUST hold the bus mutex
 * (bsp_i2c_lock / bsp_i2c_unlock, or use the register helpers below).
 */
esp_err_t bsp_i2c_init(void);

i2c_master_bus_handle_t bsp_i2c_get_bus(void);
SemaphoreHandle_t bsp_i2c_get_mutex(void);

bool bsp_i2c_lock(uint32_t timeout_ms);
void bsp_i2c_unlock(void);

/** Adds a 7-bit device on the shared bus. */
esp_err_t bsp_i2c_add_device(uint8_t addr7, i2c_master_dev_handle_t *out_dev);

/** Mutex-protected register helpers. */
esp_err_t bsp_i2c_read_regs(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *buf, size_t len);
esp_err_t bsp_i2c_write_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val);

#endif // BSP_I2C_H
