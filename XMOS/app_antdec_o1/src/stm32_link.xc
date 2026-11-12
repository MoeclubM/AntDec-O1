#include <xs1.h>
#include <platform.h>
#include "antdec_state.h"
#include "usb_compat.h"
#include "controller_link.h"

extern void device_reboot(void);

/* Baseboard & STM32 control header ports on tile[0] (100 MHz ref timer = 100 ticks/us) */
on tile[0] : out port p_plugged   = PORT_PLUGGED;
on tile[0] : out port p_stm32_tx  = PORT_STM32_TX;
on tile[0] : in  port p_stm32_rx  = PORT_STM32_RX;
on tile[0] : out port p_dsd64_128 = PORT_DSD64_128;
on tile[0] : out port p_fs_status = PORT_FS_STATUS;

#define TICKS_PER_US            100u
#define TX_PERIOD_TICKS         (60000u * TICKS_PER_US)  /* 60 ms frame cadence */
#define TX_START_LOW_TICKS      (600u * TICKS_PER_US)
#define TX_START_HIGH_TICKS     (150u * TICKS_PER_US)
#define TX_BIT1_LOW_TICKS       (280u * TICKS_PER_US)
#define TX_BIT0_LOW_TICKS       (120u * TICKS_PER_US)
#define TX_BIT_HIGH_TICKS       (120u * TICKS_PER_US)
#define TX_FAST_MUTE_LOW_TICKS  (1350u * TICKS_PER_US)
#define TX_FAST_MUTE_GAP_TICKS  (250u * TICKS_PER_US)

enum {
    TX_PHASE_IDLE = 0,
    TX_PHASE_START_LOW,
    TX_PHASE_START_HIGH,
    TX_PHASE_BIT_LOW,
    TX_PHASE_BIT_HIGH,
    TX_PHASE_FAST_MUTE_LOW,
    TX_PHASE_FAST_MUTE_GAP,
};

static unsigned char calc_crc8(unsigned char b0, unsigned char b1)
{
    unsigned char data[2] = { b0, b1 };
    unsigned char crc = 0x5Au;
    for (unsigned i = 0; i < 2u; i++) {
        crc ^= data[i];
        for (unsigned b = 0; b < 8u; b++) {
            crc = (crc & 0x80u) ? (unsigned char)((crc << 1) ^ 0x07u) : (unsigned char)(crc << 1);
        }
    }
    return crc;
}

static void usb_phy_soft_detach_and_reboot(void)
{
    timer tmr;
    unsigned t0;

    /* Pre-mute DAC on STM32 before rebooting XMOS into another USB profile */
    p_stm32_tx <: 0;
    tmr :> t0;
    tmr when timerafter(t0 + TX_FAST_MUTE_LOW_TICKS) :> void;
    p_stm32_tx <: 1;
    tmr :> t0;
    tmr when timerafter(t0 + (10000u * TICKS_PER_US)) :> void;

    unsigned cfg2 = 0u;
    /* Power down internal XU316 USB WPHY terminations so the host cleanly detects detach */
    read_sswitch_reg(get_local_tile_id(), XS1_SSWITCH_USB_PHY_CFG2_NUM, cfg2);
    cfg2 = XS1_USB_PHY_CFG2_PONRST_SET(cfg2, 0);
    cfg2 = XS1_USB_PHY_CFG2_UTMI_RESET_SET(cfg2, 1);
    write_sswitch_reg(get_local_tile_id(), XS1_SSWITCH_USB_PHY_CFG2_NUM, cfg2);

    /* Hold bus detached for 500 ms before rebooting into the newly selected USB profile */
    tmr :> t0;
    tmr when timerafter(t0 + 50000000u) :> void;

    device_reboot();
}

