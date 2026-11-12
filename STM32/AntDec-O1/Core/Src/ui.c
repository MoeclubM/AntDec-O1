#include "ui.h"
#include "board_hw.h"
#include "es9039q2m.h"
#include "power_seq.h"
#include "st7789.h"
#include "xmos_link.h"
#include "device_store.h"
#include <stdio.h>
#include <string.h>

#define COL_BG          RGB565(13, 17, 23)
#define COL_SURFACE     RGB565(22, 28, 36)
#define COL_SEL_BG      RGB565(26, 42, 64)
#define COL_BORDER      RGB565(42, 50, 62)
#define COL_TEXT        RGB565(232, 238, 245)
#define COL_MUTED       RGB565(139, 148, 158)
#define COL_ACCENT      RGB565(56, 139, 253)
#define COL_OK          RGB565(46, 160, 67)
#define COL_WARN        RGB565(210, 153, 34)
#define COL_ERR         RGB565(248, 81, 73)

typedef struct {
  GPIO_TypeDef *port;
  uint16_t pin;
  bool stable;
  bool raw_prev;
  bool long_fired;
  uint32_t debounce_ms;
  uint32_t press_ms;
  uint32_t repeat_ms;
} key_tracker_t;

static key_tracker_t s_keys[3];
static bool s_in_settings = false;
static bool s_editing = false;
static bool s_setting_error = false;
static uint8_t s_candidate = 0U;
static uint8_t s_sel_item = 0U;
static uint8_t s_usb_mode = 0U;
static uint8_t s_dsp_preset = 0U;
static bool s_crossfeed = false;
static uint32_t s_last_input_ms = 0U;
static uint32_t s_last_draw_ms = 0U;
static bool s_force_redraw = true;
static int16_t s_meter_db10[2] = {-900, -900};
static int16_t s_peak_db10[2] = {-900, -900};
static uint32_t s_peak_until_ms[2], s_meter_tick_ms;
static bool s_meter_valid;

static const char *const k_usb_names[4] = {
  "UAC 2.0",
  "山灵 ID",
  "飞傲 ID",
  "UAC 1.0",
};

static const char *const k_dsp_names[6] = {
  "直通",
  "低音加重",
  "人声",
  "暖声",
  "哈曼",
  "自定义",
};

static const char *const k_filter_names[8] = {
  "F0 最小相位",
  "F1 切趾快速",
  "F2 线性快速",
  "F3 低纹波",
  "F4 线性慢速",
  "F5 最小快速",
  "F6 最小慢速",
  "F7 低色散",
};

static void step_volume(int8_t delta_steps)
{
  const es9039_state_t *dac = ES9039_GetState();
  int16_t next = (int16_t)dac->atten_steps + (int16_t)delta_steps;
  if (next < 0) {
    next = 0;
  } else if (next > 200) {
    next = 200;
  }
  (void)ES9039_SetAttenuation((uint8_t)next);
  if (PowerSeq_GetStatus()->user_muted && delta_steps < 0) {
    PowerSeq_SetUserMute(false);
  }
}

static uint8_t setting_count(void)
{
  static const uint8_t counts[5] = {8U, 6U, 2U, 4U, 2U};
  return counts[s_sel_item];
}

static uint8_t setting_value(void)
{
  if (s_sel_item == 0U) return ES9039_GetState()->filter_idx & 7U;
  if (s_sel_item == 1U) return s_dsp_preset;
  if (s_sel_item == 2U) return s_crossfeed ? 1U : 0U;
  return s_sel_item == 3U ? s_usb_mode : 0U;
}

static void select_setting(void)
{
  if (!s_editing) {
    if (s_sel_item > 0U && s_sel_item < 4U && !XmosLink_GetTelemetry()->link_online) return;
    s_candidate = setting_value();
    s_setting_error = false;
    s_editing = true;
    return;
  }
  if (s_candidate == setting_count() || s_candidate == setting_value()) {
    s_editing = false;
    return;
  }
  if (DeviceStore_SyncActive()) { s_setting_error = true; return; }
  if (s_sel_item > 0U && s_sel_item < 4U && !XmosLink_GetTelemetry()->link_online) return;
  switch (s_sel_item) {
    case 4U:
      if (!DeviceStore_ResetPlayTime()) { s_setting_error = true; return; }
      break;
    case 3U:
      s_usb_mode = s_candidate;
      XmosLink_SendCommand(XMOS_CMD_SET_USB_MODE, s_usb_mode);
      break;
    case 1U:
      s_dsp_preset = s_candidate;
      XmosLink_SendCommand(XMOS_CMD_SET_DSP_PRESET, s_dsp_preset);
      break;
    case 2U:
      s_crossfeed = s_candidate != 0U;
      XmosLink_SendCommand(XMOS_CMD_SET_CROSSFEED, s_crossfeed ? 1U : 0U);
      break;
    case 0U:
      if (!ES9039_SetFilter(s_candidate)) {
        s_setting_error = true;
        return;
      }
      break;
    default:
      break;
  }
  s_editing = false;
}

