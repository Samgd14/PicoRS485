// The stub SDK and the models the section files drive.
//
// The state they work on is declared in suite.h; what is defined here is what the
// model alone uses.
#include "suite.h"


// Compares two values and prints both when they differ: a table row that fails
// should say what it got, not only which expression it was.
void check_eq_ll(const char *file, int line, const char *expr,
                        long long got, long long want) {
    ++g_checks;
    if (got != want) {
        ++g_failures;
        if (g_case[0])
            std::printf("FAIL %s:%d [%s]  %s: got %lld, want %lld\n",
                        file, line, g_case, expr, got, want);
        else
            std::printf("FAIL %s:%d  %s: got %lld, want %lld\n",
                        file, line, expr, got, want);
    }
}


// ---------------------------------------------------- SDK stubs and models

// ---------------------------------------------------- SDK stubs and models

static const int g_sdk_error = PICO_ERROR_BAD_ALIGNMENT;


// ---------------------------------------------------- SDK stubs and models

// ------------------------------------------------------------------- SDK stubs
uint32_t clock_get_hz(enum clock_index) { return g_sys_hz; }


int pio_slot(PIO p) { return (int)(p - g_pio_hw); }


PIO pio_get_instance(uint index) { return &g_pio_hw[index]; }

bool pio_can_add_program(PIO, const struct pio_program *) { return true; }

int pio_add_program(PIO, const struct pio_program *) {
    // init() must empty the block before loading into it.
    if (!g_memory_cleared) ++g_add_dirty;
    ++g_add_calls;
    if (g_add_calls == g_add_fail_at) return PICO_ERROR_INSUFFICIENT_RESOURCES;
    return (g_add_calls - 1) * 8;          // distinct offsets, like the real loader
}


void pio_clear_instruction_memory(PIO) { ++g_clear_mem; g_memory_cleared = true; }

// A real claim map, so a double claim or an unowned unclaim is recorded rather
// than silently tolerated - that is the invariant claim_pio() has to keep.
void pio_sm_claim(PIO p, uint sm) {
    if (g_claimed[pio_slot(p)][sm]) { ++g_double_claim; return; }
    g_claimed[pio_slot(p)][sm] = true;
    ++g_claim;
}


bool pio_sm_is_claimed(PIO p, uint sm) { return g_claimed[pio_slot(p)][sm]; }

void pio_sm_unclaim(PIO p, uint sm) {
    if (!g_claimed[pio_slot(p)][sm]) { ++g_bad_unclaim; return; }
    g_claimed[pio_slot(p)][sm] = false;
    ++g_unclaim;
}


void pio_sm_set_enabled(PIO, uint sm, bool enabled) {
    if (enabled) { if (g_enabled_n  < 8) g_enabled[g_enabled_n++]   = (int)sm; }
    else         { if (g_disabled_n < 8) g_disabled[g_disabled_n++] = (int)sm; }
}


int pio_sm_init(PIO, uint, uint, const pio_sm_config *) {
    return g_sm_ok ? PICO_OK : g_sdk_error;
}


int pio_sm_set_consecutive_pindirs(PIO, uint, uint, uint, bool) {
    return g_pindir_ok ? PICO_OK : g_sdk_error;
}


void pio_sm_exec(PIO pio, uint sm, uint16_t instr) {
    ++g_sm_exec_calls; g_sm_exec_last = instr; g_exec_pio = pio; g_exec_sm = sm;
}


// The blocking form the real helper uses for its Y write. Nothing on the host reaches
// it - the .pio helpers are recorders here - but it has to exist for the syntax checks
// that compile the generated helper against these stubs.
void pio_sm_exec_wait_blocking(PIO pio, uint sm, uint16_t instr) {
    ++g_sm_exec_calls; g_sm_exec_last = instr; g_exec_pio = pio; g_exec_sm = sm;
}


void pio_gpio_init(PIO, uint pin) { g_function[pin] = GPIO_FUNC_PIO0; }

void pio_sm_set_pins_with_mask(PIO pio, uint sm, uint32_t pin_values, uint32_t pin_mask) {
    g_pio_cleared_mask |= pin_mask & ~pin_values;   // pins driven low through the PIO
    g_pio_cleared_last = pin_mask & ~pin_values;    // ...and the most recent call
    g_mask_pio = pio; g_mask_sm = sm;
}


