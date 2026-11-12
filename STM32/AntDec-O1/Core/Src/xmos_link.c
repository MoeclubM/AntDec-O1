#include "xmos_link.h"
#include "board_hw.h"
#include "es9039q2m.h"
#include "power_seq.h"
#include "device_store.h"
static uint32_t s_sync_seen_ms;

static void reply_controller(uint8_t token, bool ok)
{
  const es9039_state_t *d = ES9039_GetState();
  const power_status_t *p = PowerSeq_GetStatus();
  uint8_t fields[6] = {
    (uint8_t)((d->filter_idx & 7U) | (d->online << 3) | (p->amp_enabled << 4) |
              (p->user_muted << 5) | ((p->state == PWR_STATE_READY) << 6) | (d->muted << 7)),
    d->atten_steps,
    (uint8_t)((p->pg1_mv + 10U) / 20U),
    (uint8_t)((p->pg2_mv + 10U) / 20U),
    (uint8_t)((p->vdda_mv + 10U) / 20U),
    (uint8_t)((uint8_t)p->state | ((uint8_t)p->last_fault << 4))
  };
  XmosLink_SendCommand((xmos_cmd_t)5U, token);
  for (uint8_t i = 0U; i < 6U; i++) XmosLink_SendCommand((xmos_cmd_t)(6U + i), fields[i]);
  uint32_t seconds = DeviceStore_PlaySeconds();
  for (uint8_t i = 0U; i < 4U; i++) XmosLink_SendCommand((xmos_cmd_t)14U, (uint8_t)(seconds >> (i * 8U)));
  /* Versioned settings follow the four time bytes in the command-14 stream. */
  XmosLink_SendCommand((xmos_cmd_t)14U, 1U);
  const uint8_t *settings = (const uint8_t *)DeviceStore_Settings();
  for (uint8_t i = 0; i < sizeof(device_settings_t); i++)
    XmosLink_SendCommand((xmos_cmd_t)14U, settings[i]);
  uint16_t revision = DeviceStore_SettingsRevision();
  XmosLink_SendCommand((xmos_cmd_t)14U, (uint8_t)revision);
  XmosLink_SendCommand((xmos_cmd_t)14U, (uint8_t)(revision >> 8));
  uint8_t storage = (DeviceStore_SaveFailed() ? 1U : 0U) | 2U | 8U | 16U |
                    (DeviceStore_SettingsPending() ? 4U : 0U) | (DeviceStore_SyncActive() ? 32U : 0U);
  XmosLink_SendCommand((xmos_cmd_t)15U, storage);
  XmosLink_SendCommand((xmos_cmd_t)(ok ? 12U : 13U), token);
}

static void apply_controller(uint32_t frame)
{
  uint8_t op = (uint8_t)((frame >> 24) & 15U);
  uint8_t token = (uint8_t)(frame >> 16);
  uint8_t value = (uint8_t)(frame >> 8);
  bool ok = true;
  if (op == 1U && value < 8U) ok = ES9039_SetFilter(value);
  else if (op == 2U && value <= 200U) ok = ES9039_SetAttenuation(value);
  else if (op == 3U && value < 2U) PowerSeq_SetUserMute(value != 0U);
  else if (op == 5U) ok = DeviceStore_ResetPlayTime();
  else if (op >= 6U && op <= 14U) ok = ES9039_SetSetting(op, value);
  else if (op == 15U) {
    ok = DeviceStore_SyncSettings(value);
    if (ok && DeviceStore_SyncActive()) (void)ES9039_SetFastMute();
  }
  else if (op != 4U) ok = false;
  if (ok && op != 4U && DeviceStore_SyncActive()) s_sync_seen_ms = HAL_GetTick();
  reply_controller(token, ok);
}

static const uint32_t k_sr_table[16] = {
  0UL,
  44100UL,
  48000UL,
  88200UL,
  96000UL,
  176400UL,
  192000UL,
  352800UL,
  384000UL,
  705600UL,
  768000UL,
  2822400UL,   /* DSD64  */
  5644800UL,   /* DSD128 */
  11289600UL,  /* DSD256 */
  22579200UL,  /* DSD512 */
  0UL
};

static const uint8_t k_bits_table[4] = {0U, 16U, 24U, 32U};

/* A complete controller reply spans more than one incoming telemetry frame. */
static volatile uint32_t s_rx_frames[8];
static volatile uint8_t s_rx_head, s_rx_tail;
static volatile bool s_fast_mute_pending = false;
static uint16_t s_fall_us = 0U;
static uint32_t s_shift_reg = 0U;
static int8_t s_bit_idx = -1;

static xmos_telemetry_t g_xmos = {
  .link_online = false,
  .board_powered = false,
  .usb_active = false,
  .playing = false,
  .muted = false,
  .peq_custom = false,
  .format = XMOS_FMT_IDLE,
  .sample_rate_hz = 0UL,
  .bit_depth = 0U,
  .usb_mode = XMOS_USB_UAC2,
  .dsp_preset = 0U,
  .rx_frames_ok = 0UL,
  .fast_mute_pulses = 0UL,
  .last_rx_ms = 0UL,
};

