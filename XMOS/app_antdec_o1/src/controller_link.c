#include "controller_link.h"
#include <string.h>
#include "settings_manager.h"

/* Tile 0 only. A snapshot is committed after all fields and the matching ACK. */
static uint8_t s_fields[24], s_staging[24];
static unsigned s_time_bytes;
static unsigned s_mask, s_snapshot_token;
static unsigned s_clock, s_seen_at, s_sent_at, s_started_at;
static unsigned s_online, s_pending, s_op, s_value, s_token, s_next_token, s_ack;
static unsigned s_result, s_sent;
static unsigned s_internal;

static void queue(unsigned op, unsigned value)
{
    if (++s_next_token > 255u) s_next_token = 1u;
    s_token = s_next_token;
    s_op = op;
    s_value = value;
    s_pending = 1u;
    s_sent = 0u;
    s_started_at = s_clock;
    s_result = 1u;
}

void ControllerLink_Tick(void)
{
    s_clock += 10u;
    if (s_online && s_clock - s_seen_at > 2500u) { s_online = 0u; SettingsManager_Offline(); }
    if (s_pending && s_clock - s_started_at >= 1800u) {
        s_pending = 0u;
        s_result = 4u;
        if (s_internal) SettingsManager_Result(0u, s_clock);
    }
    unsigned operation, value;
    if (s_online && !s_pending && SettingsManager_Next(s_clock, &operation, &value)) {
        queue(operation, value); s_internal = 1u; return;
    }
    if (!s_pending && s_clock - s_started_at >= 1000u) { queue(4u, 0u); s_internal = 0u; }
}

unsigned ControllerLink_Queue(unsigned op, unsigned value)
{
    if (op < 1u || op > 14u || op == 4u || value > 255u || (op == 1u && value > 7u) ||
        (op == 2u && value > 200u) || (op == 3u && value > 1u)) return 0u;
    if (op >= 6u && (!(s_fields[10] & 8u) || s_fields[11] != 1u)) return 0u;
    if ((op == 6u && ((int8_t)value < -24 || (int8_t)value > 24)) ||
        ((op == 7u || op == 8u) && value > 200u) ||
        ((op == 11u || op == 12u) && !value) ||
        (op == 13u && value > 3u) || (op == 14u && value > 1u)) return 0u;
    if (!s_online || (s_pending && s_op != 4u)) return 0u;
    if (!SettingsManager_Ready()) return 0u;
    queue(op, value); s_internal = 0u;
    return 1u;
}

unsigned ControllerLink_PopFrame(void)
{
    if (!s_pending || (s_sent && s_clock - s_sent_at < 300u)) return 0u;
    s_sent = 1u;
    s_sent_at = s_clock;
    uint32_t body = (0xCu << 20) | (s_op << 16) | (s_token << 8) | s_value;
    uint8_t crc = 0x5Au;
    for (int shift = 16; shift >= 0; shift -= 8) {
        crc ^= (uint8_t)(body >> shift);
        for (unsigned bit = 0u; bit < 8u; bit++) {
            crc = (crc & 0x80u) ? (uint8_t)((crc << 1) ^ 0x07u) : (uint8_t)(crc << 1);
        }
    }
    return (body << 8) | crc;
}

void ControllerLink_Receive(unsigned cmd, unsigned value)
{
    if (cmd == 5u) {
        s_snapshot_token = value;
        s_mask = 0u;
        s_time_bytes = 0u;
    } else if (cmd >= 6u && cmd <= 11u) {
        s_staging[cmd - 6u] = (uint8_t)value;
        s_mask |= 1u << (cmd - 6u);
    } else if (cmd == 14u && s_time_bytes < 17u) {
        unsigned i = s_time_bytes < 4u ? 6u + s_time_bytes : 11u + s_time_bytes - 4u;
        s_time_bytes++;
        s_staging[i] = (uint8_t)value;
        s_mask |= 1u << i;
    } else if (cmd == 15u) {
        s_staging[10] = (uint8_t)value;
        s_mask |= 1u << 10;
    } else if ((cmd == 12u || cmd == 13u) && value == s_snapshot_token) {
        unsigned expected = (s_staging[10] & 16u) ? 0xFFFFFFu : (s_staging[10] & 8u) ? 0x3FFFFFu : 0x7FFu;
        if (s_mask != expected || value != s_token) return;
        memcpy(s_fields, s_staging, sizeof(s_fields));
        s_seen_at = s_clock;
        s_online = 1u;
        s_mask = 0u;
        if (s_pending && value == s_token) {
            s_ack = value;
            s_pending = 0u;
            s_result = cmd == 12u ? 2u : 3u;
            if (s_internal) SettingsManager_Result(cmd == 12u, s_clock);
        }
        SettingsManager_Snapshot(s_fields + 12, (uint16_t)(s_fields[22] | (s_fields[23] << 8)),
                                 s_fields[10], s_fields[1], s_clock);
    }
}

void ControllerLink_OutputSettings(uint8_t out[18])
{
    memset(out, 0, 18u);
    memcpy(out, "OUT1", 4u);
    unsigned supported = s_online && (s_fields[10] & 8u) && s_fields[11] == 1u;
    out[4] = (uint8_t)(s_online | (supported << 1) | (SettingsManager_Ready() << 2) |
                      (SettingsManager_Failed() << 3) | (SettingsManager_Owner() ? 16u : 0u));
    if (supported) { out[5] = s_fields[11]; memcpy(out + 6, s_fields + 12, 10u); }
    memcpy(out + 16, s_fields + 22, 2u);
}

void ControllerLink_Status(uint8_t out[24])
{
    memset(out, 0, 24u);
    memcpy(out, "CTL1", 4u);
    out[4] = (uint8_t)(s_online | (s_pending << 1) | ((!SettingsManager_Ready()) << 2) |
                      (SettingsManager_Failed() << 3));
    out[5] = s_fields[0]; /* filter[2:0], DAC, amp, user mute, ready, DAC mute */
    out[6] = s_fields[1]; /* 0.5 dB attenuation */
    out[7] = s_fields[5] & 15u;
    out[8] = s_fields[5] >> 4;
    out[9] = s_fields[10];
    memcpy(out + 20, s_fields + 6, 4u);
    for (unsigned i = 0u; i < 3u; i++) {
        unsigned mv = s_fields[2u + i] * 20u;
        out[10u + 2u * i] = (uint8_t)mv;
        out[11u + 2u * i] = (uint8_t)(mv >> 8);
    }
    out[16] = (uint8_t)s_token;
    out[17] = (uint8_t)s_ack;
    out[18] = (uint8_t)s_result;
    out[19] = (uint8_t)s_op;
}
