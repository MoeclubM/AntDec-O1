#include "power_seq.h"
#include "board_hw.h"
#include "es9039q2m.h"
#include "xmos_link.h"
#include "device_store.h"

#define PG_VALID_MV      2400U
#define PG_BROWNOUT_MV   2000U
#define AVCC_SETTLE_MS   5U
#define PG_STABLE_MS     5U
#define TRANSIENT_MUTE_MS 48U

static power_status_t g_pwr = {
  .state = PWR_STATE_WAIT_12V,
  .last_fault = PWR_FAULT_NONE,
  .pwr_ok_4v5 = false,
  .amp_enabled = false,
  .user_muted = false,
  .vdda_mv = 3300U,
  .pg1_mv = 0U,
  .pg2_mv = 0U,
  .state_enter_ms = 0U,
};

static uint32_t s_last_adc_ms = 0U;
static uint32_t s_last_dac_poll_ms = 0U;
static uint32_t s_pwr_ok_low_since_ms = 0U;
static uint32_t s_pg_good_since_ms = 0U;
static bool s_transient_muted = false;
static uint32_t s_transient_mute_until_ms = 0U;

static void enter_state(power_state_t st, uint32_t now_ms)
{
  g_pwr.state = st;
  g_pwr.state_enter_ms = now_ms;
}

