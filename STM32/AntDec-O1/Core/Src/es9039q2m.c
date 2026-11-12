#include "es9039q2m.h"
#include "board_hw.h"
#include "device_store.h"
#include <string.h>

#define ES9039_REG_SYS_CFG          0U
#define ES9039_REG_SYS_MODE         1U
#define ES9039_REG_DAC_CLK_CFG      3U
#define ES9039_REG_CLK_GEAR         5U
#define ES9039_REG_DPLL_BW          29U
#define ES9039_REG_DATA_PATH_CFG    34U
#define ES9039_REG_GPIO78_CFG       40U
#define ES9039_REG_GPIO_OUT_EN      41U
#define ES9039_REG_GPIO_INPUT       42U
#define ES9039_REG_INPUT_SEL        57U
#define ES9039_REG_VOL_CH1          74U
#define ES9039_REG_VOL_CH2          75U
#define ES9039_REG_VOL_UP_RATE      82U
#define ES9039_REG_VOL_DOWN_RATE    83U
#define ES9039_REG_VOL_DOWN_FAST    84U
#define ES9039_REG_MUTE_INV         86U
#define ES9039_REG_INVERT           87U
#define ES9039_REG_FILTER_SHAPE     88U
#define ES9039_REG_AUTOMUTE_EN      123U
#define ES9039_REG_AUTOMUTE_TIME_L  124U
#define ES9039_REG_AUTOMUTE_TIME_H  125U
#define ES9039_REG_AUTOMUTE_LEVEL_L 126U
#define ES9039_REG_AUTOMUTE_LEVEL_H 127U
#define ES9039_REG_AUTOMUTE_OFF_L   128U
#define ES9039_REG_AUTOMUTE_OFF_H   129U
#define ES9039_REG_SOFT_RAMP_CFG    130U
#define ES9039_REG_CHIP_ID          225U
#define ES9039_REG_AUTOMUTE_STATUS  242U
#define ES9039_REG_STREAM_STATUS    245U

static es9039_state_t g_dac = {
  .online = false,
  .muted = true,
  .sync_mode = true,
  .automute_active = true,
  .i2c_addr7 = 0x48U,
  .chip_id = 0U,
  .atten_steps = 40U, /* -20.0 dB safe startup level */
  .filter_idx = 0U,   /* Minimum Phase */
  .raw_reg242 = 0U,
  .raw_reg245 = 0U,
  .stream_mode = DAC_STREAM_NONE,
};

static uint8_t s_fail_streak = 0U;
static bool s_fast_down;

static bool dac_write_reg(uint8_t reg, uint8_t val)
{
  g_dac.last_reg = reg;
  g_dac.expected_value = val;
  HAL_StatusTypeDef status = HAL_I2C_Mem_Write(&hi2c1, (uint16_t)(g_dac.i2c_addr7 << 1), reg,
                           I2C_MEMADD_SIZE_8BIT, &val, 1U, 4U);
  g_dac.i2c_status = (uint8_t)status;
  g_dac.i2c_error = HAL_I2C_GetError(&hi2c1);
  return status == HAL_OK;
}

static bool dac_read_reg(uint8_t reg, uint8_t *val)
{
  g_dac.last_reg = reg;
  HAL_StatusTypeDef status = HAL_I2C_Mem_Read(&hi2c1, (uint16_t)(g_dac.i2c_addr7 << 1), reg,
                          I2C_MEMADD_SIZE_8BIT, val, 1U, 4U);
  g_dac.i2c_status = (uint8_t)status;
  g_dac.i2c_error = HAL_I2C_GetError(&hi2c1);
  if (status == HAL_OK) {
    g_dac.actual_value = *val;
  }
  return status == HAL_OK;
}

static bool dac_verify_reg(uint8_t reg, uint8_t expected, uint8_t mask)
{
  uint8_t actual = 0U;
  g_dac.expected_value = expected;
  return dac_read_reg(reg, &actual) && (actual & mask) == (expected & mask);
}

static bool dac_set_reg(uint8_t reg, uint8_t value)
{
  return dac_write_reg(reg, value) && dac_verify_reg(reg, value, 0xFFU);
}

static bool dac_volume(const device_settings_t *settings, uint8_t volume)
{
  uint8_t left = volume + (settings->balance > 0 ? settings->balance : 0);
  uint8_t right = volume + (settings->balance < 0 ? -settings->balance : 0);
  return dac_set_reg(ES9039_REG_VOL_CH1, left) && dac_set_reg(ES9039_REG_VOL_CH2, right);
}

