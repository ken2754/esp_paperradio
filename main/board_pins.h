#ifndef BOARD_PINS_H
#define BOARD_PINS_H

/*
 * Waveshare ESP32-S3 e-Paper 3.97" - pin assignment.
 * All GPIO numbers MUST be referenced through this header (no magic numbers).
 */

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "hal/spi_types.h"

// --- Rotary switch (active low, internal pull-up) ---
#define GPIO_ROTARY_UP          GPIO_NUM_4
#define GPIO_ROTARY_PUSH        GPIO_NUM_5
#define GPIO_ROTARY_DOWN        GPIO_NUM_6

// --- System buttons ---
#define GPIO_BOOT_BUTTON        GPIO_NUM_0
#define GPIO_PWR_IRQ            GPIO_NUM_38  // AXP2101 IRQ (active low)

// --- USB ---
#define GPIO_USB_D_MINUS        GPIO_NUM_19
#define GPIO_USB_D_PLUS         GPIO_NUM_20

// --- Shared I2C bus (ES8311 / AXP2101 / PCF85063) ---
#define I2C_MASTER_SDA_GPIO     GPIO_NUM_41
#define I2C_MASTER_SCL_GPIO     GPIO_NUM_42
#define I2C_MASTER_PORT         I2C_NUM_0
#define I2C_MASTER_FREQ_HZ      100000

#define I2C_ADDR_ES8311         0x18
#define I2C_ADDR_AXP2101        0x34
#define I2C_ADDR_PCF85063       0x51

// --- e-Paper SSD1677 (SPI2_HOST / FSPI) ---
#define EPD_BUSY_GPIO           GPIO_NUM_3
#define EPD_DC_GPIO             GPIO_NUM_9
#define EPD_CS_GPIO             GPIO_NUM_10
#define EPD_SCK_GPIO            GPIO_NUM_11
#define EPD_DIN_MOSI_GPIO       GPIO_NUM_12
#define EPD_RST_GPIO            GPIO_NUM_46
#define EPD_SPI_HOST            SPI2_HOST
#define EPD_SPI_CLOCK_HZ        (10 * 1000 * 1000)

// --- Audio: ES8311 codec + NS4150B amplifier ---
#define I2S_MCLK_GPIO           GPIO_NUM_13
#define I2S_SCLK_BCLK_GPIO      GPIO_NUM_14
#define I2S_LRCK_WS_GPIO        GPIO_NUM_47
#define I2S_DSOUT_DOUT_GPIO     GPIO_NUM_48  // ESP32 -> ES8311 DAC
#define I2S_DSDIN_DIN_GPIO      GPIO_NUM_21  // ES8311 ADC -> ESP32 (mic, unused)
#define PIN_AUDIO_CTR           GPIO_NUM_39  // AudioCTR / PA_EN (NS4150B)

// --- MicroSD (SDSPI, SPI3_HOST) - reserved, not used yet ---
#define SDSPI_MISO_GPIO         GPIO_NUM_15
#define SDSPI_CLK_GPIO          GPIO_NUM_16
#define SDSPI_MOSI_GPIO         GPIO_NUM_17
#define SDSPI_CS_GPIO           GPIO_NUM_18
#define SDSPI_HOST              SPI3_HOST

#endif // BOARD_PINS_H