static void move_selection(int8_t direction)
{
  uint8_t count = s_editing ? setting_count() + 1U : 5U;
  uint8_t value = s_editing ? s_candidate : s_sel_item;
  value = (uint8_t)(((int)value + count + direction) % count);
  if (s_editing) s_candidate = value;
  else s_sel_item = value;
  s_setting_error = false;
}

static void poll_keys(uint32_t now_ms)
{
  for (uint8_t i = 0U; i < 3U; i++) {
    key_tracker_t *k = &s_keys[i];
    bool pressed = (HAL_GPIO_ReadPin(k->port, k->pin) == GPIO_PIN_RESET);
    if (pressed != k->raw_prev) {
      k->raw_prev = pressed;
      k->debounce_ms = now_ms;
    }
    if ((now_ms - k->debounce_ms) < 20U) {
      continue;
    }

    if (pressed != k->stable) {
      k->stable = pressed;
      s_last_input_ms = now_ms;
      s_force_redraw = true;

      if (pressed) {
        k->press_ms = now_ms;
        k->repeat_ms = now_ms;
        k->long_fired = false;
        if (i == 0U) {
          if (s_in_settings) {
            move_selection(-1);
          } else {
            step_volume(+2);
          }
        } else if (i == 2U) {
          if (s_in_settings) {
            move_selection(1);
          } else {
            step_volume(-2);
          }
        }
      } else {
        if (i == 1U && !k->long_fired) {
          if (s_in_settings) {
            select_setting();
          } else {
            if (XmosLink_GetTelemetry()->link_online) {
              XmosLink_SendCommand(XMOS_CMD_HID_MEDIA, XMOS_HID_PLAY_PAUSE);
            } else {
              PowerSeq_SetUserMute(!PowerSeq_GetStatus()->user_muted);
            }
          }
        }
      }
    } else if (pressed) {
      if (i == 1U && !k->long_fired && (now_ms - k->press_ms) >= 550U) {
        k->long_fired = true;
        if (s_editing) s_editing = false;
        else s_in_settings = !s_in_settings;
        if (s_in_settings && XmosLink_GetTelemetry()->link_online) {
          s_usb_mode = (uint8_t)XmosLink_GetTelemetry()->usb_mode;
          s_dsp_preset = XmosLink_GetTelemetry()->dsp_preset;
          s_crossfeed = XmosLink_GetTelemetry()->crossfeed_enabled;
        }
        s_last_input_ms = now_ms;
        s_force_redraw = true;
      } else if (!s_in_settings && (i == 0U || i == 2U) &&
                 (now_ms - k->press_ms) >= 420U &&
                 (now_ms - k->repeat_ms) >= 90U) {
        k->repeat_ms = now_ms;
        s_last_input_ms = now_ms;
        step_volume((i == 0U) ? +2 : -2);
        s_force_redraw = true;
      }
    }
  }

  if (s_in_settings && (now_ms - s_last_input_ms) >= 8000U) {
    s_in_settings = false;
    s_editing = false;
    s_force_redraw = true;
  }
}

static const char *stream_name(const xmos_telemetry_t *xm, const es9039_state_t *dac)
{
  if (!xm->link_online) return dac->online ? "等待音频流" : "等待解码";
  if (!xm->playing) return "等待音频流";
  return xm->format == XMOS_FMT_NATIVE_DSD ? "DSD" :
         xm->format == XMOS_FMT_DOP ? "DoP" : "PCM";
}

static void text_at(int16_t x, int16_t y, const char *text,
                    const ui_font_t *font, uint16_t color)
{
  ST7789_DrawText(x, y, text, font, color, COL_BG);
}

static void text_right(int16_t y, const char *text, const ui_font_t *font, uint16_t color)
{
  text_at((int16_t)(ST7789_Width() - 4 - ST7789_MeasureText(text, font)), y, text, font, color);
}

