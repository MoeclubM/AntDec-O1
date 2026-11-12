#include "dsp_engine.h"
#include "dsp_filters.h"
#include <math.h>
#include <string.h>
#include <xcore/channel_streaming.h>

#define DSP_Q_FORMAT        28
#define DSP_Q28_ONE         (1 << DSP_Q_FORMAT)
#define DSP_PI              3.14159265358979323846

/* 8-byte aligned double-buffered Q28 coefficient banks & biquad delay lines for ldd/std in dsp_biquad.S */
static int32_t __attribute__((aligned(8))) s_coeff_banks[2][ANTDEC_PEQ_BANDS * DSP_NUM_COEFFS_PER_BIQUAD];
static int32_t __attribute__((aligned(8))) s_state_left[ANTDEC_PEQ_BANDS * DSP_NUM_STATES_PER_BIQUAD];
static int32_t __attribute__((aligned(8))) s_state_right[ANTDEC_PEQ_BANDS * DSP_NUM_STATES_PER_BIQUAD];
static int32_t s_pregain_q28[2] = { DSP_Q28_ONE, DSP_Q28_ONE };
static volatile uint32_t s_active_bank = 0u;
static volatile uint32_t s_completed_bank = 0u;
static uint32_t s_bypass_eq[2] = { 1u, 1u };

/* Bauer crossfeed low-pass state (~700 Hz interaural shelf) */
static int32_t s_xfeed_lp_l = 0;
static int32_t s_xfeed_lp_r = 0;
static int32_t s_xfeed_alpha_q28[2] = { 0, 0 };

/* Hardware 1-cycle streaming channel linking AudioHub core <-> DspWorkerTask core on tile[1] */
static streaming_channel_t s_worker_chan = { 0, 0 };
static volatile uint32_t s_worker_ready = 0u;

antdec_aud_tile_state_t g_aud_state = {
    .sample_rate = 48000u,
    .mclk_hz = 49152000u,
    .dsd_mode = 0u,
    .bit_depth = 32u,
    .hw_muted = 0u,
    .fast_mute_req = 0u,
    .mute_drained = 0u,
    .format = ANTDEC_FMT_IDLE,
    .mqa_mode = ANTDEC_FMT_PCM,
    .mqa_orig_rate = 0u,
    .dsp_preset = ANTDEC_DSP_BYPASS,
    .crossfeed_en = 0u,
    .peq_custom_active = 0u,
    .coeff_rebuild_seq = 1u,
    .telemetry_dirty = 1u,
};

