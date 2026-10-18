#include "pmic_axp2101.h"
#include "bsp_i2c.h"
#include "board_pins.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "AXP2101";

// --- AXP2101 registers ---
#define AXP_REG_STATUS1          0x00  // bit5: VBUS good, bit3: battery present
#define AXP_REG_STATUS2          0x01  // bit[6:5]: 01=charging, 10=discharging
#define AXP_REG_CHIP_ID          0x03
#define AXP_REG_PWR_OFF          0x10  // bit0: software power off
#define AXP_REG_GAUGE_CTRL       0x18  // bit3: fuel gauge enable
#define AXP_REG_ADC_EN           0x30
#define AXP_REG_VBAT_H           0x34  // [5:0] = VBAT[13:8]
#define AXP_REG_VBAT_L           0x35
#define AXP_REG_IRQ_EN0          0x40
#define AXP_REG_IRQ_EN1          0x41
#define AXP_REG_IRQ_EN2          0x42
#define AXP_REG_IRQ_STS0         0x48
#define AXP_REG_IRQ_STS1         0x49
#define AXP_REG_IRQ_STS2         0x4A
#define AXP_REG_BAT_PERCENT      0xA4

// IRQ status 1 (0x49) power-key bits
#define AXP_IRQ1_PKEY_POS_EDGE   (1 << 0)  // key released
#define AXP_IRQ1_PKEY_NEG_EDGE   (1 << 1)  // key pressed
#define AXP_IRQ1_PKEY_LONG       (1 << 2)
#define AXP_IRQ1_PKEY_SHORT      (1 << 3)

#define PEK_LONG_PRESS_MS        4000
#define PEK_MIN_PRESS_MS         50

static i2c_master_dev_handle_t s_axp = NULL;
static bool s_pek_pressed = false;
static bool s_pek_long_sent = false;
static TickType_t s_pek_press_tick = 0;

static esp_err_t axp_read(uint8_t reg, uint8_t *val)
{
    return bsp_i2c_read_regs(s_axp, reg, val, 1);
}

static esp_err_t axp_write(uint8_t reg, uint8_t val)
{
    return bsp_i2c_write_reg(s_axp, reg, val);
}

esp_err_t pmic_init(void)
{
    esp_err_t err = bsp_i2c_add_device(I2C_ADDR_AXP2101, &s_axp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add device failed: %s", esp_err_to_name(err));
        return err;
    }

    uint8_t id = 0;
    err = axp_read(AXP_REG_CHIP_ID, &id);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "not responding: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "chip id 0x%02X", id);

    // Enable VBAT/VBUS/VSYS/TS ADC channels
    axp_write(AXP_REG_ADC_EN, 0x37);

    // Enable fuel gauge
    uint8_t g = 0;
    if (axp_read(AXP_REG_GAUGE_CTRL, &g) == ESP_OK) {
        axp_write(AXP_REG_GAUGE_CTRL, g | (1 << 3));
    }

    // IRQ: only power-key events are of interest
    axp_write(AXP_REG_IRQ_EN0, 0x00);
    axp_write(AXP_REG_IRQ_EN1, AXP_IRQ1_PKEY_POS_EDGE | AXP_IRQ1_PKEY_NEG_EDGE |
                               AXP_IRQ1_PKEY_LONG | AXP_IRQ1_PKEY_SHORT);
    axp_write(AXP_REG_IRQ_EN2, 0x00);
    axp_write(AXP_REG_IRQ_STS0, 0xFF);
    axp_write(AXP_REG_IRQ_STS1, 0xFF);
    axp_write(AXP_REG_IRQ_STS2, 0xFF);

    ESP_LOGI(TAG, "init done: VBAT=%d mV, %d%%", pmic_get_battery_mv(), pmic_get_battery_percent());
    return ESP_OK;
}

int pmic_get_battery_mv(void)
{
    uint8_t buf[2] = { 0 };
    if (bsp_i2c_read_regs(s_axp, AXP_REG_VBAT_H, buf, 2) != ESP_OK) {
        return -1;
    }
    return ((int)(buf[0] & 0x3F) << 8) | buf[1];
}

int pmic_get_battery_percent(void)
{
    uint8_t sts = 0;
    if (axp_read(AXP_REG_STATUS1, &sts) != ESP_OK) {
        return -1;
    }
    if (!(sts & (1 << 3))) {
        return -1; // no battery
    }

    uint8_t v = 0;
    if (axp_read(AXP_REG_BAT_PERCENT, &v) == ESP_OK) {
        int pct = v & 0x7F;
        if (pct > 0 && pct <= 100) {
            return pct;
        }
    }

    // Fallback: linear estimate from voltage
    int mv = pmic_get_battery_mv();
    if (mv <= 0) {
        return -1;
    }
    if (mv >= 4150) {
        return 100;
    }
    if (mv <= 3300) {
        return 0;
    }
    return (mv - 3300) * 100 / (4150 - 3300);
}

bool pmic_is_vbus_present(void)
{
    uint8_t sts = 0;
    return axp_read(AXP_REG_STATUS1, &sts) == ESP_OK && (sts & (1 << 5));
}

bool pmic_is_charging(void)
{
    uint8_t sts = 0;
    return axp_read(AXP_REG_STATUS2, &sts) == ESP_OK && (((sts >> 5) & 0x03) == 0x01);
}

pmic_pek_event_t pmic_poll_pek(void)
{
    if (!s_axp) {
        return PMIC_PEK_NONE;
    }

    pmic_pek_event_t evt = PMIC_PEK_NONE;
    uint8_t sts = 0;
    if (axp_read(AXP_REG_IRQ_STS1, &sts) == ESP_OK && sts != 0) {
        if (sts & AXP_IRQ1_PKEY_NEG_EDGE) {
            s_pek_pressed = true;
            s_pek_long_sent = false;
            s_pek_press_tick = xTaskGetTickCount();
        }
        if ((sts & AXP_IRQ1_PKEY_POS_EDGE) && s_pek_pressed) {
            uint32_t dur = pdTICKS_TO_MS(xTaskGetTickCount() - s_pek_press_tick);
            s_pek_pressed = false;
            if (!s_pek_long_sent) {
                if (dur >= PEK_LONG_PRESS_MS) {
                    evt = PMIC_PEK_LONG;
                } else if (dur >= PEK_MIN_PRESS_MS) {
                    evt = PMIC_PEK_SHORT;
                }
            }
        }
        axp_write(AXP_REG_IRQ_STS1, sts); // write-1-to-clear
    }

    // Emit LONG while still held so power-off happens without waiting for release
    if (s_pek_pressed && !s_pek_long_sent &&
        pdTICKS_TO_MS(xTaskGetTickCount() - s_pek_press_tick) >= PEK_LONG_PRESS_MS) {
        s_pek_long_sent = true;
        evt = PMIC_PEK_LONG;
    }
    return evt;
}

esp_err_t pmic_power_off(void)
{
    ESP_LOGW(TAG, "power off");
    return axp_write(AXP_REG_PWR_OFF, 0x01);
}
