// Host-side stub of <hardware/timer.h>. The test owns the clock, and it advances on
// every read, which is what lets receive_for() reach its timeout.
//
// It is inline over a volatile global on purpose: the SDK's time_us_32() is
// inline too (it reads a volatile timer register), so a wait loop calling it has
// no compiler barrier in its body. Defining it out-of-line here would protect the
// loop and hide bugs of exactly the kind that matters.
#ifndef _STUB_HARDWARE_TIMER_H
#define _STUB_HARDWARE_TIMER_H

#include <cstdint>

extern volatile uint32_t g_time_us;
/// Reads of the clock, so a test can pin how many times a wait went round.
extern volatile int g_time_reads;

static inline uint32_t time_us_32(void) {
    g_time_us += 1000u;
    ++g_time_reads;
    return g_time_us;
}

#endif
