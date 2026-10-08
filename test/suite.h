// Shared surface for the host suite: the stub state, the models' types and the
// CHECK/CASE machinery, all of which the section files and the model both need.
//
// The state is defined here as inline variables, so one instance is shared by every
// translation unit in the binary. The model's functions are declared at the bottom
// and defined in suite_model.cpp.
#ifndef TEST_SUITE_H
#define TEST_SUITE_H

#include "PicoRS485.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "pico/platform.h"
#include "pico/time.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
// ------------------------------------------------- stub state and types


// ---------------------------------------------------------------- stub state

// ---------------------------------------------------------------- stub state

// ---------------------------------------------------------------- stub state

// ---------------------------------------------------------------- stub state
inline uint32_t g_sys_hz    = 125000000;


inline uint     g_pio_base[NUM_PIOS] = {};   // per-PIO GPIO base (0 or 16)


inline int      g_add_fail_at = -1;          // 1-based pio_add_program call that fails


inline int      g_add_calls = 0;


inline bool     g_memory_cleared = false;    // has the block been emptied since reset?


inline int      g_add_dirty = 0;             // loads into memory nobody had emptied


inline bool     g_pindir_ok = true;          // pio_sm_set_consecutive_pindirs() verdict


inline bool     g_sm_ok = true;         // pio_sm_init() verdict


// The PIO flags the driver routes to the CPU: the TX machine's own IRQ, raised with the DE drop
// that ends a burst, and the RX machine's, raised by the idle timer once the line goes quiet.
inline constexpr uint k_tx_done_irq = 0;


inline constexpr uint k_rx_done_irq = 1;


inline bool     g_claimed[NUM_PIOS][NUM_PIO_STATE_MACHINES] = {};


inline int      g_double_claim = 0;    // pio_sm_claim() on an already-claimed SM


inline int      g_bad_unclaim  = 0;    // pio_sm_unclaim() on an unclaimed SM


inline int      g_bad_irq_remove = 0;  // irq_remove_handler() with a handler that is not installed


inline int      g_clear_mem    = 0;


inline int      g_claim        = 0;


inline int      g_unclaim      = 0;


inline int      g_enabled_n    = 0;


inline int      g_disabled_n   = 0;


inline int      g_enabled[8]   = {};


inline int      g_disabled[8]  = {};


// Pin state, so the teardown can be checked rather than assumed. Modelled on the
// SDK: gpio_init() is dir IN, then value 0, then FUNCSEL SIO.
inline uint64_t g_pio_cleared_mask = 0;      // pins driven low through the PIO


inline uint64_t g_pio_cleared_last = 0;      // ...and the most recent such call alone


inline PIO      g_mask_pio = nullptr;        // block and machine that call named


inline uint     g_mask_sm  = 99;


inline bool     g_dir_out[NUM_BANK0_GPIOS] = {};


inline bool     g_value[NUM_BANK0_GPIOS] = {};


inline int      g_function[NUM_BANK0_GPIOS] = {};


struct record {
    int      calls = 0;
    uint32_t offset = 0;
    uint32_t clkdiv_int = 0;
    uint8_t  clkdiv_frac8 = 0;
    uint32_t count = 0;      // the count the helper derives Y from: char bits, ISR bits,
                             // idle steps or gap bits
    uint32_t pin_a = 0, pin_b = 0;
};


inline record g_tx, g_rx, g_rxidle, g_txgap;


// Runtime Y updates, recorded so set_packet_config() can be checked.
inline int      g_set_tx_calls = 0, g_set_rx_calls = 0, g_set_rxidle_calls = 0, g_set_gap_calls = 0;


inline uint32_t g_set_tx_bits = 0, g_set_rx_bits = 0, g_set_rxidle_steps = 0, g_set_gap_bits = 0;


// The .pio helpers are replaced by recorders in this suite, so the only pio_sm_exec
// that reaches here is the driver's own jump after a restart. Keeping the last
// instruction lets a test check it targets the receiver's program, and the block
// and machine it named.
inline int      g_sm_exec_calls = 0;


inline uint16_t g_sm_exec_last = 0xffff;


inline PIO      g_exec_pio = nullptr;


inline uint     g_exec_sm  = 99;


// Interrupt masking, modelled as a count so a test can see it happened, plus the
// deepest nesting reached, so an unpinned critical section is visible.
inline int g_irq_masked_depth = 0;


inline int g_irq_masked_max = 0;


// The machine and the block each setting was applied to: without these the
// recorders would be blind to a swap of the four roles or of the block.
inline uint g_set_tx_sm = 99, g_set_rx_sm = 99, g_set_rxidle_sm = 99, g_set_gap_sm = 99;


inline PIO  g_set_tx_pio = nullptr, g_set_rx_pio = nullptr;


