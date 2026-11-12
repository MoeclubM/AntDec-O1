#ifndef ANTDEC_STATE_H_
#define ANTDEC_STATE_H_

#include <stdint.h>

/* Audio format IDs matching STM32 xmos_link.h */
#define ANTDEC_FMT_IDLE             0u
#define ANTDEC_FMT_PCM              1u
#define ANTDEC_FMT_DOP              2u
#define ANTDEC_FMT_NATIVE_DSD       3u
#define ANTDEC_FMT_MQA              4u
#define ANTDEC_FMT_MQA_STUDIO       5u
#define ANTDEC_FMT_MQA_RENDERER     6u

/* USB Driver compatibility modes matching STM32 xmos_link.h */
#define ANTDEC_USB_UAC2             0u  /* 0x20B1:0x0016 AntDec O1 UAC 2.0 HS */
#define ANTDEC_USB_SHANLING         1u  /* 0x20B1:0x301F Shanling USB Audio ASIO */
#define ANTDEC_USB_FIIO             2u  /* 0x2972:0x0093 FiiO USB DAC-O1 ASIO */
#define ANTDEC_USB_UAC1             3u  /* 0x20B1:0x0017 AntDec O1 UAC 1.0 FS */
#define ANTDEC_USB_MODE_COUNT       4u

/* STM32 -> XMOS commands matching STM32 xmos_link.h */
#define ANTDEC_CMD_SET_USB_MODE     1u
#define ANTDEC_CMD_SET_DSP_PRESET   2u
#define ANTDEC_CMD_HID_MEDIA        3u
#define ANTDEC_CMD_SET_CROSSFEED    4u

/* STM32 HID media key IDs */
#define ANTDEC_HID_PLAY_PAUSE       1u
#define ANTDEC_HID_NEXT_TRACK       2u
#define ANTDEC_HID_PREV_TRACK       3u
#define ANTDEC_HID_MUTE             4u
#define ANTDEC_HID_VOL_UP           5u
#define ANTDEC_HID_VOL_DOWN         6u

/* DSP presets */
#define ANTDEC_DSP_BYPASS           0u
#define ANTDEC_DSP_BASS             1u
#define ANTDEC_DSP_VOCAL            2u
#define ANTDEC_DSP_WARM             3u
#define ANTDEC_DSP_HARMAN           4u
#define ANTDEC_DSP_CUSTOM           5u
#define ANTDEC_DSP_PRESET_COUNT     6u

#define ANTDEC_PEQ_BANDS            10u
/* Higher PCM rates remain bit-transparent until a timing budget is measured on hardware. */
#define ANTDEC_DSP_MAX_RATE         192000u

/* Filter types for custom PEQ */
#define ANTDEC_FILT_OFF             0u
#define ANTDEC_FILT_PEAK            1u
#define ANTDEC_FILT_LOW_SHELF       2u
#define ANTDEC_FILT_HIGH_SHELF      3u
#define ANTDEC_FILT_LOW_PASS        4u
#define ANTDEC_FILT_HIGH_PASS       5u

/* Inter-tile bridge message tags (tile[0] <-> tile[1]) */
#define BRIDGE_MSG_POLL_TELEMETRY   0x10u
#define BRIDGE_MSG_POLL_FAST_MUTE   0x11u
#define BRIDGE_MSG_POLL_METERS      0x12u
#define BRIDGE_MSG_SET_DSP          0x20u
#define BRIDGE_MSG_SET_PEQ_BAND     0x21u
#define BRIDGE_MSG_CLEAR_PEQ        0x22u

#ifndef __XC__
typedef struct {
    uint16_t freq_hz;       /* 20 .. 40000 Hz */
    int16_t  gain_cB;       /* -1800 .. +1800 (-18.00 .. +18.00 dB, 0.01 dB step) */
    uint16_t q_x100;        /* 10 .. 1600 (0.10 .. 16.00) */
    uint8_t  filter_type;   /* ANTDEC_FILT_* */
    uint8_t  enabled;
} antdec_peq_band_t;

/* Shared state on tile[0] (USB / Endpoint0 / Stm32LinkTask) */
typedef struct {
    volatile uint32_t usb_active;
    volatile uint32_t stream_playing;
    volatile uint32_t usb_muted;
    volatile uint32_t usb_mode;
    volatile uint32_t pending_usb_mode;
    volatile uint32_t reboot_requested;
    volatile uint32_t save_flash_requested;
    volatile uint32_t dsp_preset;
    volatile uint32_t crossfeed_en;
    volatile uint32_t peq_custom_active;
    volatile uint32_t peq_dirty_mask;
    volatile uint32_t hid_pending_mask;
    /* Latest audio telemetry mirrored from tile[1] */
    volatile uint32_t aud_format;
    volatile uint32_t aud_sample_rate;
    volatile uint32_t aud_bit_depth;
    volatile uint32_t aud_mqa_orig_rate;
    volatile uint32_t aud_hw_muted;
    volatile uint32_t meter_left;
    volatile uint32_t meter_right;
    volatile uint32_t meter_valid;
} antdec_usb_tile_state_t;

/* Shared state on tile[1] (AudioHub / DspWorkerTask / AudioCtrlTask) */
typedef struct {
    volatile uint32_t sample_rate;
    volatile uint32_t mclk_hz;
    volatile uint32_t dsd_mode;
    volatile uint32_t bit_depth;
    volatile uint32_t hw_muted;
    volatile uint32_t fast_mute_req;
    volatile uint32_t mute_drained;
    volatile uint32_t format;
    volatile uint32_t mqa_mode;
    volatile uint32_t mqa_orig_rate;
    volatile uint32_t dsp_preset;
    volatile uint32_t crossfeed_en;
    volatile uint32_t peq_custom_active;
    volatile uint32_t coeff_rebuild_seq;
    volatile uint32_t telemetry_dirty;
    volatile uint32_t peak_left;
    volatile uint32_t peak_right;
} antdec_aud_tile_state_t;

extern antdec_usb_tile_state_t g_usb_state;
extern antdec_peq_band_t g_usb_custom_peq[ANTDEC_PEQ_BANDS];
extern int16_t g_usb_preamp_cB;

extern antdec_aud_tile_state_t g_aud_state;
extern antdec_peq_band_t g_aud_custom_peq[ANTDEC_PEQ_BANDS];
extern int16_t g_aud_preamp_cB;
#endif

#endif /* ANTDEC_STATE_H_ */