// The 64-bit form is the one that can name a pin past 31; the mask is absolute in
// both, so this records exactly what the driver asked for, and of which machine.
void pio_sm_set_pins_with_mask64(PIO pio, uint sm, uint64_t pin_values, uint64_t pin_mask) {
    g_pio_cleared_mask |= pin_mask & ~pin_values;
    g_pio_cleared_last = pin_mask & ~pin_values;
    g_mask_pio = pio; g_mask_sm = sm;
}


uint32_t save_and_disable_interrupts(void) {
    ++g_irq_masked_depth;
    if (g_irq_masked_depth > g_irq_masked_max) g_irq_masked_max = g_irq_masked_depth;
    return 0;
}


void restore_interrupts(uint32_t) { --g_irq_masked_depth; }

uint pio_get_gpio_base(PIO p) { return g_pio_base[pio_slot(p)]; }

int  pio_set_gpio_base(PIO p, uint base) { g_pio_base[pio_slot(p)] = base; return PICO_OK; }

int  pio_encode_set(enum pio_src_dest, uint) { return 0; }

// A JMP carries its target in the low bits, so the recorded instruction doubles as
// the address the driver jumped to.
uint16_t pio_encode_jmp(uint addr) { return (uint16_t)(addr & 0x1fu); }

void sm_config_set_out_pins(pio_sm_config *, uint, uint) {}

void sm_config_set_sideset_pins(pio_sm_config *, uint) {}

void sm_config_set_in_pins(pio_sm_config *, uint) {}

void sm_config_set_set_pins(pio_sm_config *, uint, uint) {}

void sm_config_set_jmp_pin(pio_sm_config *, uint) {}

void sm_config_set_out_shift(pio_sm_config *, bool, bool, uint) {}

void sm_config_set_in_shift(pio_sm_config *, bool, bool, uint) {}

void sm_config_set_clkdiv_int_frac8(pio_sm_config *, uint16_t, uint8_t) {}


void gpio_init(uint gpio) {          // the SDK's order: direction, value, function
    g_dir_out[gpio]  = false;

    g_value[gpio]    = false;
    g_function[gpio] = GPIO_FUNC_SIO;
}


void gpio_set_dir(uint gpio, bool out) { g_dir_out[gpio] = out; }

void gpio_put(uint gpio, bool value) { g_value[gpio] = value; }

void gpio_pull_up(uint) {}

void gpio_disable_pulls(uint) {}

void gpio_set_function(uint gpio, gpio_function_t fn) { g_function[gpio] = (int)fn; }

bool gpio_get(uint gpio) { return g_value[gpio]; }   // what the pad shows right now


static const uint16_t g_words[8] = {};


extern const struct pio_program rs485_tx_program    = { g_words, 4, -1 };


extern const struct pio_program rs485_txgap_program = { g_words, 4, -1 };


extern const struct pio_program rs485_rx_program    = { g_words, 4, -1 };


extern const struct pio_program rs485_rxidle_program  = { g_words, 4, -1 };


extern const struct pio_program rs485_tx_program;


extern const struct pio_program rs485_txgap_program;


extern const struct pio_program rs485_rx_program;


extern const struct pio_program rs485_rxidle_program;


pio_sm_config rs485_tx_program_get_default_config(uint) { return pio_sm_config{}; }

pio_sm_config rs485_txgap_program_get_default_config(uint) { return pio_sm_config{}; }

pio_sm_config rs485_rx_program_get_default_config(uint) { return pio_sm_config{}; }

pio_sm_config rs485_rxidle_program_get_default_config(uint) { return pio_sm_config{}; }


// Each recorder mimics its .pio counterpart: it records what it was handed,
// returns the SDK's verdict on the pin setup and the machine config, and only
// enables the machine once both succeeded.
int rs485_tx_program_init(PIO pio, uint sm, uint offset, uint pin_tx, uint pin_de,
                          uint clkdiv_int, uint8_t clkdiv_frac8, uint char_bits) {
    ++g_tx.calls; g_tx.offset = offset; g_tx.clkdiv_int = clkdiv_int;
    g_tx.clkdiv_frac8 = clkdiv_frac8; g_tx.count = char_bits;
    g_tx.pin_a = pin_tx; g_tx.pin_b = pin_de;
    if (!g_pindir_ok) return g_sdk_error;
    pio_gpio_init(pio, pin_tx);          // mux TX to the PIO...
    pio_gpio_init(pio, pin_de);          // ...and DE with it
    if (!g_sm_ok) return g_sdk_error;
    pio_sm_set_enabled(pio, sm, true);
    return PICO_OK;
}


