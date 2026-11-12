#ifndef USB_DEBUG_H
#define USB_DEBUG_H

#include <stdint.h>

void UsbDebug_Tick(uint32_t now_ms);
void UsbDebug_OnRx(const uint8_t *data, uint32_t length);
void UsbDebug_SetOpen(uint8_t open);

#endif
