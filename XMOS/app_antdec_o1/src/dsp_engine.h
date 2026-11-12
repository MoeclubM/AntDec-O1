#ifndef DSP_ENGINE_H_
#define DSP_ENGINE_H_

#include <stdint.h>
#include "antdec_state.h"

#ifdef __cplusplus
extern "C" {
#endif

void DspEngine_WorkerLoop(void);

#ifndef __XC__
void DspEngine_Init(void);
void DspEngine_ClearDelayLines(void);
unsigned DspEngine_RebuildCoefficients(uint32_t sample_rate_hz,
                                   uint32_t preset,
                                   const antdec_peq_band_t custom_bands[ANTDEC_PEQ_BANDS],
                                   int16_t custom_preamp_cB);
void DspEngine_ProcessStereo(int32_t *left_q31, int32_t *right_q31, uint32_t crossfeed_en);
#endif

#ifdef __cplusplus
}
#endif

#endif /* DSP_ENGINE_H_ */