int rs485_rx_program_init(PIO pio, uint sm, uint offset, uint pin_rx, uint pin_rx_act,
                          uint clkdiv_int, uint8_t clkdiv_frac8, uint isr_bits) {
    ++g_rx.calls; g_rx.offset = offset; g_rx.clkdiv_int = clkdiv_int;
    g_rx.clkdiv_frac8 = clkdiv_frac8; g_rx.count = isr_bits;
    g_rx.pin_a = pin_rx; g_rx.pin_b = pin_rx_act;
    if (!g_pindir_ok) return g_sdk_error;
    pio_gpio_init(pio, pin_rx);
    pio_gpio_init(pio, pin_rx_act);
    if (!g_sm_ok) return g_sdk_error;
    pio_sm_set_enabled(pio, sm, true);
    return PICO_OK;
}


int rs485_rxidle_program_init(PIO pio, uint sm, uint offset, uint pin_rx_act,
                              uint clkdiv_int, uint8_t clkdiv_frac8, uint steps) {
    ++g_rxidle.calls; g_rxidle.offset = offset; g_rxidle.clkdiv_int = clkdiv_int;
    g_rxidle.clkdiv_frac8 = clkdiv_frac8; g_rxidle.count = steps;
    g_rxidle.pin_a = pin_rx_act;
    if (!g_sm_ok) return g_sdk_error;
    pio_sm_set_enabled(pio, sm, true);
    return PICO_OK;
}


int rs485_txgap_program_init(PIO pio, uint sm, uint offset, uint clkdiv_int,
                             uint8_t clkdiv_frac8, uint gap_bits) {
    ++g_txgap.calls; g_txgap.offset = offset; g_txgap.clkdiv_int = clkdiv_int;
    g_txgap.clkdiv_frac8 = clkdiv_frac8; g_txgap.count = gap_bits;
    if (!g_sm_ok) return g_sdk_error;
    pio_sm_set_enabled(pio, sm, true);
    return PICO_OK;
}


void rs485_tx_set_char_bits(PIO pio, uint sm, uint char_bits) {
    ++g_set_tx_calls; g_set_tx_bits = char_bits; g_set_tx_sm = sm; g_set_tx_pio = pio;
}


void rs485_txgap_set_gap_bits(PIO pio, uint sm, uint gap_bits) {
    ++g_set_gap_calls; g_set_gap_bits = gap_bits; g_set_gap_sm = sm; g_set_gap_pio = pio;
}


void rs485_rx_set_isr_bits(PIO pio, uint sm, uint isr_bits) {
    ++g_set_rx_calls; g_set_rx_bits = isr_bits; g_set_rx_sm = sm; g_set_rx_pio = pio;
}


void rs485_rxidle_set_steps(PIO pio, uint sm, uint steps) {
    ++g_set_rxidle_calls; g_set_rxidle_steps = steps; g_set_rxidle_sm = sm; g_set_rxidle_pio = pio;
}


// One received sample. LSB-first data and a one bit stop, as the wire presents them.
void deliver_sample(PIO pio, uint sm, uint32_t bit, uint isr_bits) {
    const int slot = (int)PIO_NUM(pio);
    rx_group_state &g = g_rxgrp[slot][sm];
    if (g.stalled) return;                     // the machine is not executing
    g.isr = (g.isr >> 1) | ((bit & 1u) << 31);  // the receiver shifts right
    if (++g.count >= isr_bits) {
        if (g_rxq_n[slot][sm] < fifo_depth) {   // autopush completes
            g_rxq[slot][sm][g_rxq_n[slot][sm]++] = g.isr;
            g.isr = 0;
            g.count = 0;
        } else {                                // ...and here it does not
            g.stalled = true;
            g.held = (g.isr >> 31) & 1u;        // the shift this instruction carries
        }
    }
}


// A whole character's samples: eight data bits LSB first, then the stop sample.
void deliver_char_bits(PIO pio, uint sm, uint8_t byte, uint isr_bits) {
    for (int i = 0; i < 8; ++i) deliver_sample(pio, sm, (uint32_t)(byte >> i) & 1u, isr_bits);
    deliver_sample(pio, sm, 1u, isr_bits);
}


