/**
 * @file PicoRS485.h
 * @brief RS-485 half-duplex channel driven by four PIO state machines.
 */

#ifndef PICO_RS485_H
#define PICO_RS485_H

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "hardware/pio.h"
#include "pico/sem.h"

// ============================== BUILD SETTINGS ===============================

/**
 * @brief Largest burst the driver can send or receive, in bytes.
 */
#ifndef PICO_RS485_MAX_PACKET_BYTES
#define PICO_RS485_MAX_PACKET_BYTES 256
#endif

/**
 * @brief Whether to place the end-of-burst interrupt handlers in flash (0, default) or RAM (1)
 */
#ifndef PICO_RS485_ISR_IN_RAM
#define PICO_RS485_ISR_IN_RAM 0
#endif

#if PICO_RS485_ISR_IN_RAM
#include "pico/platform.h"   // __not_in_flash_func
#endif

// ============================== TYPES AND HOOKS ==============================

/**
 * @brief Character framing and the timing windows around a burst.
 *
 * tx_gap_bits is the transmission gap between the characters of a burst.
 * Must be less than rx_idle_bits_thresh by at least (stop_bits + 3).
 *
 * rx_idle_bits_thresh is the line silence the receiver takes as the end of a
 * burst, in bit times. Must be at least (tx_gap_bits + stop_bits + 3).
 *
 * Values are ordered: data, parity, stop, gap, threshold, parity mode.
 */
struct pico_rs485_packet_config {
    uint8_t data_bits    = 8;   ///< Data bits per character (5-8)
    uint8_t parity_bits  = 0;   ///< Parity bits per character (0-1)
    uint8_t stop_bits    = 1;   ///< Stop bits per character (1-2)
    uint8_t tx_gap_bits  = 8;   ///< Bit times left between each TX character (1-31)
    /// Bit times of line silence that end a receive burst: 1-64, rounded up to a
    /// step of rx_idle_bits_per_step when the driver loads it into the PIO.
    uint8_t rx_idle_bits_thresh = 16;
    bool even_parity = true;    ///< Even parity when true, odd when false (ignored if parity_bits is 0)
};

/**
 * @brief Passed to send(), called from the end-of-burst interrupt
 *        once the burst has left the wire.
 *
 * @param user     The pointer given to send()
 * @param burst_id The burst that finished (IDs count up from 1)
 */
using pico_rs485_sent_callback = void (*)(void *user, uint32_t burst_id);

/**
 * @brief PIO clock divider for one channel, plus the values it came from.
 *
 * Only the divider is used by PIO state machines, but all fields must be
 * filled for the config to be valid.
 */
struct pico_rs485_clock_config {
    uint32_t sys_hz       = 0;  ///< clk_sys the divider was derived from.
    uint32_t baudrate     = 0;  ///< Requested bit rate.
    uint16_t clkdiv_int   = 0;  ///< Whole part of the divider.
    uint8_t  clkdiv_frac8 = 0;  ///< Fractional part, in 1/256 of a divider step.
};

/**
 * @brief Called when a driver function is called from a core other than the one init() ran on.
 *
 * Used by functions that cannot return an error code on their own.
 *
 * Empty by default, but a host may implement it to act on a violation.
 */
extern "C" void pico_rs485_core_violation(void);

// ========================= PicoRS485 CLASS TEMPLATE ==========================

/**
 * @brief Half-duplex RS-485 channel class template; use PicoRS485().
 */
template <size_t MaxBurstBytes>
class PicoRS485_t {
public:

    // ================================ LIMITS =================================
    static constexpr uint8_t data_bits_min = 5;            ///< Fewest data bits per character.
    static constexpr uint8_t data_bits_max = 8;            ///< Most data bits per character.
    static constexpr uint8_t parity_bits_max = 1;          ///< Most parity bits per character.
    static constexpr uint8_t stop_bits_min = 1;            ///< Fewest stop bits per character.
    static constexpr uint8_t stop_bits_max = 2;            ///< Most stop bits per character.
    static constexpr uint8_t tx_gap_bits_min = 1;          ///< Fewest gap bits; zero would wrap the counter.
    static constexpr uint8_t tx_gap_bits_max = 31;         ///< Most gap bits the counter holds.
    static constexpr uint8_t rx_idle_bits_thresh_min = 1;  ///< Fewest threshold bits; zero would wrap.
    static constexpr uint8_t rx_idle_bits_thresh_max = 64; ///< Most the idle timer can hold, in steps of 2.
    static constexpr uint8_t rx_idle_bits_per_step = 2;    ///< Bit times per step the idle timer counts in.
    static constexpr uint8_t rx_idle_margin_bits = 3;      ///< Bit times the threshold must clear gap + stop by.

