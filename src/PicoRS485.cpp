#include "PicoRS485.h"

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/timer.h"

#include "pico/platform.h"

#include "rs485.pio.h"

// pico-sdk 2.3 and later define PIO_IS_INSTANCE; earlier ones require a manual check
#ifndef PIO_IS_INSTANCE
#if NUM_PIOS > 2
#define PIO_IS_INSTANCE(pio) ((pio) == pio0 || (pio) == pio1 || (pio) == pio2)
#else
#define PIO_IS_INSTANCE(pio) ((pio) == pio0 || (pio) == pio1)
#endif
#endif

// =============================== STATIC STATE ================================
// The instance that owns each block, for the per-block handlers to look up.
template <size_t N>
PicoRS485_t<N> *PicoRS485_t<N>::instances_[NUM_PIOS] = {};

extern "C" __weak void pico_rs485_core_violation(void) {}

// ============================== INITIALISATION ===============================
template <size_t N>
int PicoRS485_t<N>::init(PIO pio, uint32_t baudrate, const pico_rs485_packet_config &pkt) {
    // Check if channel is already initialized
    if (pio_ != nullptr) return err_busy;

    // Compute clock config for the requested baudrate
    pico_rs485_clock_config clk{};
    if (!compute_clock_config(baudrate, clk)) return err_baudrate;

    // Initialize the driver
    return init(pio, clk, pkt);
}

