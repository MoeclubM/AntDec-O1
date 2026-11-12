#include "settings_manager.h"
#include <string.h>

enum { WAITING, RESTORING, READY, FAILED };
static uint8_t s_desired[10], s_current[10];
static uint16_t s_revision, s_current_revision;
static unsigned s_loaded, s_valid, s_seen, s_capabilities, s_volume, s_state, s_step, s_retry_at;
static unsigned s_startup_changed;
static volatile unsigned s_export_lock, s_generation;
static const unsigned s_operations[10] = {8, 7, 6, 9, 10, 11, 12, 13, 14, 1};
static const unsigned s_fields[10] = {3, 2, 1, 4, 5, 6, 7, 8, 9, 0};

void SettingsManager_Load(const uint8_t settings[10], unsigned valid, uint16_t revision)
{
    memcpy(s_desired, settings, 10u); s_valid = valid; s_revision = revision;
    s_state = WAITING;
    __asm__ __volatile__("" ::: "memory");
    s_loaded = 1u;
}

unsigned SettingsManager_Export(uint8_t settings[10], uint16_t *revision, unsigned *generation)
{
    unsigned before, valid;
    do {
        before = s_export_lock;
        if (before & 1u) continue;
        __asm__ __volatile__("" ::: "memory");
        memcpy(settings, s_desired, 10u); *revision = s_revision;
        *generation = s_generation; valid = s_valid;
        __asm__ __volatile__("" ::: "memory");
    } while ((before & 1u) || before != s_export_lock);
    return valid;
}
unsigned SettingsManager_Generation(void) { return s_generation; }
unsigned SettingsManager_Ready(void) { return s_loaded && s_seen && s_state == READY; }
unsigned SettingsManager_Owner(void) { return SettingsManager_Ready() && (s_capabilities & 16u); }
unsigned SettingsManager_Failed(void) { return s_state == FAILED; }

static void adopt(void)
{
    if (!s_valid || memcmp(s_desired, s_current, 10u) || s_revision != s_current_revision) {
        ++s_export_lock;
        __asm__ __volatile__("" ::: "memory");
        memcpy(s_desired, s_current, 10u); s_revision = s_current_revision; s_valid = 1u;
        ++s_generation;
        __asm__ __volatile__("" ::: "memory");
        ++s_export_lock;
    }
    s_state = READY;
}

static void reconcile(void)
{
    if (!(s_capabilities & 16u)) { s_state = READY; return; }
    if (!s_valid || (int16_t)(s_current_revision - s_revision) > 0) { adopt(); return; }
    if (!memcmp(s_desired, s_current, 10u) && s_revision == s_current_revision) { s_state = READY; return; }
    s_startup_changed = s_desired[2] != s_current[2];
    s_step = 0; s_state = RESTORING;
}

void SettingsManager_Snapshot(const uint8_t settings[10], uint16_t revision, unsigned capabilities, unsigned volume, unsigned now_ms)
{
    memcpy(s_current, settings, 10u); s_current_revision = revision;
    s_capabilities = capabilities; s_volume = volume; s_seen = 1u;
    if (!s_loaded) return;
    if (s_state == WAITING) reconcile();
    else if (s_state == READY) reconcile();
    else if (s_state == RESTORING && s_step == 15u) {
        if (!memcmp(s_desired, s_current, 10u) && s_revision == s_current_revision) s_state = READY;
        else SettingsManager_Result(0u, now_ms);
    }
}

unsigned SettingsManager_Next(unsigned now_ms, unsigned *operation, unsigned *value)
{
    if (!s_loaded || !s_seen) return 0u;
    if (s_state == FAILED && (int)(now_ms - s_retry_at) >= 0) { s_state = WAITING; reconcile(); }
    if (s_state != RESTORING) return 0u;
    while (s_step < 15u) {
        unsigned step = s_step++;
        *operation = 15u;
        if (step == 0u) *value = s_revision >> 12;
        else if (step == 1u) *value = 0x40u | (s_revision & 63u);
        else if (step == 2u) *value = 0x80u | ((s_revision >> 6) & 63u);
        else if (step < 13u) {
            unsigned field = s_fields[step - 3u];
            if (s_current[field] == s_desired[field]) continue;
            *operation = s_operations[step - 3u]; *value = s_desired[field];
        } else if (step == 13u) {
            if (!s_startup_changed || s_volume >= s_desired[2]) continue;
            *operation = 2u; *value = s_desired[2];
        } else *value = 0xC0u | (s_revision >> 12);
        return 1u;
    }
    return 0u;
}
void SettingsManager_Result(unsigned ok, unsigned now_ms)
{
    if (!ok && s_state == RESTORING) { s_state = FAILED; s_retry_at = now_ms + 5000u; }
}
void SettingsManager_Offline(void) { s_seen = 0u; s_state = WAITING; }