static bool dac_automute_level(uint8_t level)
{
  uint16_t off = level + (level / 4U > 2U ? level / 4U : 2U);
  return dac_set_reg(ES9039_REG_AUTOMUTE_LEVEL_L, level) &&
         dac_set_reg(ES9039_REG_AUTOMUTE_LEVEL_H, 0U) &&
         dac_set_reg(ES9039_REG_AUTOMUTE_OFF_L, (uint8_t)off) &&
         dac_set_reg(ES9039_REG_AUTOMUTE_OFF_H, (uint8_t)(off >> 8));
}

void ES9039_RestoreSettings(void)
{
  const device_settings_t *s = DeviceStore_Settings();
  g_dac.filter_idx = s->filter;
  g_dac.atten_steps = s->startup;
}

void ES9039_ResetState(void)
{
  g_dac.online = false;
  g_dac.chip_id = 0U;
  g_dac.stream_mode = DAC_STREAM_NONE;
  g_dac.automute_active = true;
  g_dac.init_stage = DAC_INIT_NONE;
  s_fail_streak = 0U;
  s_fast_down = false;
}

bool ES9039_InitMuted(void)
{
  /* ADDR1/ADDR0 are grounded on this PCB: 0x48. Probe all documented straps. */
  static const uint8_t candidate_addrs[4] = {0x48U, 0x49U, 0x4AU, 0x4BU};
  uint8_t id = 0U;
  bool found = false;

  g_dac.init_attempts++;
  g_dac.init_stage = DAC_INIT_PROBE;
  es9039_state_t first_ack = g_dac;
  bool any_ack = false;
  for (uint8_t i = 0; i < 4U; i++) {
    g_dac.i2c_addr7 = candidate_addrs[i];
    if (dac_read_reg(ES9039_REG_CHIP_ID, &id)) {
      g_dac.chip_id = id;
      if (id == 0x63U) {
        found = true;
        break;
      }
      if (!any_ack) {
        first_ack = g_dac;
        any_ack = true;
      }
    }
  }

  if (!found) {
    /* Keep an acknowledged unexpected ID visible instead of the final NACK. */
    if (any_ack) {
      g_dac = first_ack;
    } else {
      g_dac.i2c_addr7 = 0x48U;
    }
    if (++s_fail_streak >= 8U) {
      s_fail_streak = 0U;
      Board_I2C1_BusRecover();
    }
    g_dac.online = false;
    return false;
  }

  g_dac.chip_id = id;

  /* Mute outputs before changing clock/datapath registers */
  g_dac.init_stage = DAC_INIT_WRITE;
  bool ok = dac_write_reg(ES9039_REG_MUTE_INV, 0x03U);

  /* Keep GPIO8 strictly in Analog Shutdown (0x0) so ES9312Q RCAL1/2 works undisturbed */
  ok = ok && dac_write_reg(ES9039_REG_DATA_PATH_CFG, 0x00U);
  ok = ok && dac_write_reg(ES9039_REG_GPIO_OUT_EN, 0x00U);
  ok = ok && dac_write_reg(ES9039_REG_GPIO78_CFG, 0x00U);
  ok = ok && dac_write_reg(ES9039_REG_GPIO_INPUT, 0x00U);

  /* Keep DAC1A/1B and DAC2A/2B at AVCC/2 (+1.65 V) during normal mute & automute
   * (clear MUTE_RAMP_TO_GROUND in Reg 125[3]) so muting never steps DC on the
   * DC-coupled OPA1612 -> OPA1622 output stage. Configure fast digital ramp-down
   * (~2.7 ms @ 48 kHz) and smooth fade-in before asserting DAC_MODE. */
  const device_settings_t *settings = DeviceStore_Settings();
  ok = ok && dac_set_reg(ES9039_REG_VOL_UP_RATE, settings->fade_up);
  ok = ok && dac_set_reg(ES9039_REG_VOL_DOWN_RATE, settings->fade_down);
  ok = ok && dac_write_reg(ES9039_REG_VOL_DOWN_FAST, 0xFFU);
  ok = ok && dac_set_reg(ES9039_REG_AUTOMUTE_EN, settings->automute_enabled ? 0xFFU : 0xFCU);
  ok = ok && dac_set_reg(ES9039_REG_AUTOMUTE_TIME_L, settings->automute_time);
  ok = ok && dac_write_reg(ES9039_REG_AUTOMUTE_TIME_H, 0x00U);
  ok = ok && dac_automute_level(settings->automute_level);
  ok = ok && dac_set_reg(ES9039_REG_INVERT, settings->invert);
  ok = ok && dac_write_reg(ES9039_REG_SOFT_RAMP_CFG, 0x06U);

  /* AUTO_FS_DETECT selects 64FS only when required; forcing it breaks lower rates. */
  ok = ok && dac_write_reg(ES9039_REG_SYS_CFG, 0x02U);

  /* System Mode: TDM+DSD+DoP decode enabled, SYNC(0xF7) or ASYNC(0xB7), reserved [5:4]=11b */
  ok = ok && dac_write_reg(ES9039_REG_SYS_MODE, g_dac.sync_mode ? 0xF7U : 0xB7U);

  /* Auto FS detect + Auto MCLK gear + allow 64FS + DPLL BW = 4 */
  ok = ok && dac_write_reg(ES9039_REG_DAC_CLK_CFG, 0x80U);
  ok = ok && dac_write_reg(ES9039_REG_CLK_GEAR, 0x34U);
  ok = ok && dac_write_reg(ES9039_REG_DPLL_BW, 0x40U);

  /* Auto input select (PCM / DoP / Native DSD) + DSD fault protection */
  ok = ok && dac_write_reg(ES9039_REG_INPUT_SEL, 0x41U);

  /* Apply volume and FIR filter shape while still muted */
  ok = ok && dac_volume(settings, g_dac.atten_steps);
  ok = ok && dac_write_reg(ES9039_REG_FILTER_SHAPE, (uint8_t)(0xB8U | (g_dac.filter_idx & 0x07U)));

  if (ok) {
    g_dac.init_stage = DAC_INIT_VERIFY;
    ok = dac_verify_reg(ES9039_REG_SYS_CFG, 0x02U, 0xFFU) &&
         dac_verify_reg(ES9039_REG_SYS_MODE, g_dac.sync_mode ? 0xF7U : 0xB7U, 0xFFU) &&
         dac_verify_reg(ES9039_REG_FILTER_SHAPE, (uint8_t)(0xB8U | g_dac.filter_idx), 0xFFU) &&
         dac_verify_reg(ES9039_REG_AUTOMUTE_TIME_H, 0x00U, 0x08U) &&
         dac_verify_reg(ES9039_REG_MUTE_INV, 0x03U, 0x03U);
  }
  if (ok) {
    g_dac.init_stage = DAC_INIT_READY;
  }
  g_dac.muted = true;
  g_dac.online = ok;
  s_fast_down = false;
  s_fail_streak = 0U;
  return ok;
}