// The FIFO gaining room is what retires a stalled autopush.
void release_stalled(int slot, int sm, uint isr_bits) {
    rx_group_state &g = g_rxgrp[slot][sm];
    if (!g.stalled) return;
    g.stalled = false;
    if (g.count != 0) {                         // no restart got in first: it just retires
        if (g_rxq_n[slot][sm] < fifo_depth) {
            g_rxq[slot][sm][g_rxq_n[slot][sm]++] = g.isr;
        }
        g.isr = 0;
        g.count = 0;
    } else {                                    // a restart cleared the counter, not the shift
        g.isr = (g.isr >> 1) | (g.held << 31);
        g.count = 1;
    }
    (void)isr_bits;
}


struct pio_hw *pio_hw_of(int slot) { return &g_pio_hw[slot]; }


bool fifo_of(uint32_t *addr, bool *is_tx, int *slot, int *sm) {
    for (int p = 0; p < NUM_PIOS; ++p) {
        struct pio_hw *hw = pio_hw_of(p);
        for (int s = 0; s < NUM_PIO_STATE_MACHINES; ++s) {
            if (addr == (uint32_t *)(uintptr_t)&hw->txf[s]) {
                *is_tx = true; *slot = p; *sm = s; return true;
            }
            if (addr == (uint32_t *)(uintptr_t)&hw->rxf[s]) {
                *is_tx = false; *slot = p; *sm = s; return true;
            }
        }
    }
    return false;
}


// True when the channel's DREQ would let one word through.
bool dma_dreq_ready(const dma_state &d) {
    const uint span = DREQ_PIO1_TX0 - DREQ_PIO0_TX0;
    if (d.dreq < DREQ_PIO0_TX0 || d.dreq >= DREQ_PIO0_TX0 + NUM_PIOS * span) return false;
    const uint slot   = (d.dreq - DREQ_PIO0_TX0) / span;
    const uint within = (d.dreq - DREQ_PIO0_TX0) % span;
    if (within < NUM_PIO_STATE_MACHINES) {
        return g_txq_n[slot][within] < fifo_depth;                  // TX not full
    }
    return g_rxq_n[slot][within - NUM_PIO_STATE_MACHINES] > 0;       // RX not empty
}


void dma_run() {
    for (uint ch = 0; ch < NUM_DMA_CHANNELS; ++ch) {
        dma_state &d = g_dma[ch];
        while (d.busy && d.count != 0 && dma_dreq_ready(d)) {
            bool src_tx = false, dst_tx = false;
            int src_slot = 0, src_sm = 0, dst_slot = 0, dst_sm = 0;
            const bool src_fifo = fifo_of(d.read, &src_tx, &src_slot, &src_sm);
            const bool dst_fifo = fifo_of(d.write, &dst_tx, &dst_slot, &dst_sm);

            uint32_t value = 0;
            if (src_fifo) {                                  // RX: FIFO to memory
                value = g_rxq[src_slot][src_sm][0];
                for (int i = 1; i < g_rxq_n[src_slot][src_sm]; ++i) {
                    g_rxq[src_slot][src_sm][i - 1] = g_rxq[src_slot][src_sm][i];
                }
                --g_rxq_n[src_slot][src_sm];
            } else {
                value = *d.read;
                if (d.rinc) ++d.read;
            }

            if (dst_fifo) {                                  // TX: memory to FIFO
                // The model's FIFO is a fixed array: a push past its depth means the
                // driver programmed a channel the FIFO cannot absorb, which is a
                // harness-visible bug worth failing on rather than corrupting memory.
                if (g_txq_n[dst_slot][dst_sm] < fifo_depth) {
                    g_txq[dst_slot][dst_sm][g_txq_n[dst_slot][dst_sm]++] = value;
                } else {
                    ++g_model_overrun;
                }
            } else {
                *d.write = value;
                if (d.winc) ++d.write;
            }

            --d.count;
            // With a lag set the write has not retired, so the pointer the driver can
            // see is exactly one word behind until a later read refreshes it. The view
            // is left as the channel stood before this transfer retired.
            if (g_retire_lag != 0) {
                g_hw_view[ch] = d.hw;
                g_retire_pending[ch] = g_retire_lag;
            }
            d.hw.transfer_count = d.count;
            d.hw.write_addr     = (uint32_t)(uintptr_t)d.write;
            if (d.count == 0) d.busy = false;
        }
    }
}


