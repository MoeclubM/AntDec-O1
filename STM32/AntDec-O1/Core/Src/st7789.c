#include "st7789.h"
#include "board_hw.h"

static uint16_t s_band_buf[2][TFT_MAX_WIDTH * TFT_BAND_HEIGHT];
static uint8_t s_active_idx = 0U;
static int16_t s_width = 320;
static int16_t s_height = 240;
static uint16_t s_x_ofs = 0U;
static uint16_t s_y_ofs = 0U;
static int16_t s_band_y0 = 0;
static int16_t s_band_y1 = 0;
static volatile bool s_dma_busy = false;
volatile uint32_t g_tft_rddid = 0U;
volatile uint32_t g_tft_rddst = 0U;

static uint32_t tft_read_reg_bits(uint8_t cmd, uint8_t bits, uint8_t dummy_clks)
{
  (void)HAL_SPI_DeInit(&hspi1);
  GPIO_InitTypeDef gpio = {0};
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5 | GPIO_PIN_7, GPIO_PIN_RESET);
  gpio.Pin = GPIO_PIN_5 | GPIO_PIN_7;
  gpio.Mode = GPIO_MODE_OUTPUT_PP;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(GPIOA, &gpio);

  HAL_GPIO_WritePin(TFT_DC_PORT, TFT_DC_PIN, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(TFT_CS_PORT, TFT_CS_PIN, GPIO_PIN_RESET);
  Board_DelayUs(2U);

  for (int8_t i = 7; i >= 0; i--) {
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_7, ((cmd >> i) & 1U) ? GPIO_PIN_SET : GPIO_PIN_RESET);
    Board_DelayUs(2U);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, GPIO_PIN_SET);
    Board_DelayUs(2U);
  }
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, GPIO_PIN_RESET);

  gpio.Pin = GPIO_PIN_7;
  gpio.Mode = GPIO_MODE_INPUT;
  gpio.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOA, &gpio);
  HAL_GPIO_WritePin(TFT_DC_PORT, TFT_DC_PIN, GPIO_PIN_SET);
  Board_DelayUs(2U);

  for (uint8_t d = 0U; d < dummy_clks; d++) {
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, GPIO_PIN_SET);
    Board_DelayUs(2U);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, GPIO_PIN_RESET);
    Board_DelayUs(2U);
  }

  uint32_t val = 0U;
  for (uint8_t b = 0U; b < bits; b++) {
    val <<= 1;
    if (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_7) == GPIO_PIN_SET) {
      val |= 1U;
    }
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, GPIO_PIN_SET);
    Board_DelayUs(2U);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, GPIO_PIN_RESET);
    Board_DelayUs(2U);
  }

  HAL_GPIO_WritePin(TFT_CS_PORT, TFT_CS_PIN, GPIO_PIN_SET);
  Board_DelayUs(2U);
  MX_SPI1_Init();
  return val;
}

static inline uint16_t swap16(uint16_t v)
{
  return (uint16_t)((v << 8) | (v >> 8));
}

static void tft_wait_dma(void)
{
  uint32_t t0 = HAL_GetTick();
  while (s_dma_busy) {
    if ((HAL_GetTick() - t0) > 25U) {
      (void)HAL_SPI_DMAStop(&hspi1);
      HAL_GPIO_WritePin(TFT_CS_PORT, TFT_CS_PIN, GPIO_PIN_SET);
      s_dma_busy = false;
      break;
    }
  }
  while ((SPI1->SR & SPI_SR_BSY) != 0U) {
    if ((HAL_GetTick() - t0) > 25U) {
      break;
    }
  }
}

