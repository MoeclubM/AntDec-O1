#include "usb_compat.h"
#include "clock_monitor.h"
#include "antdec_state.h"
#include "xua.h"
#include "xud_device.h"
#include "xua_endpoint0.h"
#include "vendorrequests.h"
#include "audiostream.h"
#include "hostactive.h"
#include "user_hid.h"
#include "controller_link.h"
#include "settings_manager.h"
#include "config_store.h"
#include <string.h>

/* Untouched top-of-SRAM retention slot on tile[0] (0xFFFCC is used by dfu.xc, 0xFFFD0..0xFFFFF is free) */
#define SRAM_RETENTION_ADDR         0xFFFD0u
#define SRAM_RETENTION_MAGIC        0x414E5431u

/* AntDec-specific USB control requests; third-party protocols are unverified. */
#define VREQ_ANTDEC_GET_STATUS      0xA0u
#define VREQ_ANTDEC_SET_USB_MODE    0xA1u
#define VREQ_ANTDEC_SET_DSP_PRESET  0xA2u
#define VREQ_ANTDEC_SET_CROSSFEED   0xA3u
#define VREQ_ANTDEC_SET_PEQ_BAND    0xA4u
#define VREQ_ANTDEC_GET_PEQ_BAND    0xA5u
#define VREQ_ANTDEC_CLEAR_PEQ       0xA6u
#define VREQ_ANTDEC_SAVE_FLASH      0xA7u
#define VREQ_ANTDEC_SET_PREAMP      0xA8u
#define VREQ_ANTDEC_GET_CLOCK       0xA9u
#define VREQ_ANTDEC_GET_METERS      0xAAu
#define VREQ_ANTDEC_GET_CONTROLLER  0xABu
#define VREQ_ANTDEC_SET_CONTROLLER  0xACu
#define VREQ_ANTDEC_GET_DEVICE_INFO 0xADu
#define VREQ_ANTDEC_GET_OUTPUT      0xAEu

extern unsigned g_desSpeed;
extern unsigned g_curStreamAlt_Out;
extern unsigned int mutesOut[];
extern unsigned int DFU_mode_active;
extern void XUA_Endpoint0_setVendorId(unsigned short vid);
extern void XUA_Endpoint0_setProductId(unsigned short pid);
extern void XUA_Endpoint0_setBcdDevice(unsigned short bcdDevice);
extern void XUA_Endpoint0_setVendorStr(char *vendor_str);
extern void XUA_Endpoint0_setProductStr(char *product_str);

typedef struct {
    uint32_t magic;
    uint8_t  usb_mode;
    uint8_t  dsp_preset;
    uint8_t  crossfeed_en;
    uint8_t  dirty_flash;
    int16_t  preamp_cB;
    uint16_t reserved;
    uint32_t checksum;
} sram_retention_t;


antdec_usb_tile_state_t g_usb_state = {
    .usb_active = 0u,
    .stream_playing = 0u,
    .usb_muted = 0u,
    .usb_mode = ANTDEC_USB_UAC2,
    .pending_usb_mode = ANTDEC_USB_UAC2,
    .reboot_requested = 0u,
    .save_flash_requested = 0u,
    .dsp_preset = ANTDEC_DSP_BYPASS,
    .crossfeed_en = 0u,
    .peq_custom_active = 0u,
    .peq_dirty_mask = 0u,
    .hid_pending_mask = 0u,
    .aud_format = ANTDEC_FMT_IDLE,
    .aud_sample_rate = 48000u,
    .aud_bit_depth = 32u,
    .aud_mqa_orig_rate = 0u,
    .aud_hw_muted = 0u,
};

antdec_peq_band_t g_usb_custom_peq[ANTDEC_PEQ_BANDS] = {
    {   32u,    0,  71u, ANTDEC_FILT_LOW_SHELF,  1u },
    {   64u,    0, 100u, ANTDEC_FILT_PEAK,       1u },
    {  125u,    0, 100u, ANTDEC_FILT_PEAK,       1u },
    {  250u,    0, 100u, ANTDEC_FILT_PEAK,       1u },
    {  500u,    0, 100u, ANTDEC_FILT_PEAK,       1u },
    { 1000u,    0, 100u, ANTDEC_FILT_PEAK,       1u },
    { 2000u,    0, 100u, ANTDEC_FILT_PEAK,       1u },
    { 4000u,    0, 100u, ANTDEC_FILT_PEAK,       1u },
    { 8000u,    0, 100u, ANTDEC_FILT_PEAK,       1u },
    {16000u,    0,  71u, ANTDEC_FILT_HIGH_SHELF, 1u },
};
int16_t g_usb_preamp_cB = 0;

static volatile uint32_t s_dsp_sync_dirty = 1u;
static volatile uint32_t s_peq_clear_dirty = 0u;
static uint8_t s_hid_release_pending = 0u;
static volatile uint32_t s_enumeration_ready = 0u;
static volatile uint32_t s_play_seconds = 0u;
static unsigned s_play_ms;
static volatile unsigned s_idle_ms;
static volatile unsigned s_save_status; /* 0 unchanged, 1 queued, 2 saved, 3 failed */
static volatile unsigned s_control_ms;
static unsigned s_change_ms, s_retry_at, s_retries, s_observed_output_generation;
static unsigned s_audio_generation, s_saved_audio_generation, s_last_write_audio, s_last_write_output;
static unsigned s_idle_service_available = 1u;
static volatile unsigned s_command_head, s_command_tail;
static unsigned s_commands[8];