static uint8_t crc8_calc(const uint8_t *data, uint8_t len)
{
  uint8_t crc = 0x5AU;
  for (uint8_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t b = 0; b < 8U; b++) {
      crc = (crc & 0x80U) ? (uint8_t)((crc << 1) ^ 0x07U) : (uint8_t)(crc << 1);
    }
  }
  return crc;
}

void XmosLink_Init(void)
{
  DeviceStore_Init();
  ES9039_RestoreSettings();
  s_bit_idx = -1;
  s_rx_head = s_rx_tail = 0U;
  s_fast_mute_pending = false;
}

void XmosLink_OnRxEdge(void)
{
  uint16_t now_us = (uint16_t)(TIM2->CNT);
  if (HAL_GPIO_ReadPin(XMOS_RX_PORT, XMOS_RX_PIN) == GPIO_PIN_RESET) {
    s_fall_us = now_us;
    return;
  }

  uint16_t low_us = (uint16_t)(now_us - s_fall_us);
  if (low_us >= 1000U && low_us <= 3000U) {
    /* Out-of-band 1.35 ms pre-switch fast-mute pulse from XMOS */
    s_fast_mute_pending = true;
    g_xmos.fast_mute_pulses++;
    s_bit_idx = -1;
  } else if (low_us >= 450U && low_us <= 900U) {
    s_shift_reg = 0U;
    s_bit_idx = 0;
  } else if (s_bit_idx >= 0 && low_us >= 55U && low_us <= 420U) {
    uint8_t bit = (low_us >= 200U) ? 1U : 0U;
    s_shift_reg = (s_shift_reg << 1) | bit;
    s_bit_idx++;
    if (s_bit_idx == 32) {
      uint8_t next = (s_rx_head + 1U) & 7U;
      if (next != s_rx_tail) {
        s_rx_frames[s_rx_head] = s_shift_reg;
        s_rx_head = next;
      }
      s_bit_idx = -1;
    }
  } else {
    s_bit_idx = -1;
  }
}

void XmosLink_Tick(uint32_t now_ms)
{
  while (s_rx_tail != s_rx_head) {
    __disable_irq();
    uint32_t frame = s_rx_frames[s_rx_tail];
    s_rx_tail = (s_rx_tail + 1U) & 7U;
    __enable_irq();

    uint8_t bytes[3] = {
      (uint8_t)(frame >> 24),
      (uint8_t)(frame >> 16),
      (uint8_t)(frame >> 8),
    };
    bool crc_ok = crc8_calc(bytes, 3U) == (uint8_t)(frame & 0xFFU);
    if ((frame >> 28) == 0x0DU && crc_ok) {
      g_xmos.play_seconds = (frame >> 8) & 0xFFFFFU;
      DeviceStore_ObservePlayTime(g_xmos.play_seconds, now_ms);
    } else if ((frame >> 28) == 0x0CU && crc_ok) {
      apply_controller(frame);
      now_ms = HAL_GetTick();
    } else if ((frame >> 28) == 0x0BU && crc_ok) {
      g_xmos.meter_valid = ((frame >> 24) & 1U) != 0U;
      g_xmos.meter_left = (uint8_t)(frame >> 16);
      g_xmos.meter_right = (uint8_t)(frame >> 8);
      g_xmos.last_meter_ms = now_ms;
    } else if (((frame >> 28) == 0x0AU) && crc_ok) {
      xmos_format_t new_fmt = (xmos_format_t)((frame >> 25) & 0x07U);
      uint8_t sr_id = (uint8_t)((frame >> 20) & 0x1FU);
      uint32_t new_sr = (sr_id < 16U) ? k_sr_table[sr_id] : 0UL;
      if (g_xmos.link_online) {
        bool sr_changed = (new_sr != 0UL) && (g_xmos.sample_rate_hz != 0UL) &&
                          (new_sr != g_xmos.sample_rate_hz);
        bool fmt_changed = (new_fmt != XMOS_FMT_IDLE) && (g_xmos.format != XMOS_FMT_IDLE) &&
                           (new_fmt != g_xmos.format);
        if (sr_changed || fmt_changed) {
          s_fast_mute_pending = true;
        }
      }
      g_xmos.format = new_fmt;
      g_xmos.sample_rate_hz = new_sr;
      g_xmos.bit_depth = k_bits_table[(frame >> 18) & 0x03U];
      g_xmos.usb_mode = (xmos_usb_mode_t)((frame >> 16) & 0x03U);
      g_xmos.dsp_preset = (uint8_t)((frame >> 12) & 0x07U);
      g_xmos.crossfeed_enabled = ((frame >> 15) & 0x01U) != 0U;
      g_xmos.muted = ((frame >> 11) & 0x01U) != 0U;
      g_xmos.usb_active = ((frame >> 10) & 0x01U) != 0U;
      g_xmos.playing = ((frame >> 9) & 0x01U) != 0U;
      g_xmos.peq_custom = ((frame >> 8) & 0x01U) != 0U;
      g_xmos.link_online = true;
      g_xmos.board_powered = true;
      g_xmos.rx_frames_ok++;
      g_xmos.last_rx_ms = now_ms;
    }
  }

  /* Fallback to Amanero static levels when no valid telemetry frame within 300 ms */
  if ((now_ms - g_xmos.last_meter_ms) > 400U) {
    g_xmos.meter_valid = false;
    g_xmos.meter_left = g_xmos.meter_right = 0U;
  }
  if (g_xmos.link_online && (now_ms - g_xmos.last_rx_ms) > 300U) {
    g_xmos.link_online = false;
  }

  if (!g_xmos.link_online) {
    g_xmos.sample_rate_hz = 0UL;
    g_xmos.bit_depth = 0U;
    g_xmos.playing = false;
    g_xmos.usb_active = false;
    /* XMOS board pulls PC6/PC7 up via 4.7k to XU_3V3 against baseboard 100k pull-downs */
    bool pc6_high = (HAL_GPIO_ReadPin(XMOS_RX_PORT, XMOS_RX_PIN) == GPIO_PIN_SET);
    bool pc7_high = (HAL_GPIO_ReadPin(XMOS_TX_PORT, XMOS_TX_PIN) == GPIO_PIN_SET);
    bool xu_present_low = (HAL_GPIO_ReadPin(XU_PWR_PRESENT_N_PORT, XU_PWR_PRESENT_N_PIN) == GPIO_PIN_RESET);
    g_xmos.board_powered = pc6_high || pc7_high || xu_present_low;
    if (g_xmos.board_powered) {
      g_xmos.muted = pc6_high;
      g_xmos.format = pc7_high ? XMOS_FMT_NATIVE_DSD : XMOS_FMT_IDLE;
    } else {
      g_xmos.format = XMOS_FMT_IDLE;
      g_xmos.sample_rate_hz = 0UL;
      g_xmos.bit_depth = 0U;
      g_xmos.playing = false;
      g_xmos.usb_active = false;
    }
  }
  if (DeviceStore_SyncActive() && now_ms - s_sync_seen_ms >= 5000U) DeviceStore_AbortSync();
  DeviceStore_Tick(g_xmos.link_online && g_xmos.playing, now_ms);
}