    /// @brief Default bit rate, in bits per second.
    static constexpr uint32_t default_baudrate = 921600;

    /// @brief PIO cycles corresponding to one bit time
    static constexpr uint32_t pio_cycles_per_bit = 8;
    /// @brief Fractional steps in the PIO divider
    static constexpr uint32_t clkdiv_frac_scale = 256;

    /// @brief Largest burst this instance can send or receive, in bytes.
    static constexpr size_t max_burst_bytes = MaxBurstBytes;
    static_assert(max_burst_bytes >= 1,
                  "a burst holds at least one byte; MaxBurstBytes is a build setting");
    static_assert(max_burst_bytes <= 65535,
                  "staged burst lengths are held in a uint16_t");

    // ============================== ERROR CODES ==============================
    /**
     * @brief Error return values.
     */
    enum error : int {
        ok                  = 0,   ///< Success.
        err_pin             = -1,  ///< A pin is outside the GPIO range or overlapping another.
        err_packet          = -2,  ///< Framing the PIO programs cannot express.
        err_baudrate        = -3,  ///< clk_sys cannot produce that bit time.
        err_clock           = -4,  ///< Supplied pico_rs485_clock_config is unusable.
        err_wrong_core      = -5,  ///< Called from a core other than the one init() ran on.
        err_state_machine   = -6,  ///< Not every machine the channel needs is free.
        err_gpio_base       = -7,  ///< A pin is outside the window the PIO's GPIO base selects.
        err_program_load    = -8,  ///< pio_add_program() refused a program.
        err_busy            = -9,  ///< Already initialized.
        err_pio             = -10, ///< Not a usable PIO instance.
        err_sm_config       = -11, ///< The SDK refused a state machine configuration.
        err_not_initialized = -12, ///< Called on a channel that is not live.
        err_in_flight       = -13, ///< A burst is being sent or received.
        err_no_dma          = -14, ///< No DMA channel was available.
        err_timeout         = -15, ///< No burst arrived in time.
        err_incomplete_tx   = -16, ///< A burst was sent but not yet reported by the end-of-burst interrupt.
        err_argument        = -17, ///< A pointer or a length the call cannot work with.
        err_irq_in_use      = -18, ///< The PIO's IRQ 0 line belongs to something else.
    };

    // =============================== LIFECYCLE ===============================
    /**
     * @brief Records where the channel is wired.
     *
     * @param pin_tx     GPIO the transmitter drives
     * @param pin_rx     GPIO the received data is read from
     * @param pin_de     GPIO driving the transceiver's DE and /RE
     * @param pin_rx_act GPIO the receiver raises while RX is active
     */
    PicoRS485_t(uint pin_tx, uint pin_rx, uint pin_de, uint pin_rx_act)
        : pin_tx_(pin_tx), pin_rx_(pin_rx), pin_de_(pin_de), pin_rx_act_(pin_rx_act) {}

    /**
     * @brief Releases the PIO, its state machines and the instruction memory.
     *
     * Safe on an object that was never initialized or whose init() failed.
     */
    ~PicoRS485_t();

    /// @brief Deleted: a channel owns a PIO, so it is not copyable.
    PicoRS485_t(const PicoRS485_t &)            = delete;
    PicoRS485_t &operator=(const PicoRS485_t &) = delete;

    /// @brief Deleted: a channel owns a PIO, so it is not movable.
    PicoRS485_t(PicoRS485_t &&)            = delete;
    PicoRS485_t &operator=(PicoRS485_t &&) = delete;

    /**
     * @brief True once init() has succeeded.
     *
     * @return false before init() has run, or after a failed init().
     */
    bool is_valid() const { return pio_ != nullptr; }

