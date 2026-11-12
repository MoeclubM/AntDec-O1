#ifndef MQA_CORE_H_
#define MQA_CORE_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t  active_format;     /* PCM until a verified decoder is integrated. */
    uint32_t orig_sample_rate;  /* Zero when no verified metadata is available. */
} mqa_status_t;

void MqaCore_Reset(uint32_t stream_rate_hz);
void MqaCore_ProcessStereo(int32_t *left_q31, int32_t *right_q31, uint32_t apply_filter);
void MqaCore_GetStatus(mqa_status_t *out);

#ifdef __cplusplus
}
#endif

#endif /* MQA_CORE_H_ */