bool ES9039_SetMute(bool mute)
{
  if (!g_dac.online) {
    g_dac.muted = mute;
    return true;
  }
  if (s_fast_down && !mute) {
    if (!dac_write_reg(ES9039_REG_VOL_DOWN_RATE, DeviceStore_Settings()->fade_down)) return false;
    s_fast_down = false;
  }
  if (!dac_write_reg(ES9039_REG_MUTE_INV, mute ? 0x03U : 0x00U)) {
    return false;
  }
  g_dac.muted = mute;
  return true;
}

bool ES9039_SetFastMute(void)
{
  if (!g_dac.online) { g_dac.muted = true; return true; }
  if (!dac_write_reg(ES9039_REG_VOL_DOWN_RATE, 0xFFU) ||
      !dac_write_reg(ES9039_REG_MUTE_INV, 0x03U)) return false;
  s_fast_down = true; g_dac.muted = true;
  return true;
}

bool ES9039_SetAttenuation(uint8_t steps)
{
  const device_settings_t *s = DeviceStore_Settings();
  if (steps < s->ceiling) steps = s->ceiling;
  if (steps > 200U) steps = 200U;
  if (g_dac.online && !dac_volume(s, steps)) return false;
  g_dac.atten_steps = steps;
  return true;
}

bool ES9039_SetFilter(uint8_t filter_idx)
{
  return ES9039_SetSetting(1U, filter_idx & 7U);
}