antdec_peq_band_t g_aud_custom_peq[ANTDEC_PEQ_BANDS] = {
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
int16_t g_aud_preamp_cB = 0;

/* Built-in audiophile EQ curves (10 bands each) */
static const antdec_peq_band_t k_preset_bands[4][ANTDEC_PEQ_BANDS] = {
    /* 1: Bass (Clean sub-bass shelf + punch, zero mid mud) */
    {
        {   45u,  450,  71u, ANTDEC_FILT_LOW_SHELF,  1u },
        {   95u,  220, 110u, ANTDEC_FILT_PEAK,       1u },
        {  220u, -120, 120u, ANTDEC_FILT_PEAK,       1u },
        {  500u,    0, 100u, ANTDEC_FILT_OFF,        0u },
        { 1000u,    0, 100u, ANTDEC_FILT_OFF,        0u },
        { 2000u,    0, 100u, ANTDEC_FILT_OFF,        0u },
        { 4000u,   80, 120u, ANTDEC_FILT_PEAK,       1u },
        { 8000u,    0, 100u, ANTDEC_FILT_OFF,        0u },
        {12000u,  100,  71u, ANTDEC_FILT_HIGH_SHELF, 1u },
        {16000u,    0, 100u, ANTDEC_FILT_OFF,        0u },
    },
    /* 2: Vocal (Forward midrange presence, gentle sub-rumble trim, silky air) */
    {
        {   35u, -150,  71u, ANTDEC_FILT_LOW_SHELF,  1u },
        {  160u,  -80, 110u, ANTDEC_FILT_PEAK,       1u },
        {  650u,  120,  90u, ANTDEC_FILT_PEAK,       1u },
        { 1400u,  240,  95u, ANTDEC_FILT_PEAK,       1u },
        { 2800u,  280, 110u, ANTDEC_FILT_PEAK,       1u },
        { 5200u,  -90, 160u, ANTDEC_FILT_PEAK,       1u },
        {11000u,  140,  71u, ANTDEC_FILT_HIGH_SHELF, 1u },
        {14000u,    0, 100u, ANTDEC_FILT_OFF,        0u },
        {16000u,    0, 100u, ANTDEC_FILT_OFF,        0u },
        {18000u,    0, 100u, ANTDEC_FILT_OFF,        0u },
    },
    /* 3: Warm (Tube-like full lower-mid warmth & relaxed upper-treble glare) */
    {
        {   60u,  250,  71u, ANTDEC_FILT_LOW_SHELF,  1u },
        {  180u,  180,  85u, ANTDEC_FILT_PEAK,       1u },
        {  450u,   80, 100u, ANTDEC_FILT_PEAK,       1u },
        { 2500u, -120, 110u, ANTDEC_FILT_PEAK,       1u },
        { 4500u, -180, 120u, ANTDEC_FILT_PEAK,       1u },
        { 9000u, -150,  71u, ANTDEC_FILT_HIGH_SHELF, 1u },
        {12000u,    0, 100u, ANTDEC_FILT_OFF,        0u },
        {14000u,    0, 100u, ANTDEC_FILT_OFF,        0u },
        {16000u,    0, 100u, ANTDEC_FILT_OFF,        0u },
        {18000u,    0, 100u, ANTDEC_FILT_OFF,        0u },
    },
    /* 4: Harman Target Compensation (Sub-bass shelf + ear-canal pinna contour) */
    {
        {   55u,  380,  71u, ANTDEC_FILT_LOW_SHELF,  1u },
        {  150u,  100, 100u, ANTDEC_FILT_PEAK,       1u },
        {  300u, -110, 110u, ANTDEC_FILT_PEAK,       1u },
        { 1200u,   80, 120u, ANTDEC_FILT_PEAK,       1u },
        { 3100u,  260, 115u, ANTDEC_FILT_PEAK,       1u },
        { 6000u, -140, 180u, ANTDEC_FILT_PEAK,       1u },
        {10500u,  160,  71u, ANTDEC_FILT_HIGH_SHELF, 1u },
        {14000u,    0, 100u, ANTDEC_FILT_OFF,        0u },
        {16000u,    0, 100u, ANTDEC_FILT_OFF,        0u },
        {18000u,    0, 100u, ANTDEC_FILT_OFF,        0u },
    },
};

static const int16_t k_preset_preamp_cB[4] = { -480, -320, -280, -420 };

static int32_t clamp_q28(double v)
{
    double scaled = v * (double)DSP_Q28_ONE;
    if (scaled > 2147483647.0) return 2147483647;
    if (scaled < -2147483648.0) return (int32_t)-2147483648LL;
    return (int32_t)llround(scaled);
}

static void set_unity_biquad(int32_t *dst)
{
    dst[0] = DSP_Q28_ONE;
    dst[1] = 0;
    dst[2] = 0;
    dst[3] = 0;
    dst[4] = 0;
}

static void compute_rbj_biquad(const antdec_peq_band_t *band, uint32_t fs, int32_t *dst)
{
    if (!band->enabled || band->filter_type == ANTDEC_FILT_OFF || band->freq_hz == 0u || fs == 0u) {
        set_unity_biquad(dst);
        return;
    }

    double f0 = (double)band->freq_hz;
    double nyq = (double)fs * 0.47;
    if (f0 >= nyq) {
        set_unity_biquad(dst);
        return;
    }
    if (f0 < 15.0) {
        f0 = 15.0;
    }

    double q = (double)band->q_x100 / 100.0;
    if (q < 0.15) q = 0.15;
    if (q > 16.0) q = 16.0;

    double gain_db = (double)band->gain_cB / 100.0;
    if ((band->filter_type == ANTDEC_FILT_PEAK ||
         band->filter_type == ANTDEC_FILT_LOW_SHELF ||
         band->filter_type == ANTDEC_FILT_HIGH_SHELF) &&
        fabs(gain_db) < 0.02) {
        set_unity_biquad(dst);
        return;
    }

    double A  = pow(10.0, gain_db / 40.0);
    double w0 = 2.0 * DSP_PI * f0 / (double)fs;
    double cw = cos(w0);
    double sw = sin(w0);
    double alpha = sw / (2.0 * q);

    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a0 = 1.0, a1 = 0.0, a2 = 0.0;

    switch (band->filter_type) {
        case ANTDEC_FILT_PEAK:
            b0 = 1.0 + alpha * A;
            b1 = -2.0 * cw;
            b2 = 1.0 - alpha * A;
            a0 = 1.0 + alpha / A;
            a1 = -2.0 * cw;
            a2 = 1.0 - alpha / A;
            break;

        case ANTDEC_FILT_LOW_SHELF: {
            double two_sqrtA_alpha = 2.0 * sqrt(A) * alpha;
            b0 = A * ((A + 1.0) - (A - 1.0) * cw + two_sqrtA_alpha);
            b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cw);
            b2 = A * ((A + 1.0) - (A - 1.0) * cw - two_sqrtA_alpha);
            a0 = (A + 1.0) + (A - 1.0) * cw + two_sqrtA_alpha;
            a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cw);
            a2 = (A + 1.0) + (A - 1.0) * cw - two_sqrtA_alpha;
            break;
        }

        case ANTDEC_FILT_HIGH_SHELF: {
            double two_sqrtA_alpha = 2.0 * sqrt(A) * alpha;
            b0 = A * ((A + 1.0) + (A - 1.0) * cw + two_sqrtA_alpha);
            b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cw);
            b2 = A * ((A + 1.0) + (A - 1.0) * cw - two_sqrtA_alpha);
            a0 = (A + 1.0) - (A - 1.0) * cw + two_sqrtA_alpha;
            a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cw);
            a2 = (A + 1.0) - (A - 1.0) * cw - two_sqrtA_alpha;
            break;
        }

        case ANTDEC_FILT_LOW_PASS:
            b0 = (1.0 - cw) * 0.5;
            b1 = 1.0 - cw;
            b2 = (1.0 - cw) * 0.5;
            a0 = 1.0 + alpha;
            a1 = -2.0 * cw;
            a2 = 1.0 - alpha;
            break;

        case ANTDEC_FILT_HIGH_PASS:
            b0 = (1.0 + cw) * 0.5;
            b1 = -(1.0 + cw);
            b2 = (1.0 + cw) * 0.5;
            a0 = 1.0 + alpha;
            a1 = -2.0 * cw;
            a2 = 1.0 - alpha;
            break;

        default:
            set_unity_biquad(dst);
            return;
    }

    /* dsp_filters_biquads expects [b0, b1, b2, -a1, -a2] in Q28 */
    dst[0] = clamp_q28(b0 / a0);
    dst[1] = clamp_q28(b1 / a0);
    dst[2] = clamp_q28(b2 / a0);
    dst[3] = clamp_q28(-a1 / a0);
    dst[4] = clamp_q28(-a2 / a0);
}