    /**
     * @brief Claims the channel on the given PIO, derives the divider, and
     * configures the four state machines, for a specified baud rate.
     *
     * Everything claimed is released on failure, so the call can be retried.
     *
     * @note Takes over the PIO's IRQ 0 line and empties the block's instruction
     *       memory, since the channel owns the whole block, and claims two DMA
     *       channels.
     *
     * @param pio      PIO block to host the channel
     * @param baudrate Wire baud rate
     * @param pkt      Framing and timing windows configuration
     * @return ok, or a negative error code
     */
    [[nodiscard]] int init(PIO pio, uint32_t baudrate = default_baudrate,
                           const pico_rs485_packet_config &pkt = pico_rs485_packet_config{});

    /**
     * @brief Claims the channel on the given PIO and configures the four state
     * machines, for a specified PIO clock divider.
     *
     * @param pio PIO block to host the channel
     * @param clk Divider to program the PIO clock
     * @param pkt Framing and timing windows configuration
     * @return ok, or a negative error code
     */
    [[nodiscard]] int init(PIO pio, const pico_rs485_clock_config &clk,
                           const pico_rs485_packet_config &pkt = pico_rs485_packet_config{});

    // ========================= PACKET CONFIGURATION ==========================
    /**
     * @brief Applies a packet config to the PicoRS485 instance.
     *
     * Call only when both RX and TX are idle; will otherwise fail.
     *
     * Only place where packet config can be written; reconfig should
     * ALWAYS be routed through this function.
     *
     * @param pkt Framing and timing windows configuration to apply
     * @return ok, or a negative error code
     */
    [[nodiscard]] int set_packet_config(const pico_rs485_packet_config &pkt);

    /**
     * @brief The current framing and timing windows configuration.
     *
     * @return The current packet configuration, or the default if not initialized
     */
    pico_rs485_packet_config get_packet_config() const { return pkt_; }

    // =============================== TRANSMIT ================================
    /**
     * @brief Frames @p data as one burst and queues it for transmission.
     *
     * Returns as soon as the burst is queued; all transmission is handled
     * asynchronously by the PIO and DMA. No other send() is accepted until
     * the end-of-burst interrupt has reported it.
     *
     * @param data Pointer to the data to send
     * @param len  Number of bytes (at most max_burst_bytes)
     * @param cb   Called once the burst has finished sending
     * @param user Caller's pointer provided to @p cb
     * @return ok, or a negative error code
     */
    [[nodiscard]] int send(const uint8_t *data, size_t len, pico_rs485_sent_callback cb = nullptr,
                           void *user = nullptr);

    /**
     * @brief True if a burst that owes a callback has not been reported yet.
     *
     * @return true from the send() that passed a callback until the end-of-burst interrupt reports it.
     *
     */
    bool tx_completion_pending() const {
        return tx_pending_ && tx_callback_ != nullptr;
    }

    /**
     * @brief When a finished burst was seen to have gone out.
     *
     * @return microseconds from time_us_32(), taken by the handler at the end-of-burst interrupt,
     *         or 0 if no burst has finished yet. The flag it answers is raised with the DE
     *         drop, so this is never earlier than the drop.
     */
    uint32_t last_sent_at_us() const { return last_sent_at_us_; }

    /**
     * @brief True while a queued burst has not been reported yet.
     *
     * @return true from the send() that queued the burst until the end-of-burst interrupt
     *         reports it, with no interval in between.
     */
    bool is_transmitting() const;

    // ================================ RECEIVE ================================
    /**
     * @brief Waits for the next received burst and unpacks it.
     *
     * Waits without a bound, which is what a slave needs: it must not give up
     * while the master is quiet. The wait sleeps until the handler publishes a burst,
     * so it blocks and must not be called from an interrupt.
     *
     * @param out Receives the data bytes of the burst; must not be null unless
     *            @p max is 0, which is the way to ask for a burst's length alone.
     * @param max Capacity of @p out.
     * @return the number of bytes in the burst, or a negative error code.
     */
    [[nodiscard]] int receive(uint8_t *out, size_t max);