// --- SDK stubs the model needs -------------------------------------------

// The SDK's default config has the channel enabled and 32-bit transfers. Nothing in the
// driver clears the enable bit any more - dma_channel_cleanup() does that - but the
// field is what dma_channel_configure() copies into the model, so it is what decides
// whether a freshly armed channel is busy.
dma_channel_config dma_channel_get_default_config(uint) {
    dma_channel_config c{};
    c.data_size      = DMA_SIZE_32;
    c.read_increment = true;
    c.enable         = true;
    return c;
}


void channel_config_set_transfer_data_size(dma_channel_config *c,
                                           enum dma_channel_transfer_size s) {
    c->data_size = s;
}


void channel_config_set_read_increment(dma_channel_config *c, bool increment) {
    c->read_increment = increment;
}


void channel_config_set_write_increment(dma_channel_config *c, bool increment) {
    c->write_increment = increment;
}


void channel_config_set_dreq(dma_channel_config *c, uint dreq) { c->dreq = dreq; }


int dma_claim_unused_channel(bool) {
    for (uint ch = 0; ch < NUM_DMA_CHANNELS; ++ch) {
        if (!g_dma[ch].claimed) {
            g_dma[ch] = dma_state{};
            g_dma[ch].claimed = true;
            ++g_dma_claimed;
            return (int)ch;
        }
    }
    return -1;
}


void dma_channel_unclaim(uint ch) {
    if (ch < NUM_DMA_CHANNELS) {
        g_dma[ch] = dma_state{};
        --g_dma_claimed;
    }
}


void dma_channel_configure(uint ch, const dma_channel_config *c, volatile void *write_addr,
                           const volatile void *read_addr, uint count, bool trigger) {
    dma_state &d = g_dma[ch];
    // A channel with work left that is still enabled must not be reprogrammed: the
    // driver is supposed to stop it first.
    if (d.enable && d.count > 0) ++g_configure_live;
    d.size = (int)c->data_size;
    d.read  = (uint32_t *)(uintptr_t)read_addr;
    d.write = (uint32_t *)(uintptr_t)write_addr;
    d.count = count;
    d.rinc  = c->read_increment;
    d.winc  = c->write_increment;
    d.dreq  = c->dreq;
    d.enable = c->enable;
    d.busy  = trigger && count != 0 && c->enable;
    d.hw.transfer_count = count;
    d.hw.write_addr     = (uint32_t)(uintptr_t)write_addr;
    // A receive arm is the one whose source is an RX FIFO.
    if (g_raise_idle_on_arm) {
        bool is_tx = false; int slot = 0, sm = 0;
        if (fifo_of((uint32_t *)(uintptr_t)read_addr, &is_tx, &slot, &sm) && !is_tx) {
            pio_hw_of(slot)->irq |= (1u << k_rx_done_irq);
            g_raise_idle_on_arm = false;      // one-shot
        }
    }
}


// Mirrors the SDK's dma_channel_cleanup() (hardware_dma/dma.c): it clears the channel's
// enable bit and CHAIN_TO with a masked write that leaves the rest of the control
// register alone, takes the channel off every DMA IRQ, and then aborts. Only the enable
// bit and the abort are modelled here - the driver neither chains nor enables a DMA IRQ,
// so CHAIN_TO and the IRQ acknowledgement have nothing to observe - but the order is the
// part that matters, and it is the SDK's: EN low before the abort, so an abort cannot be
// re-triggered by a level-paced DREQ into a buffer that is being reused.
void dma_channel_cleanup(uint ch) {
    if (ch < NUM_DMA_CHANNELS) g_dma[ch].enable = false;
    dma_channel_abort(ch);
}


void dma_channel_abort(uint ch) {
    // An abort does not clear CTRL.EN on the real hardware, so an abort of an
    // enabled channel can be re-triggered by a level-paced DREQ. Count them: the
    // driver reaches this only through dma_channel_cleanup(), so this counter is what
    // catches a bare dma_channel_abort() creeping back in.
    if (ch < NUM_DMA_CHANNELS && g_dma[ch].enable) ++g_abort_while_enabled;
    if (ch < NUM_DMA_CHANNELS) {
        g_dma[ch].busy = false;
        // The hardware clears TRANS_COUNT on abort. Leaving it alone here hid a real
        // bug for a whole session: the driver read the count after stopping the
        // channel, so on silicon it always saw the full arm count, judged every burst
        // over-long, and dropped bursts it had in fact collected correctly - while
        // this model reported the true remainder and stayed green.
        g_dma[ch].hw.transfer_count = 0;
    }
}