static void update_meter(uint32_t now_ms)
{
  const xmos_telemetry_t *xm = XmosLink_GetTelemetry();
  s_meter_valid = xm->link_online && xm->playing && xm->meter_valid;
  uint32_t elapsed = now_ms - s_meter_tick_ms;
  for (uint8_t ch = 0U; ch < 2U; ch++) {
    uint8_t code = ch ? xm->meter_right : xm->meter_left;
    int16_t target = code ? ((int16_t)code - 121) * 10 : -900;
    if (target < -900) target = -900;
    int32_t falling = s_meter_db10[ch] - (int32_t)(elapsed * 240U / 1000U);
    s_meter_db10[ch] = !s_meter_valid ? -900 : falling > target ? falling : target;
    if (!s_meter_valid) { s_peak_db10[ch] = -900; s_peak_until_ms[ch] = now_ms; }
    else if (target >= s_peak_db10[ch]) {
      s_peak_db10[ch] = target;
      s_peak_until_ms[ch] = now_ms + 1000U;
    } else if ((int32_t)(now_ms - s_peak_until_ms[ch]) > 0) {
      uint32_t since = (int32_t)(s_meter_tick_ms - s_peak_until_ms[ch]) > 0
                         ? s_meter_tick_ms : s_peak_until_ms[ch];
      int32_t peak = s_peak_db10[ch] - (int32_t)((now_ms - since) * 120U / 1000U);
      s_peak_db10[ch] = peak > s_meter_db10[ch] ? peak : s_meter_db10[ch];
    }
  }
  s_meter_tick_ms = now_ms;
}

static void draw_meter(bool compact)
{
  int16_t top = compact ? 28 : 43;
  int16_t height = compact ? 74 : 155;
  int16_t bw = compact ? 8 : 18;
  int16_t gap = compact ? 8 : 12;
  int16_t left = ST7789_Width() - 4 - 2 * bw - gap;
  for (uint8_t step = 0U; step <= 5U; ++step) {
    int16_t yy = top + height * step / 5;
    char tick[5];
    snprintf(tick, sizeof(tick), step ? "-%u" : "%u", step * 18U);
    text_at(left - 27, yy - 6, tick, &g_font_ui_12, COL_MUTED);
    ST7789_DrawHLine(left - 3, yy, 2 * bw + gap + 3, COL_BORDER);
  }
  for (uint8_t ch = 0U; ch < 2U; ch++) {
    int16_t xx = left + ch * (bw + gap);
    text_at(xx, top + height + 4, ch ? "R" : "L", &g_font_ui_12, COL_MUTED);
    ST7789_FillRect(xx, top, bw, height, COL_SURFACE);
    /* Post-DSP, before DAC attenuation; identical measurement point to the web. */
    int16_t fill = height * (s_meter_db10[ch] + 900) / 900;
    int16_t first = top + height - fill;
    if (fill) {
      ST7789_FillRect(xx, first, bw, fill, COL_OK);
      int16_t amber = top + height / 10;
      int16_t red = top + height / 30;
      if (first < amber) ST7789_FillRect(xx, first, bw, amber - first, COL_WARN);
      if (first < red) ST7789_FillRect(xx, first, bw, red - first, COL_ERR);
      for (int16_t yy = top + height - 4; yy > top; yy -= 5)
        ST7789_DrawHLine(xx, yy, bw, COL_BG);
    }
    if (s_meter_valid && s_peak_db10[ch] > -900) {
      int16_t peak_y = top + height - height * (s_peak_db10[ch] + 900) / 900;
      ST7789_DrawHLine(xx - 1, peak_y, bw + 2, COL_TEXT);
    }
    if (!s_meter_valid) text_at(xx, top + height / 2, "-", &g_font_ui_12, COL_MUTED);
  }
}

static const char *system_status(bool *abnormal)
{
  const power_status_t *p = PowerSeq_GetStatus();
  const es9039_state_t *d = ES9039_GetState();
  const xmos_telemetry_t *x = XmosLink_GetTelemetry();
  const char *errors[5];
  uint8_t count = 0U;
  bool settled = HAL_GetTick() >= 2000U;
  if (settled && (!p->pwr_ok_4v5 || (p->state == PWR_STATE_FAULT &&
      p->last_fault != PWR_FAULT_DAC_LOST))) errors[count++] = "供电异常";
  if (settled && !d->online) errors[count++] = "解码异常";
  if (settled && !x->link_online) errors[count++] = "通信异常";
  if (p->state == PWR_STATE_READY && !p->amp_enabled) errors[count++] = "耳放异常";
  if (DeviceStore_SaveFailed()) errors[count++] = "存储异常";
  *abnormal = count != 0U;
  if (count) return errors[(HAL_GetTick() / 2000U) % count];
  return p->state == PWR_STATE_READY ? "正常" : "启动中";
}

