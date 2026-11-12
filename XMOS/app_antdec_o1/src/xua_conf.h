#ifndef _XUA_CONF_H_
#define _XUA_CONF_H_

/* Tile placement */
#define XUA_XUD_TILE_NUM                         0
#define XUA_AUDIO_IO_TILE_NUM                    1

/* Stereo USB -> Stereo I2S / DSD DAC (ES9039Q2M) */
#define NUM_USB_CHAN_OUT                         2
#define NUM_USB_CHAN_IN                          0
#define I2S_CHANS_DAC                            2
#define I2S_CHANS_ADC                            0
#define DSD_CHANS_DAC                            2
/* Keep DoP markers and native DSD intact; volume is controlled in the DAC. */
#define OUTPUT_VOLUME_CONTROL                    0
#define INPUT_VOLUME_CONTROL                     0

/* Multiplexed DSD pins matching the board's ES9039Q2M connection:
 * - BCLK  (X1D22 / XS1_PORT_1G) -> DAC_BCLK_DSDCLK
 * - LRCLK (X1D34 / XS1_PORT_1K) -> DAC_LRCK_DSDL (Left DSD)
 * - DATA  (X1D13 / XS1_PORT_1F) -> DAC_DATA_DSDR (Right DSD)
 */
#define PORT_DSD_CLK                             XS1_PORT_1G
#define PORT_DSD_DAC0                            XS1_PORT_1K
#define PORT_DSD_DAC1                            XS1_PORT_1F

/* External ultra-low-jitter oscillators Y202 (49.152 MHz) & Y203 (45.1584 MHz) */
#define XUA_USE_SW_PLL                           0
#define CODEC_MASTER                             0
#define MCLK_441                                 (1024 * 44100)
#define MCLK_48                                  (1024 * 48000)

/* Sample rates: 44.1 kHz .. 768 kHz PCM & Native DSD64 .. DSD512 (22.5792 MHz) */
#define MIN_FREQ                                 44100
#define MAX_FREQ                                 768000
#define DEFAULT_FREQ                             48000

/* Dual USB Audio Class support: UAC 2.0 on High-Speed, UAC 1.0 on Full-Speed */
#define AUDIO_CLASS                              2
#define XUA_AUDIO_CLASS_HS                       2
#define XUA_AUDIO_CLASS_FS                       1

/* High-Speed Output Stream Formats:
 * Alt 1: 32-bit PCM (up to 768 kHz)
 * Alt 2: 24-bit PCM (MQA / DoP / 24-bit bit-perfect)
 * Alt 3: 32-bit RAW Native DSD (DSD64 / DSD128 / DSD256 / DSD512)
 */
#define OUTPUT_FORMAT_COUNT                      3
#define NATIVE_DSD_FORMAT_NUM                    3

#define STREAM_FORMAT_OUTPUT_1_RESOLUTION_BITS   32
#define STREAM_FORMAT_OUTPUT_2_RESOLUTION_BITS   24
#define STREAM_FORMAT_OUTPUT_3_RESOLUTION_BITS   32

#define HS_STREAM_FORMAT_OUTPUT_1_RESOLUTION_BITS 32
#define HS_STREAM_FORMAT_OUTPUT_1_SUBSLOT_BYTES   4
#define HS_STREAM_FORMAT_OUTPUT_2_RESOLUTION_BITS 24
#define HS_STREAM_FORMAT_OUTPUT_2_SUBSLOT_BYTES   4
#define HS_STREAM_FORMAT_OUTPUT_3_RESOLUTION_BITS 32
#define HS_STREAM_FORMAT_OUTPUT_3_SUBSLOT_BYTES   4

/* Full-Speed UAC 1.0 driverless fallback formats (44.1k .. 96k) */
#define FS_STREAM_FORMAT_OUTPUT_1_RESOLUTION_BITS 24
#define FS_STREAM_FORMAT_OUTPUT_1_SUBSLOT_BYTES   3
#define FS_STREAM_FORMAT_OUTPUT_2_RESOLUTION_BITS 16
#define FS_STREAM_FORMAT_OUTPUT_2_SUBSLOT_BYTES   2
#define FS_STREAM_FORMAT_OUTPUT_3_RESOLUTION_BITS 24
#define FS_STREAM_FORMAT_OUTPUT_3_SUBSLOT_BYTES   3

/* Self-powered via isolated baseboard 5V rail, DFU + HID Consumer Control enabled */
#define XUA_POWERMODE                            XUA_POWERMODE_SELF
#define XUA_DFU_EN                               1
#define XUA_QUAD_SPI_FLASH                       1
#define HID_CONTROLS                             1
#define VENDOR_AUDIO_REQS                        1

/* Default USB Identity (dynamically overridden at boot by usb_compat.c) */
#define VENDOR_ID                                0x20B1
#define PID_AUDIO_2                              0x0016
#define PID_AUDIO_1                              0x0017
#define DFU_PID                                  0xD016
#define BCD_DEVICE                               0x0100

#define VENDOR_STR                               "AntDec"
#define PRODUCT_STR_A2                           "AntDec O1 USB DAC"
#define PRODUCT_STR_A1                           "AntDec O1 UAC1"
#define SERIAL_STR                               ""

#if defined(__XC__) && !defined(__ASSEMBLER__)
void Stm32LinkTask(chanend c_bridge);
void AudioCtrlTask(chanend c_bridge);
void DspWorkerTask(void);
#endif

#endif /* _XUA_CONF_H_ */