bool dma_channel_is_busy(uint ch) { return g_dma[ch].busy && g_dma[ch].count != 0; }

dma_channel_hw_t *dma_channel_hw_addr(uint ch) {
    // Reads that a lag covers see the pre-retirement value; every other read sees the
    // channel as it stands, which is the behaviour the rest of the suite expects.
    if (g_retire_pending[ch] > 0) --g_retire_pending[ch];
    else                         g_hw_view[ch] = g_dma[ch].hw;
    return &g_hw_view[ch];
}


void irq_set_exclusive_handler(uint num, irq_handler_t handler) {
    if (num < 32) g_irq_handler[num] = handler;
}


void irq_remove_handler(uint num, irq_handler_t handler) {
    if (num >= 32) return;
    // The driver names the handler it registered, and with one handler per block that has to be the
    // right one: removing someone else's would leave this line live.
    if (g_irq_handler[num] != handler) ++g_bad_irq_remove;
    g_irq_handler[num] = nullptr;
}


void irq_set_enabled(uint num, bool enabled) { if (num < 32) g_irq_enabled[num] = enabled; }

bool irq_is_enabled(uint num) { return num < 32 && g_irq_enabled[num]; }

bool irq_has_handler(uint num) { return num < 32 && g_irq_handler[num] != nullptr; }


// Stands in for another library that got to the block's IRQ line first.
void other_library_irq_handler(void) {}


void sem_release(semaphore_t *sem) {
    ++g_sem_releases;
    if (sem->permits < sem->max_permits) ++sem->permits;
}


extern "C" void pico_rs485_core_violation(void) { ++g_core_hook_calls; }


bool pio_interrupt_get(PIO pio, uint n) { return (pio->irq & (1u << n)) != 0; }

void pio_interrupt_clear(PIO pio, uint n) { pio->irq &= ~(1u << n); }   // W1C in hardware: the effect is a clear

void pio_set_irq0_source_enabled(PIO, pio_interrupt_source_t source, bool enabled) {
    const int s = (int)source;
    if (s >= 0 && s < 16) { if (enabled) ++g_irq_src_set[s]; else ++g_irq_src_cleared[s]; }
}


void pio_sm_restart(PIO pio, uint sm) {
    ++g_sm_restarts; g_restart_pio = pio; g_restart_sm = sm;
    // The counters and the shift register go; a stalled autopush is a *program*
    // instruction, so it is not cancelled by this and stays pending.
    const int slot = (int)PIO_NUM(pio);
    g_rxgrp[slot][sm].isr = 0;
    g_rxgrp[slot][sm].count = 0;
}


void pio_sm_clear_fifos(PIO pio, uint sm) {
    const int slot = (int)PIO_NUM(pio);
    g_txq_n[slot][sm] = 0;
    g_rxq_n[slot][sm] = 0;
    ++g_fifo_clears;
    // Emptying the FIFO is what gives a stalled autopush room to retire - and this is
    // the whole point of the model: retires *against the counter as it stands now*.
    release_stalled(slot, (int)sm, g_rx.count != 0 ? g_rx.count : 9u);
}


uint pio_sm_get_tx_fifo_level(PIO pio, uint sm) { return (uint)g_txq_n[(int)PIO_NUM(pio)][sm]; }

bool pio_sm_is_rx_fifo_empty(PIO pio, uint sm) { return g_rxq_n[(int)PIO_NUM(pio)][sm] == 0; }

bool pio_sm_is_tx_fifo_empty(PIO pio, uint sm) { return g_txq_n[(int)PIO_NUM(pio)][sm] == 0; }

// Reading the FIFO pops it, which is what the hardware does.
uint32_t pio_sm_get(PIO pio, uint sm) {
    const int slot = (int)PIO_NUM(pio);
    if (g_rxq_n[slot][sm] == 0) return pio_hw_of(slot)->rxf[sm];
    const uint32_t value = g_rxq[slot][sm][0];
    for (int i = 1; i < g_rxq_n[slot][sm]; ++i) g_rxq[slot][sm][i - 1] = g_rxq[slot][sm][i];
    --g_rxq_n[slot][sm];
    return value;
}