void Stm32LinkTask(chanend c_bridge)
{
    timer tmr_rx;
    timer tmr_tx;
    timer tmr_ctrl;
    timer tmr_fast;
    unsigned now;

    /* Initialize status pins & idle-high single-wire TX */
    p_stm32_tx  <: 1;
    p_plugged   <: 0;
    p_dsd64_128 <: 0;
    p_fs_status <: 0;

    unsigned rx_pin_val = 1u;
    p_stm32_rx :> rx_pin_val;
    unsigned rx_fall_tick = 0u;
    unsigned rx_shift = 0u;
    int rx_bit_idx = -1;

    unsigned tx_phase = TX_PHASE_IDLE;
    unsigned tx_frame = 0u;
    unsigned last_sent_frame = 0u;
    unsigned send_meter = 0u;
    unsigned last_time_frame = 0u;
    int tx_bit_pos = 31;

    tmr_tx :> now;
    unsigned tx_next_tick = now + (20000u * TICKS_PER_US); /* First frame 20 ms after boot */
    unsigned ctrl_next_tick = now + (10000u * TICKS_PER_US);
    unsigned fast_next_tick = now + (500u * TICKS_PER_US);

    while (1) {
        select {
            /* 1. Edge-accurate non-blocking STM32 command receiver on X0D38 (PORT_STM32_RX) */
            case p_stm32_rx when pinsneq(rx_pin_val) :> rx_pin_val:
                tmr_rx :> now;
                rx_pin_val &= 1u;
                if (rx_pin_val == 0u) {
                    rx_fall_tick = now;
                } else {
                    unsigned low_us = (now - rx_fall_tick) / TICKS_PER_US;
                    if (low_us >= 450u && low_us <= 900u) {
                        rx_shift = 0u;
                        rx_bit_idx = 0;
                    } else if (rx_bit_idx >= 0 && low_us >= 55u && low_us <= 420u) {
                        unsigned bit = (low_us >= 200u) ? 1u : 0u;
                        rx_shift = (rx_shift << 1) | bit;
                        rx_bit_idx++;
                        if (rx_bit_idx == 24) {
                            rx_bit_idx = -1;
                            unsigned char b0  = (unsigned char)((rx_shift >> 16) & 0xFFu);
                            unsigned char b1  = (unsigned char)((rx_shift >> 8) & 0xFFu);
                            unsigned char crc = (unsigned char)(rx_shift & 0xFFu);
                            if (((b0 >> 4) == 0x05u) && (calc_crc8(b0, b1) == crc)) {
                                unsigned cmd = b0 & 0x0Fu;
                                if (cmd >= 5u) ControllerLink_Receive(cmd, b1);
                                else UsbCompat_OnStm32Command(cmd, b1);
                            }
                        }
                    } else {
                        rx_bit_idx = -1;
                    }
                }
                break;

            /* 2. Non-blocking PWM telemetry frame transmitter on X0D37 (PORT_STM32_TX) */
            case tmr_tx when timerafter(tx_next_tick) :> void:
                if (tx_phase == TX_PHASE_FAST_MUTE_LOW) {
                    p_stm32_tx <: 1;
                    tx_phase = TX_PHASE_FAST_MUTE_GAP;
                    tx_next_tick += TX_FAST_MUTE_GAP_TICKS;
                } else if (tx_phase == TX_PHASE_FAST_MUTE_GAP) {
                    tx_phase = TX_PHASE_IDLE;
                    tx_next_tick += (50u * TICKS_PER_US);
                } else if (tx_phase == TX_PHASE_IDLE) {
                    unsigned plugged = 0u, dsd_hi = 0u, fs_port = 0u;
                    tx_frame = UsbCompat_BuildStm32TxFrame(plugged, dsd_hi, fs_port);
                    last_sent_frame = tx_frame;
                    if (send_meter) {
                        unsigned time_frame = UsbCompat_BuildTimeFrame();
                        if (time_frame != last_time_frame) {
                            tx_frame = time_frame;
                            last_time_frame = time_frame;
                        } else {
                            tx_frame = UsbCompat_BuildMeterFrame();
                        }
                    }
                    send_meter = !send_meter;
                    unsigned control_frame = ControllerLink_PopFrame();
                    if (control_frame) tx_frame = control_frame;
                    p_plugged   <: plugged;
                    p_dsd64_128 <: dsd_hi;
                    p_fs_status <: fs_port;

                    p_stm32_tx <: 0;
                    tx_phase = TX_PHASE_START_LOW;
                    tx_next_tick += TX_START_LOW_TICKS;
                } else if (tx_phase == TX_PHASE_START_LOW) {
                    p_stm32_tx <: 1;
                    tx_phase = TX_PHASE_START_HIGH;
                    tx_bit_pos = 31;
                    tx_next_tick += TX_START_HIGH_TICKS;
                } else if (tx_phase == TX_PHASE_START_HIGH || tx_phase == TX_PHASE_BIT_HIGH) {
                    if (tx_bit_pos < 0) {
                        p_stm32_tx <: 1;
                        tx_phase = TX_PHASE_IDLE;
                        tx_next_tick += TX_PERIOD_TICKS;
                    } else {
                        unsigned bit = (tx_frame >> tx_bit_pos) & 1u;
                        p_stm32_tx <: 0;
                        tx_phase = TX_PHASE_BIT_LOW;
                        tx_next_tick += bit ? TX_BIT1_LOW_TICKS : TX_BIT0_LOW_TICKS;
                    }
                } else {
                    p_stm32_tx <: 1;
                    tx_bit_pos--;
                    tx_phase = TX_PHASE_BIT_HIGH;
                    tx_next_tick += TX_BIT_HIGH_TICKS;
                }
                break;

            /* 2b. Sub-millisecond fast-mute poll (every 500 us) */
            case tmr_fast when timerafter(fast_next_tick) :> void:
                fast_next_tick += (500u * TICKS_PER_US);
                if (tx_phase != TX_PHASE_FAST_MUTE_LOW && tx_phase != TX_PHASE_FAST_MUTE_GAP) {
                    unsigned fast_mute = 0u;
                    c_bridge <: (unsigned)BRIDGE_MSG_POLL_FAST_MUTE;
                    c_bridge :> fast_mute;
                    if (fast_mute) {
                        /* Preempt any in-progress telemetry frame with an immediate 1.35 ms mute pulse */
                        p_stm32_tx <: 0;
                        tx_phase = TX_PHASE_FAST_MUTE_LOW;
                        tmr_tx :> now;
                        tx_next_tick = now + TX_FAST_MUTE_LOW_TICKS;
                    }
                }
                break;

            /* 3. Periodic DSP sync, cross-tile telemetry poll & USB mode switch handler (every 10 ms) */
            case tmr_ctrl when timerafter(ctrl_next_tick) :> void:
                ctrl_next_tick += (10000u * TICKS_PER_US);
                ControllerLink_Tick();
                UsbCompat_Tick10ms();

                if (UsbCompat_PopClearPeq()) {
                    c_bridge <: (unsigned)BRIDGE_MSG_CLEAR_PEQ;
                }

                unsigned idx, freq_hz, q_x100, ftype, en;
                int gain_cB, preamp_cB;
                while (UsbCompat_PopDirtyPeqBand(idx, freq_hz, gain_cB, q_x100, ftype, en, preamp_cB) >= 0) {
                    c_bridge <: (unsigned)BRIDGE_MSG_SET_PEQ_BAND;
                    c_bridge <: idx;
                    c_bridge <: freq_hz;
                    c_bridge <: gain_cB;
                    c_bridge <: q_x100;
                    c_bridge <: ftype;
                    c_bridge <: en;
                    c_bridge <: preamp_cB;
                }

                unsigned preset = 0u, xfeed = 0u;
                if (UsbCompat_PopDspSync(preset, xfeed)) {
                    c_bridge <: (unsigned)BRIDGE_MSG_SET_DSP;
                    c_bridge <: preset;
                    c_bridge <: xfeed;
                }

                unsigned has_tel = 0u;
                c_bridge <: (unsigned)BRIDGE_MSG_POLL_TELEMETRY;
                c_bridge :> has_tel;
                if (has_tel) {
                    unsigned fmt, sr, bits, mqa_sr, hw_mute;
                    c_bridge :> fmt;
                    c_bridge :> sr;
                    c_bridge :> bits;
                    c_bridge :> mqa_sr;
                    c_bridge :> hw_mute;
                    UsbCompat_UpdateAudioTelemetry(fmt, sr, bits, mqa_sr, hw_mute);
                }

                if (tx_phase == TX_PHASE_IDLE) {
                    unsigned p_tmp = 0u, d_tmp = 0u, f_tmp = 0u;
                    unsigned cur_frame = UsbCompat_BuildStm32TxFrame(p_tmp, d_tmp, f_tmp);
                    if (cur_frame != last_sent_frame) {
                        tmr_tx :> now;
                        tx_next_tick = now + (50u * TICKS_PER_US);
                    }
                }

                unsigned meter_valid, meter_left, meter_right;
                c_bridge <: (unsigned)BRIDGE_MSG_POLL_METERS;
                c_bridge :> meter_valid;
                c_bridge :> meter_left;
                c_bridge :> meter_right;
                UsbCompat_UpdateMeters(meter_valid, meter_left, meter_right);

                if (tx_phase == TX_PHASE_IDLE && UsbCompat_CheckRebootRequest()) {
                    usb_phy_soft_detach_and_reboot();
                }
                break;
        }
    }
}
