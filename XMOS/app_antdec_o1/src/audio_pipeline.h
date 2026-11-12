#ifndef AUDIO_PIPELINE_H_
#define AUDIO_PIPELINE_H_

#include <stdint.h>
#include <xccompat.h>

#ifdef __cplusplus
extern "C" {
#endif

void AudioPipeline_OnHwInit(void);
void AudioPipeline_OnFastMuteNotify(void);
unsigned AudioPipeline_ConsumeMuteDrained(void);
unsigned AudioPipeline_PopFastMuteRequest(void);
void AudioPipeline_OnMute(void);
void AudioPipeline_OnUnMute(void);
void AudioPipeline_OnHwConfig(unsigned samFreq, unsigned mClk, unsigned dsdMode, unsigned sampRes_DAC);

void AudioPipeline_ApplyDspCommand(unsigned preset, unsigned crossfeed_en);
void AudioPipeline_ApplyPeqBand(unsigned idx, unsigned freq_hz, int gain_cB,
                                unsigned q_x100, unsigned filter_type, unsigned enabled, int preamp_cB);
void AudioPipeline_ClearCustomPeq(void);
unsigned AudioPipeline_ReadMeters(REFERENCE_PARAM(unsigned, left),
                                  REFERENCE_PARAM(unsigned, right));

unsigned AudioPipeline_PollTelemetry(REFERENCE_PARAM(unsigned, out_format),
                                     REFERENCE_PARAM(unsigned, out_sample_rate),
                                     REFERENCE_PARAM(unsigned, out_bit_depth),
                                     REFERENCE_PARAM(unsigned, out_mqa_orig_rate),
                                     REFERENCE_PARAM(unsigned, out_hw_muted));

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_PIPELINE_H_ */