template <size_t N>
int PicoRS485_t<N>::init(PIO pio, const pico_rs485_clock_config &clk, const pico_rs485_packet_config &pkt) {
    // One permit at most: a signal is a wake-up, and the count itself says whether there is a burst
    sem_init(&rx_sem_, 0, 1);
    // Check if channel is already initialized
    if (pio_ != nullptr) return err_busy;

    // Check the pin numbers are valid
    if (pin_tx_ >= NUM_BANK0_GPIOS || pin_rx_ >= NUM_BANK0_GPIOS ||
        pin_de_ >= NUM_BANK0_GPIOS || pin_rx_act_ >= NUM_BANK0_GPIOS) {
        return err_pin;
    }

    // Check that all pins are different
    if (pin_tx_ == pin_rx_ || pin_tx_ == pin_de_ || pin_tx_ == pin_rx_act_ ||
        pin_rx_ == pin_de_ || pin_rx_ == pin_rx_act_ || pin_de_ == pin_rx_act_) {
        return err_pin;
    }

    // Validate packet/clock configurations
    if (!is_packet_config_valid(pkt)) return err_packet;
    if (!is_clock_config_valid(clk)) return err_clock;

    // Check if the PIO can host the channel as currently wired
    const int pio_err = is_pio_valid(pio);
    if (pio_err != ok) return pio_err;

    // is_pio_valid() checked every machine is free, so claim all SMs
    for (uint sm = 0; sm < NUM_PIO_STATE_MACHINES; ++sm) {
        pio_sm_claim(pio, sm);
    }

    // Marks the channel live
    pio_ = pio;

    // PIO block is claimed, so clear instruction memory
    pio_clear_instruction_memory(pio_);

    // Add all programs
    int tx_offset    = pio_add_program(pio_, &rs485_tx_program);
    int rx_offset    = pio_add_program(pio_, &rs485_rx_program);
    int rxidle_offset = pio_add_program(pio_, &rs485_rxidle_program);
    int txgap_offset = pio_add_program(pio_, &rs485_txgap_program);

    // Verify that all programs are loaded successfully
    if (tx_offset < 0 || rx_offset < 0 || rxidle_offset < 0 || txgap_offset < 0) {
        release();
        return err_program_load;
    }

    // Only the receiver's offset outlives init(): rx_rewind() rewinds to it
    const uint off_tx     = (uint)tx_offset;
    const uint off_rxidle = (uint)rxidle_offset;
    const uint off_txgap  = (uint)txgap_offset;
    offset_rx_ = (uint)rx_offset;

    // Set the packet config (generates the character table and configures SMs)
    const int cfg_err = set_packet_config(pkt);
    if (cfg_err != ok) {
        release();
        return cfg_err;
    }

    pio_interrupt_clear(pio_, tx_done_irq);
    pio_interrupt_clear(pio_, rx_done_irq);
    pio_interrupt_clear(pio_, tx_char_irq);
    pio_interrupt_clear(pio_, tx_gap_irq);

    // Arm the receiver before any machine is enabled
    tx_dma_ = dma_claim_unused_channel(false);
    rx_dma_ = dma_claim_unused_channel(false);
    if (tx_dma_ < 0 || rx_dma_ < 0) {
        release();
        return err_no_dma;
    }
    arm_receive(rx_active_);

    // From here the helpers drive the pins through the PIO, so the channel owns them.
    // The framing is applied a second time, per machine: set_packet_config() wrote Y and the shift
    // threshold before the programs were loaded, and these write the pins and the dividers.
    pins_claimed_ = true;
    int rc = rs485_tx_program_init(pio_, sm_tx, off_tx, pin_tx_, pin_de_,
                                   clk.clkdiv_int, clk.clkdiv_frac8, char_bits_);
    if (rc == PICO_OK)
        rc = rs485_rx_program_init(pio_, sm_rx, offset_rx_, pin_rx_, pin_rx_act_,
                                   clk.clkdiv_int, clk.clkdiv_frac8, isr_bits_);
    if (rc == PICO_OK)
        rc = rs485_rxidle_program_init(pio_, sm_rxidle, off_rxidle, pin_rx_act_,
                                       clk.clkdiv_int, clk.clkdiv_frac8,
                                       rx_idle_steps_(pkt_.rx_idle_bits_thresh));
    if (rc == PICO_OK)
        rc = rs485_txgap_program_init(pio_, sm_txgap, off_txgap,
                                      clk.clkdiv_int, clk.clkdiv_frac8, pkt_.tx_gap_bits);
    if (rc != PICO_OK) {
        release();
        return err_sm_config;
    }

    // Start from a clean edge: a burst that began during init() had nowhere to land
    rx_rewind();

    // Route only the two completion flags; flags 4 and 5 are the transmitter's per-character handshake
    instances_[PIO_NUM(pio_)] = this;
    irq_set_exclusive_handler(PIO_IRQ_NUM(pio_, 0), handler_for_(PIO_NUM(pio_)));
    // Both completions, on the one line: flag 0 is the TX machine's, flag 1 the RX machine's
    pio_set_irq0_source_enabled(pio_, pis_interrupt0, true);
    pio_set_irq0_source_enabled(pio_, pis_interrupt1, true);
    irq_set_enabled(PIO_IRQ_NUM(pio_, 0), true);
    // Recorded before the handler is routed.
    core_ = get_core_num();
    irq_registered_ = true;

    // Start with none of this channel's flags set
    pio_interrupt_clear(pio_, tx_done_irq);
    pio_interrupt_clear(pio_, rx_done_irq);

    baudrate_ = clk.baudrate;

    return ok;
}

// ================================= TEARDOWN ==================================
template <size_t N>
PicoRS485_t<N>::~PicoRS485_t() {
    release();
}

