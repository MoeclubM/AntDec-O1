#include "usb_debug.h"
#include "board_hw.h"
#include "es9039q2m.h"
#include "power_seq.h"
#include "xmos_link.h"
#include "usb_device.h"
#include "usbd_cdc_if.h"
#include <stdio.h>
#include <string.h>

extern USBD_HandleTypeDef hUsbDeviceFS;

static uint8_t s_tx[1024];
static char s_rx[64];
static volatile uint8_t s_rx_ready;
static uint8_t s_rx_len;
static volatile uint8_t s_open;
static uint8_t s_stream = 1U;
static uint32_t s_last_ms;
static uint8_t s_json;
/* J-Link recovery mailbox: select standard USB even if an experimental identity
 * has no Windows driver. UINT32_MAX means no request. Consumed by the main loop. */
volatile uint32_t g_debug_usb_mode_request = UINT32_MAX;

static const char *power_name(power_state_t state)
{
  static const char *const names[] = {
    "WAIT_POWER", "AVDD_SETTLE", "VCCA_SETTLE", "WAIT_PG", "DAC_RESET",
    "WAIT_DAC", "AMP_SETTLE", "UNMUTE", "READY", "FAULT"
  };
  return names[state];
}

void UsbDebug_SetOpen(uint8_t open)
{
  s_open = open;
  s_rx_ready = s_rx_len = 0U;
  s_json = 0U;
  s_stream = 1U;
  s_last_ms = HAL_GetTick() - 1000U;
}

static int api_status(unsigned id, bool ok)
{
  const power_status_t *p = PowerSeq_GetStatus();
  const es9039_state_t *d = ES9039_GetState();
  const xmos_telemetry_t *x = XmosLink_GetTelemetry();
  return snprintf((char *)s_tx, sizeof(s_tx),
    "{\"type\":\"status\",\"version\":1,\"id\":%u,\"ok\":%s,"
    "\"power\":\"%s\",\"fault\":%u,\"amp\":%u,\"mute\":%u,\"dac\":%u,"
    "\"filter\":%u,\"attenuation\":%u,\"pg1\":%u,\"pg2\":%u,\"vdda\":%u,"
    "\"link\":%u,\"usb\":%u,\"playing\":%u,\"format\":%u,\"rate\":%lu,"
    "\"bits\":%u,\"dsp\":%u,\"crossfeed\":%u,\"usbMode\":%u,"
    "\"meterValid\":%u,\"left\":%u,\"right\":%u}\r\n",
    id, ok ? "true" : "false", power_name(p->state), (unsigned)p->last_fault,
    p->amp_enabled, p->user_muted, d->online, d->filter_idx, d->atten_steps,
    p->pg1_mv, p->pg2_mv, p->vdda_mv, x->link_online, x->usb_active,
    x->playing, (unsigned)x->format, (unsigned long)x->sample_rate_hz,
    x->bit_depth, x->dsp_preset, x->crossfeed_enabled, (unsigned)x->usb_mode,
    x->link_online && x->playing && x->meter_valid, x->meter_left, x->meter_right);
}

static int api_command(void)
{
  unsigned id = 0U;
  int value = 0;
  char op[16] = {0};
  int fields = sscanf(s_rx, "api %u %15s %d", &id, op, &value);
  bool ok = fields >= 2;
  s_json = 1U;
  s_stream = 0U;
  if (ok && strcmp(op, "status") == 0) {
    return api_status(id, true);
  }
  if (fields != 3) return api_status(id, false);
  if (strcmp(op, "filter") == 0 && value >= 0 && value <= 7) {
    ok = ES9039_SetFilter((uint8_t)value);
  } else if (strcmp(op, "volume") == 0 && value >= 0 && value <= 200) {
    ok = ES9039_SetAttenuation((uint8_t)value);
  } else if (strcmp(op, "mute") == 0 && (value == 0 || value == 1)) {
    PowerSeq_SetUserMute(value != 0);
  } else if (strcmp(op, "dsp") == 0 && value >= 0 && value < 6 && XmosLink_GetTelemetry()->link_online) {
    XmosLink_SendCommand(XMOS_CMD_SET_DSP_PRESET, (uint8_t)value);
  } else if (strcmp(op, "crossfeed") == 0 && (value == 0 || value == 1) && XmosLink_GetTelemetry()->link_online) {
    XmosLink_SendCommand(XMOS_CMD_SET_CROSSFEED, (uint8_t)value);
  } else {
    ok = false;
  }
  return api_status(id, ok);
}