inline PIO  g_set_rxidle_pio = nullptr, g_set_gap_pio = nullptr;


inline const int fifo_depth = 8;


inline uint32_t g_txq[NUM_PIOS][NUM_PIO_STATE_MACHINES][fifo_depth] = {};


inline int      g_txq_n[NUM_PIOS][NUM_PIO_STATE_MACHINES] = {};


inline uint32_t g_rxq[NUM_PIOS][NUM_PIO_STATE_MACHINES][fifo_depth] = {};


inline int      g_rxq_n[NUM_PIOS][NUM_PIO_STATE_MACHINES] = {};


// ---------------------- bit-level receiver model --------------------------
// deliver_rx() hands the receiver an already-grouped word, which assumes the very
// thing that was wrong on hardware: where the autopush puts the group boundary. This
// model groups the samples itself, so the boundary is a consequence of the sequence of
// restarts and flushes rather than a property of the test.
//
// The state is per machine: the input shift register, the autopush sample counter, and
// a stall. The stall is the interesting one. When the ninth sample of a character arrives
// and the FIFO is full, the autopush cannot complete and the instruction stalls with
// its state intact. pio_sm_restart() then clears the counter and the shift register -
// but it cannot cancel that instruction, so the flush that finally gives the FIFO room
// retires it afterwards, committing its shift against the cleared counter and leaving
// the counter at one.
//
// That last step is not documented; it is calibrated. On hardware the two orderings
// produce two different words for the same 0xA5 character, and this model is built to
// reproduce both: restart-then-flush gives 0xA5800000 (the group one sample early, the
// stop slot reading the data MSB) and flush-then-restart gives 0xD2800000. A test below
// pins those two words, so the model cannot drift away from the silicon it came from.
struct rx_group_state {
    uint32_t isr = 0;          ///< samples shifted in so far, newest at the top
    uint16_t count = 0;        ///< samples since the last completed push
    bool     stalled = false;  ///< an autopush that has not retired: the FIFO was full
    uint32_t held = 0;         ///< the sample that stalled instruction is carrying
};


inline rx_group_state g_rxgrp[NUM_PIOS][NUM_PIO_STATE_MACHINES];


struct dma_state {
    bool     claimed = false;
    bool     busy = false;
    bool     enable = false;
    bool     rinc = false, winc = false;
    uint     dreq = 0;
    uint32_t *read = nullptr;
    uint32_t *write = nullptr;
    uint32_t count = 0;
    int      size = -1;
    dma_channel_hw_t hw = {};
};


inline dma_state g_dma[NUM_DMA_CHANNELS];


inline int       g_dma_claimed = 0;


inline int       g_abort_while_enabled = 0;     // aborts that left EN set (RP2350-E5)


inline int       g_configure_live = 0;          // reconfigures of a channel with work left


inline int       g_fifo_clears = 0;             // FIFO flushes, so init/recovery are visible


inline int       g_model_overrun = 0;           // a push past the model's FIFO depth


// A retirement lag, so a test can put the driver in the window RP2040-E13 describes:
// the transfer's read has happened but its write has not, the abort's BUSY wait returns
// early, and the pointer the driver reads is one word behind until it reads again. Off
// by default, and the view below is what dma_channel_hw_addr() hands out.
inline int g_retire_lag = 0;


inline int g_retire_pending[NUM_DMA_CHANNELS] = {};


inline dma_channel_hw_t g_hw_view[NUM_DMA_CHANNELS] = {};


// Optional, one-shot: model the idle timer raising flag 1 at the instant the
// receiver is armed, which is the window init()'s last clear covers. One-shot so
// the handler's own later re-arm is not affected.
inline bool      g_raise_idle_on_arm = false;


inline irq_handler_t g_irq_handler[32] = {};


inline bool          g_irq_enabled[32] = {};


inline int           g_sm_restarts = 0;


inline PIO           g_restart_pio = nullptr;


inline uint          g_restart_sm  = 99;


// Which PIO sources were routed to IRQ 0, so "only the two completion flags" is checked
// rather than assumed. The stub only has sources for flags 0-3, as RP2040 does.
inline int           g_irq_src_set[16] = {};


inline int           g_irq_src_cleared[16] = {};


// The semaphore the driver signals when it publishes a burst; counted so a test can watch for it.
inline int g_sem_releases = 0;


// The driver's weak core-violation hook, replaced here so a test can see it called.
inline int g_core_hook_calls = 0;


// The clock the stub's inline time_us_32() advances. Volatile, like the SDK's
// timer register, so that a wait loop calling it still has no barrier over the
// publication flag it is waiting on - which is what makes the volatile flag in
// the driver load-bearing rather than decorative.
//
// Note: the driver's receive() waits without reading the clock, so a test that
// called it with an empty queue would spin forever. Tests call receive() only
// when a burst is already queued, and receive_for() otherwise.
inline volatile uint32_t g_time_us = 0;