template <size_t N>
void PicoRS485_t<N>::release() {
    if (pio_ == nullptr) return;

    // Unroute the line and give the DMA channels back first
    if (irq_registered_) {
        // The entry first: the handler looks the instance up by block
        instances_[PIO_NUM(pio_)] = nullptr;
        pio_set_irq0_source_enabled(pio_, pis_interrupt0, false);
        pio_set_irq0_source_enabled(pio_, pis_interrupt1, false);
        pio_interrupt_clear(pio_, tx_done_irq);
        pio_interrupt_clear(pio_, rx_done_irq);
        pio_interrupt_clear(pio_, tx_char_irq);
        pio_interrupt_clear(pio_, tx_gap_irq);
        irq_set_enabled(PIO_IRQ_NUM(pio_, 0), false);
        irq_remove_handler(PIO_IRQ_NUM(pio_, 0), handler_for_(PIO_NUM(pio_)));
        irq_registered_ = false;
    }
    if (rx_dma_ >= 0) {
        dma_channel_cleanup(rx_dma_);
        dma_channel_unclaim(rx_dma_);
        rx_dma_ = -1;
    }
    if (tx_dma_ >= 0) {
        dma_channel_cleanup(tx_dma_);
        dma_channel_unclaim(tx_dma_);
        tx_dma_ = -1;
    }
    // A stale count would make the next init() refuse as in-flight
    rx_ready_count_ = 0;
    sem_reset(&rx_sem_, 0);

    // An unreported completion is dropped.
    tx_pending_  = false;
    tx_callback_ = nullptr;
    tx_user_     = nullptr;

    // Disable the machines, then set DE low through the PIO
    for (uint sm = 0; sm < NUM_PIO_STATE_MACHINES; ++sm) {
        pio_sm_set_enabled(pio_, sm, false);
    }

    // The 64-bit mask form is the only one that can express a pin past 31
    pio_sm_set_pins_with_mask64(pio_, sm_tx, 0u, 1ull << pin_de_);

    pio_clear_instruction_memory(pio_);
    for (uint sm = 0; sm < NUM_PIO_STATE_MACHINES; ++sm) {
        pio_sm_unclaim(pio_, sm);
    }

    if (pins_claimed_) {
        // Reset only the pins the channel took over
        gpio_disable_pulls(pin_rx_);
        gpio_init(pin_tx_);
        gpio_init(pin_rx_act_);
        gpio_init(pin_rx_);

        // DE is left low: driver disabled
        gpio_put(pin_de_, false);
        gpio_set_dir(pin_de_, GPIO_OUT);
        gpio_set_function(pin_de_, GPIO_FUNC_SIO);
        pins_claimed_ = false;
    }

    pio_ = nullptr;
}

// =========================== PACKET CONFIGURATION ============================
template <size_t N>
int PicoRS485_t<N>::set_packet_config(const pico_rs485_packet_config &pkt) {
    if (pio_ == nullptr) return err_not_initialized;
    if (!is_core_ok_()) return err_wrong_core;
    if (!is_packet_config_valid(pkt)) return err_packet;

    const uint irq = PIO_IRQ_NUM(pio_, 0);
    const bool irq_was_enabled = irq_is_enabled(irq);
    irq_set_enabled(irq, false);
    if (in_flight()) {
        irq_set_enabled(irq, irq_was_enabled);
        return err_in_flight;
    }

    // Record the new config to the class
    pkt_        = pkt;
    char_bits_ = 1 + pkt.data_bits + pkt.parity_bits + pkt.stop_bits;
    isr_bits_   = pkt.data_bits + pkt.parity_bits + 1;

    // One framed FIFO word per data byte
    const uint32_t data_mask = (1u << pkt.data_bits) - 1u;
    for (uint32_t byte = 0; byte < 256; ++byte) {
        uint32_t word = (byte & data_mask) << 1;
        if (pkt.parity_bits != 0) {
            uint32_t parity = (uint32_t)__builtin_parity(byte & data_mask);
            if (!pkt.even_parity) parity ^= 1u;
            word |= parity << (1u + pkt.data_bits);
        }
        for (uint32_t stop = 0; stop < pkt.stop_bits; ++stop) {
            word |= 1u << (1u + pkt.data_bits + pkt.parity_bits + stop);
        }
        char_table_[byte] = word;
    }

    // Update the state machines with the new Y values
    rs485_tx_set_char_bits(pio_, sm_tx, char_bits_);
    rs485_rx_set_isr_bits(pio_, sm_rx, isr_bits_);
    rs485_rxidle_set_steps(pio_, sm_rxidle, rx_idle_steps_(pkt_.rx_idle_bits_thresh));
    rs485_txgap_set_gap_bits(pio_, sm_txgap, pkt_.tx_gap_bits);

    irq_set_enabled(irq, irq_was_enabled);
    return ok;
}

