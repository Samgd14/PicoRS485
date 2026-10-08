// Host-side stub of the Pico SDK <hardware/irq.h>: enough to record that a
// handler was registered, and to let the test invoke it.
#ifndef _STUB_HARDWARE_IRQ_H
#define _STUB_HARDWARE_IRQ_H

typedef unsigned int uint;

typedef void (*irq_handler_t)(void);

void irq_set_exclusive_handler(uint num, irq_handler_t handler);
void irq_remove_handler(uint num, irq_handler_t handler);
void irq_set_enabled(uint num, bool enabled);
// Lets a caller mask and then restore the line as it found it, which is what the
// driver does around a packet-config change.
bool irq_is_enabled(uint num);
// True for an exclusive handler and for a shared chain alike, which is the question
// the driver asks before it commits to taking the line.
bool irq_has_handler(uint num);

#endif
