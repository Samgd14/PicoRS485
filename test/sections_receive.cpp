// RECEIVE
//
#include "suite.h"

void suite_receive() {
    // --- 35. receive: one silence-delimited burst ----------------------------
    CASE("receive: one silence-delimited burst");
    reset_stubs();
    {
        PicoRS485 ch(0, 1, 2, 3);
        bring_up(ch);

        const uint8_t sent[3] = {0xA5, 0x01, 0xFE};
        for (int i = 0; i < 3; ++i) deliver_rx(pio0, SM_RX, (uint32_t)sent[i] | (1u << 8), 9);
        CHECK(ch.is_receiving());                  // arrived, no silence yet
        end_burst();
        CHECK(!pio_interrupt_get(pio0, k_rx_done_irq));        // the handler cleared the flag
        CHECK(!ch.is_receiving());                 // and re-armed on the other buffer
        // The channel was stopped before it was re-armed: rearming one that still had
        // work left would count, and so would an abort that left it enabled.
        CHECK_EQ(g_configure_live, 0);
        CHECK_EQ(g_abort_while_enabled, 0);

        uint8_t got[8] = {};
        CHECK_EQ(ch.receive(got, sizeof(got)), 3); // the burst is queued, so this cannot spin
        CHECK(got[0] == 0xA5 && got[1] == 0x01 && got[2] == 0xFE);
        CHECK_EQ(ch.framing_error_count(), 0);
        CHECK_EQ(ch.dropped_burst_count(), 0);

        // is_receiving() reads the active buffer and the DMA's pointer together. It masks
        // its own interrupt line rather than the core, so the global mask is never taken and
        // unrelated interrupts keep running.
        CASE("receive: is_receiving masks its own line, not the core");
        (void)ch.is_receiving();
        CHECK_EQ(g_irq_masked_max, 0);
        CHECK_EQ(g_irq_masked_depth, 0);
        CHECK(irq_is_enabled(PIO_IRQ_NUM(pio0, 0)));

        // A burst the DMA kept up with needs no flush: the drain is gated on the FIFO being
        // non-empty, so a clean burst costs no peripheral writes at all.
        CASE("receive: a clean burst does not flush the receive FIFO");
        const int clears_before = g_fifo_clears;
        deliver_rx(pio0, SM_RX, 0x66u | (1u << 8), 9);
        end_burst();
        CHECK_EQ(g_fifo_clears, clears_before);
        uint8_t clean[2] = {};
        CHECK_EQ(ch.receive(clean, sizeof(clean)), 1);
        CHECK_EQ(clean[0], 0x66u);
    }

    // --- 36. receive: decoding across configurations -------------------------
    CASE("receive: decoding across configurations");
    // One row is one burst: the sampled bits the receiver's ISR would push, the
    // bytes it must deliver, and the two counters it must move. The expected bytes
    // and counts are written out, not derived from the config.
    {
        struct rx_row {
            const char *what; pico_rs485_packet_config pkt; uint isr_bits;
            uint32_t words[4]; int nwords;
            uint8_t  bytes[4]; int nbytes;
            uint32_t framing;  uint32_t parity;
        };
        const rx_row rows[] = {
            {"8N1 good",         {8, 0, 1, 8, 16},              9,  {0x141}, 1, {0x41}, 1, 0, 0},
            {"8N1 stop bit low", {8, 0, 1, 8, 16},              9,  {0x077}, 1, {0x77}, 1, 1, 0},
            {"8N1 4 chars, 3 without a stop bit",
                                 {8, 0, 1, 8, 16},              9,
                                 {0x141, 0x041, 0x042, 0x043}, 4, {0x41, 0x41, 0x42, 0x43}, 4, 3, 0},
            {"8E1 good",         {8, 1, 1, 8, 16, true}, 10, {0x203}, 1, {0x03}, 1, 0, 0},
            {"8E1 parity wrong", {8, 1, 1, 8, 16, true}, 10, {0x303}, 1, {0x03}, 1, 0, 1},
            {"8O1 good",         {8, 1, 1, 8, 16, false},  10, {0x303}, 1, {0x03}, 1, 0, 0},
            {"8O1 parity wrong", {8, 1, 1, 8, 16, false},  10, {0x203}, 1, {0x03}, 1, 0, 1},
            {"8E1 3 chars, every parity wrong",
                                 {8, 1, 1, 8, 16, true}, 10,
                                 {0x210, 0x220, 0x240}, 3, {0x10, 0x20, 0x40}, 3, 0, 3},
            {"7N1 masks the stop bit", {7, 0, 1, 8, 16},        8,  {0x0D5}, 1, {0x55}, 1, 0, 0},
            {"7E1 good",         {7, 1, 1, 8, 16, true}, 9,  {0x155}, 1, {0x55}, 1, 0, 0},
            {"7E1 parity wrong", {7, 1, 1, 8, 16, true}, 9,  {0x1D5}, 1, {0x55}, 1, 0, 1},
            {"5N1 good",         {5, 0, 1, 8, 16},              6,  {0x03F}, 1, {0x1F}, 1, 0, 0},
        };
        for (const rx_row &r : rows) {
            CASE("receive: RX %s", r.what);
            reset_stubs();
            PicoRS485 ch(0, 1, 2, 3);
            bring_up(ch, pio0, 9600, r.pkt);
            for (int i = 0; i < r.nwords; ++i) deliver_rx(pio0, SM_RX, r.words[i], r.isr_bits);
            end_burst();
            uint8_t got[PicoRS485::max_burst_bytes] = {};
            const int n = ch.receive_for(got, sizeof(got), 50000);
            CHECK_EQ(n, r.nbytes);
            for (int i = 0; i < r.nbytes && i < n; ++i) {
                CASE("receive: RX %s, byte %d", r.what, i);
                CHECK_EQ(got[i], r.bytes[i]);
            }
            CASE("receive: RX %s, error counters", r.what);
            CHECK_EQ(ch.framing_error_count(), r.framing);
            CHECK_EQ(ch.parity_error_count(), r.parity);
        }
    }

    // --- 37. receive: the queue, its overflow and the length limit -----------
    CASE("receive: the queue, its overflow and the length limit");
    reset_stubs();
    {
        PicoRS485 ch(0, 1, 2, 3);
        bring_up(ch);
        uint8_t got[8] = {};

        // A character whose stop bit was low is counted, and still delivered.
        CASE("receive: a short burst");
        feed(pio0, SM_RX, 9, {0x77u});
        end_burst();
        CHECK_EQ(ch.receive(got, sizeof(got)), 1);
        CHECK_EQ(got[0], 0x77);
        CHECK_EQ(ch.framing_error_count(), 1);

        // Two bursts queue; a third drops the oldest.
        CASE("receive: the queue is two deep");
        const uint8_t bursts[3] = {0x11, 0x22, 0x33};
        for (int i = 0; i < 3; ++i) {
            feed(pio0, SM_RX, 9, {(uint32_t)bursts[i] | (1u << 8)});
            end_burst();
        }
        CHECK_EQ(ch.dropped_burst_count(), 1);
        CHECK_EQ(ch.receive(got, sizeof(got)), 1);
        CHECK_EQ(got[0], 0x22);
        CHECK_EQ(ch.receive(got, sizeof(got)), 1);
        CHECK_EQ(got[0], 0x33);

        // A burst of exactly the maximum is kept; one character more is dropped.
        CASE("receive: exactly the maximum length");
        for (uint32_t i = 0; i < PicoRS485::max_burst_bytes; ++i) {
            deliver_rx(pio0, SM_RX, 0x41u | (1u << 8), 9);
        }
        end_burst();
        CHECK_EQ(ch.dropped_burst_count(), 1);     // the earlier queue overflow only
        CHECK_EQ(ch.receive(got, sizeof(got)), (int)PicoRS485::max_burst_bytes);
        CHECK_EQ(got[0], 0x41);

        CASE("receive: one character past the maximum");
        for (uint32_t i = 0; i < PicoRS485::max_burst_bytes + 1; ++i) {
            deliver_rx(pio0, SM_RX, 0x42u | (1u << 8), 9);
        }
        end_burst();
        CHECK_EQ(ch.dropped_burst_count(), 2);
        CHECK_EQ(ch.receive_for(got, sizeof(got), 5000), (int)PicoRS485::err_timeout);
    }
    // A silence flag with nothing behind it publishes nothing and drops nothing.
    reset_stubs();
    {
        PicoRS485 ch(0, 1, 2, 3);
        bring_up(ch);
        CASE("receive: a silence flag with no burst behind it");
        end_burst();
        uint8_t got[4] = {};
        CHECK_EQ(ch.receive_for(got, sizeof(got), 1000), (int)PicoRS485::err_timeout);
        CHECK_EQ(ch.dropped_burst_count(), 0);
        CHECK_EQ(ch.framing_error_count(), 0);
    }
    // An empty wait times out after exactly two clock reads: one to decide, one to
    // report.
    reset_stubs();
    {
        PicoRS485 ch(0, 1, 2, 3);
        bring_up(ch);
        uint8_t one[1] = {};
        CASE("receive: the timeout boundary is exact");
        g_time_reads = 0;
        CHECK_EQ(ch.receive_for(one, 1, 1000), (int)PicoRS485::err_timeout);
        CHECK_EQ(g_time_reads, 2);
    }

    // --- 38. receive: take_burst() writes only what the caller offered -------
    CASE("receive: take_burst() writes only what the caller offered");
    reset_stubs();
    {
        PicoRS485 ch(0, 1, 2, 3);
        bring_up(ch);
        for (uint32_t i = 0; i < PicoRS485::max_burst_bytes; ++i) {
            deliver_rx(pio0, SM_RX, (i & 0xFFu) | (1u << 8), 9);
        }
        end_burst();

        struct { uint8_t data[4]; uint32_t canary; } out{};
        out.canary = 0xC0FFEEu;
        // The return is the true burst length even though only four bytes fit.
        CHECK_EQ(ch.receive(out.data, sizeof(out.data)), (int)PicoRS485::max_burst_bytes);
        CHECK(out.data[0] == 0x00u && out.data[3] == 0x03u);
        CHECK_EQ(out.canary, 0xC0FFEEu);           // nothing ran past the buffer
    }

    // --- 39. receive: what the API refuses -----------------------------------
    CASE("receive: what the API refuses");
    reset_stubs();
    {
        PicoRS485 ch(0, 1, 2, 3);
        uint8_t one[1] = {0x5A};
        CASE("receive: inert channel");
        CHECK_EQ(ch.receive(nullptr, 1), (int)PicoRS485::err_not_initialized);
        // The bounded wait is checked first because the unbounded one would spin.
        const bool refuses = ch.receive_for(one, 1, 1000) == PicoRS485::err_not_initialized;
        CHECK(refuses);
        if (refuses) CHECK_EQ(ch.receive(one, 1), (int)PicoRS485::err_not_initialized);
        bring_up(ch);
        CASE("receive: a null buffer with a non-zero capacity");
        // The bounded call gives a verdict without spinning. The unbounded one has
        // the same guard, and a burst is queued first so that a driver missing the
        // guard fails fast rather than waiting for a burst that is already there.
        CHECK_EQ(ch.receive_for(nullptr, 1, 1000), (int)PicoRS485::err_argument);
        feed(pio0, SM_RX, 9, {0x5Au | (1u << 8)});
        end_burst();
        CHECK_EQ(ch.receive(nullptr, 1), (int)PicoRS485::err_argument);
        uint8_t got[4] = {};
        CHECK_EQ(ch.receive(got, sizeof(got)), 1);   // and the burst was not consumed
        CHECK_EQ(got[0], 0x5A);
    }

    // --- 40. receive: a burst arriving while two are queued cannot overwrite them ----
    CASE("receive: a burst arriving while two are queued cannot overwrite them");
    reset_stubs();
    {
        PicoRS485 ch(0, 1, 2, 3);
        CHECK(ch.init(pio0, 921600) == PicoRS485::ok);

        deliver_rx(pio0, 1, 0x11u | (1u << 8), 9);
        raise_pio_irq(pio0, k_rx_done_irq);                  // burst A queued
        deliver_rx(pio0, 1, 0x22u | (1u << 8), 9);
        raise_pio_irq(pio0, k_rx_done_irq);                  // burst B queued: the queue is full

        // Burst C starts arriving while A and B are still queued. With only two
        // staging buffers this character landed on top of A.
        deliver_rx(pio0, 1, 0x33u | (1u << 8), 9);

        uint8_t got[4] = {};
        CHECK(ch.receive(got, sizeof(got)) == 1);
        CHECK(got[0] == 0x11);                   // A survived C starting
        CHECK(ch.receive(got, sizeof(got)) == 1);
        CHECK(got[0] == 0x22);                   // and so did B

        raise_pio_irq(pio0, k_rx_done_irq);                  // C completes into an empty queue
        CHECK(ch.receive(got, sizeof(got)) == 1);
        CHECK(got[0] == 0x33);
        CHECK(ch.dropped_burst_count() == 0);
    }

    // --- 41. overrun recovery: drain, restart, rewind, clean next burst ------
    CASE("overrun recovery: drain, restart, rewind, clean next burst");
    // init() drains, rewinds and flushes the receiver rather than only configuring it.
    // The drain comes first so that the flush releases any stalled push before the
    // restart clears the shift counter - see section 43.
    reset_stubs();
    {
        PicoRS485 ch(0, 1, 2, 3);
        bring_up(ch);
        CASE("receive: init rewinds the receiver");
        CHECK_EQ(g_sm_restarts, 1);
        CHECK_EQ(g_sm_exec_calls, 1);
        CHECK_EQ((int)g_sm_exec_last, (int)g_rx.offset);
        CHECK(g_exec_pio == pio0 && g_exec_sm == SM_RX);      // and to the receiver
        CHECK(g_restart_pio == pio0 && g_restart_sm == SM_RX);
        CHECK(g_fifo_clears >= 1);
        // Every setting reached its own machine, on the block that was chosen.
        CHECK(g_set_tx_sm == SM_TX && g_set_rx_sm == SM_RX);
        CHECK(g_set_rxidle_sm == SM_RXIDLE && g_set_gap_sm == SM_GAP);
        CHECK(g_set_tx_pio == pio0 && g_set_rx_pio == pio0);
        CHECK(g_set_rxidle_pio == pio0 && g_set_gap_pio == pio0);

        // A burst longer than the buffer is dropped, counted, and the receiver
        // starts over from the top of its program rather than mid-character.
        CASE("receive: an oversized burst");
        const int exec_before = g_sm_exec_calls;
        for (uint32_t i = 0; i < PicoRS485::max_burst_bytes + 2; ++i) {
            deliver_rx(pio0, SM_RX, 0x41u | (1u << 8), 9);
        }
        end_burst();
        CHECK_EQ(ch.dropped_burst_count(), 1);
        CHECK_EQ(g_sm_exec_calls, exec_before + 1);
        CHECK_EQ((int)g_sm_exec_last, (int)g_rx.offset);
        CHECK(g_exec_pio == pio0 && g_exec_sm == SM_RX);
        // Here the channel had already run its count to zero, so only the abort half
        // can trip; the rearm with work left is checked in the receive section.
        CHECK_EQ(g_abort_while_enabled, 0);
        uint8_t got[4] = {};
        CHECK_EQ(ch.receive_for(got, sizeof(got), 5000), (int)PicoRS485::err_timeout);
    }
    // A character the DMA never took means the burst is lost, not published short.
    reset_stubs();
    {
        PicoRS485 ch(0, 1, 2, 3);
        bring_up(ch);
        const int restarts_after_init = g_sm_restarts;
        CASE("receive: a character left in the FIFO");
        deliver_rx_unsent(pio0, SM_RX, 0x9Cu | (1u << 8), 9);
        CHECK(!pio_sm_is_rx_fifo_empty(pio0, SM_RX));   // left there, never moved
        end_burst();
        CHECK_EQ(ch.dropped_burst_count(), 1);
        uint8_t got[4] = {};
        CHECK_EQ(ch.receive_for(got, sizeof(got), 5000), (int)PicoRS485::err_timeout);
        CHECK_EQ(g_sm_restarts, restarts_after_init + 1);   // and the receiver resynced
        CHECK_EQ(g_abort_while_enabled, 0);
    }
    // A dropped burst is flushed, so the next one is one clean byte rather than
    // the stale character the dropped burst left ahead of it.
    reset_stubs();
    {
        PicoRS485 ch(0, 1, 2, 3);
        bring_up(ch);
        CASE("receive: the flush leaves a clean next burst");
        deliver_rx_unsent(pio0, SM_RX, 0x9Cu | (1u << 8), 9);
        const int clears_before = g_fifo_clears;
        end_burst();
        CHECK_EQ(ch.dropped_burst_count(), 1);
        CHECK(g_fifo_clears > clears_before);
        feed(pio0, SM_RX, 9, {0x11u | (1u << 8)});
        end_burst();
        uint8_t got[4] = {};
        CHECK_EQ(ch.receive(got, sizeof(got)), 1);
        CHECK_EQ(got[0], 0x11u);
        CHECK_EQ(ch.framing_error_count(), 0);
    }

    // --- 42. receive: a burst of N characters publishes N bytes and does not drop ----
    // The receiver's length is derived from the RX channel's write pointer, read once
    // the channel has been stopped. A length derived from the transfer count cannot
    // work here: the stop that ends the burst is what clears the count, so the count
    // reads as a full arm and every good burst looks over-long. The double advances the
    // pointer as each transfer retires, so these checks depend on that pointer moving;
    // section 44 covers the case where the retirement lags the abort, which is the one
    // thing that can leave the read one word short.
    // Eight characters in, eight bytes out, no drop; a full buffer published rather than
    // mistaken for an over-run; one character more than that dropped.
    CASE("receive: a burst of N characters publishes N bytes and does not drop");
    reset_stubs();
    {
        PicoRS485 ch(0, 1, 2, 3);
        CHECK(ch.init(pio0, 9600) == PicoRS485::ok);
        for (uint32_t i = 0; i < 8; ++i) {
            deliver_rx(pio0, 1, (0x40u + i) | (1u << 8), 9);   // data 0-7, stop at 8
        }
        raise_pio_irq(pio0, k_rx_done_irq);

        CHECK_EQ(ch.dropped_burst_count(), 0u);
        uint8_t got[16] = {};
        CHECK_EQ(ch.receive(got, sizeof(got)), 8);
        CHECK(got[0] == 0x40u && got[7] == 0x47u);

        // Exactly full: the arm count is one more than a maximum burst, so 256
        // characters leave the pointer at base + 256 words, short of the arm count,
        // and the burst is published rather than dropped as over-long.
        for (uint32_t i = 0; i < (uint32_t)PicoRS485::max_burst_bytes; ++i) {
            deliver_rx(pio0, 1, 0x5Au | (1u << 8), 9);
        }
        raise_pio_irq(pio0, k_rx_done_irq);
        CHECK_EQ(ch.dropped_burst_count(), 0u);
        uint8_t full[PicoRS485::max_burst_bytes] = {};
        CHECK_EQ(ch.receive(full, sizeof(full)), (int)PicoRS485::max_burst_bytes);
        CHECK(full[0] == 0x5Au && full[PicoRS485::max_burst_bytes - 1] == 0x5Au);

        // One over: 257 characters run the channel to completion, leaving the pointer
        // at base + 257 words, and the burst is dropped, so the over-long branch keeps
        // its meaning.
        const uint32_t drops_before = ch.dropped_burst_count();
        for (uint32_t i = 0; i < (uint32_t)PicoRS485::max_burst_bytes + 1u; ++i) {
            deliver_rx(pio0, 1, 0x5Au | (1u << 8), 9);
        }
        raise_pio_irq(pio0, k_rx_done_irq);
        CHECK_EQ(ch.dropped_burst_count(), drops_before + 1u);
    }

    // --- 43. receive: a stalled autopush must not re-phase the grouping ------
    // On hardware this was the whole bug: the receiver restarted while an autopush was
    // stalled on a full FIFO, and the flush that followed retired that instruction
    // against the counter the restart had just cleared. From then on every word held
    // the group one sample along, so every byte decoded rotated, at every bit rate -
    // and whether the window was there at all depended on the compiled shape, which is
    // why it looked random for a whole session. The driver now drains before it
    // restarts, in init() and in the recovery branch.
    //
    // The double's bit-level model above groups the samples, so these checks can reach
    // the ordering at all. The first pair pins that model to the two words measured on
    // silicon for the same character; without that, a green regression check would only
    // prove the model agrees with itself.
    CASE("receive: the model reproduces both measured words for one character");
    {
        uint32_t measured[2] = {0, 0};
        const uint32_t expected_correct = 0xD2800000u;   // measured on the healthy build
        const uint32_t expected_rotated = 0xA5800000u;   // measured on the failing build
        for (int order = 0; order < 2; ++order) {
            reset_stubs();
            // Nine characters with no DMA consumer: the ninth sample of the ninth
            // stalls the autopush with the FIFO full.
            for (int c = 0; c < 9; ++c) deliver_char_bits(pio0, SM_RX, 0x11u, 9);
            CHECK_EQ(g_rxgrp[0][SM_RX].stalled, true);
            // Both orders flush twice, as the driver does: the first flush releases the
            // stalled push into the FIFO it just emptied, and the second clears what
            // that left behind.
            if (order == 0) {                    // restart, then drain: the bug
                pio_sm_restart(pio0, SM_RX);
                pio_sm_clear_fifos(pio0, SM_RX);
                pio_sm_clear_fifos(pio0, SM_RX);
            } else {                             // drain, then restart: the fix
                pio_sm_clear_fifos(pio0, SM_RX);
                pio_sm_clear_fifos(pio0, SM_RX);
                pio_sm_restart(pio0, SM_RX);
            }
            CHECK_EQ(g_rxq_n[0][SM_RX], 0);      // nothing stale left either way
            // One character arrives. Whatever grouping the model is in, it must push
            // exactly once for it, because a character pushes what a character consumes.
            deliver_char_bits(pio0, SM_RX, 0xA5u, 9);
            CHECK_EQ(g_rxq_n[0][SM_RX], 1);
            measured[order] = g_rxq[0][SM_RX][0];
        }
        std::printf("  model: restart-then-flush %08X, flush-then-restart %08X\n",
                    (unsigned)measured[0], (unsigned)measured[1]);
        std::printf("  silicon: %08X (rotated) and %08X (correct)\n",
                    (unsigned)expected_rotated, (unsigned)expected_correct);
        // The one property the model exists to capture: the order changes the word.
        CHECK(measured[0] != measured[1]);
        // And it has to be the measured rotation, not merely a difference.
        CHECK_EQ(measured[0], expected_rotated);
        CHECK_EQ(measured[1], expected_correct);
    }

    // Through the driver, not the primitives: a recovery taken with the FIFO full must
    // still leave the next burst correctly grouped.
    reset_stubs();
    {
        PicoRS485 ch(0, 1, 2, 3);
        bring_up(ch);
        CASE("receive: a recovery with the FIFO full leaves the grouping alone");
        // Fill the FIFO without servicing the DMA, so the autopush stalls, then let the
        // silence flag find it full - the recovery branch, which is where the bug bit.
        for (int c = 0; c < 9; ++c) deliver_char_bits(pio0, SM_RX, 0x11u, 9);
        CHECK_EQ(g_rxgrp[0][SM_RX].stalled, true);
        end_burst();
        CHECK_EQ(ch.dropped_burst_count(), 1u);

        // A burst of two characters now, taken through the DMA the way the driver does.
        deliver_char_bits(pio0, SM_RX, 0xA5u, 9);
        deliver_char_bits(pio0, SM_RX, 0x5Au, 9);
        dma_run();
        end_burst();
        uint8_t got[4] = {};
        CHECK_EQ(ch.receive(got, sizeof(got)), 2);
        CHECK_EQ(got[0], 0xA5u);
        CHECK_EQ(got[1], 0x5Au);
        CHECK_EQ(ch.framing_error_count(), 0u);   // the stop slots still line up
    }

    // --- 44. RP2040-E13: a write still in flight when the channel is stopped ----
    // The errata: aborting a channel with a transfer in flight lets the ABORT status bit
    // clear prematurely, so a single read of the write pointer can be one word short and
    // a burst of N characters is published as N-1. The receiver reads the pointer until
    // it stops moving. The double models the window with g_retire_lag: the first read
    // after a transfer still sees the old pointer, a later one sees where it landed.
    CASE("receive: a write in flight at the abort is not read as a short burst");
    reset_stubs();
    {
        PicoRS485 ch(0, 1, 2, 3);
        bring_up(ch);
        g_retire_lag = 1;
        for (uint32_t i = 0; i < 16; ++i) {
            deliver_rx(pio0, SM_RX, (0x40u + i) | (1u << 8), 9);
        }
        end_burst();
        uint8_t got[32] = {};
        CHECK_EQ(ch.receive(got, sizeof(got)), 16);   // not 15
        CHECK_EQ(ch.dropped_burst_count(), 0u);
        CHECK(got[0] == 0x40u && got[15] == 0x4Fu);
        g_retire_lag = 0;
        for (uint i = 0; i < NUM_DMA_CHANNELS; ++i) g_retire_pending[i] = 0;
    }

    // --- 45. receive: a silence flag with nothing behind it is counted -------
    // The driver used to pass such a decision over in silence, which made it
    // indistinguishable from a burst that never arrived.
    CASE("receive: an empty silence decision is counted, not passed over");
    reset_stubs();
    {
        PicoRS485 ch(0, 1, 2, 3);
        bring_up(ch);
        CHECK_EQ(ch.empty_burst_count(), 0u);
        end_burst();                        // a flag with nothing in the buffer
        CHECK_EQ(ch.empty_burst_count(), 1u);
        CHECK_EQ(ch.dropped_burst_count(), 0u);

        // A real burst still publishes, and leaves the counter alone.
        deliver_rx(pio0, SM_RX, 0x5Au | (1u << 8), 9);
        end_burst();
        uint8_t got[4] = {};
        CHECK_EQ(ch.receive(got, sizeof(got)), 1);
        CHECK_EQ(got[0], 0x5Au);
        CHECK_EQ(ch.empty_burst_count(), 1u);
        CHECK_EQ(ch.dropped_burst_count(), 0u);
    }

    // --- 46. receive: leaves the interrupt line as it found it ---------------
    // The mask it takes is restored rather than simply cleared: a caller who masked the
    // line on purpose should still find it masked when the call returns.
    CASE("receive: a masked interrupt line is still masked after a receive");
    reset_stubs();
    {
        PicoRS485 ch(0, 1, 2, 3);
        bring_up(ch);
        const uint irq = PIO_IRQ_NUM(pio0, 0);
        deliver_rx(pio0, SM_RX, 0x33u | (1u << 8), 9);
        end_burst();                                    // queued while the line is enabled
        irq_set_enabled(irq, false);                    // the caller masks it itself
        uint8_t got[4] = {};
        CHECK_EQ(ch.receive_for(got, sizeof(got), 5000), 1);
        CHECK_EQ(got[0], 0x33u);
        CHECK(!g_irq_enabled[irq]);                     // and it is left masked
        irq_set_enabled(irq, true);
        // The normal case is unchanged: an enabled line stays enabled.
        deliver_rx(pio0, SM_RX, 0x44u | (1u << 8), 9);
        end_burst();
        CHECK_EQ(ch.receive_for(got, sizeof(got), 5000), 1);
        CHECK_EQ(got[0], 0x44u);
        CHECK(g_irq_enabled[irq]);
    }
}