// =========================== CHECKS AND DERIVATION ===========================
template <size_t N>
bool PicoRS485_t<N>::is_core_ok_() const {
    // Check if the call site is on the core init() ran on
    return !irq_registered_ || get_core_num() == core_;
}

template <size_t N>
bool PicoRS485_t<N>::compute_clock_config(uint32_t baudrate, pico_rs485_clock_config &out) {
    return compute_clock_config(baudrate, clock_get_hz(clk_sys), out);
}

template <size_t N>
int PicoRS485_t<N>::is_pio_valid(PIO pio) const {
    // Check if the PIO exists and is a valid instance
    if (pio == nullptr || !PIO_IS_INSTANCE(pio)) return err_pio;

#if PICO_PIO_USE_GPIO_BASE
    // Check that specified pins are in range for the selected PIO
    uint base = pio_get_gpio_base(pio);
    if (pin_tx_ < base || pin_tx_ >= base + 32 ||
        pin_rx_ < base || pin_rx_ >= base + 32 ||
        pin_de_ < base || pin_de_ >= base + 32 ||
        pin_rx_act_ < base || pin_rx_act_ >= base + 32) {
        return err_gpio_base;
    }
#endif

    // The channel takes all four machines
    for (uint sm = 0; sm < NUM_PIO_STATE_MACHINES; ++sm) {
        if (pio_sm_is_claimed(pio, sm)) return err_state_machine;
    }

    // init() takes the line exclusively, and the SDK hard-asserts on a conflict rather than
    // reporting one. irq_has_handler() is true for an exclusive handler and a shared chain alike.
    if (irq_has_handler(PIO_IRQ_NUM(pio, 0))) return err_irq_in_use;

    return ok;
}

// ================================= TRANSMIT ==================================
template <size_t N>
int PicoRS485_t<N>::send(const uint8_t *data, size_t len, pico_rs485_sent_callback cb, void *user) {
    // Returns if the channel is not initialized
    if (pio_ == nullptr) return err_not_initialized;
    // Returns if the call is not on the core the channel belongs to
    if (!is_core_ok_()) return err_wrong_core;
    // Returns if the data pointer is null, or if the length is invalid
    if (data == nullptr || len == 0 || len > max_burst_bytes) return err_argument;
    // Return if a previous callback is incomplete
    if (tx_completion_pending()) return err_incomplete_tx;
    // Refuse while the channel is active.
    if (is_transmitting() || is_receiving()) return err_in_flight;

    // Fill the transmit scratch from the character table
    for (size_t i = 0; i < len; ++i) {
        tx_scratch_[i] = char_table_[data[i]];
    }

    // Mark the last word, so the transmitter releases DE after it.
    tx_scratch_[len - 1] |= 1u << char_bits_;

    // Record transmit state before the DMA is started: the burst's end can raise the flag before
    // this function returns, and the handler must find something owed when it does
    tx_pending_  = true;
    tx_callback_ = cb;
    tx_user_     = user;
    ++tx_burst_id_;

    // Configure the DMA channel to the TX FIFO and enable it
    dma_channel_config c = dma_channel_get_default_config(tx_dma_);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, PIO_DREQ_NUM(pio_, sm_tx, true));
    dma_channel_configure(tx_dma_, &c, &pio_->txf[sm_tx], tx_scratch_, len, true);
    return ok;
}

// ================================== RECEIVE ==================================
template <size_t N>
int PicoRS485_t<N>::receive(uint8_t *out, size_t max) { return receive_timed(out, max, 0u, false); }

template <size_t N>
int PicoRS485_t<N>::receive_for(uint8_t *out, size_t max, uint32_t timeout_us) {
    return receive_timed(out, max, timeout_us, true);
}

