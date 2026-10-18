#include "bsp_i2c.h"
#include "board_pins.h"
#include "esp_log.h"

static const char *TAG = "BSP_I2C";

static i2c_master_bus_handle_t s_i2c_bus = NULL;
static SemaphoreHandle_t s_i2c_mutex = NULL;

#define I2C_XFER_TIMEOUT_MS   100
#define I2C_LOCK_TIMEOUT_MS   200

esp_err_t bsp_i2c_init(void)
{
    if (s_i2c_bus) {
        return ESP_OK;
    }

    s_i2c_mutex = xSemaphoreCreateMutex();
    if (!s_i2c_mutex) {
        return ESP_ERR_NO_MEM;
    }

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_MASTER_PORT,
        .sda_io_num = I2C_MASTER_SDA_GPIO,
        .scl_io_num = I2C_MASTER_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
        return err;
    }

    // Diagnostic scan of expected devices
    const uint8_t addrs[] = { I2C_ADDR_ES8311, I2C_ADDR_AXP2101, I2C_ADDR_PCF85063 };
    const char *names[] = { "ES8311", "AXP2101", "PCF85063" };
    for (size_t i = 0; i < sizeof(addrs); i++) {
        esp_err_t p = i2c_master_probe(s_i2c_bus, addrs[i], I2C_XFER_TIMEOUT_MS);
        ESP_LOGI(TAG, "probe 0x%02X (%s): %s", addrs[i], names[i], p == ESP_OK ? "found" : "missing");
    }
    return ESP_OK;
}

i2c_master_bus_handle_t bsp_i2c_get_bus(void)
{
    return s_i2c_bus;
}

SemaphoreHandle_t bsp_i2c_get_mutex(void)
{
    return s_i2c_mutex;
}

bool bsp_i2c_lock(uint32_t timeout_ms)
{
    if (!s_i2c_mutex) {
        return false;
    }
    return xSemaphoreTake(s_i2c_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void bsp_i2c_unlock(void)
{
    if (s_i2c_mutex) {
        xSemaphoreGive(s_i2c_mutex);
    }
}

esp_err_t bsp_i2c_add_device(uint8_t addr7, i2c_master_dev_handle_t *out_dev)
{
    if (!s_i2c_bus || !out_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr7,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };
    return i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, out_dev);
}

esp_err_t bsp_i2c_read_regs(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *buf, size_t len)
{
    if (!dev) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!bsp_i2c_lock(I2C_LOCK_TIMEOUT_MS)) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = i2c_master_transmit_receive(dev, &reg, 1, buf, len, I2C_XFER_TIMEOUT_MS);
    bsp_i2c_unlock();
    return err;
}

esp_err_t bsp_i2c_write_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    if (!dev) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t buf[2] = { reg, val };
    if (!bsp_i2c_lock(I2C_LOCK_TIMEOUT_MS)) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = i2c_master_transmit(dev, buf, sizeof(buf), I2C_XFER_TIMEOUT_MS);
    bsp_i2c_unlock();
    return err;
}
