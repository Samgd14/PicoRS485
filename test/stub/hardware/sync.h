// Host-side stub of <hardware/sync.h>: only the interrupt save/restore pair, which the
// driver uses to read two values the interrupt handler can update together.
#ifndef _STUB_HARDWARE_SYNC_H
#define _STUB_HARDWARE_SYNC_H

#include <cstdint>

uint32_t save_and_disable_interrupts(void);
void restore_interrupts(uint32_t status);

#endif