template <size_t N>
int PicoRS485_t<N>::receive_timed(uint8_t *out, size_t max, uint32_t timeout_us, bool bounded) {
    // Returns if the channel is not initialized
    if (pio_ == nullptr) return err_not_initialized;
    // Returns if the call is not on the core the channel belongs to
    if (!is_core_ok_()) return err_wrong_core;
    // Returns if the out buffer is invalid
    if (out == nullptr && max != 0) return err_argument;

    // An unbounded wait never reads the clock
    const uint32_t start = bounded ? time_us_32() : 0u;
    while (rx_ready_count_ == 0) {
        if (bounded) {
            const uint32_t spent = (uint32_t)(time_us_32() - start);
            if (spent >= timeout_us) return err_timeout;
#if !PICO_TIME_DEFAULT_ALARM_POOL_DISABLED
            // Sleep until the handler signals a burst, or the remaining time runs out
            if (!sem_acquire_timeout_us(&rx_sem_, timeout_us - spent)) return err_timeout;
#else
            tight_loop_contents();
#endif
        } else {
            // Sleep until the handler signals; a spurious permit just re-runs the test above
            sem_acquire_blocking(&rx_sem_);
        }
    }
    return take_burst(out, max);
}

template <size_t N>
int PicoRS485_t<N>::take_burst(uint8_t *out, size_t max) {
    // Mask this line while the queue and the buffer being read are in flux; restore it as found
    const uint irq = PIO_IRQ_NUM(pio_, 0);
    const bool irq_was_enabled = irq_is_enabled(irq);
    irq_set_enabled(irq, false);

    const uint8_t queued = rx_ready_count_;
    const uint8_t buffer = queued != 0 ? rx_ready_[0] : 0;
    if (queued == 0 || buffer >= rx_buffer_count) {
        // Nothing queued, or an entry the handler cannot have written
        irq_set_enabled(irq, irq_was_enabled);
        return 0;
    }
    __compiler_memory_barrier();   // the entry is read before what it publishes
    const uint16_t chars  = rx_length_[buffer];
    --rx_ready_count_;
    if (rx_ready_count_ != 0) rx_ready_[0] = rx_ready_[1];

    // Sampled bits are left-justified: bit 0 is the first data bit, the stop bit sits above
    // data + parity. Read out of the members first: out[] may alias this object
    const uint32_t data_bits   = pkt_.data_bits;
    const uint32_t parity_bits = pkt_.parity_bits;
    const uint32_t shift       = 32u - isr_bits_;   // isr_bits_ is in [6, 10] here
    const uint32_t data_mask   = (1u << data_bits) - 1u;
    const uint32_t parity_mask = 1u << data_bits;
    const uint32_t stop_mask   = 1u << (data_bits + parity_bits);
    const bool     want_parity = parity_bits != 0;
    const uint32_t *const table = char_table_;
    // Written by the RX channel, so read through a volatile view
    const volatile uint32_t *const staged = rx_staging_[buffer];
    uint32_t framing = 0, parity = 0;
    for (uint16_t i = 0; i < chars; ++i) {
        const uint32_t bits = staged[i] >> shift;
        if ((bits & stop_mask) == 0) ++framing;
        if (want_parity) {
            // The expected parity sits at 1 + data_bits in this byte's table entry.
            const uint32_t expected = (table[bits & data_mask] >> (1u + data_bits)) & 1u;
            if (((bits & parity_mask) != 0) != (expected != 0)) ++parity;
        }
        if (i < max) out[i] = (uint8_t)(bits & data_mask);
    }
    framing_errors_ += framing;
    parity_errors_  += parity;

    irq_set_enabled(irq, irq_was_enabled);
    return (int)chars;
}

template <size_t N>
bool PicoRS485_t<N>::is_queued_at(uint8_t buffer, uint8_t count) const {
    // The count is bounded here as well: it is written by the handler
    for (uint8_t i = 0; i < count && i < rx_queue_depth; ++i) {
        if (rx_ready_[i] == buffer) return true;
    }
    return false;
}

