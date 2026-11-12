#ifndef SETTINGS_MANAGER_H_
#define SETTINGS_MANAGER_H_
#include <stdint.h>
void SettingsManager_Load(const uint8_t settings[10], unsigned valid, uint16_t revision);
unsigned SettingsManager_Export(uint8_t settings[10], uint16_t *revision, unsigned *generation);
unsigned SettingsManager_Generation(void);
void SettingsManager_Snapshot(const uint8_t settings[10], uint16_t revision, unsigned capabilities, unsigned volume, unsigned now_ms);
unsigned SettingsManager_Next(unsigned now_ms, unsigned *operation, unsigned *value);
void SettingsManager_Result(unsigned ok, unsigned now_ms);
void SettingsManager_Offline(void);
unsigned SettingsManager_Ready(void);
unsigned SettingsManager_Owner(void);
unsigned SettingsManager_Failed(void);
#endif