static void config_changed(void)
{
    ++s_audio_generation; s_change_ms = s_control_ms; s_retry_at = 0u; s_retries = 0u;
    g_usb_state.save_flash_requested = 1u; s_save_status = 1u;
}

void UsbCompat_Tick10ms(void)
{
    s_control_ms += 10u;
    if (g_usb_state.usb_active && g_usb_state.stream_playing) {
        s_idle_ms = 0u;
        s_play_ms += 10u;
        if (s_play_ms >= 1000u) {
            s_play_ms -= 1000u;
            s_play_seconds++;
        }
    } else if (s_idle_ms < 1000u) {
        s_idle_ms += 10u;
    }
}

static uint8_t crc8_stm32(const uint8_t *data, unsigned len)
{
    uint8_t crc = 0x5Au;
    for (unsigned i = 0; i < len; i++) {
        crc ^= data[i];
        for (unsigned b = 0; b < 8u; b++) {
            crc = (crc & 0x80u) ? (uint8_t)((crc << 1) ^ 0x07u) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static uint32_t calc_hash32(const void *buf, unsigned len)
{
    return ConfigStore_Checksum(buf, len);
}

static void sram_read_retention(sram_retention_t *out)
{
    const uint32_t *src = (const uint32_t *)SRAM_RETENTION_ADDR;
    uint32_t *dst = (uint32_t *)out;
    for (unsigned i = 0; i < sizeof(sram_retention_t) / 4u; i++) {
        __asm__ __volatile__("ldw %0, %1[%2]" : "=r"(dst[i]) : "r"(src), "r"(i));
    }
}

static void sram_write_retention(const sram_retention_t *in)
{
    uint32_t *dst = (uint32_t *)SRAM_RETENTION_ADDR;
    const uint32_t *src = (const uint32_t *)in;
    for (unsigned i = 0; i < sizeof(sram_retention_t) / 4u; i++) {
        __asm__ __volatile__("stw %0, %1[%2]" :: "r"(src[i]), "r"(dst), "r"(i));
    }
}

void UsbCompat_LatchSramState(uint32_t usb_mode, uint32_t dsp_preset, uint32_t crossfeed_en, uint32_t dirty_flash)
{
    sram_retention_t r;
    r.magic        = SRAM_RETENTION_MAGIC;
    r.usb_mode     = (uint8_t)(usb_mode & 0x03u);
    r.dsp_preset   = (uint8_t)(dsp_preset % ANTDEC_DSP_PRESET_COUNT);
    r.crossfeed_en = (uint8_t)(crossfeed_en ? 1u : 0u);
    r.dirty_flash  = (uint8_t)(dirty_flash ? 1u : 0u);
    r.preamp_cB    = g_usb_preamp_cB;
    r.reserved     = 0u;
    r.checksum     = calc_hash32(&r, sizeof(r) - sizeof(uint32_t));
    sram_write_retention(&r);
}

/* Configuration ownership and flash I/O stay on the Endpoint0 thread. */
static void flash_load_config(int *loaded_ok)
{
    device_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    *loaded_ok = ConfigStore_Load(&cfg);
    if (*loaded_ok) {
        g_usb_state.usb_mode = cfg.usb_mode % ANTDEC_USB_MODE_COUNT;
        g_usb_state.dsp_preset = cfg.dsp_preset % ANTDEC_DSP_PRESET_COUNT;
        g_usb_state.crossfeed_en = !!cfg.crossfeed_en;
        g_usb_state.peq_custom_active = !!cfg.peq_custom_active;
        g_usb_preamp_cB = cfg.preamp_cB;
        memcpy(g_usb_custom_peq, cfg.bands, sizeof(g_usb_custom_peq));
        if (g_usb_state.peq_custom_active) g_usb_state.peq_dirty_mask = (1u << ANTDEC_PEQ_BANDS) - 1u;
    }
    SettingsManager_Load(cfg.output, *loaded_ok && cfg.output_valid && cfg.output_version == 1u, cfg.output_revision);
}

static int flash_save_config(void)
{
    device_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    s_last_write_audio = s_audio_generation;
    cfg.usb_mode = (uint8_t)g_usb_state.usb_mode;
    cfg.dsp_preset = (uint8_t)g_usb_state.dsp_preset;
    cfg.crossfeed_en = (uint8_t)g_usb_state.crossfeed_en;
    cfg.peq_custom_active = (uint8_t)g_usb_state.peq_custom_active;
    cfg.preamp_cB = g_usb_preamp_cB;
    memcpy(cfg.bands, g_usb_custom_peq, sizeof(cfg.bands));
    cfg.output_valid = SettingsManager_Export(cfg.output, &cfg.output_revision, &s_last_write_output);
    cfg.output_version = 1u;
    return ConfigStore_Save(&cfg);
}

static void apply_stm32_command(unsigned cmd, unsigned arg);
void XUD_UserSetupIdle(void)
{
    if (!s_enumeration_ready || DFU_mode_active) return;
    while (s_command_tail != s_command_head) {
        unsigned command = s_commands[s_command_tail];
        s_command_tail = (s_command_tail + 1u) & 7u;
        apply_stm32_command(command >> 8, command & 255u);
    }
    unsigned generation = SettingsManager_Generation();
    if (generation != s_observed_output_generation) { s_observed_output_generation = generation; config_changed(); }
    unsigned now = s_control_ms;
    if (!g_usb_state.save_flash_requested || (s_retry_at && (int)(now - s_retry_at) < 0)) return;
    if (!g_usb_state.reboot_requested && (g_usb_state.stream_playing || s_idle_ms < 500u || now - s_change_ms < 2000u)) return;
    if (flash_save_config() == 0) {
        s_saved_audio_generation = s_last_write_audio; s_observed_output_generation = s_last_write_output;
        s_retries = 0; s_retry_at = 0;
        if (s_audio_generation == s_saved_audio_generation && SettingsManager_Generation() == s_last_write_output) {
            g_usb_state.save_flash_requested = 0u; s_save_status = 2u;
            UsbCompat_LatchSramState(g_usb_state.usb_mode, g_usb_state.dsp_preset, g_usb_state.crossfeed_en, 0u);
        } else { s_change_ms = now; s_save_status = 1u; }
    } else {
        unsigned delay = 5000u << (s_retries < 3u ? s_retries++ : 3u);
        s_retry_at = now + (delay < 30000u ? delay : 30000u); s_save_status = 3u;
    }
}
unsigned XUD_UserSetupIdleInterval(void) { return DFU_mode_active ? 0u : 5000000u; }
void XUD_UserSetupIdleUnavailable(void) { s_idle_service_available = 0u; s_save_status = 3u; }

static void apply_usb_descriptor_profile(uint32_t mode)
{
    switch (mode & 0x03u) {
        case ANTDEC_USB_SHANLING:
            /* Experimental identity profile; driver handshake is unverified. */
            g_desSpeed = XUD_SPEED_HS;
            XUA_Endpoint0_setVendorId(0x20B1);
            XUA_Endpoint0_setProductId(0x301F);
            XUA_Endpoint0_setBcdDevice(0x0206);
            XUA_Endpoint0_setVendorStr("Shanling");
            XUA_Endpoint0_setProductStr("Shanling USB Audio");
            break;

        case ANTDEC_USB_FIIO:
            /* Experimental identity profile; driver handshake is unverified. */
            g_desSpeed = XUD_SPEED_HS;
            XUA_Endpoint0_setVendorId(0x2972);
            XUA_Endpoint0_setProductId(0x0093);
            XUA_Endpoint0_setBcdDevice(0x0120);
            XUA_Endpoint0_setVendorStr("FiiO");
            XUA_Endpoint0_setProductStr("FiiO USB DAC-O1");
            break;

        case ANTDEC_USB_UAC1:
            /* Driverless Full-Speed USB Audio Class 1.0 profile (consoles / legacy OS) */
            g_desSpeed = XUD_SPEED_FS;
            XUA_Endpoint0_setVendorId(0x20B1);
            XUA_Endpoint0_setProductId(0x0017);
            XUA_Endpoint0_setBcdDevice(0x0100);
            XUA_Endpoint0_setVendorStr("AntDec");
            XUA_Endpoint0_setProductStr("AntDec O1 UAC1");
            break;

        case ANTDEC_USB_UAC2:
        default:
            /* Standard USB Audio Class 2.0 identity. */
            g_desSpeed = XUD_SPEED_HS;
            XUA_Endpoint0_setVendorId(0x20B1);
            XUA_Endpoint0_setProductId(0x0016);
            XUA_Endpoint0_setBcdDevice(0x0100);
            XUA_Endpoint0_setVendorStr("AntDec");
            XUA_Endpoint0_setProductStr("AntDec O1 USB DAC");
            break;
    }
}

void VendorRequests_Init(VENDOR_REQUESTS_PARAMS_DEC)
{
    int flash_ok = 0;
    flash_load_config(&flash_ok);

    sram_retention_t ret;
    sram_read_retention(&ret);
    uint32_t csum = calc_hash32(&ret, sizeof(ret) - sizeof(uint32_t));
    if (ret.magic == SRAM_RETENTION_MAGIC && ret.checksum == csum) {
        g_usb_state.usb_mode     = ret.usb_mode % ANTDEC_USB_MODE_COUNT;
        g_usb_state.dsp_preset   = ret.dsp_preset % ANTDEC_DSP_PRESET_COUNT;
        g_usb_state.crossfeed_en = ret.crossfeed_en ? 1u : 0u;
        g_usb_preamp_cB          = ret.preamp_cB;
        if (ret.dirty_flash) {
            config_changed();
            if (!flash_save_config()) {
                g_usb_state.save_flash_requested = 0u; s_save_status = 2u;
                s_saved_audio_generation = s_last_write_audio; s_observed_output_generation = s_last_write_output;
                UsbCompat_LatchSramState(g_usb_state.usb_mode, g_usb_state.dsp_preset, g_usb_state.crossfeed_en, 0u);
            } else { s_save_status = 3u; s_retry_at = s_control_ms + 5000u; }
        }
    } else {
        UsbCompat_LatchSramState(g_usb_state.usb_mode, g_usb_state.dsp_preset, g_usb_state.crossfeed_en, 0u);
    }

    s_dsp_sync_dirty = 1u;
    apply_usb_descriptor_profile(g_usb_state.usb_mode);
    __asm__ __volatile__("" ::: "memory");
    s_enumeration_ready = 1u;
}

/* Runs on the XUD thread, after all endpoint handles have been handed out. */
void XUD_UserEnumerationReady(void)
{
    while (!s_enumeration_ready) {
        __asm__ __volatile__("" ::: "memory");
    }
}

void VendorAudioRequestsInit(chanend c_aud_ctl, NULLABLE_RESOURCE(chanend, c_mix_ctl), NULLABLE_RESOURCE(chanend, c_clk_ctl))
{
    (void)c_aud_ctl; (void)c_mix_ctl; (void)c_clk_ctl;
}

int VendorAudioRequests(XUD_ep ep0_out, XUD_ep ep0_in, unsigned char bRequest, unsigned char cs, unsigned char cn,
                        unsigned short unitId, unsigned char direction, NULLABLE_RESOURCE(chanend, c_aud_ctl),
                        NULLABLE_RESOURCE(chanend, c_mix_ctl), NULLABLE_RESOURCE(chanend, c_clk_ctL))
{
    (void)ep0_out; (void)ep0_in; (void)bRequest; (void)cs; (void)cn;
    (void)unitId; (void)direction; (void)c_aud_ctl; (void)c_mix_ctl; (void)c_clk_ctL;
    return XUD_RES_ERR;
}

int VendorRequests(XUD_ep ep0_out, XUD_ep ep0_in, USB_SetupPacket_t *sp VENDOR_REQUESTS_PARAMS_DEC_)
{
    unsigned char buf[64];
    unsigned rx_len = 0u;
    XUD_UserSetupIdle();

    /* Type == 2 is USB Vendor-Specific request (Device or Interface recipient) */
    if (sp->bmRequestType.Type != 2u) {
        return XUD_RES_ERR;
    }

    if (sp->bmRequestType.Direction == USB_BM_REQTYPE_DIRECTION_H2D) {
        if (sp->wLength > sizeof(buf)) {
            return XUD_RES_ERR;
        }
        if (sp->wLength != 0u) {
            if (XUD_GetBuffer(ep0_out, buf, &rx_len) != XUD_RES_OKAY || rx_len != sp->wLength) {
                return XUD_RES_ERR;
            }
        }

        switch (sp->bRequest) {
            case VREQ_ANTDEC_SET_CONTROLLER:
                if (sp->wLength != 0u || !ControllerLink_Queue(sp->wIndex, sp->wValue)) {
                    return XUD_RES_ERR;
                }
                return XUD_DoSetRequestStatus(ep0_in);
            case VREQ_ANTDEC_SET_USB_MODE: {
                uint32_t new_mode = sp->wValue & 0x03u;
                if (new_mode != g_usb_state.usb_mode) {
                    g_usb_state.usb_mode = new_mode;
                    config_changed();
                    UsbCompat_LatchSramState(new_mode, g_usb_state.dsp_preset, g_usb_state.crossfeed_en, 1u);
                    g_usb_state.reboot_requested = 1u;
                }
                return XUD_DoSetRequestStatus(ep0_in);
            }

            case VREQ_ANTDEC_SET_DSP_PRESET:
                config_changed();
                g_usb_state.dsp_preset = sp->wValue % ANTDEC_DSP_PRESET_COUNT;
                s_dsp_sync_dirty = 1u;
                UsbCompat_LatchSramState(g_usb_state.usb_mode, g_usb_state.dsp_preset, g_usb_state.crossfeed_en, 0u);
                return XUD_DoSetRequestStatus(ep0_in);

            case VREQ_ANTDEC_SET_CROSSFEED:
                config_changed();
                g_usb_state.crossfeed_en = sp->wValue ? 1u : 0u;
                s_dsp_sync_dirty = 1u;
                UsbCompat_LatchSramState(g_usb_state.usb_mode, g_usb_state.dsp_preset, g_usb_state.crossfeed_en, 0u);
                return XUD_DoSetRequestStatus(ep0_in);

            case VREQ_ANTDEC_SET_PREAMP:
                config_changed();
                g_usb_preamp_cB = (int16_t)sp->wValue;
                g_usb_state.peq_dirty_mask |= 1u;
                UsbCompat_LatchSramState(g_usb_state.usb_mode, g_usb_state.dsp_preset, g_usb_state.crossfeed_en, 0u);
                return XUD_DoSetRequestStatus(ep0_in);

            case VREQ_ANTDEC_SET_PEQ_BAND: {
                unsigned idx = sp->wIndex & 0xFFu;
                if (idx >= ANTDEC_PEQ_BANDS || sp->wLength != 8u) {
                    return XUD_RES_ERR;
                }
                if (idx < ANTDEC_PEQ_BANDS) {
                    config_changed();
                    g_usb_custom_peq[idx].freq_hz     = (uint16_t)(buf[0] | ((uint16_t)buf[1] << 8));
                    g_usb_custom_peq[idx].gain_cB     = (int16_t)(buf[2] | ((uint16_t)buf[3] << 8));
                    g_usb_custom_peq[idx].q_x100      = (uint16_t)(buf[4] | ((uint16_t)buf[5] << 8));
                    g_usb_custom_peq[idx].filter_type = buf[6];
                    g_usb_custom_peq[idx].enabled     = buf[7] ? 1u : 0u;
                    g_usb_state.peq_custom_active     = 1u;
                    g_usb_state.dsp_preset            = ANTDEC_DSP_CUSTOM;
                    g_usb_state.peq_dirty_mask       |= (1u << idx);
                    s_dsp_sync_dirty                  = 1u;
                }
                return XUD_DoSetRequestStatus(ep0_in);
            }

            case VREQ_ANTDEC_CLEAR_PEQ: {
                config_changed();
                static const uint16_t defaults[ANTDEC_PEQ_BANDS] =
                    {32u, 64u, 125u, 250u, 500u, 1000u, 2000u, 4000u, 8000u, 16000u};
                for (unsigned i = 0; i < ANTDEC_PEQ_BANDS; i++) {
                    g_usb_custom_peq[i].freq_hz = defaults[i];
                    g_usb_custom_peq[i].gain_cB = 0;
                    g_usb_custom_peq[i].q_x100 = (i == 0u || i == 9u) ? 71u : 100u;
                    g_usb_custom_peq[i].filter_type = i == 0u ? ANTDEC_FILT_LOW_SHELF :
                        i == 9u ? ANTDEC_FILT_HIGH_SHELF : ANTDEC_FILT_PEAK;
                    g_usb_custom_peq[i].enabled = 1u;
                }
                g_usb_preamp_cB = 0;
                g_usb_state.peq_custom_active = 0u;
                s_peq_clear_dirty = 1u;
                return XUD_DoSetRequestStatus(ep0_in);
            }

            case VREQ_ANTDEC_SAVE_FLASH:
                g_usb_state.save_flash_requested = 1u;
                s_save_status = 1u;
                return XUD_DoSetRequestStatus(ep0_in);

            default:
                return XUD_RES_ERR;
        }
    } else {
        memset(buf, 0, sizeof(buf));
        switch (sp->bRequest) {
            case VREQ_ANTDEC_GET_DEVICE_INFO: {
                memcpy(buf, "ADC1", 4u);
                buf[4] = 1u; /* Protocol revision */
                buf[5] = 1u; /* Model: O1 */
                buf[6] = 0x7Fu | (s_idle_service_available ? 0x80u : 0u);
                uint32_t pcm = 768000u, dsp = ANTDEC_DSP_MAX_RATE;
                memcpy(buf + 8, &pcm, 4u);
                memcpy(buf + 12, &dsp, 4u);
                buf[16] = ANTDEC_PEQ_BANDS;
                memcpy(buf + 20, "O1", 2u);
                unsigned len = sp->wLength < 32u ? sp->wLength : 32u;
                return XUD_DoGetRequest(ep0_out, ep0_in, buf, len, sp->wLength);
            }
            case VREQ_ANTDEC_GET_CONTROLLER: {
                ControllerLink_Status(buf);
                unsigned len = sp->wLength < 24u ? sp->wLength : 24u;
                return XUD_DoGetRequest(ep0_out, ep0_in, buf, len, sp->wLength);
            }
            case VREQ_ANTDEC_GET_OUTPUT: {
                ControllerLink_OutputSettings(buf);
                unsigned len = sp->wLength < 18u ? sp->wLength : 18u;
                return XUD_DoGetRequest(ep0_out, ep0_in, buf, len, sp->wLength);
            }
            case VREQ_ANTDEC_GET_METERS: {
                memcpy(buf, "MTR1", 4u);
                buf[4] = (uint8_t)g_usb_state.meter_valid;
                buf[5] = (uint8_t)g_usb_state.meter_left;
                buf[6] = (uint8_t)g_usb_state.meter_right;
                buf[7] = 1u; /* post-DSP, before DAC attenuation */
                unsigned len = sp->wLength < 8u ? sp->wLength : 8u;
                return XUD_DoGetRequest(ep0_out, ep0_in, buf, len, sp->wLength);
            }
            case VREQ_ANTDEC_GET_CLOCK: {
                unsigned cycles, reference_ticks;
                ClockMonitor_Measure(&cycles, &reference_ticks);
                memcpy(buf, "CLK1", 4u);
                for (unsigned i = 0; i < 4u; ++i) {
                    buf[4u + i] = (uint8_t)(cycles >> (8u * i));
                    buf[8u + i] = (uint8_t)(reference_ticks >> (8u * i));
                }
                unsigned len = sp->wLength < 12u ? sp->wLength : 12u;
                return XUD_DoGetRequest(ep0_out, ep0_in, buf, len, sp->wLength);
            }

            case VREQ_ANTDEC_GET_STATUS: {
                uint32_t sr = g_usb_state.aud_sample_rate;
                uint32_t mqa_sr = g_usb_state.aud_mqa_orig_rate;
                buf[0]  = 'A';
                buf[1]  = 'D';
                buf[2]  = 'O';
                buf[3]  = '1';
                buf[4]  = (uint8_t)g_usb_state.usb_mode;
                buf[5]  = (uint8_t)g_usb_state.dsp_preset;
                buf[6]  = (uint8_t)g_usb_state.crossfeed_en;
                buf[7]  = (uint8_t)(g_usb_state.stream_playing ? g_usb_state.aud_format : ANTDEC_FMT_IDLE);
                buf[8]  = (uint8_t)(sr & 0xFFu);
                buf[9]  = (uint8_t)((sr >> 8) & 0xFFu);
                buf[10] = (uint8_t)((sr >> 16) & 0xFFu);
                buf[11] = (uint8_t)((sr >> 24) & 0xFFu);
                buf[12] = (uint8_t)g_usb_state.aud_bit_depth;
                buf[13] = (uint8_t)((g_usb_state.usb_active ? 1u : 0u) |
                                    (g_usb_state.stream_playing ? 2u : 0u) |
                                    (g_usb_state.peq_custom_active ? 4u : 0u));
                buf[14] = (uint8_t)(g_usb_preamp_cB & 0xFF);
                buf[15] = (uint8_t)((uint16_t)g_usb_preamp_cB >> 8);
                buf[16] = (uint8_t)(mqa_sr & 0xFFu);
                buf[17] = (uint8_t)((mqa_sr >> 8) & 0xFFu);
                buf[18] = (uint8_t)((mqa_sr >> 16) & 0xFFu);
                buf[19] = (uint8_t)((mqa_sr >> 24) & 0xFFu);
                for (unsigned i = 0u; i < 4u; i++) buf[20u + i] = (uint8_t)(s_play_seconds >> (i * 8u));
                buf[24] = (uint8_t)s_save_status;
                buf[25] = (uint8_t)((s_idle_service_available ? 1u : 4u) |
                                   (SettingsManager_Owner() ? 2u : 0u) | (SettingsManager_Failed() ? 8u : 0u));
                unsigned len = (sp->wLength < 28u) ? sp->wLength : 28u;
                return XUD_DoGetRequest(ep0_out, ep0_in, buf, len, sp->wLength);
            }

            case VREQ_ANTDEC_GET_PEQ_BAND: {
                unsigned idx = sp->wIndex & 0xFFu;
                if (idx >= ANTDEC_PEQ_BANDS) {
                    return XUD_RES_ERR;
                }
                buf[0] = (uint8_t)(g_usb_custom_peq[idx].freq_hz & 0xFFu);
                buf[1] = (uint8_t)(g_usb_custom_peq[idx].freq_hz >> 8);
                buf[2] = (uint8_t)(g_usb_custom_peq[idx].gain_cB & 0xFF);
                buf[3] = (uint8_t)((uint16_t)g_usb_custom_peq[idx].gain_cB >> 8);
                buf[4] = (uint8_t)(g_usb_custom_peq[idx].q_x100 & 0xFFu);
                buf[5] = (uint8_t)(g_usb_custom_peq[idx].q_x100 >> 8);
                buf[6] = g_usb_custom_peq[idx].filter_type;
                buf[7] = g_usb_custom_peq[idx].enabled;
                unsigned len = (sp->wLength < 8u) ? sp->wLength : 8u;
                return XUD_DoGetRequest(ep0_out, ep0_in, buf, len, sp->wLength);
            }

            default:
                return XUD_RES_ERR;
        }
    }
}

void UserHostActive(int active)
{
    g_usb_state.usb_active = active ? 1u : 0u;
    if (!active) {
        g_usb_state.stream_playing = 0u;
    }
}

void UserAudioStreamState(int inputActive, int outputActive)
{
    (void)inputActive;
    g_usb_state.stream_playing = outputActive ? 1u : 0u;
}

void UserHIDInit(void)
{
    s_hid_release_pending = 0u;
}

size_t UserHIDGetData(const unsigned id, unsigned char hidData[HID_MAX_DATA_BYTES])
{
    (void)id;
    uint32_t pending = g_usb_state.hid_pending_mask;
    if (pending != 0u) {
        g_usb_state.hid_pending_mask = 0u;
        hidData[0] = (unsigned char)(pending & 0x3Fu);
        s_hid_release_pending = 1u;
        return 1u;
    }
    if (s_hid_release_pending) {
        s_hid_release_pending = 0u;
        hidData[0] = 0u;
        return 1u;
    }
    hidData[0] = 0u;
    return 0u;
}

void UsbCompat_OnStm32Command(unsigned cmd, unsigned arg)
{
    unsigned next = (s_command_head + 1u) & 7u;
    if (next == s_command_tail) return;
    s_commands[s_command_head] = (cmd << 8) | (arg & 255u);
    __asm__ __volatile__("" ::: "memory");
    s_command_head = next;
}

static void apply_stm32_command(unsigned cmd, unsigned arg)
{
    switch (cmd) {
        case ANTDEC_CMD_SET_USB_MODE: {
            uint32_t target = arg & 0x03u;
            if (target != g_usb_state.usb_mode) {
                g_usb_state.usb_mode = target;
                config_changed();
                UsbCompat_LatchSramState(target, g_usb_state.dsp_preset, g_usb_state.crossfeed_en, 1u);
                g_usb_state.reboot_requested = 1u;
            }
            break;
        }

        case ANTDEC_CMD_SET_DSP_PRESET:
            config_changed();
            g_usb_state.dsp_preset = arg % ANTDEC_DSP_PRESET_COUNT;
            s_dsp_sync_dirty = 1u;
            UsbCompat_LatchSramState(g_usb_state.usb_mode, g_usb_state.dsp_preset, g_usb_state.crossfeed_en, 1u);
            break;

        case ANTDEC_CMD_HID_MEDIA:
            if (arg == ANTDEC_HID_PLAY_PAUSE) g_usb_state.hid_pending_mask |= (1u << 0);
            else if (arg == ANTDEC_HID_NEXT_TRACK) g_usb_state.hid_pending_mask |= (1u << 1);
            else if (arg == ANTDEC_HID_PREV_TRACK) g_usb_state.hid_pending_mask |= (1u << 2);
            else if (arg == ANTDEC_HID_VOL_UP) g_usb_state.hid_pending_mask |= (1u << 3);
            else if (arg == ANTDEC_HID_VOL_DOWN) g_usb_state.hid_pending_mask |= (1u << 4);
            else if (arg == ANTDEC_HID_MUTE) g_usb_state.hid_pending_mask |= (1u << 5);
            break;

        case ANTDEC_CMD_SET_CROSSFEED:
            config_changed();
            g_usb_state.crossfeed_en = arg ? 1u : 0u;
            s_dsp_sync_dirty = 1u;
            UsbCompat_LatchSramState(g_usb_state.usb_mode, g_usb_state.dsp_preset, g_usb_state.crossfeed_en, 1u);
            break;

        default:
            break;
    }
}

void UsbCompat_UpdateAudioTelemetry(unsigned fmt, unsigned sr, unsigned bits, unsigned mqa_sr, unsigned hw_muted)
{
    g_usb_state.aud_format        = fmt;
    g_usb_state.aud_sample_rate   = sr;
    g_usb_state.aud_bit_depth     = bits;
    g_usb_state.aud_mqa_orig_rate = mqa_sr;
    g_usb_state.aud_hw_muted      = hw_muted;
}

void UsbCompat_UpdateMeters(unsigned valid, unsigned left, unsigned right)
{
    unsigned pcm = g_usb_state.aud_format == ANTDEC_FMT_PCM;
    unsigned active = g_usb_state.stream_playing && g_usb_state.usb_active;
    g_usb_state.meter_valid = valid && pcm && active;
    g_usb_state.meter_left = g_usb_state.meter_valid ? left : 0u;
    g_usb_state.meter_right = g_usb_state.meter_valid ? right : 0u;
}

unsigned UsbCompat_BuildMeterFrame(void)
{
    uint32_t body = (0xBu << 20) | ((g_usb_state.meter_valid & 1u) << 16) |
                    ((g_usb_state.meter_left & 0xFFu) << 8) |
                    (g_usb_state.meter_right & 0xFFu);
    uint8_t bytes[3] = { (uint8_t)(body >> 16), (uint8_t)(body >> 8), (uint8_t)body };
    return (body << 8) | crc8_stm32(bytes, 3u);
}

unsigned UsbCompat_BuildTimeFrame(void)
{
    uint32_t body = (0xDu << 20) | (s_play_seconds & 0xFFFFFu);
    uint8_t bytes[3] = { (uint8_t)(body >> 16), (uint8_t)(body >> 8), (uint8_t)body };
    return (body << 8) | crc8_stm32(bytes, 3u);
}

static uint8_t encode_sr_id(uint32_t fmt, uint32_t sr, uint32_t mqa_sr)
{
    if (fmt == ANTDEC_FMT_IDLE || sr == 0u) return 0u;

    /* Report unfolded master rate for MQA streams up to 384 kHz */
    if (fmt >= ANTDEC_FMT_MQA && mqa_sr >= sr) {
        sr = mqa_sr;
    }

    if (fmt == ANTDEC_FMT_DOP || fmt == ANTDEC_FMT_NATIVE_DSD) {
        if (sr >= 22579200u) return 14u; /* DSD512 */
        if (sr >= 11289600u) return 13u; /* DSD256 */
        if (sr >=  5644800u) return 12u; /* DSD128 */
        return 11u;                      /* DSD64  */
    }

    switch (sr) {
        case  44100u: return 1u;
        case  48000u: return 2u;
        case  88200u: return 3u;
        case  96000u: return 4u;
        case 176400u: return 5u;
        case 192000u: return 6u;
        case 352800u: return 7u;
        case 384000u: return 8u;
        case 705600u: return 9u;
        case 768000u: return 10u;
        default:      return 0u;
    }
}

static uint8_t encode_bits_id(uint32_t fmt, uint32_t bits)
{
    if (fmt == ANTDEC_FMT_IDLE) return 0u;
    if (fmt == ANTDEC_FMT_DOP || fmt == ANTDEC_FMT_NATIVE_DSD) return 3u;
    if (bits >= 32u) return 3u;
    if (bits >= 24u) return 2u;
    if (bits >= 16u) return 1u;
    return 0u;
}

unsigned UsbCompat_BuildStm32TxFrame(unsigned *out_plugged, unsigned *out_dsd_hi, unsigned *out_fs_port)
{
    uint32_t playing    = g_usb_state.stream_playing && g_usb_state.usb_active;
    uint32_t host_muted = (mutesOut[0] != 0u) || (mutesOut[1] != 0u && mutesOut[2] != 0u);
    uint32_t muted      = host_muted || g_usb_state.aud_hw_muted;
    uint32_t fmt        = playing ? g_usb_state.aud_format : ANTDEC_FMT_IDLE;
    uint8_t  sr_id      = encode_sr_id(fmt, g_usb_state.aud_sample_rate, g_usb_state.aud_mqa_orig_rate);
    uint8_t  bits_id    = encode_bits_id(fmt, g_usb_state.aud_bit_depth);
    uint32_t dsp_active = (fmt == ANTDEC_FMT_PCM && g_usb_state.aud_sample_rate <= ANTDEC_DSP_MAX_RATE);

    *out_plugged = g_usb_state.usb_active ? 1u : 0u;
    *out_dsd_hi  = (sr_id >= 12u) ? 1u : 0u;
    *out_fs_port = ((unsigned)(sr_id & 0x0Fu)) << 4;

    uint32_t body24 = (0x0Au << 20) |
                      ((fmt & 0x07u) << 17) |
                      (((uint32_t)sr_id & 0x1Fu) << 12) |
                      (((uint32_t)bits_id & 0x03u) << 10) |
                      ((g_usb_state.usb_mode & 0x03u) << 8) |
                      ((g_usb_state.crossfeed_en & 1u) << 7) |
                      (((dsp_active ? g_usb_state.dsp_preset : ANTDEC_DSP_BYPASS) & 0x07u) << 4) |
                      ((muted ? 1u : 0u) << 3) |
                      ((g_usb_state.usb_active ? 1u : 0u) << 2) |
                      ((playing ? 1u : 0u) << 1) |
                      ((dsp_active && g_usb_state.dsp_preset == ANTDEC_DSP_CUSTOM) ? 1u : 0u);

    uint8_t bytes[3] = {
        (uint8_t)((body24 >> 16) & 0xFFu),
        (uint8_t)((body24 >> 8) & 0xFFu),
        (uint8_t)(body24 & 0xFFu),
    };
    uint8_t crc = crc8_stm32(bytes, 3u);
    return (body24 << 8) | (uint32_t)crc;
}

unsigned UsbCompat_CheckRebootRequest(void)
{
    if (g_usb_state.reboot_requested && !g_usb_state.save_flash_requested && s_save_status == 2u) {
        g_usb_state.reboot_requested = 0u;
        return 1u;
    }
    return 0u;
}

unsigned UsbCompat_PopDspSync(unsigned *out_preset, unsigned *out_crossfeed)
{
    if (!s_dsp_sync_dirty) {
        return 0u;
    }
    s_dsp_sync_dirty = 0u;
    *out_preset    = g_usb_state.dsp_preset;
    *out_crossfeed = g_usb_state.crossfeed_en;
    return 1u;
}

int UsbCompat_PopDirtyPeqBand(unsigned *out_idx, unsigned *out_freq_hz, int *out_gain_cB,
                              unsigned *out_q_x100, unsigned *out_ftype, unsigned *out_en, int *out_preamp_cB)
{
    uint32_t mask = g_usb_state.peq_dirty_mask;
    if (mask == 0u) {
        return -1;
    }
    for (unsigned i = 0; i < ANTDEC_PEQ_BANDS; i++) {
        if (mask & (1u << i)) {
            g_usb_state.peq_dirty_mask &= ~(1u << i);
            *out_idx       = i;
            *out_freq_hz   = g_usb_custom_peq[i].freq_hz;
            *out_gain_cB   = g_usb_custom_peq[i].gain_cB;
            *out_q_x100    = g_usb_custom_peq[i].q_x100;
            *out_ftype     = g_usb_custom_peq[i].filter_type;
            *out_en        = g_usb_custom_peq[i].enabled;
            *out_preamp_cB = g_usb_preamp_cB;
            return (int)i;
        }
    }
    return -1;
}

unsigned UsbCompat_PopClearPeq(void)
{
    if (!s_peq_clear_dirty) {
        return 0u;
    }
    s_peq_clear_dirty = 0u;
    return 1u;
}
