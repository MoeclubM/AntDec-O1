#ifndef USB_COMPAT_H_
#define USB_COMPAT_H_

#include <stdint.h>
#include <xccompat.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Snapshot / command helpers called by Stm32LinkTask on tile[0] */
void UsbCompat_LatchSramState(uint32_t usb_mode, uint32_t dsp_preset, uint32_t crossfeed_en, uint32_t dirty_flash);
void UsbCompat_OnStm32Command(unsigned cmd, unsigned arg);
void UsbCompat_UpdateAudioTelemetry(unsigned fmt, unsigned sr, unsigned bits, unsigned mqa_sr, unsigned hw_muted);
void UsbCompat_UpdateMeters(unsigned valid, unsigned left, unsigned right);
unsigned UsbCompat_BuildMeterFrame(void);
unsigned UsbCompat_BuildTimeFrame(void);
void UsbCompat_Tick10ms(void);

unsigned UsbCompat_BuildStm32TxFrame(REFERENCE_PARAM(unsigned, out_plugged),
                                     REFERENCE_PARAM(unsigned, out_dsd_hi),
                                     REFERENCE_PARAM(unsigned, out_fs_port));
unsigned UsbCompat_CheckRebootRequest(void);
unsigned UsbCompat_PopDspSync(REFERENCE_PARAM(unsigned, out_preset),
                              REFERENCE_PARAM(unsigned, out_crossfeed));
int      UsbCompat_PopDirtyPeqBand(REFERENCE_PARAM(unsigned, out_idx),
                                   REFERENCE_PARAM(unsigned, out_freq_hz),
                                   REFERENCE_PARAM(int, out_gain_cB),
                                   REFERENCE_PARAM(unsigned, out_q_x100),
                                   REFERENCE_PARAM(unsigned, out_ftype),
                                   REFERENCE_PARAM(unsigned, out_en),
                                   REFERENCE_PARAM(int, out_preamp_cB));
unsigned UsbCompat_PopClearPeq(void);

#ifdef __cplusplus
}
#endif

#endif /* USB_COMPAT_H_ */
