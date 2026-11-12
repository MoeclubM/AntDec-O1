#ifndef POWER_SEQ_H
#define POWER_SEQ_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
  PWR_STATE_WAIT_12V = 0,     /* Waiting for LM27762 PGOOD == LOW (+-4.5V rail valid) */
  PWR_STATE_EN_AVDD,          /* Step 1: DAC_AVDD_EN HIGH -> 5 ms */
  PWR_STATE_EN_VCCA,          /* Step 2: DAC_VCCA_EN HIGH -> 5 ms */
  PWR_STATE_WAIT_9312PG,      /* Step 3: >=5 ms AVCC settle, both PG >=2.40V stable for 5 ms */
  PWR_STATE_DAC_RESET_WAIT,   /* Step 4a: DAC_CHIP_EN HIGH -> 10 ms t_init */
  PWR_STATE_WAIT_DAC_MCLK,    /* Step 4b: Probe/init ES9039Q2M over I2C1 (needs MCLK) */
  PWR_STATE_AMP_SETTLE,       /* Step 5a: 20 ms I/V settle -> enable OPA1622 + audio path */
  PWR_STATE_UNMUTE_WAIT,      /* Step 5b: 5 ms amp settle -> unmute ES9039Q2M */
  PWR_STATE_READY,            /* Normal playback operation */
  PWR_STATE_FAULT,            /* ES9312 PG timeout/brownout cooldown before retry */
} power_state_t;

typedef enum {
  PWR_FAULT_NONE = 0,
  PWR_FAULT_9312_PG_TIMEOUT,
  PWR_FAULT_9312_BROWNOUT,
  PWR_FAULT_DAC_LOST,
} power_fault_t;

typedef struct {
  power_state_t state;
  power_fault_t last_fault;
  bool pwr_ok_4v5;
  bool amp_enabled;
  bool user_muted;
  uint16_t vdda_mv;
  uint16_t pg1_mv;
  uint16_t pg2_mv;
  uint32_t state_enter_ms;
} power_status_t;

void PowerSeq_Init(void);
void PowerSeq_Tick(uint32_t now_ms);
void PowerSeq_SetUserMute(bool mute);
const power_status_t *PowerSeq_GetStatus(void);

#endif /* POWER_SEQ_H */