void UsbDebug_OnRx(const uint8_t *data, uint32_t length)
{
  if (s_rx_ready) return;
  for (uint32_t i = 0U; i < length; ++i) {
    if (data[i] == '\r' || data[i] == '\n') {
      if (s_rx_len != 0U) {
        s_rx[s_rx_len] = '\0';
        s_rx_ready = 1U;
        s_rx_len = 0U;
        break;
      }
    } else if (s_rx_len < sizeof(s_rx) - 1U) {
      s_rx[s_rx_len++] = (char)data[i];
    }
  }
}

static int status_text(uint32_t now_ms)
{
  const power_status_t *p = PowerSeq_GetStatus();
  const es9039_state_t *d = ES9039_GetState();
  const xmos_telemetry_t *x = XmosLink_GetTelemetry();
  return snprintf((char *)s_tx, sizeof(s_tx),
    "AntDec O1 USB debug\r\nms=%lu power=%s fault=%u amp=%u mute=%u\r\n"
    "pgood_n=%u PG1=%umV PG2=%umV VDDA=%umV\r\n"
    "enable: AVDD=%u VCCA=%u AVCC=%u CHIP=%u AMP=%u\r\n"
    "DAC online=%u addr=0x%02X id=0x%02X init_stage=%u attempts=%lu\r\n"
    "I2C status=%u error=0x%08lX reg=%u expected=0x%02X actual=0x%02X SCL=%u SDA=%u SR1=0x%04lX SR2=0x%04lX\r\n"
    "XMOS link=%u powered_hint=%u usb=%u streaming=%u rate=%lu bits=%u frames=%lu RX=%u TX=%u\r\n"
    "MCLK=not_measurable_on_STM32; I2C failure alone does not prove missing MCLK.\r\n",
    (unsigned long)now_ms, power_name(p->state), (unsigned)p->last_fault,
    p->amp_enabled, p->user_muted,
    (unsigned)HAL_GPIO_ReadPin(PWR_OK_PORT, PWR_OK_PIN), p->pg1_mv, p->pg2_mv, p->vdda_mv,
    !!(GPIOC->ODR & GPIO_PIN_1), !!(GPIOC->ODR & GPIO_PIN_2), !!(GPIOC->ODR & GPIO_PIN_3),
    !!(GPIOC->ODR & GPIO_PIN_0), !!(GPIOC->ODR & GPIO_PIN_4),
    d->online, d->i2c_addr7, d->chip_id, (unsigned)d->init_stage, (unsigned long)d->init_attempts,
    d->i2c_status, (unsigned long)d->i2c_error, d->last_reg, d->expected_value, d->actual_value,
    (unsigned)HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_6), (unsigned)HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_7),
    (unsigned long)I2C1->SR1, (unsigned long)I2C1->SR2,
    x->link_online, x->board_powered, x->usb_active, x->playing, (unsigned long)x->sample_rate_hz,
    x->bit_depth, (unsigned long)x->rx_frames_ok,
    (unsigned)HAL_GPIO_ReadPin(XMOS_RX_PORT, XMOS_RX_PIN),
    (unsigned)HAL_GPIO_ReadPin(XMOS_TX_PORT, XMOS_TX_PIN));
}

void UsbDebug_Tick(uint32_t now_ms)
{
  if (g_debug_usb_mode_request < 4U) {
    uint8_t mode = (uint8_t)g_debug_usb_mode_request;
    g_debug_usb_mode_request = UINT32_MAX;
    XmosLink_SendCommand(XMOS_CMD_SET_USB_MODE, mode);
  }
  if (hUsbDeviceFS.dev_state != USBD_STATE_CONFIGURED || !s_open || hUsbDeviceFS.pClassData == NULL) return;
  USBD_CDC_HandleTypeDef *cdc = hUsbDeviceFS.pClassData;
  if (cdc->TxState != 0U) return;

  int length = 0;
  if (s_rx_ready) {
    if (strncmp(s_rx, "api ", 4U) == 0) {
      length = api_command();
    } else if (strcmp(s_rx, "stream off") == 0) {
      s_stream = 0U;
      length = snprintf((char *)s_tx, sizeof(s_tx), "stream off\r\n");
    } else if (strcmp(s_rx, "stream on") == 0) {
      s_stream = 1U;
      length = status_text(now_ms);
    } else if (strcmp(s_rx, "status") == 0) {
      length = status_text(now_ms);
    } else {
      length = snprintf((char *)s_tx, sizeof(s_tx), "Commands: status, stream on, stream off\r\nI2C: 0x04=NACK, 0x20=timeout; init stage 1=ID probe, 2=write, 3=verify, 4=ready.\r\n");
    }
    s_rx_ready = 0U;
  } else if (s_stream && now_ms - s_last_ms >= 1000U) {
    length = s_json ? api_status(0U, true) : status_text(now_ms);
  }
  if (length > 0 && length < (int)sizeof(s_tx)) {
    s_last_ms = now_ms;
    (void)CDC_Transmit_FS(s_tx, (uint16_t)length);
  }
}