    /**
     * @brief As receive(), but gives up after @p timeout_us.
     *
     * @param out        Receives the data bytes of the burst; must not be null
     *                   unless @p max is 0.
     * @param max        Capacity of @p out.
     * @param timeout_us How long to wait, in microseconds. Zero polls: it returns
     *                   err_timeout immediately when no burst is waiting. A non-zero
     *                   wait sleeps, and needs the SDK's default alarm pool; without it
     *                   the wait spins instead.
     * @return the number of bytes in the burst, or a negative error code:
     *         err_wrong_core, err_not_initialized, err_argument if @p out is null
     *         with a non-zero @p max, or err_timeout.
     */
    [[nodiscard]] int receive_for(uint8_t *out, size_t max, uint32_t timeout_us);

    /**
     * @brief Takes a burst if one is waiting, and returns at once if not.
     *
     * @param out Receives the data bytes of the burst; may be null when @p max is 0.
     * @param max Capacity of @p out; 0 asks for the burst's length alone.
     * @return as receive_for() with a zero timeout.
     */
    [[nodiscard]] int try_receive(uint8_t *out, size_t max) {
        return receive_for(out, max, 0u);
    }

    /**
     * @brief True when a completed burst is waiting to be unpacked.
     *
     * Poll this from a loop that has other work, and call receive() only when it
     * says so: receive() then returns without waiting.
     *
     * @return true while a burst sits in the completed-burst queue.
     */
    bool has_burst() const {
        if (!is_core_ok_()) {
            pico_rs485_core_violation();
            return true;   // the read that follows reports the wrong core
        }
        return rx_ready_count_ != 0;
    }

    /**
     * @brief True while a character is arriving or a received burst is in flight.
     *
     * @return true from the middle of a character's start bit, through the idle window that
     *         follows it, until the handler has published the burst and armed the next buffer.
     */
    bool is_receiving() const;

    // =========================== STATE AND TIMING ============================
    /**
     * @brief True while this channel is sending, receiving or holding a queued burst.
     *
     * What set_packet_config() requires to be clear before it will reconfigure.
     *
     * @return true while a burst owes a report or a character is arriving, or a burst waits to be unpacked.
     */
    bool in_flight() const {
        // The two it calls make this check once the channel is live.
        return is_transmitting() || is_receiving() || rx_ready_count_ != 0;
    }

    /**
     * @brief An upper bound on how long a burst of @p len bytes occupies the wire.
     *
     * Every character is charged its framing bits, the gap that follows it and three bit times of
     * program overhead, including the last character, which is followed by no gap. Deliberately
     * high rather than a measurement.
     *
     * @param len Number of bytes in the burst.
     * @return microseconds, rounded up, saturating at UINT32_MAX if a very long
     *         burst at a very slow rate would not fit.
     */
    uint32_t burst_duration_us(size_t len) const;

    // ============================ ERROR COUNTERS =============================
    /**
     * @brief Characters received with a low stop bit since construction.
     * @return the count, which only grows and is not reset by init() or release().
     *         Only characters of bursts the caller unpacks are counted, so a
     *         dropped burst's are not.
     */
    uint32_t framing_error_count() const { return framing_errors_; }
    /**
     * @brief Characters received whose parity bit did not match the mode.
     * @return the count, which only grows and is not reset; always 0 without a
     *         parity bit, and counted on unpack only, as framing errors are.
     */
    uint32_t parity_error_count() const { return parity_errors_; }
    /**
     * @brief Bursts discarded: the queue was full, the burst was too long, or the
     *        DMA did not finish it.
     * @return the count, which only grows and is not reset by init() or release().
     */
    uint32_t dropped_burst_count() const { return dropped_bursts_; }
    /**
     * @brief Idle flags serviced that had no received characters behind them.
     *
     * A burst that ended with nothing in the staging buffer: the timer fired without a
     * character having been collected. Counted so that it can be told apart from a burst
     * that simply never arrived, which otherwise looks identical from the outside.
     * @return the count, which only grows and is not reset by init() or release().
     */
    uint32_t empty_burst_count() const { return empty_bursts_; }

