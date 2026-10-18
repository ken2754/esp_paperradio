#include <string.h>
#include "epd_ssd1677.h"
#include "board_pins.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "EPD";

// SSD1677 commands
#define CMD_BOOSTER_SOFT_START   0x0C
#define CMD_DEEP_SLEEP           0x10
#define CMD_SW_RESET             0x12
#define CMD_TEMP_SENSOR_CTRL     0x18
#define CMD_TEMP_SENSOR_WRITE    0x1A
#define CMD_MASTER_ACTIVATION    0x20
#define CMD_DISPLAY_UPDATE_CTRL2 0x22
#define CMD_WRITE_RAM_BW         0x24  // 4-Gray Plane 0 (LSB of 2-bit gray)
#define CMD_WRITE_RAM_RED        0x26  // 4-Gray Plane 1 (MSB of 2-bit gray)
#define CMD_BORDER_WAVEFORM      0x3C
#define CMD_SET_RAM_X_COUNT      0x4E
#define CMD_SET_RAM_Y_COUNT      0x4F

#define UPDATE_4GRAY             0xD7
#define UPDATE_PARTIAL           0xFF

#define BUSY_TIMEOUT_MS          8000
#define SPI_CHUNK                4096

#define CHECK(x) do { esp_err_t __e = (x); if (__e != ESP_OK) return __e; } while (0)

static SemaphoreHandle_t s_mutex = NULL;
static spi_device_handle_t s_spi = NULL;
static uint8_t *s_plane_buf = NULL;
static bool s_sleeping = true;
static int s_partial_count = 0;
static bool s_force_full = true;

static esp_err_t wait_busy(void)
{
    vTaskDelay(pdMS_TO_TICKS(10));
    TickType_t start = xTaskGetTickCount();
    while (gpio_get_level(EPD_BUSY_GPIO) == 1) {   // 1 = busy, 0 = idle
        vTaskDelay(pdMS_TO_TICKS(10));
        if (pdTICKS_TO_MS(xTaskGetTickCount() - start) > BUSY_TIMEOUT_MS) {
            ESP_LOGE(TAG, "busy timeout");
            return ESP_ERR_TIMEOUT;
        }
    }
    return ESP_OK;
}

static esp_err_t spi_tx(const uint8_t *data, size_t len, bool is_data)
{
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = data,
    };
    gpio_set_level(EPD_DC_GPIO, is_data ? 1 : 0);
    gpio_set_level(EPD_CS_GPIO, 0);
    esp_err_t err = spi_device_polling_transmit(s_spi, &t);
    gpio_set_level(EPD_CS_GPIO, 1);
    return err;
}

static esp_err_t cmd(uint8_t c)
{
    return spi_tx(&c, 1, false);
}

static esp_err_t data1(uint8_t d)
{
    return spi_tx(&d, 1, true);
}

static esp_err_t data_buf(const uint8_t *buf, size_t len)
{
    for (size_t off = 0; off < len; off += SPI_CHUNK) {
        size_t n = (len - off > SPI_CHUNK) ? SPI_CHUNK : (len - off);
        CHECK(spi_tx(buf + off, n, true));
    }
    return ESP_OK;
}

