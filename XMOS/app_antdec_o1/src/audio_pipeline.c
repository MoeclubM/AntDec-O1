#include "audio_pipeline.h"
#include "antdec_state.h"
#include "dsp_engine.h"
#include "mqa_core.h"
#include "dsd_support.h"
#include <string.h>
#include <math.h>

static uint32_t s_last_built_seq = 0u;
static uint32_t s_last_built_rate = 0u;
static uint32_t s_startup_mute_frames = 0u;
static uint32_t s_peak_left, s_peak_right, s_meter_frames;

void AudioPipeline_OnHwInit(void)
{
    DspEngine_Init();
    MqaCore_Reset(48000u);
    g_aud_state.sample_rate = 48000u;
    g_aud_state.mclk_hz = 49152000u;
    g_aud_state.dsd_mode = DSD_MODE_OFF;
    g_aud_state.bit_depth = 32u;
    g_aud_state.format = ANTDEC_FMT_PCM;
    g_aud_state.telemetry_dirty = 1u;
}

void AudioPipeline_OnMute(void)
{
    g_aud_state.hw_muted = 1u;
    g_aud_state.fast_mute_req = 1u;
    g_aud_state.telemetry_dirty = 1u;
}

void AudioPipeline_OnFastMuteNotify(void)
{
    g_aud_state.hw_muted = 1u;
    g_aud_state.fast_mute_req = 1u;
    g_aud_state.mute_drained = 1u;
    g_aud_state.telemetry_dirty = 1u;
}

unsigned AudioPipeline_ConsumeMuteDrained(void)
{
    if (g_aud_state.mute_drained) {
        g_aud_state.mute_drained = 0u;
        g_aud_state.hw_muted = 1u;
        return 1u;
    }
    return 0u;
}

unsigned AudioPipeline_PopFastMuteRequest(void)
{
    if (g_aud_state.fast_mute_req) {
        g_aud_state.fast_mute_req = 0u;
        return 1u;
    }
    return 0u;
}

void AudioPipeline_OnUnMute(void)
{
    g_aud_state.hw_muted = 0u;
    g_aud_state.telemetry_dirty = 1u;
}

void AudioPipeline_OnHwConfig(unsigned samFreq, unsigned mClk, unsigned dsdMode, unsigned sampRes_DAC)
{
    g_aud_state.sample_rate = samFreq;
    g_aud_state.mclk_hz = mClk;
    g_aud_state.dsd_mode = dsdMode;
    g_aud_state.bit_depth = (dsdMode != DSD_MODE_OFF) ? 1u : sampRes_DAC;
    g_aud_state.mute_drained = 0u;
    s_startup_mute_frames = (dsdMode == DSD_MODE_OFF) ? (samFreq / 100u) : 0u;
    s_peak_left = s_peak_right = s_meter_frames = 0u;
    g_aud_state.peak_left = g_aud_state.peak_right = 0u;

    if (dsdMode == DSD_MODE_NATIVE) {
        g_aud_state.format = ANTDEC_FMT_NATIVE_DSD;
        g_aud_state.mqa_orig_rate = 0u;
    } else if (dsdMode == DSD_MODE_DOP) {
        g_aud_state.format = ANTDEC_FMT_DOP;
        g_aud_state.mqa_orig_rate = 0u;
    } else {
        g_aud_state.format = ANTDEC_FMT_PCM;
        MqaCore_Reset(samFreq);
    }

    DspEngine_ClearDelayLines();
    g_aud_state.coeff_rebuild_seq++;
    g_aud_state.telemetry_dirty = 1u;
}

void UserBufferManagementInit(unsigned sampFreq)
{
    (void)sampFreq;
    DspEngine_ClearDelayLines();
}

void UserBufferManagement(unsigned sampsFromUsbToAudio[], unsigned sampsFromAudioToUsb[])
{
    (void)sampsFromAudioToUsb;

    /* Native DSD and DoP streams must remain 100% bit-exact */
    if (g_aud_state.dsd_mode != DSD_MODE_OFF) {
        return;
    }

    uint32_t u0 = sampsFromUsbToAudio[0];
    uint32_t u1 = sampsFromUsbToAudio[1];
    uint32_t sr = g_aud_state.sample_rate;

    /* Preserve the FIRST possible DoP marker, before lib_xua examines the samples. */
    if (sr >= 176400u) {
        uint32_t m0 = u0 >> 24;
        uint32_t m1 = u1 >> 24;
        if (m0 == m1 && (m0 == DSD_MARKER_1 || m0 == DSD_MARKER_2)) {
            return;
        }
    }

    int32_t l = (int32_t)u0;
    int32_t r = (int32_t)u1;
    if (s_startup_mute_frames > 0u) {
        s_startup_mute_frames--;
        l = 0;
        r = 0;
    }

    /* Run dual-core 10-band Q28 biquad PEQ + optional Bauer crossfeed */
    DspEngine_ProcessStereo(&l, &r, g_aud_state.crossfeed_en);

    /* AudioHub owns accumulation; publish 50 ms windows without cross-core resets.
     * Keep the unmeasured high-rate path unchanged. DSD is never interpreted as PCM. */
    if (sr <= ANTDEC_DSP_MAX_RATE) {
        uint32_t al = l < 0 ? 0u - (uint32_t)l : (uint32_t)l;
        uint32_t ar = r < 0 ? 0u - (uint32_t)r : (uint32_t)r;
        if (al > s_peak_left) s_peak_left = al;
        if (ar > s_peak_right) s_peak_right = ar;
        if (++s_meter_frames >= sr / 20u) {
            g_aud_state.peak_left = s_peak_left;
            g_aud_state.peak_right = s_peak_right;
            s_peak_left = s_peak_right = s_meter_frames = 0u;
        }
    }

    sampsFromUsbToAudio[0] = (unsigned)l;
    sampsFromUsbToAudio[1] = (unsigned)r;
}

