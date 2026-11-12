#include <xs1.h>
#include "clock_monitor.h"

/* Called on USB tile[0]. Read the same MCLK-clocked port timer as XUA feedback;
 * GETTS does not consume samples or change the port/clock configuration. */
void ClockMonitor_Measure(unsigned &cycles, unsigned &reference_ticks)
{
    timer reference;
    unsigned port_id, first_count, last_count, first_time, last_time;
    asm volatile("ldw %0, dp[p_for_mclk_count]" : "=r"(port_id));
    asm volatile("getts %0, res[%1]" : "=r"(first_count) : "r"(port_id));
    reference :> first_time;
    /* 500 us stays below one 16-bit counter wrap at either audio MCLK. */
    reference when timerafter(first_time + 50000u) :> last_time;
    asm volatile("getts %0, res[%1]" : "=r"(last_count) : "r"(port_id));
    cycles = (last_count - first_count) & 0xFFFFu;
    reference_ticks = last_time - first_time;
}
