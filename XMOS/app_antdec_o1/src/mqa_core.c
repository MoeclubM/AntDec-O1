#include "mqa_core.h"
#include "antdec_state.h"

/* No verified MQA decoder or authentication library is integrated.
 * Keep PCM unchanged and report PCM until a real decoder is supplied. */
void MqaCore_Reset(uint32_t stream_rate_hz)
{
    (void)stream_rate_hz;
}

void MqaCore_ProcessStereo(int32_t *left_q31, int32_t *right_q31, uint32_t apply_filter)
{
    (void)left_q31;
    (void)right_q31;
    (void)apply_filter;
}

void MqaCore_GetStatus(mqa_status_t *out)
{
    out->active_format = ANTDEC_FMT_PCM;
    out->orig_sample_rate = 0u;
}
