#include "lcd.h"
#include <string.h>
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "lcd";
static spi_device_handle_t s_spi;
static uint16_t *s_buf[2];
static int s_cur;
static bool s_pending;
static spi_transaction_t s_trans[2];
static bool s_bl_invert;
static int s_bl_pct = 80;

static void lcd_cmd(uint8_t cmd, const uint8_t *data, int len)
{
    spi_transaction_t t = { .length = 8, .tx_buffer = &cmd };
    gpio_set_level(LCD_PIN_DC, 0);
    spi_device_polling_transmit(s_spi, &t);
    if (len) {
        spi_transaction_t d = { .length = len * 8, .tx_buffer = data };
        gpio_set_level(LCD_PIN_DC, 1);
        spi_device_polling_transmit(s_spi, &d);
    }
}

static void bl_init(void)
{
    ledc_timer_config_t tc = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_13_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 1000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&tc));
    ledc_channel_config_t cc = {
        .gpio_num = LCD_PIN_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&cc));
}

void lcd_backlight(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    s_bl_pct = pct;
    uint32_t duty = (uint32_t)pct * 8191 / 100;
    if (s_bl_invert) duty = 8191 - duty;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

void lcd_set_bl_invert(bool inv)
{
    s_bl_invert = inv;
    lcd_backlight(s_bl_pct);
}

void lcd_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << LCD_PIN_DC) | (1ULL << LCD_PIN_RST),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);

    spi_bus_config_t bus = {
        .mosi_io_num = LCD_PIN_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = LCD_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_W * BAND_H * 2 + 16,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));
    spi_device_interface_config_t dev = {
        .clock_speed_hz = 40 * 1000 * 1000,
        .mode = 3,
        .spics_io_num = -1,
        .queue_size = 2,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev, &s_spi));

    for (int i = 0; i < 2; i++) {
        s_buf[i] = heap_caps_malloc(LCD_W * BAND_H * 2, MALLOC_CAP_DMA);
        assert(s_buf[i]);
    }

    bl_init();

    gpio_set_level(LCD_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(LCD_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    lcd_cmd(0x11, NULL, 0);                 // SLPOUT
    vTaskDelay(pdMS_TO_TICKS(150));
    lcd_cmd(0x3A, (uint8_t[]){0x55}, 1);    // RGB565
    lcd_cmd(0x36, (uint8_t[]){0x00}, 1);    // MADCTL
    lcd_cmd(0x21, NULL, 0);                 // INVON (panel needs it)
    lcd_cmd(0x13, NULL, 0);                 // NORON

    // clear to black before turning the panel on
    lcd_frame_begin();
    for (int y = 0; y < LCD_H; y += BAND_H) {
        memset(lcd_band_buf(), 0, LCD_W * BAND_H * 2);
        lcd_band_push(BAND_H);
    }
    lcd_frame_end();
    lcd_cmd(0x29, NULL, 0);                 // DISPON
    ESP_LOGI(TAG, "ST7789 up");
}

void lcd_frame_begin(void)
{
    lcd_cmd(0x2A, (uint8_t[]){0, 0, 0, LCD_W - 1}, 4);
    lcd_cmd(0x2B, (uint8_t[]){0, 0, 0, LCD_H - 1}, 4);
    lcd_cmd(0x2C, NULL, 0);
    gpio_set_level(LCD_PIN_DC, 1);
    s_pending = false;
    s_cur = 0;
}

uint16_t *lcd_band_buf(void)
{
    return s_buf[s_cur];
}

void lcd_band_push(int rows)
{
    spi_transaction_t *r;
    if (s_pending) spi_device_get_trans_result(s_spi, &r, portMAX_DELAY);
    spi_transaction_t *t = &s_trans[s_cur];
    memset(t, 0, sizeof(*t));
    t->length = LCD_W * rows * 16;
    t->tx_buffer = s_buf[s_cur];
    spi_device_queue_trans(s_spi, t, portMAX_DELAY);
    s_pending = true;
    s_cur ^= 1;
}

void lcd_frame_end(void)
{
    spi_transaction_t *r;
    if (s_pending) spi_device_get_trans_result(s_spi, &r, portMAX_DELAY);
    s_pending = false;
}