void DspEngine_ClearDelayLines(void)
{
    memset(s_state_left, 0, sizeof(s_state_left));
    memset(s_state_right, 0, sizeof(s_state_right));
    s_xfeed_lp_l = 0;
    s_xfeed_lp_r = 0;
}

void DspEngine_Init(void)
{
    for (unsigned bank = 0; bank < 2u; bank++) {
        for (unsigned i = 0; i < ANTDEC_PEQ_BANDS; i++) {
            set_unity_biquad(&s_coeff_banks[bank][i * DSP_NUM_COEFFS_PER_BIQUAD]);
        }
        s_pregain_q28[bank] = DSP_Q28_ONE;
    }
    DspEngine_ClearDelayLines();

    if (s_worker_chan.end_a == 0) {
        s_worker_chan = s_chan_alloc();
        __asm__ __volatile__("" ::: "memory");
        s_worker_ready = (s_worker_chan.end_a != 0) ? 1u : 0u;
    }
}

unsigned DspEngine_RebuildCoefficients(uint32_t sample_rate_hz,
                                   uint32_t preset,
                                   const antdec_peq_band_t custom_bands[ANTDEC_PEQ_BANDS],
                                   int16_t custom_preamp_cB)
{
    /* Do not reuse the previous bank until BOTH audio workers have finished with it. */
    if (s_active_bank != s_completed_bank) {
        return 0u;
    }
    uint32_t next_bank = s_active_bank ^ 1u;
    if (sample_rate_hz == 0u) {
        sample_rate_hz = 48000u;
    }

    /* Update Bauer ~700 Hz 1-pole crossfeed coefficient for current sample rate */
    double alpha = 1.0 - exp(-2.0 * DSP_PI * 700.0 / (double)sample_rate_hz);
    s_xfeed_alpha_q28[next_bank] = clamp_q28(alpha);

    if (preset == ANTDEC_DSP_BYPASS || preset >= ANTDEC_DSP_PRESET_COUNT) {
        s_bypass_eq[next_bank] = 1u;
        __asm__ __volatile__("" ::: "memory");
        s_active_bank = next_bank;
        return 1u;
    }

    const antdec_peq_band_t *bands = NULL;
    int16_t preamp_cB = 0;

    if (preset == ANTDEC_DSP_CUSTOM) {
        bands = custom_bands;
        preamp_cB = custom_preamp_cB;
        if (preamp_cB == 0) {
            /* Automatic headroom protection against positive peak boosts */
            int16_t max_boost = 0;
            for (unsigned i = 0; i < ANTDEC_PEQ_BANDS; i++) {
                if (bands[i].enabled && bands[i].gain_cB > max_boost) {
                    max_boost = bands[i].gain_cB;
                }
            }
            preamp_cB = (int16_t)(-max_boost);
        }
    } else {
        bands = k_preset_bands[preset - 1u];
        preamp_cB = k_preset_preamp_cB[preset - 1u];
    }

    for (unsigned i = 0; i < ANTDEC_PEQ_BANDS; i++) {
        compute_rbj_biquad(&bands[i], sample_rate_hz, &s_coeff_banks[next_bank][i * DSP_NUM_COEFFS_PER_BIQUAD]);
    }
    s_pregain_q28[next_bank] = clamp_q28(pow(10.0, (double)preamp_cB / 2000.0));
    s_bypass_eq[next_bank] = 0u;

    __asm__ __volatile__("" ::: "memory");
    s_active_bank = next_bank;
    return 1u;
}

