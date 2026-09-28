#pragma once
#include <stdint.h>
#include <stdbool.h>

// Pin map recovered from the stock "Smart Weather Clock" firmware
#define LCD_PIN_MOSI 6
#define LCD_PIN_SCLK 4
#define LCD_PIN_DC   5
#define LCD_PIN_RST  1
#define LCD_PIN_BL   18

#define LCD_W 240
#define LCD_H 240
#define BAND_H 20   // rows rendered per DMA chunk (240*20*2 = 9.6 KB, x2 buffers)

void lcd_init(void);
void lcd_backlight(int percent);   // 0..100
void lcd_set_bl_invert(bool inv);

// Frame streaming: call lcd_frame_begin(), then for each band fill the buffer
// returned by lcd_band_buf() and hand it over with lcd_band_push().
void lcd_frame_begin(void);
uint16_t *lcd_band_buf(void);
void lcd_band_push(int rows);
void lcd_frame_end(void);