    // ========================= CHECKS AND DERIVATION =========================
    /**
     * @brief Derives the divider for a bit rate from an explicit clk_sys.
     *
     * constexpr, so a caller can pin its own divider at compile time.
     *
     * @param baudrate Bit rate to derive the divider for.
     * @param sys_hz   clk_sys to derive against.
     * @param out      Receives the divider and the values it came from.
     * @return true if the rate is representable; false leaves @p out untouched.
     */
    [[nodiscard]] static constexpr bool compute_clock_config(uint32_t baudrate, uint32_t sys_hz,
                                                             pico_rs485_clock_config &out) {
        // Refuse a zero rate or anything that would need a divider below 1.0
        if (baudrate == 0 || (uint64_t)baudrate * pio_cycles_per_bit > sys_hz) return false;

        // Nearest integer + fractional divider; 64-bit since large sys_hz over small baudrate overflows 32 bits
        const uint64_t denom  = (uint64_t)baudrate * pio_cycles_per_bit;
        const uint64_t scaled = ((uint64_t)sys_hz * clkdiv_frac_scale + denom / 2) / denom;
        if (scaled / clkdiv_frac_scale > 65535u) return false;
        const uint32_t clkdiv_int  = (uint32_t)(scaled / clkdiv_frac_scale);
        const uint8_t  clkdiv_frac = (uint8_t)(scaled % clkdiv_frac_scale);

        out.sys_hz       = sys_hz;
        out.baudrate     = baudrate;
        out.clkdiv_int   = (uint16_t)clkdiv_int;
        out.clkdiv_frac8 = clkdiv_frac;
        return true;
    }

    /**
     * @brief Derives the divider for a bit rate from the current clk_sys.
     *
     * Defined in the library rather than here: it is the one overload that cannot be constexpr,
     * because it reads the running clock, and keeping it out keeps a clocks include out of this
     * header.
     *
     * @param baudrate Bit rate to derive the divider for.
     * @param out      Receives the divider and the values it came from.
     * @return true if the rate is representable; false leaves @p out untouched.
     */
    [[nodiscard]] static bool compute_clock_config(uint32_t baudrate, pico_rs485_clock_config &out);

    /**
     * @brief True if a divider follows from the sys_hz and baudrate it carries.
     *
     * constexpr, so a caller can pin its own divider at compile time.
     *
     * @param clk Divider to check, with the values it was derived from.
     * @return false if the divider cannot be programmed, or if recomputing it from
     *         @c sys_hz and @c baudrate gives a different one - a divider that does
     *         not follow from them would make burst_duration_us() describe a bit
     *         rate the channel does not run at.
     */
    static constexpr bool is_clock_config_valid(const pico_rs485_clock_config &clk) {
        pico_rs485_clock_config rebuilt{};
        if (!compute_clock_config(clk.baudrate, clk.sys_hz, rebuilt)) return false;
        return rebuilt.clkdiv_int == clk.clkdiv_int && rebuilt.clkdiv_frac8 == clk.clkdiv_frac8;
    }

    /**
     * @brief True if the PIO programs can express this framing and these windows.
     *
     * constexpr, so a caller can pin its own framing at compile time.
     *
     * @param pkt Framing and timing windows to check.
     * @return true if every field is inside the range its program supports.
     */
    static constexpr bool is_packet_config_valid(const pico_rs485_packet_config &pkt) {
        if (pkt.data_bits < data_bits_min ||
            pkt.data_bits > data_bits_max) {
            return false;
        }
        if (pkt.parity_bits > parity_bits_max) return false;
        if (pkt.stop_bits < stop_bits_min ||
            pkt.stop_bits > stop_bits_max) {
            return false;
        }
        if (pkt.rx_idle_bits_thresh < rx_idle_bits_thresh_min ||
            pkt.rx_idle_bits_thresh > rx_idle_bits_thresh_max) {
            return false;
        }
        if (pkt.tx_gap_bits < tx_gap_bits_min ||
            pkt.tx_gap_bits > tx_gap_bits_max) {
            return false;
        }

        // The threshold must clear the gap and the last character's stop bits, or the receiver
        // splits a burst the driver just sent.
        if (pkt.rx_idle_bits_thresh <
            (pkt.tx_gap_bits + pkt.stop_bits + rx_idle_margin_bits)) {
            return false;
        }

        return true;
    }

