#ifndef ST7789_H
#define ST7789_H

#include "font_ui.h"
#include <stdbool.h>
#include <stdint.h>

#define TFT_MAX_WIDTH   320
#define TFT_MAX_HEIGHT  240
#define TFT_BAND_HEIGHT 16

#define RGB565(r, g, b) \
  ((uint16_t)((((uint16_t)(r) & 0xF8U) << 8) | (((uint16_t)(g) & 0xFCU) << 3) | (((uint16_t)(b) & 0xF8U) >> 3)))

void ST7789_Init(bool invert_colors);
int16_t ST7789_Width(void);
int16_t ST7789_Height(void);
uint32_t ST7789_ChipId(void);

void ST7789_BeginBand(int16_t y0, int16_t h, uint16_t bg_color);
void ST7789_FlushBand(void);
void ST7789_WaitIdle(void);

void ST7789_FillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
void ST7789_DrawHLine(int16_t x, int16_t y, int16_t w, uint16_t color);
void ST7789_DrawVLine(int16_t x, int16_t y, int16_t h, uint16_t color);
void ST7789_DrawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
int16_t ST7789_MeasureText(const char *utf8, const ui_font_t *font);
int16_t ST7789_DrawText(int16_t x, int16_t y, const char *utf8, const ui_font_t *font,
                        uint16_t fg_color, uint16_t bg_color);

#endif /* ST7789_H */
