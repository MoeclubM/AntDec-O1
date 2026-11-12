#include <xs1.h>
#include <platform.h>
#include "xua.h"
#include "antdec_state.h"
#include "audio_pipeline.h"
#include "dsp_engine.h"

/* X1D09 = bit 3 of XS1_PORT_4A -> Q201 oscillator enable selection:
 *   0x00 (bit3=0): Y202 49.152 MHz ON,  Y203 45.1584 MHz OFF (48k family up to 768 kHz)
 *   0x08 (bit3=1): Y202 49.152 MHz OFF, Y203 45.1584 MHz ON  (44.1k family & DSD64..DSD512)
 */
on tile[1] : out port p_clk_sel = PORT_CLK_SEL;

#define CLK_SEL_48K_49M152      0x00u
#define CLK_SEL_44K1_45M1584    0x08u
/* 8 ms oscillator startup & MCLK stabilization at 100 MHz reference clock */
#define OSC_SETTLE_TICKS        800000u
/* 10 ms pre-switch mute settle when not already drained in AudioHub_MainLoop */
#define PRE_MUTE_SETTLE_TICKS   1000000u

static unsigned s_cur_clk_sel = 0xFFu;

static void delay_ref_ticks(unsigned ticks)
{
    timer tmr;
    unsigned t0;
    tmr :> t0;
    tmr when timerafter(t0 + ticks) :> void;
}

void AudioHwInit(void)
{
    /* Enable 49.152 MHz oscillator (matching DEFAULT_FREQ = 48 kHz) and wait for stable MCLK */
    s_cur_clk_sel = CLK_SEL_48K_49M152;
    p_clk_sel <: s_cur_clk_sel;
    delay_ref_ticks(OSC_SETTLE_TICKS);
    AudioPipeline_OnHwInit();
}

void AudioHwShutdown(void)
{
}

void AudioHwConfig_FastMuteNotify(void)
{
    AudioPipeline_OnFastMuteNotify();
}

void AudioHwConfig_Mute(void)
{
    if (AudioPipeline_ConsumeMuteDrained()) {
        return;
    }
    AudioPipeline_OnMute();
    delay_ref_ticks(PRE_MUTE_SETTLE_TICKS);
}

void AudioHwConfig_UnMute(void)
{
    AudioPipeline_OnUnMute();
}

void AudioHwConfig(unsigned samFreq, unsigned mClk, unsigned dsdMode, unsigned sampRes_DAC, unsigned sampRes_ADC)
{
    (void)sampRes_ADC;

    unsigned target_sel = (mClk == MCLK_441) ? CLK_SEL_44K1_45M1584 : CLK_SEL_48K_49M152;
    if (target_sel != s_cur_clk_sel) {
        s_cur_clk_sel = target_sel;
        p_clk_sel <: s_cur_clk_sel;
        delay_ref_ticks(OSC_SETTLE_TICKS);
    }

    AudioPipeline_OnHwConfig(samFreq, mClk, dsdMode, sampRes_DAC);
}

/* Dedicated 150-MIPS core #2 on tile[1]: Right-channel 10-band Q28 biquad co-processor */
void DspWorkerTask(void)
{
    DspEngine_WorkerLoop();
}

/* Dedicated 150-MIPS core #3 on tile[1]: RBJ biquad coefficient synthesizer & cross-tile bridge */
void AudioCtrlTask(chanend c_bridge)
{
    timer tmr;
    unsigned next_poll;
    tmr :> next_poll;
    next_poll += 2000000u; /* 20 ms periodic check */
    unsigned cached_dirty = 0u;
    unsigned cached_fmt = 0u, cached_sr = 0u, cached_bits = 0u, cached_mqa_sr = 0u, cached_mute = 0u;

    while (1) {
        unsigned cmd;
        select {
            case c_bridge :> cmd:
                if (cmd == BRIDGE_MSG_POLL_TELEMETRY) {
                    unsigned fmt = 0u, sr = 0u, bits = 0u, mqa_sr = 0u, hw_mute = 0u;
                    if (AudioPipeline_PollTelemetry(fmt, sr, bits, mqa_sr, hw_mute)) {
                        cached_dirty = 1u;
                        cached_fmt = fmt;
                        cached_sr = sr;
                        cached_bits = bits;
                        cached_mqa_sr = mqa_sr;
                        cached_mute = hw_mute;
                    }
                    c_bridge <: cached_dirty;
                    if (cached_dirty) {
                        c_bridge <: cached_fmt;
                        c_bridge <: cached_sr;
                        c_bridge <: cached_bits;
                        c_bridge <: cached_mqa_sr;
                        c_bridge <: cached_mute;
                        cached_dirty = 0u;
                    }
                } else if (cmd == BRIDGE_MSG_POLL_FAST_MUTE) {
                    c_bridge <: AudioPipeline_PopFastMuteRequest();
                } else if (cmd == BRIDGE_MSG_POLL_METERS) {
                    unsigned left, right;
                    unsigned valid = AudioPipeline_ReadMeters(left, right);
                    c_bridge <: valid;
                    c_bridge <: left;
                    c_bridge <: right;
                } else if (cmd == BRIDGE_MSG_SET_DSP) {
                    unsigned preset, xfeed;
                    c_bridge :> preset;
                    c_bridge :> xfeed;
                    AudioPipeline_ApplyDspCommand(preset, xfeed);
                } else if (cmd == BRIDGE_MSG_SET_PEQ_BAND) {
                    unsigned idx, freq_hz, q_x100, ftype, en;
                    int gain_cB, preamp_cB;
                    c_bridge :> idx;
                    c_bridge :> freq_hz;
                    c_bridge :> gain_cB;
                    c_bridge :> q_x100;
                    c_bridge :> ftype;
                    c_bridge :> en;
                    c_bridge :> preamp_cB;
                    AudioPipeline_ApplyPeqBand(idx, freq_hz, gain_cB, q_x100, ftype, en, preamp_cB);
                } else if (cmd == BRIDGE_MSG_CLEAR_PEQ) {
                    AudioPipeline_ClearCustomPeq();
                }
                break;

            case tmr when timerafter(next_poll) :> void:
                next_poll += 2000000u;
                unsigned fmt = 0, sr = 0, bits = 0, mqa_sr = 0, hw_mute = 0;
                if (AudioPipeline_PollTelemetry(fmt, sr, bits, mqa_sr, hw_mute)) {
                    cached_dirty = 1u;
                    cached_fmt = fmt;
                    cached_sr = sr;
                    cached_bits = bits;
                    cached_mqa_sr = mqa_sr;
                    cached_mute = hw_mute;
                }
                break;
        }
    }
}