template <size_t N>
void PicoRS485_t<N>::arm_receive(uint8_t buffer) {
    // Precondition: the channel is stopped at both call sites
    rx_active_ = buffer;
    rx_next_free_ = rx_next_(buffer);   // where the next search for a free buffer starts
    dma_channel_config c = dma_channel_get_default_config(rx_dma_);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, PIO_DREQ_NUM(pio_, sm_rx, false));
    // One count more than a maximum burst, so exactly-full stays distinguishable from over-long
    dma_channel_configure(rx_dma_, &c, rx_staging_[buffer], &pio_->rxf[sm_rx],
                          max_burst_bytes + 1, true);
    // What is_receiving() compares the write pointer against, written after the arm so the two
    // never disagree in the direction that reads as idle
    rx_armed_base_ = (uint32_t)(uintptr_t)rx_staging_[buffer];
}

template <size_t N>
void PicoRS485_t<N>::rx_rewind() {
    // Empty the FIFO first: a push stalled on a full FIFO is re-counted by the restarted
    // machine, which leaves the sample phase one bit out for every burst that follows.
    // A released push refills the FIFO, so flush twice.
    pio_sm_clear_fifos(pio_, sm_rx);
    if (!pio_sm_is_rx_fifo_empty(pio_, sm_rx)) pio_sm_clear_fifos(pio_, sm_rx);
    // Restart, then rewind: the restart does not move the program counter
    pio_sm_restart(pio_, sm_rx);
    pio_sm_exec(pio_, sm_rx, pio_encode_jmp(offset_rx_));
}

// ============================== INTERRUPT PATH ===============================
// One out-of-line copy, shared by every block's thunk
template <size_t N>
__attribute__((noinline))
void PicoRS485_t<N>::service_irq_() {
    // Each flag is cleared before its work runs: left set, the level-triggered line would re-enter
    if (pio_interrupt_get(pio_, rx_done_irq)) {
        pio_interrupt_clear(pio_, rx_done_irq);
        rx_done_isr();
    }
    if (pio_interrupt_get(pio_, tx_done_irq)) {
        pio_interrupt_clear(pio_, tx_done_irq);
        tx_done_isr();
    }
}

template <size_t N>
void PicoRS485_t<N>::rx_done_isr() {
    // A channel init() has not claimed must not be touched
    if (rx_dma_ < 0) return;

    // Stop the channel, then read its write pointer for the burst length
    dma_channel_cleanup(rx_dma_);

    const uint8_t active = rx_active_;
    const uint32_t base = (uint32_t)(uintptr_t)rx_staging_[active];
    // RP2040-E13: the abort's BUSY wait can return before an in-flight write retires, so read the
    // pointer until it stops moving
    uint32_t end = dma_channel_hw_addr(rx_dma_)->write_addr;
    for (int again_i = 0; again_i < 4; ++again_i) {
        const uint32_t again = dma_channel_hw_addr(rx_dma_)->write_addr;
        if (again == end) break;
        end = again;
    }
    const uint32_t chars = (end >= base) ? (end - base) / sizeof(uint32_t) : 0u;

    // A character still in the FIFO means the DMA fell behind: a stalled push, or the residue of a
    // transfer that retired anyway. Either way the burst is not trustworthy.
    const bool dma_fell_behind = !pio_sm_is_rx_fifo_empty(pio_, sm_rx);

    if (dma_fell_behind || chars > max_burst_bytes) {
        ++dropped_bursts_;
    } else if (chars != 0) {
        // The length is written before the entry that publishes it
        rx_length_[active] = (uint16_t)chars;
        __compiler_memory_barrier();
        if (rx_ready_count_ >= rx_queue_depth) {  // make room by dropping the oldest
            rx_ready_[0] = rx_ready_[1];
            --rx_ready_count_;
            ++dropped_bursts_;
        }
        rx_ready_[rx_ready_count_++] = active;
        sem_release(&rx_sem_);   // wake a receive() that is waiting for this burst
    } else {
        // A flag with nothing behind it.
        ++empty_bursts_;
    }

    // Where the last arm left off, stepped past anything queued: at most rx_queue_depth buffers can
    // be queued, so that many steps reach a free one without a full scan
    const uint8_t count = rx_ready_count_;
    uint8_t next = rx_next_free_;
    for (uint8_t tries = 0; tries < rx_queue_depth && is_queued_at(next, count); ++tries) {
        next = rx_next_(next);
    }
    if (is_queued_at(next, count)) {
        // Unreachable by the arithmetic above; free the oldest entry if it ever happens
        next = rx_ready_[0];
        rx_ready_[0] = rx_ready_[1];
        --rx_ready_count_;
        ++dropped_bursts_;
    }

    // A burst that ran too long, or one the DMA did not finish, left the receiver mid-character
    if (dma_fell_behind || chars > max_burst_bytes) rx_rewind();
    if (next < rx_buffer_count) arm_receive(next);
}

