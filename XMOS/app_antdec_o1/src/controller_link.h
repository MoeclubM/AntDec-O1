#ifndef CONTROLLER_LINK_H_
#define CONTROLLER_LINK_H_
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void ControllerLink_Tick(void);
void ControllerLink_Receive(unsigned cmd, unsigned value);
unsigned ControllerLink_Queue(unsigned op, unsigned value);
unsigned ControllerLink_PopFrame(void);
#ifndef __XC__
void ControllerLink_Status(uint8_t out[24]);
void ControllerLink_OutputSettings(uint8_t out[18]);
#endif
#ifdef __cplusplus
}
#endif
#endif