// --- helpers that drive the simulated hardware ---------------------------

// Hands one already-grouped character to the receiver's FIFO, the way an autopush that
// completed would. For a burst built sample by sample, where the grouping itself is what
// is under test, use deliver_sample() / deliver_char_bits() below instead.
void deliver_rx(PIO pio, uint sm, uint32_t sampled_bits, uint isr_bits) {
    const int slot = (int)PIO_NUM(pio);
    g_rxq[slot][sm][g_rxq_n[slot][sm]++] = sampled_bits << (32 - isr_bits);
    dma_run();
}


// Pushes a character into the receive FIFO without letting the DMA take it,
// which is the instant where the idle flag wins the race with the DMA.
void deliver_rx_unsent(PIO pio, uint sm, uint32_t sampled_bits, uint isr_bits) {
    const int slot = (int)PIO_NUM(pio);
    g_rxq[slot][sm][g_rxq_n[slot][sm]++] = sampled_bits << (32 - isr_bits);
}


// Clocks one character out of the transmitter and returns the framed word.
bool run_tx(PIO pio, uint sm, uint32_t *word_out) {
    const int slot = (int)PIO_NUM(pio);
    dma_run();
    if (g_txq_n[slot][sm] == 0) return false;
    *word_out = g_txq[slot][sm][0];
    for (int i = 1; i < g_txq_n[slot][sm]; ++i) g_txq[slot][sm][i - 1] = g_txq[slot][sm][i];
    --g_txq_n[slot][sm];
    return true;
}


// Raises a PIO flag and runs the routed handler, as the hardware would.
void raise_pio_irq(PIO pio, uint flag) {
    pio->irq |= (1u << flag);
    const uint num = PIO_IRQ_NUM(pio, 0);
    if (num < 32 && g_irq_enabled[num] && g_irq_handler[num] != nullptr) g_irq_handler[num]();
}


// Clocks the transmitter empty: the test refills the FIFO through the DMA and
// then drains it one character at a time.
void drain_tx(PIO pio, uint sm) {
    uint32_t word = 0;
    while (run_tx(pio, sm, &word)) {}
}


void sent_probe(void *user, uint32_t burst_id) {
    ++g_sent_calls;
    g_sent_id = burst_id;
    g_sent_user = user;
}


void sent_then_send(void *user, uint32_t) {
    ++g_sent_calls;
    PicoRS485 *ch = (PicoRS485 *)user;
    const uint8_t again[1] = {0x07};
    if (ch->send(again, 1) == PicoRS485::ok) ++g_sent_resends;
}


void reset_stubs() {
    g_add_calls = 0; g_add_fail_at = -1; g_clear_mem = 0;
    g_memory_cleared = false;
    g_pindir_ok = true; g_sm_ok = true;
    g_claim = 0; g_unclaim = 0; g_enabled_n = 0; g_disabled_n = 0;
    for (int i = 0; i < NUM_PIOS; ++i) {
        g_pio_base[i] = 0;
        for (int s = 0; s < NUM_PIO_STATE_MACHINES; ++s) g_claimed[i][s] = false;
    }
    for (int i = 0; i < 8; ++i) { g_enabled[i] = -1; g_disabled[i] = -1; }
    g_pio_cleared_mask = 0;
    g_set_tx_calls = g_set_rx_calls = g_set_rxidle_calls = g_set_gap_calls = 0;
    g_set_tx_bits = g_set_rx_bits = g_set_rxidle_steps = g_set_gap_bits = 0;
    for (int i = 0; i < NUM_BANK0_GPIOS; ++i) {
        g_dir_out[i] = false; g_value[i] = false; g_function[i] = 0;
    }
    for (int p = 0; p < NUM_PIOS; ++p) {
        pio_hw_of(p)->irq = 0;
        for (int s = 0; s < NUM_PIO_STATE_MACHINES; ++s) {
            g_txq_n[p][s] = 0;
            g_rxq_n[p][s] = 0;
        }
    }
    for (uint ch = 0; ch < NUM_DMA_CHANNELS; ++ch) g_dma[ch] = dma_state{};
    g_dma_claimed = 0;
    for (int i = 0; i < 32; ++i) { g_irq_handler[i] = nullptr; g_irq_enabled[i] = false; }
    g_core_num = 0;
    g_time_us = 0;
    g_time_reads = 0;
    g_sm_restarts = 0;
    g_restart_pio = nullptr;
    g_restart_sm = 99;
    g_sm_exec_calls = 0;
    g_sm_exec_last = 0xffff;
    g_exec_pio = nullptr;
    g_exec_sm = 99;
    g_irq_masked_depth = 0;
    g_irq_masked_max = 0;
    g_fifo_clears = 0;
    g_model_overrun = 0;
    g_raise_idle_on_arm = false;
    for (int i = 0; i < 16; ++i) { g_irq_src_set[i] = 0; g_irq_src_cleared[i] = 0; }
    g_mask_pio = nullptr;
    g_mask_sm = 99;
    g_pio_cleared_last = 0;
    g_set_tx_pio = g_set_rx_pio = g_set_rxidle_pio = g_set_gap_pio = nullptr;
    g_abort_while_enabled = 0;
    g_configure_live = 0;
    g_set_tx_sm = g_set_rx_sm = g_set_rxidle_sm = g_set_gap_sm = 99;
    g_sent_calls = 0;
    g_sent_id = 0;
    g_sent_user = nullptr;
    g_sent_resends = 0;
    g_tx = record{}; g_rx = record{}; g_rxidle = record{}; g_txgap = record{};
}


