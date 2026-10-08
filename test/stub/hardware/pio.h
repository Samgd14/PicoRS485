// Host-side stub of the Pico SDK <hardware/pio.h>, only as wide as
// src/PicoRS485.cpp needs. Signatures mirror the SDK so a syntax check is real.
#ifndef _STUB_HARDWARE_PIO_H
#define _STUB_HARDWARE_PIO_H

#include <cstdint>
#include <cstddef>

typedef unsigned int uint;

enum pio_src_dest { pio_x, pio_y };
enum pio_instr_bits { dummy_none };

typedef struct pio_hw *PIO;

#define NUM_PIOS 2
#define NUM_PIO_STATE_MACHINES 4
#define NUM_PIO_IRQS 2
#ifndef PICO_PIO_USE_GPIO_BASE
#define PICO_PIO_USE_GPIO_BASE 1
#endif

// Only the sign matters to the driver, which compares against PICO_OK; these
// stand in for the SDK's own codes rather than reproducing their exact values.
#define PICO_OK                            0
#define PICO_ERROR_GENERIC                -1
#define PICO_ERROR_INVALID_ARG            -6
#define PICO_ERROR_BAD_ALIGNMENT         -10
#define PICO_ERROR_INSUFFICIENT_RESOURCES -11

// Real objects, not fake addresses: the driver takes the address of a FIFO and
// the interrupt helpers read the flag register, so these have to be memory.
struct pio_sm_hw {
    uint32_t shiftctrl;
    uint32_t pinctrl;
    uint32_t execctrl;
};

struct pio_hw {
    struct pio_sm_hw sm[NUM_PIO_STATE_MACHINES];
    volatile uint32_t txf[NUM_PIO_STATE_MACHINES];
    volatile uint32_t rxf[NUM_PIO_STATE_MACHINES];
    volatile uint32_t irq;
    // Sticky per-SM stall flags, write-1-to-clear, declared because the SDK has them.
    // Nothing in the driver reads this now that the bring-up probe is gone, and the
    // bit-level receive model stalls a push without raising the flag - see the model in
    // test_pico_rs485.cpp.
    volatile uint32_t fdebug;
};

// One bit per state machine, as the hardware has it.
#define PIO_FDEBUG_RXSTALL_BITS 0x0000000fu

extern struct pio_hw g_pio_hw[NUM_PIOS];

// The two instances as the SDK exposes them, plus the instance test its own
// parameter checks use.
#define pio0 (&g_pio_hw[0])
#define pio1 (&g_pio_hw[1])
#define PIO_IS_INSTANCE(p) ((p) == pio0 || (p) == pio1)
#define PIO_NUM(p) ((uint)((p) - g_pio_hw))

// Distinct, arbitrary numbers: what matters is that they are unique per block.
#define PIO0_IRQ_0   10
#define DREQ_PIO0_TX0 20
#define DREQ_PIO1_TX0 30
#define PIO_IRQ_NUM(p, irqn) (PIO0_IRQ_0 + NUM_PIO_IRQS * PIO_NUM(p) + (irqn))
#define PIO_DREQ_NUM(p, sm, is_tx)                          \
    (DREQ_PIO0_TX0 + (sm) +                                 \
     (((is_tx) ? 0 : NUM_PIO_STATE_MACHINES) + PIO_NUM(p) * (DREQ_PIO1_TX0 - DREQ_PIO0_TX0)))

struct pio_program {
    const uint16_t *instructions;
    uint8_t        length;
    int8_t         origin;
};

typedef struct {
    uint32_t clkdiv;
} pio_sm_config;

enum pio_fifo_join {
    PIO_FIFO_JOIN_NONE = 0,
    PIO_FIFO_JOIN_TX   = 1,
    PIO_FIFO_JOIN_RX   = 2,
};

// The eight flags exist; only 0-3 can be routed to the CPU, which is why the
// driver's TX-to-gap handshake on 4 and 5 is safe by construction.
enum pio_interrupt_source {
    pis_interrupt0 = 8,
    pis_interrupt1,
    pis_interrupt2,
    pis_interrupt3,
};

typedef enum pio_interrupt_source pio_interrupt_source_t;

PIO  pio_get_instance(uint index);
bool pio_can_add_program(PIO pio, const struct pio_program *program);
int  pio_add_program(PIO pio, const struct pio_program *program);
void pio_remove_program(PIO pio, const struct pio_program *program, uint loaded_offset);
void pio_clear_instruction_memory(PIO pio);
void pio_sm_claim(PIO pio, uint sm);
bool pio_sm_is_claimed(PIO pio, uint sm);
void pio_sm_unclaim(PIO pio, uint sm);
void pio_sm_set_enabled(PIO pio, uint sm, bool enabled);
int  pio_sm_init(PIO pio, uint sm, uint initial_pc, const pio_sm_config *config);
int  pio_sm_set_consecutive_pindirs(PIO pio, uint sm, uint pins_base, uint pin_count, bool is_out);
void pio_sm_set_pins_with_mask(PIO pio, uint sm, uint32_t pin_values, uint32_t pin_mask);
void pio_sm_set_pins_with_mask64(PIO pio, uint sm, uint64_t pin_values, uint64_t pin_mask);
void pio_sm_exec(PIO pio, uint sm, uint16_t instr);
// The blocking form waits for the injected instruction to run, which is what the
// receiver's Y write needs so it cannot be replaced before it takes effect.
void pio_sm_exec_wait_blocking(PIO pio, uint sm, uint16_t instr);
void pio_sm_restart(PIO pio, uint sm);
void pio_sm_clear_fifos(PIO pio, uint sm);
void pio_gpio_init(PIO pio, uint pin);
uint pio_get_gpio_base(PIO pio);
int  pio_set_gpio_base(PIO pio, uint gpio_base);

uint pio_sm_get_tx_fifo_level(PIO pio, uint sm);
bool pio_sm_is_rx_fifo_empty(PIO pio, uint sm);
bool pio_sm_is_tx_fifo_empty(PIO pio, uint sm);
uint32_t pio_sm_get(PIO pio, uint sm);

bool pio_interrupt_get(PIO pio, uint pio_interrupt_num);
void pio_interrupt_clear(PIO pio, uint pio_interrupt_num);
void pio_set_irq0_source_enabled(PIO pio, pio_interrupt_source_t source, bool enabled);

int pio_encode_set(enum pio_src_dest dest, uint value);
uint16_t pio_encode_jmp(uint addr);

void sm_config_set_out_pins(pio_sm_config *c, uint pin, uint count);
void sm_config_set_sideset_pins(pio_sm_config *c, uint pin);
void sm_config_set_in_pins(pio_sm_config *c, uint pin);
void sm_config_set_set_pins(pio_sm_config *c, uint pin, uint count);
void sm_config_set_jmp_pin(pio_sm_config *c, uint pin);
void sm_config_set_out_shift(pio_sm_config *c, bool shift_right, bool autopull, uint pull_threshold);
void sm_config_set_in_shift(pio_sm_config *c, bool shift_right, bool autopush, uint push_threshold);
void sm_config_set_clkdiv_int_frac8(pio_sm_config *c, uint16_t div_int, uint8_t div_frac8);
void sm_config_set_fifo_join(pio_sm_config *c, enum pio_fifo_join join);

#endif
