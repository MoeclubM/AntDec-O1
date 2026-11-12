#ifndef CONFIG_STORE_H_
#define CONFIG_STORE_H_
#include "antdec_state.h"
typedef struct {
    uint8_t usb_mode, dsp_preset, crossfeed_en, peq_custom_active;
    int16_t preamp_cB;
    uint16_t reserved;
    antdec_peq_band_t bands[ANTDEC_PEQ_BANDS];
    uint8_t output[10];
    uint16_t output_revision;
    uint8_t output_valid, output_version;
    uint16_t reserved2;
} device_config_t;
uint32_t ConfigStore_Checksum(const void *data, unsigned length);
int ConfigStore_Load(device_config_t *config);
int ConfigStore_Save(const device_config_t *config);
#endif
