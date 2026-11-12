#ifndef ES9039Q2M_H
#define ES9039Q2M_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
  DAC_STREAM_NONE = 0,
  DAC_STREAM_PCM  = 1,
  DAC_STREAM_DSD  = 2,
  DAC_STREAM_DOP  = 3,
} dac_stream_mode_t;

typedef enum {
  DAC_INIT_NONE = 0,
  DAC_INIT_PROBE,
  DAC_INIT_WRITE,
  DAC_INIT_VERIFY,
  DAC_INIT_READY,
} dac_init_stage_t;

typedef struct {
  bool online;
  bool muted;
  bool sync_mode;
  bool automute_active;
  uint8_t i2c_addr7;
  uint8_t chip_id;
  uint8_t atten_steps;   /* 0..255, 0.5 dB per step */
  uint8_t filter_idx;    /* 0..7 */
  uint8_t raw_reg242;
  uint8_t raw_reg245;
  dac_stream_mode_t stream_mode;
  dac_init_stage_t init_stage;
  uint8_t last_reg;
  uint8_t expected_value;
  uint8_t actual_value;
  uint8_t i2c_status;
  uint32_t i2c_error;
  uint32_t init_attempts;
} es9039_state_t;

void ES9039_ResetState(void);
void ES9039_RestoreSettings(void);
bool ES9039_InitMuted(void);
bool ES9039_SetMute(bool mute);
bool ES9039_SetFastMute(void);
bool ES9039_SetAttenuation(uint8_t steps);
bool ES9039_SetFilter(uint8_t filter_idx);
bool ES9039_SetSetting(uint8_t operation, uint8_t value);
bool ES9039_SetSyncMode(bool sync_mode);
bool ES9039_PollStatus(void);
const es9039_state_t *ES9039_GetState(void);

#endif /* ES9039Q2M_H */