template <size_t N>
void PicoRS485_t<N>::tx_done_isr() {
    // A report already made, or a flag with nothing owed behind it
    if (!tx_pending_) return;

    tx_pending_      = false;
    last_sent_at_us_ = time_us_32();

    // Store the callback and user pointer before clearing them
    const pico_rs485_sent_callback cb = tx_callback_;
    void *const user = tx_user_;
    const uint32_t id = tx_burst_id_;
    tx_callback_ = nullptr;
    tx_user_ = nullptr;
    if (cb != nullptr) cb(user, id);
}

template <size_t N>
void (*PicoRS485_t<N>::handler_for_(uint index))() {
    switch (index) {
        case 0:  return &PicoRS485_t<N>::template irq_handler_at_<0>;
        case 1:  return &PicoRS485_t<N>::template irq_handler_at_<1>;
#if NUM_PIOS > 2
        case 2:  return &PicoRS485_t<N>::template irq_handler_at_<2>; // RP2350 has three blocks
#endif
        default: return nullptr;
    }
}

// ============================= STATE AND TIMING ==============================
template <size_t N>
bool PicoRS485_t<N>::is_transmitting() const {
    if (!is_core_ok_()) {
        pico_rs485_core_violation();
        return true;   // busy, so a caller acts on nothing
    }
    // One latch, set by send() and cleared by the end-of-burst interrupt: no window, unlike the
    // three hardware terms it replaces
    return tx_pending_;
}

template <size_t N>
bool PicoRS485_t<N>::is_receiving() const {
    if (!is_core_ok_()) {
        pico_rs485_core_violation();
        return true;   // busy, so a caller acts on nothing
    }
    // Returns false if the channel is not initialized or if the receiver is idle
    if (pio_ == nullptr || rx_dma_ < 0) return false;

    // rx_act = 1 -> the RX PIO is receiving data
    if (irq_registered_ && gpio_get(pin_rx_act_)) return true;

    // A word has landed since the last arming: the write pointer has left the base the channel
    // was armed with. One member read and one register read, and no interrupt mask
    return dma_channel_hw_addr(rx_dma_)->write_addr != rx_armed_base_;
}

template <size_t N>
uint32_t PicoRS485_t<N>::burst_duration_us(size_t len) const {
    if (pio_ == nullptr || baudrate_ == 0 || len == 0) return 0;
    // Three bit times per character of program overhead: DE lead-in and hold, the character-complete flag
    // and the pull that starts the next character
    constexpr uint32_t pio_overhead_bits = 3;
    const uint64_t bytes = len > 0xffffffffu ? 0xffffffffu : (uint64_t)len;
    const uint64_t bits  = bytes * (char_bits_ + pkt_.tx_gap_bits + pio_overhead_bits);
    const uint64_t us = (bits * 1000000u + baudrate_ - 1) / baudrate_;
    return us > 0xffffffffu ? 0xffffffffu : (uint32_t)us;
}

// ========================== EXPORTED INSTANTIATIONS ==========================
// The instantiations the library exports. CMake writes its own copy of this list from
// PICO_RS485_MAX_PACKET_BYTES and PICO_RS485_BURST_SIZES; without it, the checked-in default
// exports the configured maximum burst length alone.
#if __has_include(<pico_rs485_instantiations.h>)
#include <pico_rs485_instantiations.h>
#else
#include "rs485_instantiations_default.h"
#endif