static void hw_reset(void)
{
    gpio_set_level(EPD_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(EPD_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(5));
    gpio_set_level(EPD_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
}

static esp_err_t reset_ram_counter(void)
{
    CHECK(cmd(CMD_SET_RAM_X_COUNT));
    CHECK(data1(0x00));
    CHECK(data1(0x00));
    CHECK(cmd(CMD_SET_RAM_Y_COUNT));
    CHECK(data1(0x00));
    CHECK(data1(0x00));
    return ESP_OK;
}

static esp_err_t panel_setup_4gray(void)
{
    hw_reset();
    CHECK(wait_busy());
    // SW reset loads 800x480 geometry & RAM entry mode from OTP
    CHECK(cmd(CMD_SW_RESET));
    CHECK(wait_busy());

    // Booster soft-start configuration (from Waveshare EPD_Init_4GRAY)
    static const uint8_t soft_start[] = { 0xAE, 0xC7, 0xC3, 0xC0, 0x80 };
    CHECK(cmd(CMD_BOOSTER_SOFT_START));
    CHECK(data_buf(soft_start, sizeof(soft_start)));

    CHECK(reset_ram_counter());
    CHECK(wait_busy());

    CHECK(cmd(CMD_BORDER_WAVEFORM));
    CHECK(data1(0x01));

    // Select internal temperature sensor and load 4-Gray LUT (0x5A)
    CHECK(cmd(CMD_TEMP_SENSOR_CTRL));
    CHECK(data1(0x80));
    CHECK(cmd(CMD_TEMP_SENSOR_WRITE));
    CHECK(data1(0x5A));

    s_sleeping = false;
    return ESP_OK;
}

// Extracts bits 6, 4, 2, 0 of `b` into a 4-bit nibble (bits 3..0)
static inline uint8_t pack_even_bits(uint8_t b)
{
    return (uint8_t)(((b & 0x40) >> 3) |
                     ((b & 0x10) >> 2) |
                     ((b & 0x04) >> 1) |
                     (b & 0x01));
}

/*
 * Waveshare SSD1677 4-Gray bitplane mapping:
 *   Color 0 (White):      RAM_BW(0x24)=0, RAM_RED(0x26)=0
 *   Color 1 (Light Gray): RAM_BW(0x24)=1, RAM_RED(0x26)=0
 *   Color 2 (Dark Gray):  RAM_BW(0x24)=0, RAM_RED(0x26)=1
 *   Color 3 (Black):      RAM_BW(0x24)=1, RAM_RED(0x26)=1
 */
static void extract_plane_bw(const uint8_t *frame_2bpp, uint8_t *plane_out)
{
    for (size_t i = 0; i < EPD_PLANE_BUFFER_SIZE; i++) {
        uint8_t b0 = frame_2bpp[2 * i];
        uint8_t b1 = frame_2bpp[2 * i + 1];
        plane_out[i] = (uint8_t)((pack_even_bits(b0) << 4) | pack_even_bits(b1));
    }
}

static void extract_plane_red(const uint8_t *frame_2bpp, uint8_t *plane_out)
{
    for (size_t i = 0; i < EPD_PLANE_BUFFER_SIZE; i++) {
        uint8_t b0 = (uint8_t)(frame_2bpp[2 * i] >> 1);
        uint8_t b1 = (uint8_t)(frame_2bpp[2 * i + 1] >> 1);
        plane_out[i] = (uint8_t)((pack_even_bits(b0) << 4) | pack_even_bits(b1));
    }
}

esp_err_t epd_init(void)
{
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
        if (!s_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (!s_plane_buf) {
        s_plane_buf = heap_caps_malloc(EPD_PLANE_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_plane_buf) {
            return ESP_ERR_NO_MEM;
        }
    }

    gpio_config_t out = {
        .pin_bit_mask = (1ULL << EPD_CS_GPIO) | (1ULL << EPD_DC_GPIO) | (1ULL << EPD_RST_GPIO),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out);
    gpio_set_level(EPD_CS_GPIO, 1);
    gpio_set_level(EPD_RST_GPIO, 1);

    gpio_config_t in = {
        .pin_bit_mask = (1ULL << EPD_BUSY_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&in);

    if (!s_spi) {
        spi_bus_config_t bus = {
            .mosi_io_num = EPD_DIN_MOSI_GPIO,
            .miso_io_num = -1,
            .sclk_io_num = EPD_SCK_GPIO,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = SPI_CHUNK,
        };
        CHECK(spi_bus_initialize(EPD_SPI_HOST, &bus, SPI_DMA_CH_AUTO));

        spi_device_interface_config_t dev = {
            .mode = 0,
            .clock_speed_hz = EPD_SPI_CLOCK_HZ,
            .spics_io_num = -1,      // CS driven manually
            .queue_size = 4,
        };
        CHECK(spi_bus_add_device(EPD_SPI_HOST, &dev, &s_spi));
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_err_t err = panel_setup_4gray();
    s_force_full = true;
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "init 4-gray %s", err == ESP_OK ? "ok" : esp_err_to_name(err));
    return err;
}

static esp_err_t display_locked(const uint8_t *frame_2bpp, bool partial)
{
    if (s_sleeping) {
        s_force_full = true;
    }

    bool full = !partial || s_force_full || (s_partial_count >= EPD_FULL_REFRESH_INTERVAL);

    if (full) {
        ESP_LOGI(TAG, "4-gray full refresh");
        s_partial_count = 0;
        s_force_full = false;

        CHECK(panel_setup_4gray());

        extract_plane_bw(frame_2bpp, s_plane_buf);
        CHECK(cmd(CMD_WRITE_RAM_BW));
        CHECK(data_buf(s_plane_buf, EPD_PLANE_BUFFER_SIZE));

        extract_plane_red(frame_2bpp, s_plane_buf);
        CHECK(cmd(CMD_WRITE_RAM_RED));
        CHECK(data_buf(s_plane_buf, EPD_PLANE_BUFFER_SIZE));

        CHECK(cmd(CMD_DISPLAY_UPDATE_CTRL2));
        CHECK(data1(UPDATE_4GRAY));
        CHECK(cmd(CMD_MASTER_ACTIVATION));
        CHECK(wait_busy());

        // Prime both RAM planes with the 1bpp thresholded image (~plane_red) so subsequent
        // Mode 2 differential partial refreshes (0xFF) only drive pixels that change
        // and leave unchanged 4-gray regions (such as album artwork) untouched at 0V.
        for (size_t i = 0; i < EPD_PLANE_BUFFER_SIZE; i++) {
            s_plane_buf[i] = (uint8_t)~s_plane_buf[i];
        }
        CHECK(reset_ram_counter());
        CHECK(cmd(CMD_WRITE_RAM_BW));
        CHECK(data_buf(s_plane_buf, EPD_PLANE_BUFFER_SIZE));
        CHECK(reset_ram_counter());
        CHECK(cmd(CMD_WRITE_RAM_RED));
        CHECK(data_buf(s_plane_buf, EPD_PLANE_BUFFER_SIZE));
    } else {
        s_partial_count++;
        ESP_LOGD(TAG, "partial refresh #%d", s_partial_count);

        extract_plane_red(frame_2bpp, s_plane_buf);
        for (size_t i = 0; i < EPD_PLANE_BUFFER_SIZE; i++) {
            s_plane_buf[i] = (uint8_t)~s_plane_buf[i];
        }

        CHECK(cmd(CMD_TEMP_SENSOR_CTRL));
        CHECK(data1(0x80));
        CHECK(cmd(CMD_BORDER_WAVEFORM));
        CHECK(data1(0x80));
        CHECK(reset_ram_counter());
        CHECK(cmd(CMD_WRITE_RAM_BW));
        CHECK(data_buf(s_plane_buf, EPD_PLANE_BUFFER_SIZE));
        CHECK(cmd(CMD_DISPLAY_UPDATE_CTRL2));
        CHECK(data1(UPDATE_PARTIAL));
        CHECK(cmd(CMD_MASTER_ACTIVATION));
        CHECK(wait_busy());

        // Sync "previous" RAM (0x26) for the next differential update
        CHECK(reset_ram_counter());
        CHECK(cmd(CMD_WRITE_RAM_RED));
        CHECK(data_buf(s_plane_buf, EPD_PLANE_BUFFER_SIZE));
    }
    return ESP_OK;
}

esp_err_t epd_display(const uint8_t *frame, bool partial)
{
    if (!frame || !s_mutex || !s_plane_buf) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_err_t err = display_locked(frame, partial);
    xSemaphoreGive(s_mutex);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "display failed: %s", esp_err_to_name(err));
    }
    return err;
}

void epd_request_full_refresh(void)
{
    s_force_full = true;
}

void epd_sleep(void)
{
    if (!s_mutex) {
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!s_sleeping) {
        cmd(CMD_DEEP_SLEEP);
        data1(0x01);
        s_sleeping = true;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    xSemaphoreGive(s_mutex);
}