// True if the recorded enable/disable sequence is exactly 0,1,2,3 in order.
bool saw_all_four(const int *v, int n) {
    if (n != 4) return false;
    for (int i = 0; i < 4; ++i) if (v[i] != i) return false;
    return true;
}


// True if two packet configs agree field for field.
bool same_packet(const pico_rs485_packet_config &a, const pico_rs485_packet_config &b) {
    return a.data_bits == b.data_bits && a.parity_bits == b.parity_bits &&
           a.stop_bits == b.stop_bits && a.rx_idle_bits_thresh == b.rx_idle_bits_thresh &&
           a.tx_gap_bits == b.tx_gap_bits && a.even_parity == b.even_parity;
}


// The clock maths exactly as it was inside init() before the refactor, kept
// here as the reference the refactored path has to reproduce.
bool old_clock(uint32_t baudrate, uint32_t sys_hz, uint32_t &out_int, uint8_t &out_frac) {
    constexpr uint32_t pio_cycles_per_bit = 8;
    constexpr uint32_t clkdiv_frac_scale  = 256;
    if (baudrate == 0) return false;
    if (baudrate > sys_hz / pio_cycles_per_bit) return false;

    uint32_t denom  = baudrate * pio_cycles_per_bit;
    uint32_t scaled = (uint32_t)(((uint64_t)sys_hz * clkdiv_frac_scale + denom / 2) / denom);
    uint32_t clkdiv_int  = scaled / clkdiv_frac_scale;
    uint8_t  clkdiv_frac = (uint8_t)(scaled % clkdiv_frac_scale);
    if (clkdiv_int > 65535) return false;
    if (clkdiv_int == 0 && clkdiv_frac == 0) return false;
    out_int = clkdiv_int; out_frac = clkdiv_frac;
    return true;
}


// A refused init that never got as far as claiming the block.
void expect_refused_clean(const PicoRS485 &ch, int rc, int want) {
    CHECK_EQ(rc, want);
    CHECK(!ch.is_valid());
    CHECK(g_claim == 0 && g_add_calls == 0);
    CHECK(g_enabled_n == 0 && g_disabled_n == 0);
    CHECK(g_clear_mem == 0 && g_unclaim == 0 && g_dma_claimed == 0);
    CHECK(g_tx.calls == 0 && g_rx.calls == 0 && g_rxidle.calls == 0 && g_txgap.calls == 0);
}


// A failed init that had claimed the block: everything handed back in order,
// memory re-emptied, no DMA taken.
void expect_refused_released(const PicoRS485 &ch, int rc, int want) {
    CHECK_EQ(rc, want);
    CHECK(!ch.is_valid());
    CHECK(g_claim == 4 && g_unclaim == 4);
    CHECK(saw_all_four(g_disabled, g_disabled_n));
    CHECK(g_clear_mem == 2);
    CHECK(g_dma_claimed == 0);
}


// Hands the receiver a burst: one word per sampled character, delivered the way
// the ISR's autopush would.
void feed(PIO pio, uint sm, uint isr_bits, std::initializer_list<uint32_t> words) {
    for (uint32_t w : words) deliver_rx(pio, sm, w, isr_bits);
}