static inline int32_t sat_q28_to_q31(int32_t x_q28)
{
    if (x_q28 > 0x0FFFFFFF) return 0x7FFFFFFF;
    if (x_q28 < -0x10000000) return (int32_t)0x80000000u;
    return x_q28 * 8;
}

void DspEngine_WorkerLoop(void)
{
    while (!s_worker_ready) {
        /* Wait until DspEngine_Init() allocates s_worker_chan on tile[1] */
    }

    chanend_t end_b = s_worker_chan.end_b;
    while (1) {
        int32_t right_in = (int32_t)s_chan_in_word(end_b);
        uint32_t bank = s_chan_in_word(end_b);
        int32_t scaled = (int32_t)(((int64_t)right_in * (int64_t)s_pregain_q28[bank]) >> DSP_Q_FORMAT);
        int32_t right_out = dsp_filters_biquads(scaled,
                                                s_coeff_banks[bank],
                                                s_state_right,
                                                ANTDEC_PEQ_BANDS,
                                                DSP_Q_FORMAT);
        s_chan_out_word(end_b, (uint32_t)right_out);
    }
}

void DspEngine_ProcessStereo(int32_t *left_q31, int32_t *right_q31, uint32_t crossfeed_en)
{
    uint32_t bank = s_active_bank;
    __asm__ __volatile__("" ::: "memory");
    if ((s_bypass_eq[bank] && !crossfeed_en) || g_aud_state.sample_rate > ANTDEC_DSP_MAX_RATE) {
        s_completed_bank = bank;
        return;
    }

    /* Convert Q31 -> Q28 (+18 dB internal headroom during 10-band cascade) */
    int32_t l_q28 = (*left_q31) >> 3;
    int32_t r_q28 = (*right_q31) >> 3;

    if (!s_bypass_eq[bank]) {
        if (s_worker_ready) {
            /* Dispatch Right channel to DspWorkerTask core in 1 cycle & run Left in parallel */
            s_chan_out_word(s_worker_chan.end_a, (uint32_t)r_q28);
            s_chan_out_word(s_worker_chan.end_a, bank);
            int32_t l_scaled = (int32_t)(((int64_t)l_q28 * (int64_t)s_pregain_q28[bank]) >> DSP_Q_FORMAT);
            l_q28 = dsp_filters_biquads(l_scaled,
                                        s_coeff_banks[bank],
                                        s_state_left,
                                        ANTDEC_PEQ_BANDS,
                                        DSP_Q_FORMAT);
            r_q28 = (int32_t)s_chan_in_word(s_worker_chan.end_a);
        } else {
            int32_t l_scaled = (int32_t)(((int64_t)l_q28 * (int64_t)s_pregain_q28[bank]) >> DSP_Q_FORMAT);
            int32_t r_scaled = (int32_t)(((int64_t)r_q28 * (int64_t)s_pregain_q28[bank]) >> DSP_Q_FORMAT);
            l_q28 = dsp_filters_biquads(l_scaled, s_coeff_banks[bank], s_state_left, ANTDEC_PEQ_BANDS, DSP_Q_FORMAT);
            r_q28 = dsp_filters_biquads(r_scaled, s_coeff_banks[bank], s_state_right, ANTDEC_PEQ_BANDS, DSP_Q_FORMAT);
        }
    }

    /* Optional Bauer stereophonic-to-binaural headphone crossfeed (-10.5 dB <700 Hz cross-mix) */
    if (crossfeed_en) {
        s_xfeed_lp_l += (int32_t)((((int64_t)l_q28 - s_xfeed_lp_l) * s_xfeed_alpha_q28[bank]) >> DSP_Q_FORMAT);
        s_xfeed_lp_r += (int32_t)((((int64_t)r_q28 - s_xfeed_lp_r) * s_xfeed_alpha_q28[bank]) >> DSP_Q_FORMAT);
        /* The two gains sum to unity to avoid boosting centered low-frequency content. */
        int32_t l_mix = (int32_t)(((int64_t)l_q28 * 195 + (int64_t)s_xfeed_lp_r * 61) >> 8);
        int32_t r_mix = (int32_t)(((int64_t)r_q28 * 195 + (int64_t)s_xfeed_lp_l * 61) >> 8);
        l_q28 = l_mix;
        r_q28 = r_mix;
    }

    *left_q31 = sat_q28_to_q31(l_q28);
    *right_q31 = sat_q28_to_q31(r_q28);
    __asm__ __volatile__("" ::: "memory");
    s_completed_bank = bank;
}