bool ES9039_SetSetting(uint8_t operation, uint8_t value)
{
  device_settings_t next = *DeviceStore_Settings();
  switch (operation) {
    case 1: next.filter = value; break;
    case 6: next.balance = (int8_t)value; break;
    case 7: next.startup = value; break;
    case 8:
      next.ceiling = value;
      if (next.startup < value) next.startup = value;
      break;
    case 9: next.fade_up = value; break;
    case 10: next.fade_down = value; break;
    case 11: next.automute_level = value; break;
    case 12: next.automute_time = value; break;
    case 13: next.invert = value; break;
    case 14: next.automute_enabled = value; break;
    default: return false;
  }
  if (!DeviceStore_SettingsValid(&next)) return false;
  if (memcmp(&next, DeviceStore_Settings(), sizeof(next)) == 0) return true;
  uint8_t volume = g_dac.atten_steps < next.ceiling ? next.ceiling : g_dac.atten_steps;
  bool ok = true;
  if (g_dac.online) {
    switch (operation) {
      case 1: ok = dac_set_reg(ES9039_REG_FILTER_SHAPE, 0xB8U | next.filter); break;
      case 6: case 8: ok = dac_volume(&next, volume); break;
      case 9: ok = dac_set_reg(ES9039_REG_VOL_UP_RATE, next.fade_up); break;
      case 10: if (!s_fast_down) ok = dac_set_reg(ES9039_REG_VOL_DOWN_RATE, next.fade_down); break;
      case 11: ok = dac_automute_level(next.automute_level); break;
      case 12:
        ok = dac_set_reg(ES9039_REG_AUTOMUTE_TIME_L, next.automute_time) &&
             dac_set_reg(ES9039_REG_AUTOMUTE_TIME_H, 0U);
        break;
      case 13: {
        bool was_muted = g_dac.muted;
        ok = ES9039_SetFastMute();
        if (ok) { HAL_Delay(2U); ok = dac_set_reg(ES9039_REG_INVERT, next.invert); }
        if (ok && !was_muted) ok = ES9039_SetMute(false);
        break;
      }
      case 14: ok = dac_set_reg(ES9039_REG_AUTOMUTE_EN, next.automute_enabled ? 0xFFU : 0xFCU); break;
    }
  }
  if (!ok) {
    (void)ES9039_SetFastMute(); g_dac.online = false;
    return false;
  }
  DeviceStore_UpdateSettings(&next);
  g_dac.atten_steps = volume; g_dac.filter_idx = next.filter;
  return true;
}

bool ES9039_SetSyncMode(bool sync_mode)
{
  /* This board uses clocks from the same oscillator. Async mode needs a separate
   * rate-dependent divider setup and cannot use AUTO_FS_DETECT (datasheet reg 3). */
  if (!sync_mode) {
    return false;
  }
  g_dac.sync_mode = sync_mode;
  if (!g_dac.online) {
    return true;
  }
  return dac_write_reg(ES9039_REG_SYS_MODE, sync_mode ? 0xF7U : 0xB7U);
}

bool ES9039_PollStatus(void)
{
  if (!g_dac.online) {
    return false;
  }
  uint8_t id = 0U, r242 = 0U, r245 = 0U;
  if (!dac_read_reg(ES9039_REG_CHIP_ID, &id) || id != 0x63U ||
      !dac_read_reg(ES9039_REG_AUTOMUTE_STATUS, &r242) ||
      !dac_read_reg(ES9039_REG_STREAM_STATUS, &r245)) {
    if (++s_fail_streak >= 2U) {
      g_dac.online = false;
      s_fail_streak = 0U;
      Board_I2C1_BusRecover();
    }
    return false;
  }
  s_fail_streak = 0U;
  g_dac.chip_id = id;
  g_dac.raw_reg242 = r242;
  g_dac.raw_reg245 = r245;
  g_dac.automute_active = (r242 & 0x03U) != 0U;

  if (r245 & 0x04U) {
    g_dac.stream_mode = DAC_STREAM_DOP;
  } else if ((r245 & 0x03U) == 0x01U) {
    g_dac.stream_mode = DAC_STREAM_DSD;
  } else if (r245 & 0x08U) {
    g_dac.stream_mode = DAC_STREAM_PCM;
  } else {
    g_dac.stream_mode = DAC_STREAM_NONE;
  }
  return true;
}

const es9039_state_t *ES9039_GetState(void)
{
  return &g_dac;
}