    /**
     * @brief True if the given PIO can host the channel as currently wired.
     *
     * Pure check that claims nothing, so a caller can use it to choose a block. The pin numbers
     * themselves are init()'s to report.
     *
     * @param pio PIO to test.
     * @return ok, or a negative error code: err_pio, err_gpio_base if a
     *         pin falls outside the window this block's GPIO base selects,
     *         err_state_machine, or err_irq_in_use if the block's IRQ 0
     *         line already belongs to something else.
     */
    [[nodiscard]] int is_pio_valid(PIO pio) const;

private:

    // ========================== PIO PROGRAM LAYOUT ===========================
    /**
     * @brief State machine index of each role.
     *
     * A channel claims every machine the block has, so role order is index
     * order.
     */
    enum sm_role : uint {
        sm_tx     = 0, ///< Transmitter.
        sm_rx     = 1, ///< Receiver.
        sm_rxidle = 2, ///< Receive-idle timer.
        sm_txgap  = 3, ///< Transmit gap timer.
    };

    static_assert(NUM_PIO_STATE_MACHINES == (uint)sm_txgap + 1,
                  "a channel needs one machine per program, and takes every machine");

    /// @brief The TX machine's own IRQ: raised with the DE drop that ends a burst.
    static constexpr uint tx_done_irq = 0;
    /// @brief The RX machine's own IRQ, raised by the idle timer once the line is quiet.
    static constexpr uint rx_done_irq = 1;
    /// @brief Flag the transmitter raises once a character has gone out; PIO-internal, unrouted.
    static constexpr uint tx_char_irq = 4;
    /// @brief Flag the gap timer raises once the gap has passed; PIO-internal, unrouted.
    static constexpr uint tx_gap_irq = 5;

    /// @brief Receive staging buffers: filling, queued (up to two), and idle.
    static constexpr uint rx_buffer_count = 3;
    /// @brief Completed bursts held at once, oldest dropped when a newer one arrives.
    static constexpr uint rx_queue_depth = 2;
    static_assert(rx_queue_depth + 1 == rx_buffer_count,
                  "the arm step needs a buffer that is neither queued nor being filled");

    /**
     * @brief The next staging buffer in the rotation.
     * @param buffer Staging buffer index.
     * @return its successor, wrapping, without a division.
     */
    static constexpr uint8_t rx_next_(uint8_t buffer) {
        return (uint8_t)(buffer + 1u == rx_buffer_count ? 0u : buffer + 1u);
    }

    // =========================== INTERNAL HELPERS ============================
    /**
     * @brief Rounds a receive-idle threshold up to the steps the idle timer counts in.
     * @param bits Threshold, in bit times.
     * @return the step count, 1 to 32.
     */
    static constexpr uint rx_idle_steps_(uint8_t bits) {
        return ((uint)bits + rx_idle_bits_per_step - 1u) / rx_idle_bits_per_step;
    }

    /**
     * @brief Checks if the caller is on the same core init() ran on.
     *
     * @return true if the channel is initialized and the caller is on the right core.
     */
    bool is_core_ok_() const;

    /// @brief Releases everything init() reserved; always safe to call
    void release();

    /**
     * @brief Empties the receive FIFO, restarts the receiver and rewinds it to the top of
     *        its program. The flush must come first: a push stalled on a full FIFO is
     *        re-counted by the restarted machine.
     */
    void rx_rewind();

    /**
     * @brief Points the receive DMA at a staging buffer and starts it.
     * @param buffer Which staging buffer to listen into.
     */
    void arm_receive(uint8_t buffer);

    /**
     * @brief Waits for a burst, with or without a bound, and unpacks it.
     * @param out        Receives the data bytes of the burst, up to @p max of them.
     * @param max        Capacity of @p out; 0 asks for the length alone.
     * @param timeout_us How long to wait, when @p bounded is true.
     * @param bounded    Whether @p timeout_us applies at all.
     * @return the number of bytes in the burst, or a negative error code.
     */
    [[nodiscard]] int receive_timed(uint8_t *out, size_t max, uint32_t timeout_us, bool bounded);

    /**
     * @brief Unpacks the oldest completed burst into the caller's buffer.
     * @param out Receives the data bytes, up to @p max of them.
     * @param max Capacity of @p out.
     * @return the number of bytes the burst held, which may exceed @p max.
     */
    int take_burst(uint8_t *out, size_t max);

