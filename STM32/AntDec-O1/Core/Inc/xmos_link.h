#ifndef XMOS_LINK_H
#define XMOS_LINK_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
  XMOS_FMT_IDLE         = 0,
  XMOS_FMT_PCM          = 1,
  XMOS_FMT_DOP          = 2,
  XMOS_FMT_NATIVE_DSD   = 3,
  XMOS_FMT_MQA          = 4,
  XMOS_FMT_MQA_STUDIO   = 5,
  XMOS_FMT_MQA_RENDERER = 6,
} xmos_format_t;

typedef enum {
  XMOS_USB_UAC2     = 0,
  XMOS_USB_SHANLING = 1,
  XMOS_USB_FIIO     = 2,
  XMOS_USB_UAC1     = 3,
} xmos_usb_mode_t;

typedef enum {
  XMOS_CMD_SET_USB_MODE  = 1,
  XMOS_CMD_SET_DSP_PRESET = 2,
  XMOS_CMD_HID_MEDIA     = 3,
  XMOS_CMD_SET_CROSSFEED = 4,
} xmos_cmd_t;

typedef enum {
  XMOS_HID_PLAY_PAUSE = 1,
  XMOS_HID_NEXT_TRACK = 2,
  XMOS_HID_PREV_TRACK = 3,
  XMOS_HID_MUTE       = 4,
} xmos_hid_key_t;

typedef struct {
  bool link_online;      /* Valid CRC8 PWM frame received within last 300 ms */
  bool board_powered;    /* XMOS 3.3V pull-up detected on J203/J6 */
  bool usb_active;
  bool playing;
  bool muted;
  bool peq_custom;
  bool crossfeed_enabled;
  xmos_format_t format;
  uint32_t sample_rate_hz;
  uint8_t bit_depth;
  xmos_usb_mode_t usb_mode;
  uint8_t dsp_preset;
  uint32_t rx_frames_ok;
  uint32_t fast_mute_pulses;
  uint32_t last_rx_ms;
  bool meter_valid;
  uint8_t meter_left;
  uint8_t meter_right;
  uint32_t last_meter_ms;
  uint32_t play_seconds;
} xmos_telemetry_t;

void XmosLink_Init(void);
void XmosLink_OnRxEdge(void);
void XmosLink_Tick(uint32_t now_ms);
bool XmosLink_ConsumeFastMute(void);
void XmosLink_SendCommand(xmos_cmd_t cmd, uint8_t arg);
const xmos_telemetry_t *XmosLink_GetTelemetry(void);

#endif /* XMOS_LINK_H */