static void tft_write_cmd(uint8_t cmd, const uint8_t *params, uint16_t len)
{
  tft_wait_dma();
  HAL_GPIO_WritePin(TFT_DC_PORT, TFT_DC_PIN, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(TFT_CS_PORT, TFT_CS_PIN, GPIO_PIN_RESET);
  Board_DelayUs(1U);
  (void)HAL_SPI_Transmit(&hspi1, &cmd, 1U, 10U);
  while ((SPI1->SR & SPI_SR_BSY) != 0U) {
  }
  if (params != NULL && len > 0U) {
    Board_DelayUs(1U);
    HAL_GPIO_WritePin(TFT_DC_PORT, TFT_DC_PIN, GPIO_PIN_SET);
    Board_DelayUs(1U);
    (void)HAL_SPI_Transmit(&hspi1, (uint8_t *)params, len, 10U);
    while ((SPI1->SR & SPI_SR_BSY) != 0U) {
    }
  }
  Board_DelayUs(1U);
  HAL_GPIO_WritePin(TFT_CS_PORT, TFT_CS_PIN, GPIO_PIN_SET);
  Board_DelayUs(1U);
}

static void tft_cmd_u8(uint8_t cmd, uint8_t val)
{
  tft_write_cmd(cmd, &val, 1U);
}

static void tft_clear_gram(uint16_t raw_w, uint16_t raw_h, uint16_t color)
{
  uint8_t caset[4] = {0U, 0U, (uint8_t)((raw_w - 1U) >> 8), (uint8_t)((raw_w - 1U) & 0xFFU)};
  uint8_t raset[4] = {0U, 0U, (uint8_t)((raw_h - 1U) >> 8), (uint8_t)((raw_h - 1U) & 0xFFU)};
  tft_write_cmd(0x2AU, caset, 4U);
  tft_write_cmd(0x2BU, raset, 4U);

  uint16_t be = swap16(color);
  for (uint32_t i = 0U; i < raw_w; i++) {
    s_band_buf[0][i] = be;
  }

  HAL_GPIO_WritePin(TFT_DC_PORT, TFT_DC_PIN, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(TFT_CS_PORT, TFT_CS_PIN, GPIO_PIN_RESET);
  Board_DelayUs(1U);
  uint8_t ramwr = 0x2CU;
  (void)HAL_SPI_Transmit(&hspi1, &ramwr, 1U, 10U);
  while ((SPI1->SR & SPI_SR_BSY) != 0U) {
  }
  Board_DelayUs(1U);
  HAL_GPIO_WritePin(TFT_DC_PORT, TFT_DC_PIN, GPIO_PIN_SET);
  Board_DelayUs(1U);
  for (uint16_t row = 0U; row < raw_h; row++) {
    (void)HAL_SPI_Transmit(&hspi1, (uint8_t *)s_band_buf[0], (uint16_t)(raw_w * 2U), 20U);
  }
  while ((SPI1->SR & SPI_SR_BSY) != 0U) {
  }
  Board_DelayUs(1U);
  HAL_GPIO_WritePin(TFT_CS_PORT, TFT_CS_PIN, GPIO_PIN_SET);
}

void ST7789_Init(bool invert_colors)
{
  s_dma_busy = false;
  HAL_GPIO_WritePin(TFT_CS_PORT, TFT_CS_PIN, GPIO_PIN_SET);
  HAL_GPIO_WritePin(TFT_DC_PORT, TFT_DC_PIN, GPIO_PIN_SET);

  HAL_GPIO_WritePin(TFT_RST_PORT, TFT_RST_PIN, GPIO_PIN_SET);
  HAL_Delay(5U);
  HAL_GPIO_WritePin(TFT_RST_PORT, TFT_RST_PIN, GPIO_PIN_RESET);
  HAL_Delay(20U);
  HAL_GPIO_WritePin(TFT_RST_PORT, TFT_RST_PIN, GPIO_PIN_SET);
  HAL_Delay(150U);

  tft_write_cmd(0x01U, NULL, 0U);
  HAL_Delay(150U);
  tft_write_cmd(0x11U, NULL, 0U);
  HAL_Delay(150U);

  g_tft_rddid = tft_read_reg_bits(0x04U, 24U, 1U);
  g_tft_rddst = tft_read_reg_bits(0x09U, 32U, 1U);

  uint8_t id1 = (uint8_t)((g_tft_rddid >> 16) & 0xFFU);
  if (id1 == 0x7CU || id1 == 0x5CU) {
    /* Sitronix ST7735 / ST7735S 160x128 landscape (162x132 GRAM) */
    s_width = 160;
    s_height = 128;
    s_x_ofs = 0U;
    s_y_ofs = 0U;
    tft_cmd_u8(0x3AU, 0x05U);
    tft_cmd_u8(0x36U, 0x60U);
    tft_write_cmd(0x20U, NULL, 0U);
    tft_clear_gram(162U, 132U, RGB565(13, 17, 23));
  } else {
    /* Sitronix ST7789 320x240 landscape */
    s_width = 320;
    s_height = 240;
    s_x_ofs = 0U;
    s_y_ofs = 0U;
    tft_cmd_u8(0x3AU, 0x55U);
    tft_cmd_u8(0x36U, 0x60U);
    tft_write_cmd(invert_colors ? 0x21U : 0x20U, NULL, 0U);
    tft_clear_gram(320U, 240U, RGB565(13, 17, 23));
  }

  tft_write_cmd(0x13U, NULL, 0U);
  HAL_Delay(10U);
  tft_write_cmd(0x29U, NULL, 0U);
  HAL_Delay(25U);
}

int16_t ST7789_Width(void)
{
  return s_width;
}

int16_t ST7789_Height(void)
{
  return s_height;
}

uint32_t ST7789_ChipId(void)
{
  return g_tft_rddid;
}

void ST7789_BeginBand(int16_t y0, int16_t h, uint16_t bg_color)
{
  s_band_y0 = y0;
  s_band_y1 = (int16_t)(y0 + h);
  uint16_t be = swap16(bg_color);
  uint16_t *dst = s_band_buf[s_active_idx];
  uint32_t count = (uint32_t)s_width * (uint32_t)h;
  for (uint32_t i = 0U; i < count; i++) {
    dst[i] = be;
  }
}

void ST7789_FlushBand(void)
{
  int16_t h = (int16_t)(s_band_y1 - s_band_y0);
  if (h <= 0) {
    return;
  }
  uint16_t x_start = s_x_ofs;
  uint16_t x_end = (uint16_t)(s_x_ofs + (uint16_t)s_width - 1U);
  uint16_t y_start = (uint16_t)(s_y_ofs + (uint16_t)s_band_y0);
  uint16_t y_end = (uint16_t)(s_y_ofs + (uint16_t)s_band_y1 - 1U);
  uint8_t caset[4] = {
    (uint8_t)(x_start >> 8),
    (uint8_t)(x_start & 0xFFU),
    (uint8_t)(x_end >> 8),
    (uint8_t)(x_end & 0xFFU),
  };
  uint8_t raset[4] = {
    (uint8_t)(y_start >> 8),
    (uint8_t)(y_start & 0xFFU),
    (uint8_t)(y_end >> 8),
    (uint8_t)(y_end & 0xFFU),
  };

  tft_wait_dma();
  tft_write_cmd(0x2AU, caset, 4U);
  tft_write_cmd(0x2BU, raset, 4U);

  HAL_GPIO_WritePin(TFT_DC_PORT, TFT_DC_PIN, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(TFT_CS_PORT, TFT_CS_PIN, GPIO_PIN_RESET);
  Board_DelayUs(1U);
  uint8_t ramwr = 0x2CU;
  (void)HAL_SPI_Transmit(&hspi1, &ramwr, 1U, 10U);
  while ((SPI1->SR & SPI_SR_BSY) != 0U) {
  }

  Board_DelayUs(1U);
  HAL_GPIO_WritePin(TFT_DC_PORT, TFT_DC_PIN, GPIO_PIN_SET);
  Board_DelayUs(1U);
  s_dma_busy = true;
  uint8_t tx_idx = s_active_idx;
  s_active_idx ^= 1U;
  if (HAL_SPI_Transmit_DMA(&hspi1, (const uint8_t *)s_band_buf[tx_idx],
                           (uint16_t)((uint16_t)s_width * (uint16_t)h * 2U)) != HAL_OK) {
    s_dma_busy = false;
    HAL_GPIO_WritePin(TFT_CS_PORT, TFT_CS_PIN, GPIO_PIN_SET);
  }
}

void ST7789_WaitIdle(void)
{
  tft_wait_dma();
}

void ST7789_FillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color)
{
  int16_t x0 = (x < 0) ? 0 : x;
  int16_t x1 = (x + w > s_width) ? s_width : (int16_t)(x + w);
  int16_t y0 = (y < s_band_y0) ? s_band_y0 : y;
  int16_t y1 = (y + h > s_band_y1) ? s_band_y1 : (int16_t)(y + h);
  if (x0 >= x1 || y0 >= y1) {
    return;
  }
  uint16_t be = swap16(color);
  uint16_t *buf = s_band_buf[s_active_idx];
  for (int16_t py = y0; py < y1; py++) {
    uint16_t *row = &buf[(py - s_band_y0) * s_width];
    for (int16_t px = x0; px < x1; px++) {
      row[px] = be;
    }
  }
}

void ST7789_DrawHLine(int16_t x, int16_t y, int16_t w, uint16_t color)
{
  ST7789_FillRect(x, y, w, 1, color);
}

void ST7789_DrawVLine(int16_t x, int16_t y, int16_t h, uint16_t color)
{
  ST7789_FillRect(x, y, 1, h, color);
}

void ST7789_DrawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color)
{
  ST7789_DrawHLine(x, y, w, color);
  ST7789_DrawHLine(x, (int16_t)(y + h - 1), w, color);
  ST7789_DrawVLine(x, y, h, color);
  ST7789_DrawVLine((int16_t)(x + w - 1), y, h, color);
}

static uint16_t utf8_next(const char **p)
{
  const uint8_t *s = (const uint8_t *)(*p);
  if (s[0] == 0U) {
    return 0U;
  }
  if ((s[0] & 0x80U) == 0U) {
    *p += 1;
    return s[0];
  }
  if ((s[0] & 0xE0U) == 0xC0U && s[1] != 0U) {
    *p += 2;
    return (uint16_t)(((uint16_t)(s[0] & 0x1FU) << 6) | (uint16_t)(s[1] & 0x3FU));
  }
  if ((s[0] & 0xF0U) == 0xE0U && s[1] != 0U && s[2] != 0U) {
    *p += 3;
    return (uint16_t)(((uint16_t)(s[0] & 0x0FU) << 12) |
                      ((uint16_t)(s[1] & 0x3FU) << 6) |
                      (uint16_t)(s[2] & 0x3FU));
  }
  *p += 1;
  return '?';
}

static const ui_glyph_t *find_glyph(const ui_font_t *font, uint16_t cp)
{
  int32_t lo = 0;
  int32_t hi = (int32_t)font->count - 1;
  while (lo <= hi) {
    int32_t mid = (lo + hi) >> 1;
    uint16_t mcp = font->glyphs[mid].codepoint;
    if (mcp == cp) {
      return &font->glyphs[mid];
    }
    if (mcp < cp) {
      lo = mid + 1;
    } else {
      hi = mid - 1;
    }
  }
  return NULL;
}

static uint16_t blend565_be(uint16_t fg, uint16_t bg, uint8_t a2)
{
  if (a2 == 0U) {
    return swap16(bg);
  }
  if (a2 >= 3U) {
    return swap16(fg);
  }
  uint16_t fr = (fg >> 11) & 0x1FU;
  uint16_t fg6 = (fg >> 5) & 0x3FU;
  uint16_t fb = fg & 0x1FU;
  uint16_t br = (bg >> 11) & 0x1FU;
  uint16_t bg6 = (bg >> 5) & 0x3FU;
  uint16_t bb = bg & 0x1FU;
  uint16_t r = (uint16_t)((fr * a2 + br * (3U - a2) + 1U) / 3U);
  uint16_t g = (uint16_t)((fg6 * a2 + bg6 * (3U - a2) + 1U) / 3U);
  uint16_t b = (uint16_t)((fb * a2 + bb * (3U - a2) + 1U) / 3U);
  return swap16((uint16_t)((r << 11) | (g << 5) | b));
}

int16_t ST7789_MeasureText(const char *utf8, const ui_font_t *font)
{
  int16_t w = 0;
  const char *p = utf8;
  uint16_t cp;
  while ((cp = utf8_next(&p)) != 0U) {
    const ui_glyph_t *g = find_glyph(font, cp);
    if (g != NULL) {
      w = (int16_t)(w + g->advance);
    }
  }
  return w;
}

int16_t ST7789_DrawText(int16_t x, int16_t y, const char *utf8, const ui_font_t *font,
                        uint16_t fg_color, uint16_t bg_color)
{
  if (y >= s_band_y1 || (y + (int16_t)font->height) <= s_band_y0) {
    return (int16_t)(x + ST7789_MeasureText(utf8, font));
  }

  uint16_t palette[4] = {
    0U,
    blend565_be(fg_color, bg_color, 1U),
    blend565_be(fg_color, bg_color, 2U),
    swap16(fg_color),
  };

  int16_t gy0 = (y < s_band_y0) ? (int16_t)(s_band_y0 - y) : 0;
  int16_t gy1 = ((y + (int16_t)font->height) > s_band_y1)
                  ? (int16_t)(s_band_y1 - y)
                  : (int16_t)font->height;

  uint16_t *buf = s_band_buf[s_active_idx];
  int16_t cx = x;
  const char *p = utf8;
  uint16_t cp;
  while ((cp = utf8_next(&p)) != 0U) {
    const ui_glyph_t *g = find_glyph(font, cp);
    if (g == NULL) {
      continue;
    }
    if (cx < s_width && (cx + (int16_t)g->width) > 0 && g->width > 0U) {
      int16_t gx0 = (cx < 0) ? (int16_t)(-cx) : 0;
      int16_t gx1 = ((cx + (int16_t)g->width) > s_width)
                      ? (int16_t)(s_width - cx)
                      : (int16_t)g->width;
      for (int16_t gy = gy0; gy < gy1; gy++) {
        uint16_t *row = &buf[(y + gy - s_band_y0) * s_width];
        uint32_t row_bit = (uint32_t)gy * (uint32_t)g->width * 2U;
        for (int16_t gx = gx0; gx < gx1; gx++) {
          uint32_t bit_idx = row_bit + (uint32_t)gx * 2U;
          uint8_t a2 = (font->bitmap[g->offset + (bit_idx >> 3)] >> (6U - (bit_idx & 6U))) & 0x03U;
          if (a2 != 0U) {
            row[cx + gx] = palette[a2];
          }
        }
      }
    }
    cx = (int16_t)(cx + g->advance);
  }
  return cx;
}

void HAL_SPI_TxCpltCallback(SPI_HandleTypeDef *hspi)
{
  if (hspi->Instance == SPI1) {
    while ((SPI1->SR & SPI_SR_BSY) != 0U) {
    }
    HAL_GPIO_WritePin(TFT_CS_PORT, TFT_CS_PIN, GPIO_PIN_SET);
    s_dma_busy = false;
  }
}

void HAL_SPI_ErrorCallback(SPI_HandleTypeDef *hspi)
{
  if (hspi->Instance == SPI1) {
    HAL_GPIO_WritePin(TFT_CS_PORT, TFT_CS_PIN, GPIO_PIN_SET);
    s_dma_busy = false;
  }
}