    /**
     * @brief True if a staging buffer is waiting to be read.
     * @param buffer Staging buffer index.
     * @param count  Queue entries to search, so a caller can pass a cached count.
     * @return true while it sits in the completed-burst queue.
     */
    bool is_queued_at(uint8_t buffer, uint8_t count) const;

    // ============================ INTERRUPT PATH =============================
    /// @brief Handles whichever completion this block's interrupt line is reporting.
#if PICO_RS485_ISR_IN_RAM
    void __not_in_flash_func(service_irq_)();
#else
    void service_irq_();
#endif

    /// @brief Ends a received burst: stops the RX channel, publishes or drops it, arms the next.
#if PICO_RS485_ISR_IN_RAM
    void __not_in_flash_func(rx_done_isr)();
#else
    void rx_done_isr();
#endif

    /// @brief Reports a finished transmission, exactly when the wire went quiet.
#if PICO_RS485_ISR_IN_RAM
    void __not_in_flash_func(tx_done_isr)();
#else
    void tx_done_isr();
#endif

    /**
     * @brief Offers that line to the instance that owns block Index.
     *
     * One handler per block: a handler is handed no argument.
     */
    template <uint Index>
    static void irq_handler_at_() {
        PicoRS485_t *const inst = instances_[Index];
        if (inst != nullptr) inst->service_irq_();
    }
    /// @brief The handler registered for a block index.
    static void (*handler_for_(uint index))();
    static_assert(NUM_PIOS <= 3, "add a thunk and a case for the extra block on this part");

    // ================================= PINS ==================================
    uint pin_tx_;      ///< Transmit data pin
    uint pin_rx_;      ///< Receive data pin
    uint pin_de_;      ///< Driver-enable pin
    uint pin_rx_act_;  ///< Receive-activity pin

    // =================== PACKET CONFIGURATION AND CLOCKING ===================
    pico_rs485_packet_config pkt_{}; ///< Current framing and windows configuration
    uint32_t char_bits_  = 0;        ///< TX bits per character = 1 + data + parity + stop
    uint32_t isr_bits_   = 1;        ///< RX bits per character = data + parity + 1
    uint32_t baudrate_   = 0;        ///< Current baud rate

    uint32_t char_table_[256] = {};              ///< Pre-framed FIFO word for each data byte

    // ============================ TX DMA STAGING =============================
    uint32_t tx_scratch_[max_burst_bytes] = {};  ///< Burst array handed to the TX DMA
    int      tx_dma_ = -1;                       ///< Claimed DMA channel, or -1

    // =============================== TX STATE ================================
    // The latch and the timestamp are written by the interrupt handler and read by the caller
    // through the inline predicates above, so they are volatile for the same reason as the
    // receive state: nothing in an empty loop body can change a plain object, which lets the
    // optimiser hoist the load out of a caller's poll or delete the loop outright.
    volatile bool     tx_pending_ = false;                    ///< True when a sent burst hasn't reported completion yet
    volatile pico_rs485_sent_callback tx_callback_ = nullptr; ///< Callback owed by that burst, if any
    void    *tx_user_ = nullptr;                              ///< User pointer given to send()
    uint32_t tx_burst_id_ = 0;                                ///< ID of the most recent burst
    volatile uint32_t last_sent_at_us_ = 0;                   ///< When a completion was observed
    // tx_user_ and tx_burst_id_ cross the same boundary, but no accessor reads them from the
    // caller, so they stay plain.
    static_assert(std::is_volatile<decltype(tx_pending_)>::value &&
                  std::is_volatile<decltype(tx_callback_)>::value &&
                  std::is_volatile<decltype(last_sent_at_us_)>::value,
                  "the inline predicates read these from the caller; they must stay volatile");

