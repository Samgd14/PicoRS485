// Host-side stub of <pico/platform.h>, deliberately faithful to the SDK: an
// empty always-inline no-op for tight loops, and a compiler barrier. This
// matters - an out-of-line definition would act as an implicit barrier and hide
// optimiser behaviour the driver has to survive (it hid a deleted wait loop once
// already).
#ifndef _STUB_PICO_PLATFORM_H
#define _STUB_PICO_PLATFORM_H

static inline void tight_loop_contents(void) {}

#define __compiler_memory_barrier() __asm__ volatile ("" ::: "memory")

// The SDK's own host build makes the placement macros no-ops, since a desktop has no flash to keep
// code out of. Mirrored so that a build with PICO_RS485_ISR_IN_RAM=1 still compiles here.
#define __not_in_flash(group)
#define __not_in_flash_func(func) func
#define __time_critical_func(func) func

// The SDK maps this to the weak attribute, so a build can replace a library default.
#define __weak __attribute__((weak))

/// The core the calling code is on. The test moves it, which is the only way to exercise
/// the driver's single-core assertion on a desktop.
extern unsigned g_core_num;

static inline uint get_core_num(void) { return g_core_num; }

#endif