inline volatile int      g_time_reads = 0;


// What a sent callback saw.
inline int       g_sent_calls = 0;


inline uint32_t  g_sent_id = 0;


inline void     *g_sent_user = nullptr;


// A callback that queues the next burst, which send() allows.
inline int g_sent_resends = 0;


// ------------------------------------------------------------- scenarios
// Thin helpers for the shapes every section repeats. Each one checks, so a
// scenario cannot silently skip its precondition.

// The role order the driver's private enum fixes (the header's static_assert ties
// it to NUM_PIO_STATE_MACHINES). Spelling it out here is the point: the tests pin
// the mapping, so they must not read it from the driver.
inline const uint SM_TX = 0, SM_RX = 1, SM_RXIDLE = 2, SM_GAP = 3;

inline int g_failures = 0;

inline int g_checks   = 0;


// A table-driven case names itself, so a failure is traceable to its row.
inline const char *g_case = "";

inline char g_case_buf[96];


inline struct pio_hw g_pio_hw[NUM_PIOS] = {};


// The core the code under test believes it is on, and the hard assertions the driver asked
inline unsigned g_core_num     = 0;

// ---------------------------------------------------------- test scaffolding

#define CASE(...)                                                              \
    do { std::snprintf(g_case_buf, sizeof g_case_buf, __VA_ARGS__); g_case = g_case_buf; } while (0)


#define FAIL_LINE(expr)                                                        \
    do {                                                                       \
        if (g_case[0]) std::printf("FAIL %s:%d [%s]  %s\n", __FILE__, __LINE__, g_case, expr); \
        else           std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, expr);              \
    } while (0)


#define CHECK(cond)                                                            \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) { ++g_failures; FAIL_LINE(#cond); }                       \
    } while (0)

#define CHECK_EQ(got, want)                                                    \
    check_eq_ll(__FILE__, __LINE__, #got " == " #want,                         \
                (long long)(got), (long long)(want))


// ------------------------------------------------------------------- model

void check_eq_ll(const char *file, int line, const char *expr,
                        long long got, long long want);
int pio_slot(PIO p);
void deliver_sample(PIO pio, uint sm, uint32_t bit, uint isr_bits);
void deliver_char_bits(PIO pio, uint sm, uint8_t byte, uint isr_bits);
void release_stalled(int slot, int sm, uint isr_bits);
struct pio_hw *pio_hw_of(int slot);
void dma_run();
void other_library_irq_handler(void);
template <typename Fn>
static int violations_on_core(unsigned core, Fn fn) {
    const unsigned was = g_core_num;
    const int before = g_core_hook_calls;
    g_core_num = core;
    fn();
    g_core_num = was;
    return g_core_hook_calls - before;
}
void deliver_rx(PIO pio, uint sm, uint32_t sampled_bits, uint isr_bits);
void deliver_rx_unsent(PIO pio, uint sm, uint32_t sampled_bits, uint isr_bits);
bool run_tx(PIO pio, uint sm, uint32_t *word_out);
void raise_pio_irq(PIO pio, uint flag);
void drain_tx(PIO pio, uint sm);
void sent_probe(void *user, uint32_t burst_id);
void sent_then_send(void *user, uint32_t);
void reset_stubs();
bool saw_all_four(const int *v, int n);
bool same_packet(const pico_rs485_packet_config &a, const pico_rs485_packet_config &b);
bool old_clock(uint32_t baudrate, uint32_t sys_hz, uint32_t &out_int, uint8_t &out_frac);
inline bool bring_up(PicoRS485 &ch, PIO pio = pio0, uint32_t baud = 921600,
                     const pico_rs485_packet_config &pkt = pico_rs485_packet_config{}) {
    const int rc = ch.init(pio, baud, pkt);
    CHECK_EQ(rc, (int)PicoRS485::ok);
    CHECK(ch.is_valid());
    return rc == PicoRS485::ok;
}
inline bool bring_up(PicoRS485 &ch, PIO pio, const pico_rs485_clock_config &clk,
                     const pico_rs485_packet_config &pkt = pico_rs485_packet_config{}) {
    const int rc = ch.init(pio, clk, pkt);
    CHECK_EQ(rc, (int)PicoRS485::ok);
    CHECK(ch.is_valid());
    return rc == PicoRS485::ok;
}
void expect_refused_clean(const PicoRS485 &ch, int rc, int want);
void expect_refused_released(const PicoRS485 &ch, int rc, int want);
void feed(PIO pio, uint sm, uint isr_bits, std::initializer_list<uint32_t> words);
inline void end_burst(PIO pio = pio0) { raise_pio_irq(pio, k_rx_done_irq); }

#endif