static void emergency_shutdown(power_state_t next_state, power_fault_t fault, uint32_t now_ms)
{
  /* Cut the amp before slower I2C and rail shutdown operations. */
  HAL_GPIO_WritePin(AMP_EN_REQ_PORT, AMP_EN_REQ_PIN, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(AUDIO_PATH_EN_PORT, AUDIO_PATH_EN_PIN, GPIO_PIN_RESET);
  g_pwr.amp_enabled = false;
  s_transient_muted = false;

  /* 2. Fast-mute DAC if still reachable */
  if (ES9039_GetState()->online) {
    ES9039_SetFastMute();
  }
  ES9039_ResetState();

  /* 3. Reverse-sequence DAC chip enable & LDO rails */
  HAL_GPIO_WritePin(DAC_CHIP_EN_PORT, DAC_CHIP_EN_PIN, GPIO_PIN_RESET);
  Board_DelayUs(200);
  HAL_GPIO_WritePin(DAC_AVCC_EN_PORT, DAC_AVCC_EN_PIN, GPIO_PIN_RESET);
  Board_DelayUs(200);
  HAL_GPIO_WritePin(DAC_VCCA_EN_PORT, DAC_VCCA_EN_PIN, GPIO_PIN_RESET);
  Board_DelayUs(200);
  HAL_GPIO_WritePin(DAC_AVDD_EN_PORT, DAC_AVDD_EN_PIN, GPIO_PIN_RESET);

  if (fault != PWR_FAULT_NONE) {
    g_pwr.last_fault = fault;
  }
  enter_state(next_state, now_ms);
}

void PowerSeq_Init(void)
{
  uint32_t now = HAL_GetTick();
  Board_ReadAnalogRails(&g_pwr.vdda_mv, &g_pwr.pg1_mv, &g_pwr.pg2_mv);
  g_pwr.pwr_ok_4v5 = (HAL_GPIO_ReadPin(PWR_OK_PORT, PWR_OK_PIN) == GPIO_PIN_RESET);
  s_pwr_ok_low_since_ms = now;
  emergency_shutdown(PWR_STATE_WAIT_12V, PWR_FAULT_NONE, now);
}

void PowerSeq_SetUserMute(bool mute)
{
  g_pwr.user_muted = mute;
  if (g_pwr.state == PWR_STATE_READY) {
    const xmos_telemetry_t *xm = XmosLink_GetTelemetry();
    bool want_mute = mute || DeviceStore_SyncActive() || s_transient_muted || (xm->link_online && xm->muted);
    (void)ES9039_SetMute(want_mute);
  }
}

void PowerSeq_Tick(uint32_t now_ms)
{
  /* LM27762 PGOOD is active LOW (0 = +-4.5V analog rails in regulation) */
  bool pwr_ok = (HAL_GPIO_ReadPin(PWR_OK_PORT, PWR_OK_PIN) == GPIO_PIN_RESET);
  if (!pwr_ok) {
    s_pwr_ok_low_since_ms = now_ms;
  }
  g_pwr.pwr_ok_4v5 = pwr_ok;

  if ((now_ms - s_last_adc_ms) >= 10U || g_pwr.state == PWR_STATE_WAIT_9312PG) {
    s_last_adc_ms = now_ms;
    Board_ReadAnalogRails(&g_pwr.vdda_mv, &g_pwr.pg1_mv, &g_pwr.pg2_mv);
  }

  /* Instant loss-of-12V / +-4.5V protection in any active state */
  if (!pwr_ok && g_pwr.state != PWR_STATE_WAIT_12V) {
    emergency_shutdown(PWR_STATE_WAIT_12V, PWR_FAULT_NONE, now_ms);
    return;
  }

  /* Instant ES9312Q AVCC brownout protection once AVCC has come up */
  if (g_pwr.state >= PWR_STATE_DAC_RESET_WAIT && g_pwr.state <= PWR_STATE_READY) {
    if (g_pwr.pg1_mv < PG_BROWNOUT_MV || g_pwr.pg2_mv < PG_BROWNOUT_MV) {
      emergency_shutdown(PWR_STATE_FAULT, PWR_FAULT_9312_BROWNOUT, now_ms);
      return;
    }
  }

  uint32_t elapsed = now_ms - g_pwr.state_enter_ms;

  switch (g_pwr.state) {
    case PWR_STATE_WAIT_12V:
      if (pwr_ok && (now_ms - s_pwr_ok_low_since_ms) >= 20U) {
        g_pwr.last_fault = PWR_FAULT_NONE;
        HAL_GPIO_WritePin(DAC_AVDD_EN_PORT, DAC_AVDD_EN_PIN, GPIO_PIN_SET);
        enter_state(PWR_STATE_EN_AVDD, now_ms);
      }
      break;

    case PWR_STATE_EN_AVDD:
      if (elapsed >= 5U) {
        HAL_GPIO_WritePin(DAC_VCCA_EN_PORT, DAC_VCCA_EN_PIN, GPIO_PIN_SET);
        enter_state(PWR_STATE_EN_VCCA, now_ms);
      }
      break;

    case PWR_STATE_EN_VCCA:
      if (elapsed >= 5U) {
        HAL_GPIO_WritePin(DAC_AVCC_EN_PORT, DAC_AVCC_EN_PIN, GPIO_PIN_SET);
        s_pg_good_since_ms = 0U;
        enter_state(PWR_STATE_WAIT_9312PG, now_ms);
      }
      break;

    case PWR_STATE_WAIT_9312PG:
      if (g_pwr.pg1_mv >= PG_VALID_MV && g_pwr.pg2_mv >= PG_VALID_MV) {
        if (s_pg_good_since_ms == 0U) {
          s_pg_good_since_ms = now_ms;
        }
      } else {
        s_pg_good_since_ms = 0U;
      }
      /* A single high PG sample does not establish that AVCC has settled. */
      if (elapsed >= AVCC_SETTLE_MS && s_pg_good_since_ms != 0U &&
          now_ms - s_pg_good_since_ms >= PG_STABLE_MS) {
        HAL_GPIO_WritePin(DAC_CHIP_EN_PORT, DAC_CHIP_EN_PIN, GPIO_PIN_SET);
        enter_state(PWR_STATE_DAC_RESET_WAIT, now_ms);
      } else if (elapsed >= 50U) {
        emergency_shutdown(PWR_STATE_FAULT, PWR_FAULT_9312_PG_TIMEOUT, now_ms);
      }
      break;

    case PWR_STATE_DAC_RESET_WAIT:
      if (elapsed >= 10U) {
        s_last_dac_poll_ms = now_ms - 100U;
        enter_state(PWR_STATE_WAIT_DAC_MCLK, now_ms);
      }
      break;

    case PWR_STATE_WAIT_DAC_MCLK:
      if ((now_ms - s_last_dac_poll_ms) >= 100U) {
        s_last_dac_poll_ms = now_ms;
        if (ES9039_InitMuted()) {
          enter_state(PWR_STATE_AMP_SETTLE, now_ms);
        }
      }
      break;

    case PWR_STATE_AMP_SETTLE:
      if (elapsed >= 25U) {
        HAL_GPIO_WritePin(AMP_EN_REQ_PORT, AMP_EN_REQ_PIN, GPIO_PIN_SET);
        HAL_GPIO_WritePin(AUDIO_PATH_EN_PORT, AUDIO_PATH_EN_PIN, GPIO_PIN_SET);
        g_pwr.amp_enabled = true;
        enter_state(PWR_STATE_UNMUTE_WAIT, now_ms);
      }
      break;

    case PWR_STATE_UNMUTE_WAIT:
      if (elapsed >= 5U) {
        const xmos_telemetry_t *xm = XmosLink_GetTelemetry();
        bool want_mute = g_pwr.user_muted || DeviceStore_SyncActive() || (xm->link_online && xm->muted);
        if (!ES9039_SetMute(want_mute)) {
          emergency_shutdown(PWR_STATE_FAULT, PWR_FAULT_DAC_LOST, now_ms);
          break;
        }
        s_last_dac_poll_ms = now_ms;
        enter_state(PWR_STATE_READY, now_ms);
      }
      break;

    case PWR_STATE_READY:
      if (XmosLink_ConsumeFastMute()) {
        (void)ES9039_SetFastMute();
        s_transient_muted = true;
        s_transient_mute_until_ms = now_ms + TRANSIENT_MUTE_MS;
        s_last_dac_poll_ms = now_ms;
      }
      if (s_transient_muted) {
        if ((int32_t)(now_ms - s_transient_mute_until_ms) >= 0) {
          const xmos_telemetry_t *xm_chk = XmosLink_GetTelemetry();
          if (!(xm_chk->link_online && xm_chk->muted)) {
            s_transient_muted = false;
          }
        } else {
          /* Defer DAC status poll across brief MCLK crystal switchover so OPA1622 stays enabled */
          s_last_dac_poll_ms = now_ms;
        }
      }
      {
        const xmos_telemetry_t *xm = XmosLink_GetTelemetry();
        bool want_mute = g_pwr.user_muted || DeviceStore_SyncActive() || s_transient_muted || (xm->link_online && xm->muted);
        if (ES9039_GetState()->online && ES9039_GetState()->muted != want_mute) {
          (void)ES9039_SetMute(want_mute);
        }
      }
      if ((now_ms - s_last_dac_poll_ms) >= 150U) {
        s_last_dac_poll_ms = now_ms;
        if (!ES9039_PollStatus() && !ES9039_GetState()->online) {
          /* MCLK stopped or DAC reset: cut amp pop-free and wait for MCLK recovery */
          HAL_GPIO_WritePin(AMP_EN_REQ_PORT, AMP_EN_REQ_PIN, GPIO_PIN_RESET);
          HAL_GPIO_WritePin(AUDIO_PATH_EN_PORT, AUDIO_PATH_EN_PIN, GPIO_PIN_RESET);
          g_pwr.amp_enabled = false;
          g_pwr.last_fault = PWR_FAULT_DAC_LOST;
          enter_state(PWR_STATE_WAIT_DAC_MCLK, now_ms);
        }
      }
      break;

    case PWR_STATE_FAULT:
      if (elapsed >= 1500U && pwr_ok) {
        HAL_GPIO_WritePin(DAC_AVDD_EN_PORT, DAC_AVDD_EN_PIN, GPIO_PIN_SET);
        enter_state(PWR_STATE_EN_AVDD, now_ms);
      }
      break;
  }
}

const power_status_t *PowerSeq_GetStatus(void)
{
  return &g_pwr;
}