    // ========================= RX STAGING AND QUEUE ==========================
    /// Staging words for received bursts, one FIFO word per character. Three of them,
    /// so the DMA's target is never a buffer that is still queued, and the one being
    /// unpacked is protected by the interrupt mask in take_burst().
    uint32_t rx_staging_[rx_buffer_count][max_burst_bytes + 1] = {};
    // The four members below are written by the interrupt handler and read by the
    // caller, so they are volatile: without it the optimiser may hoist the wait in
    // receive() out of its loop, or delete the loop altogether, because nothing in
    // an empty loop body can change a plain object.
    volatile uint16_t rx_length_[rx_buffer_count] = {};  ///< Characters per staged burst.
    volatile uint8_t  rx_active_ = 0;      ///< Staging buffer the RX DMA fills.
    volatile uint8_t  rx_ready_[rx_queue_depth] = {};  ///< Completed bursts, oldest first.
    volatile uint8_t  rx_ready_count_ = 0; ///< Entries in rx_ready_.
    /// Base the RX channel is armed with, for the is_receiving() test. Volatile for the same
    /// reason as the four above: the handler writes it and the caller reads it.
    volatile uint32_t rx_armed_base_ = 0;
    /// Where the handler looks first for a free buffer. Only the handler and init() touch it, so it
    /// need not be volatile like the four above.
    uint8_t  rx_next_free_ = 0;
    int      rx_dma_ = -1;             ///< Claimed DMA channel, or -1.
    /// Signalled by the handler when it publishes a burst; waited on by the receive calls.
    semaphore_t rx_sem_ = {};

    // The wait in receive() only survives optimisation because these are volatile: nothing
    // in an empty loop body can change a plain object. Pinned so that removing it breaks the
    // build rather than the firmware - see design-notes, "Optimisation-sensitive state".
    static_assert(std::is_volatile<decltype(rx_ready_count_)>::value &&
                  std::is_volatile<decltype(rx_ready_)>::value &&
                  std::is_volatile<decltype(rx_active_)>::value &&
                  std::is_volatile<decltype(rx_length_)>::value &&
                  std::is_volatile<decltype(rx_armed_base_)>::value,
                  "the interrupt handler shares these with the caller; they must stay volatile");

    // ========================== ERROR COUNTER STATE ==========================
    uint32_t framing_errors_ = 0;      ///< Characters with a low stop bit.
    uint32_t parity_errors_  = 0;      ///< Characters with the wrong parity bit.
    // The handler increments this one, so it is volatile for the same reason as the
    // RX queue state: a caller polling it in a loop must see each update.
    volatile uint32_t dropped_bursts_ = 0;  ///< Bursts discarded rather than queued.
    static_assert(std::is_volatile<decltype(dropped_bursts_)>::value,
                  "the interrupt handler increments this; keep it volatile");
    volatile uint32_t empty_bursts_ = 0;    ///< Flags serviced with no characters behind them.
    static_assert(std::is_volatile<decltype(empty_bursts_)>::value,
                  "the interrupt handler increments this; keep it volatile");

    // ============================ BLOCK OWNERSHIP ============================
    /// @brief Instances by PIO index, for the per-block handlers to look up.
    static PicoRS485_t *instances_[NUM_PIOS];
    uint     core_       = 0;    ///< Core init() ran on
    bool     pins_claimed_ = false;    ///< init() has claimed pins
    bool     irq_registered_ = false;  ///< The block's handler has been routed
    uint offset_rx_    = 0;        ///< Receiver program load offset; survives init(), for rx_rewind().
    PIO  pio_          = nullptr;  ///< Claimed PIO instance, null while inert.

};

// =============================== PUBLIC ALIAS ================================

/**
 * @brief Half-duplex RS-485 channel. init() is required before any other call.
 *
 * An instance of this class is one channel, which takes a whole PIO block. Several
 * channels can run at once on separate blocks, but nothing else can share the block.
 *
 * This driver requires the transceiver's `/RE` to be tied to `DE`, and for a free GPIO to
 * be allocated for `pin_rx_act`.
 *
 * It also requires the peer to leave `rx_idle_bits_thresh` of line silence between its bursts:
 * two bursts closer together than that window are merged into one, or the second is
 * dropped as over-long, and neither is distinguishable from real traffic.
 *
 * @note Nothing in this class is synchronised across cores. Calls from a core other than the one
 *       that executed init() will return an error code. If the call cannot return an error code,
 *       the channel returns a safe default and calls pico_rs485_core_violation().
 *
 */
using PicoRS485 = PicoRS485_t<PICO_RS485_MAX_PACKET_BYTES>;

#endif