static void render_settings(bool compact)
{
  const xmos_telemetry_t *xm = XmosLink_GetTelemetry();
  const es9039_state_t *dac = ES9039_GetState();
  const char *labels[5] = {"数字滤波", "音效预设", "交叉馈送", "USB模式", "累计时间"};
  if (s_editing) {
    text_at(5, compact ? 21 : 30, labels[s_sel_item], &g_font_ui_12, COL_ACCENT);
    uint8_t first = s_candidate > 3U ? s_candidate - 3U : 0U;
    int16_t row_h = compact ? 18 : 34;
    int16_t top = compact ? 38 : 54;
    for (uint8_t row = 0; row < 4U && first + row <= setting_count(); ++row) {
      uint8_t value = first + row;
      const char *label = value == setting_count() ? "返回" :
                          s_sel_item == 0U ? k_filter_names[value] :
                          s_sel_item == 1U ? k_dsp_names[value] :
                          s_sel_item == 2U ? (value ? "开启" : "关闭") :
                          s_sel_item == 3U ? k_usb_names[value] : (value ? "确认清零" : "保留");
      int16_t yy = top + row * row_h;
      uint16_t bg = value == s_candidate ? COL_SEL_BG : COL_BG;
      ST7789_FillRect(0, yy, ST7789_Width(), row_h - 1, bg);
      ST7789_DrawText(5, yy + 2, label, compact ? &g_font_ui_12 : &g_font_ui_16, COL_TEXT, bg);
      if (value == setting_value()) {
        ST7789_DrawText(ST7789_Width() - 19, yy + 2, "●", &g_font_ui_12, COL_OK, bg);
      }
    }
    const char *hint = s_setting_error ? "设置失败，请重试" :
                       s_sel_item == 3U && s_candidate < 4U && s_candidate != s_usb_mode
                         ? "确认后USB将断开" : "中键确认 · 长按取消";
    text_at(4, compact ? 114 : 213, hint, &g_font_ui_12, s_setting_error ? COL_ERR : COL_MUTED);
    return;
  }
  char elapsed[20];
  uint32_t seconds = DeviceStore_PlaySeconds();
  snprintf(elapsed, sizeof(elapsed), "%02lu:%02lu:%02lu", (unsigned long)(seconds / 3600UL),
           (unsigned long)(seconds / 60UL % 60UL), (unsigned long)(seconds % 60UL));
  const char *values[5] = {
    k_filter_names[dac->filter_idx & 7U],
    k_dsp_names[s_dsp_preset],
    s_crossfeed ? "开启" : "关闭",
    k_usb_names[s_usb_mode], elapsed
  };
  int16_t top = compact ? 20 : 34;
  int16_t row_h = compact ? 23 : 42;
  int16_t w = ST7789_Width();
  uint8_t first = s_sel_item > 3U ? s_sel_item - 3U : 0U;
  for (uint8_t i = first; i < 5U && i < first + 4U; i++) {
    int16_t yy = top + (i - first) * row_h;
    bool selected = i == s_sel_item;
    uint16_t bg = selected ? COL_SEL_BG : COL_BG;
    if (selected) {
      ST7789_FillRect(0, yy, w, row_h - 1, bg);
      ST7789_FillRect(0, yy, 2, row_h - 1, COL_ACCENT);
    }
    ST7789_DrawHLine(0, yy + row_h - 1, w, COL_BORDER);
    const ui_font_t *font = compact ? &g_font_ui_12 : &g_font_ui_16;
    ST7789_DrawText(5, yy + 4, labels[i], font, COL_MUTED, bg);
    int16_t vx = w - 5 - ST7789_MeasureText(values[i], font);
    ST7789_DrawText(vx, yy + 4, values[i], font,
                   i && i < 4U && !xm->link_online ? COL_MUTED : COL_TEXT, bg);
  }
  text_at(4, compact ? 114 : 213, "左右选择 · 中键进入", &g_font_ui_12, COL_MUTED);
  if (!compact) text_at(4, 228, "长按中键返回", &g_font_ui_12, COL_MUTED);
}