bool XmosLink_ConsumeFastMute(void)
{
  if (!s_fast_mute_pending) {
    return false;
  }
  __disable_irq();
  bool pending = s_fast_mute_pending;
  s_fast_mute_pending = false;
  __enable_irq();
  return pending;
}

void XmosLink_SendCommand(xmos_cmd_t cmd, uint8_t arg)
{
  uint8_t hdr_bytes[2] = {
    (uint8_t)(0x50U | ((uint8_t)cmd & 0x0FU)),
    arg,
  };
  uint8_t crc = crc8_calc(hdr_bytes, 2U);
  uint32_t frame24 = ((uint32_t)hdr_bytes[0] << 16) | ((uint32_t)hdr_bytes[1] << 8) | crc;

  /* Configure PC7 temporarily as open-drain output (pulled up by R211 4.7k on XMOS board) */
  GPIO_InitTypeDef gpio = {0};
  HAL_GPIO_WritePin(XMOS_TX_PORT, XMOS_TX_PIN, GPIO_PIN_SET);
  gpio.Pin = XMOS_TX_PIN;
  gpio.Mode = GPIO_MODE_OUTPUT_OD;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(XMOS_TX_PORT, &gpio);

  /* Start pulse: 600 us LOW + 150 us HIGH */
  HAL_GPIO_WritePin(XMOS_TX_PORT, XMOS_TX_PIN, GPIO_PIN_RESET);
  Board_DelayUs(600U);
  HAL_GPIO_WritePin(XMOS_TX_PORT, XMOS_TX_PIN, GPIO_PIN_SET);
  Board_DelayUs(150U);

  for (int8_t i = 23; i >= 0; i--) {
    uint16_t low_us = ((frame24 >> i) & 1UL) ? 280U : 120U;
    HAL_GPIO_WritePin(XMOS_TX_PORT, XMOS_TX_PIN, GPIO_PIN_RESET);
    Board_DelayUs(low_us);
    HAL_GPIO_WritePin(XMOS_TX_PORT, XMOS_TX_PIN, GPIO_PIN_SET);
    /* High gaps may be stretched; low pulse widths must remain precise.
     * Service the pre-switch mute even while returning a status snapshot. */
    if (s_fast_mute_pending) PowerSeq_Tick(HAL_GetTick());
    Board_DelayUs(120U);
  }

  /* Restore the Cube-configured input with internal pull-down. */
  gpio.Mode = GPIO_MODE_INPUT;
  gpio.Pull = GPIO_PULLDOWN;
  HAL_GPIO_Init(XMOS_TX_PORT, &gpio);
  PowerSeq_Tick(HAL_GetTick());
}

const xmos_telemetry_t *XmosLink_GetTelemetry(void)
{
  return &g_xmos;
}
