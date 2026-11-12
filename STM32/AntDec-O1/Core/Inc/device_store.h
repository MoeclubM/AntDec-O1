#ifndef DEVICE_STORE_H
#define DEVICE_STORE_H
#include <stdint.h>
#include <stdbool.h>

/* Ten byte wire/storage layout, version 1. Balance is signed 0.5 dB steps. */
typedef struct {
  uint8_t filter;
  int8_t balance;
  uint8_t startup, ceiling, fade_up, fade_down;
  uint8_t automute_level, automute_time, invert, automute_enabled;
} device_settings_t;

void DeviceStore_Init(void);
void DeviceStore_ObservePlayTime(uint32_t source_seconds, uint32_t now_ms);
void DeviceStore_Tick(bool playing, uint32_t now_ms);
uint32_t DeviceStore_PlaySeconds(void);
bool DeviceStore_ResetPlayTime(void);
bool DeviceStore_SaveFailed(void);
const device_settings_t *DeviceStore_Settings(void);
bool DeviceStore_SettingsValid(const device_settings_t *settings);
void DeviceStore_UpdateSettings(const device_settings_t *settings);
bool DeviceStore_SettingsPending(void);
uint16_t DeviceStore_SettingsRevision(void);
bool DeviceStore_SyncSettings(uint8_t part);
bool DeviceStore_SyncActive(void);
void DeviceStore_AbortSync(void);
#endif
