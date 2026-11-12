#ifndef CLOCK_MONITOR_H_
#define CLOCK_MONITOR_H_

#include <xccompat.h>

void ClockMonitor_Measure(REFERENCE_PARAM(unsigned, cycles),
                          REFERENCE_PARAM(unsigned, reference_ticks));

#endif