static void render_scene(void)
{
  const power_status_t *p = PowerSeq_GetStatus();
  const es9039_state_t *d = ES9039_GetState();
  const xmos_telemetry_t *x = XmosLink_GetTelemetry();
  bool compact = ST7789_Width() <= 160;
  uint8_t usb = x->link_online ? ((uint8_t)x->usb_mode & 3U) : s_usb_mode;
  bool abnormal;
  const char *state = system_status(&abnormal);
  text_at(4, 2, s_in_settings ? "设置" : k_usb_names[usb], &g_font_ui_12, COL_ACCENT);
  text_right(2, state, &g_font_ui_12, abnormal ? COL_ERR : COL_OK);
  ST7789_DrawHLine(0, 17, ST7789_Width(), COL_BORDER);
  if (s_in_settings) {
    render_settings(compact);
    return;
  }

  char stream[40], volume[20];
  const char *name = stream_name(x, d);
  if (x->link_online && x->playing && x->sample_rate_hz) {
    if (x->format == XMOS_FMT_NATIVE_DSD || x->format == XMOS_FMT_DOP) {
      snprintf(stream, sizeof(stream), "%s%lu", name, (unsigned long)(x->sample_rate_hz / 44100UL));
    } else {
      uint32_t khz10 = (x->sample_rate_hz + 50UL) / 100UL;
      if (khz10 % 10UL) snprintf(stream, sizeof(stream), "PCM %lu.%luk", (unsigned long)(khz10 / 10UL), (unsigned long)(khz10 % 10UL));
      else snprintf(stream, sizeof(stream), "PCM %luk", (unsigned long)(khz10 / 10UL));
    }
  } else snprintf(stream, sizeof(stream), "%s", name);
  bool muted = p->user_muted || (x->link_online && x->muted);
  uint16_t db10 = (uint16_t)d->atten_steps * 5U;
  if (muted) snprintf(volume, sizeof(volume), "静音");
  else snprintf(volume, sizeof(volume), "-%u.%udB", db10 / 10U, db10 % 10U);
  text_at(4, compact ? 23 : 36, stream, compact ? &g_font_ui_16 : &g_font_ui_24, COL_TEXT);
  text_at(4, compact ? 46 : 78, volume, &g_font_ui_24, muted ? COL_WARN : COL_TEXT);

  char detail[56];
  if (x->playing && x->link_online) {
    unsigned bits = x->format == XMOS_FMT_NATIVE_DSD || x->format == XMOS_FMT_DOP ? 1U : x->bit_depth;
    if (compact) snprintf(detail, sizeof(detail), "%u-bit", bits);
    else snprintf(detail, sizeof(detail), "%u-bit · %s", bits, k_dsp_names[x->dsp_preset % 6U]);
  } else snprintf(detail, sizeof(detail), "%s", x->usb_active ? "USB在线 · 空闲" : "USB未连接");
  text_at(4, compact ? 78 : 124, detail, &g_font_ui_12, COL_MUTED);
  text_at(4, compact ? 96 : 151, k_filter_names[d->filter_idx & 7U], &g_font_ui_12, COL_MUTED);
  char elapsed[20];
  uint32_t seconds = DeviceStore_PlaySeconds();
  snprintf(elapsed, sizeof(elapsed), "%02lu:%02lu:%02lu",
           (unsigned long)(seconds / 3600UL), (unsigned long)(seconds / 60UL % 60UL),
           (unsigned long)(seconds % 60UL));
  text_at(4, compact ? 114 : 191, elapsed, &g_font_ui_12, COL_TEXT);
  if (!compact) text_at(4, 217, "累计播放 · 长按中键设置", &g_font_ui_12, COL_MUTED);
  draw_meter(compact);
}

void UI_Init(void)
{
  s_keys[0] = (key_tracker_t){.port = KEY_PREV_PORT, .pin = KEY_PREV_PIN};
  s_keys[1] = (key_tracker_t){.port = KEY_PLAY_PORT, .pin = KEY_PLAY_PIN};
  s_keys[2] = (key_tracker_t){.port = KEY_NEXT_PORT, .pin = KEY_NEXT_PIN};
  s_force_redraw = true;
}

void UI_Tick(uint32_t now_ms)
{
  poll_keys(now_ms);
  if (!s_force_redraw && now_ms - s_last_draw_ms < 80U) return;
  s_force_redraw = false;
  s_last_draw_ms = now_ms;
  update_meter(now_ms);
  int16_t height = ST7789_Height();
  /* Keep input and protection responsive during SPI DMA display refreshes. */
  for (int16_t y = 0; y < height; y += TFT_BAND_HEIGHT) {
    uint32_t tick = HAL_GetTick();
    poll_keys(tick);
    XmosLink_Tick(tick);
    PowerSeq_Tick(tick);
    int16_t bh = y + TFT_BAND_HEIGHT <= height ? TFT_BAND_HEIGHT : height - y;
    ST7789_BeginBand(y, bh, COL_BG);
    render_scene();
    ST7789_FlushBand();
  }
  ST7789_WaitIdle();
}