void AudioPipeline_ApplyDspCommand(unsigned preset, unsigned crossfeed_en)
{
    if (preset < ANTDEC_DSP_PRESET_COUNT) {
        g_aud_state.dsp_preset = preset;
    }
    g_aud_state.crossfeed_en = crossfeed_en ? 1u : 0u;
    g_aud_state.coeff_rebuild_seq++;
}

void AudioPipeline_ApplyPeqBand(unsigned idx, unsigned freq_hz, int gain_cB,
                                unsigned q_x100, unsigned filter_type, unsigned enabled, int preamp_cB)
{
    if (idx < ANTDEC_PEQ_BANDS) {
        g_aud_custom_peq[idx].freq_hz     = (uint16_t)freq_hz;
        g_aud_custom_peq[idx].gain_cB     = (int16_t)gain_cB;
        g_aud_custom_peq[idx].q_x100      = (uint16_t)q_x100;
        g_aud_custom_peq[idx].filter_type = (uint8_t)filter_type;
        g_aud_custom_peq[idx].enabled     = (uint8_t)(enabled ? 1u : 0u);
    }
    g_aud_preamp_cB = (int16_t)preamp_cB;
    g_aud_state.peq_custom_active = 1u;
    g_aud_state.coeff_rebuild_seq++;
}

void AudioPipeline_ClearCustomPeq(void)
{
    static const uint16_t defaults[ANTDEC_PEQ_BANDS] =
        {32u, 64u, 125u, 250u, 500u, 1000u, 2000u, 4000u, 8000u, 16000u};
    for (unsigned i = 0; i < ANTDEC_PEQ_BANDS; i++) {
        g_aud_custom_peq[i].freq_hz = defaults[i];
        g_aud_custom_peq[i].gain_cB = 0;
        g_aud_custom_peq[i].q_x100 = (i == 0u || i == 9u) ? 71u : 100u;
        g_aud_custom_peq[i].filter_type = i == 0u ? ANTDEC_FILT_LOW_SHELF :
            i == 9u ? ANTDEC_FILT_HIGH_SHELF : ANTDEC_FILT_PEAK;
        g_aud_custom_peq[i].enabled = 1u;
    }
    g_aud_preamp_cB = 0;
    g_aud_state.peq_custom_active = 0u;
    g_aud_state.coeff_rebuild_seq++;
}

static unsigned peak_level(uint32_t peak)
{
    if (peak == 0u) return 0u;
    /* 0 = silence, 1..121 = -120..0 dBFS. Runs on the control thread. */
    float db = 20.0f * log10f((float)peak / 2147483648.0f);
    return db <= -120.0f ? 0u : (db >= 0.0f ? 121u : (unsigned)(db + 121.0f));
}

unsigned AudioPipeline_ReadMeters(unsigned *left, unsigned *right)
{
    unsigned valid = g_aud_state.dsd_mode == DSD_MODE_OFF &&
                     g_aud_state.sample_rate <= ANTDEC_DSP_MAX_RATE &&
                     !g_aud_state.hw_muted;
    *left = valid ? peak_level(g_aud_state.peak_left) : 0u;
    *right = valid ? peak_level(g_aud_state.peak_right) : 0u;
    return valid;
}

unsigned AudioPipeline_PollTelemetry(unsigned *out_format,
                                     unsigned *out_sample_rate,
                                     unsigned *out_bit_depth,
                                     unsigned *out_mqa_orig_rate,
                                     unsigned *out_hw_muted)
{
    /* Rebuild RBJ biquad coefficients on AudioCtrlTask core whenever preset/PEQ/rate changes */
    uint32_t seq = g_aud_state.coeff_rebuild_seq;
    uint32_t sr  = (g_aud_state.dsd_mode == DSD_MODE_OFF) ? g_aud_state.sample_rate : 0u;
    if ((seq != s_last_built_seq || sr != s_last_built_rate) && sr != 0u) {
        if (DspEngine_RebuildCoefficients(sr,
                                      g_aud_state.dsp_preset,
                                      g_aud_custom_peq,
                                      g_aud_preamp_cB)) {
            s_last_built_seq = seq;
            s_last_built_rate = sr;
        }
    }

    if (!g_aud_state.telemetry_dirty) {
        return 0u;
    }

    g_aud_state.telemetry_dirty = 0u;
    *out_format        = g_aud_state.format;
    *out_sample_rate   = g_aud_state.sample_rate;
    *out_bit_depth     = g_aud_state.bit_depth;
    *out_mqa_orig_rate = g_aud_state.mqa_orig_rate;
    *out_hw_muted      = g_aud_state.hw_muted;
    return 1u;
}
