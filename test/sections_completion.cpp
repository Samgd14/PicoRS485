// COMPLETION
//
#include "suite.h"

void suite_completion() {
    // --- 47. completion: reported once the wire is clear ---------------------
    CASE("completion: reported once the wire is clear");
    reset_stubs();
    {
        PicoRS485 ch(0, 1, 2, 3);
        CHECK(ch.init(pio0, 921600) == PicoRS485::ok);
        CHECK(!ch.tx_completion_pending());
        CHECK(ch.last_sent_at_us() == 0);
        raise_pio_irq(pio0, k_tx_done_irq);                                  // nothing owed: does nothing
        CHECK(g_sent_calls == 0);

        const uint8_t payload[2] = {0xC3, 0x5A};
        CHECK(ch.send(payload, 2, sent_probe, &ch) == PicoRS485::ok);
        CHECK(ch.tx_completion_pending());

        dma_run();                                     // the words are in the FIFO now

        // The bit above the character is the release mark the TX program shifts into X: it
        // must be absent from every character of a burst except the last one.
        const pico_rs485_packet_config cfg = ch.get_packet_config();
        const uint32_t release_mark =
            1u << (1 + cfg.data_bits + cfg.parity_bits + cfg.stop_bits);
        uint32_t word = 0;
        CHECK(run_tx(pio0, 0, &word));
        CHECK((word & release_mark) == 0);             // not the last character: bus held

        CHECK(run_tx(pio0, 0, &word));                 // last character out, DE released
        CHECK((word & release_mark) != 0);             // because the payload says so
        raise_pio_irq(pio0, k_tx_done_irq);
        CHECK(g_sent_calls == 1);
        CHECK(g_sent_id == 1);                         // the first burst of this channel
        CHECK(g_sent_user == &ch);
        CHECK(!ch.tx_completion_pending());
        CHECK(ch.last_sent_at_us() != 0);

        raise_pio_irq(pio0, k_tx_done_irq);                                  // and it does not report twice
        CHECK(g_sent_calls == 1);

        // A burst of one character carries the mark too, or the bus would never be released.
        CASE("completion: a single-character burst marks its only character");
        CHECK_EQ(ch.send(payload, 1), (int)PicoRS485::ok);
        CHECK(run_tx(pio0, 0, &word));                 // the one and only character
        CHECK((word & release_mark) != 0);
        CHECK((word & 1u) == 0u);                      // still a well-formed character: start bit
    }

    // --- 48. completion: the slot, and what a callback-less send owes --------
    CASE("completion: the slot, and what a callback-less send owes");
    reset_stubs();
    {
        PicoRS485 ch(0, 1, 2, 3);
        CHECK(ch.init(pio0, 921600) == PicoRS485::ok);
        const uint8_t payload[1] = {0x11};

        CHECK(ch.send(payload, 1, sent_probe, nullptr) == PicoRS485::ok);
        drain_tx(pio0, 0);                             // the wire is clear now
        // The wire is clear, so this is not err_in_flight: what holds the slot
        // is the completion the first send() owes and the interrupt has not reported.
        CHECK(ch.send(payload, 1, sent_probe, nullptr) == PicoRS485::err_incomplete_tx);
        raise_pio_irq(pio0, k_tx_done_irq);
        CHECK(g_sent_calls == 1);
        CHECK(!ch.tx_completion_pending());

        // With that reported, the slot is free and the id counts on.
        CHECK(ch.send(payload, 1, sent_probe, nullptr) == PicoRS485::ok);
        drain_tx(pio0, 0);
        raise_pio_irq(pio0, k_tx_done_irq);
        CHECK(g_sent_calls == 2);
        CHECK(g_sent_id == 2);

        // A send that asks for nothing still holds the slot until its end-of-burst interrupt
        // reports it: the interrupt clears the latch, callback or not.
        drain_tx(pio0, 0);
        CHECK(ch.send(payload, 1) == PicoRS485::ok);
        CHECK(!ch.tx_completion_pending());
        drain_tx(pio0, 0);
        raise_pio_irq(pio0, k_tx_done_irq);            // reported, though nothing is called
        CHECK(ch.send(payload, 1) == PicoRS485::ok);
        drain_tx(pio0, 0);
        raise_pio_irq(pio0, k_tx_done_irq);            // and reported again
        CHECK(g_sent_calls == 2);
        CHECK(ch.last_sent_at_us() != 0);
        CHECK(g_sent_calls == 2);
        CHECK(ch.last_sent_at_us() != 0);

        // The id counts every burst, the two callback-less ones above included: four
        // sends have gone out, so the next callback carries id 5.
        CASE("completion: the id counts callback-less bursts too");
        CHECK_EQ(ch.send(payload, 1, sent_probe, nullptr), (int)PicoRS485::ok);
        drain_tx(pio0, 0);
        raise_pio_irq(pio0, k_tx_done_irq);
        CHECK_EQ(g_sent_id, 5);
    }

    // --- 49. completion: teardown drops an unobserved one --------------------
    CASE("completion: teardown drops an unobserved one");
    reset_stubs();
    {
        PicoRS485 ch(0, 1, 2, 3);
        CHECK(ch.init(pio0, 921600) == PicoRS485::ok);
        const uint8_t payload[1] = {0x22};
        CHECK(ch.send(payload, 1, sent_probe, nullptr) == PicoRS485::ok);
        drain_tx(pio0, 0);                             // the wire is clear...
        CHECK(ch.tx_completion_pending());             // ...and still owed
    }                                                  // scope ends: teardown runs
    CHECK(g_sent_calls == 0);                          // so nothing was reported
    CHECK(g_dma_claimed == 0);

    // --- 50. completion: the timestamp is latched when observed --------------
    CASE("completion: the timestamp is latched when observed");
    reset_stubs();
    {
        PicoRS485 ch(0, 1, 2, 3);
        CHECK(ch.init(pio0, 921600) == PicoRS485::ok);
        const uint8_t payload[1] = {0x33};

        g_time_us = 0;
        CHECK(ch.send(payload, 1, sent_probe, nullptr) == PicoRS485::ok);
        drain_tx(pio0, 0);
        g_time_us = 100000;                            // long after it went out
        raise_pio_irq(pio0, k_tx_done_irq);
        CHECK(g_sent_calls == 1);
        CHECK(ch.last_sent_at_us() >= 100000);         // so it was read here,
        CHECK(ch.last_sent_at_us() <= 102000);         // not at send() time
        const uint32_t stamped = ch.last_sent_at_us();
        g_time_us = 900000;
        raise_pio_irq(pio0, k_tx_done_irq);                                  // idle: it must not restamp
        CHECK_EQ(ch.last_sent_at_us(), stamped);
    }

    // --- 51. completion: the callback may queue the next burst ---------------
    CASE("completion: the callback may queue the next burst");
    reset_stubs();
    {
        PicoRS485 ch(0, 1, 2, 3);
        CHECK(ch.init(pio0, 921600) == PicoRS485::ok);
        const uint8_t payload[1] = {0x44};
        CHECK(ch.send(payload, 1, sent_then_send, &ch) == PicoRS485::ok);
        drain_tx(pio0, 0);
        raise_pio_irq(pio0, k_tx_done_irq);
        CHECK(g_sent_calls == 1);
        CHECK(g_sent_resends == 1);                    // its own send went through
        CHECK(ch.is_transmitting());                   // and is on its way
        CHECK(!ch.tx_completion_pending());            // the re-send asked for nothing
    }

    // --- 52. completion: the end-of-burst interrupt reports the transmission ----
    CASE("completion: the end-of-burst interrupt reports the transmission");
    reset_stubs();
    {
        PicoRS485 ch(0, 1, 2, 3);
        bring_up(ch);
        static int reported = 0;
        const uint8_t payload[1] = {0x11};

        CASE("completion: a burst reports itself when the wire goes quiet");
        CHECK_EQ(ch.send(payload, 1, [](void *, uint32_t) { ++reported; }), (int)PicoRS485::ok);
        CHECK(ch.tx_completion_pending());
        CHECK_EQ(reported, 0);
        // The TX machine raises its own IRQ as DE drops; both flags share the block's IRQ 0 line.
        g_time_us += 100;
        raise_pio_irq(pio0, k_tx_done_irq);
        CHECK_EQ(reported, 1);                            // no poll was needed
        CHECK(!ch.tx_completion_pending());
        CHECK(ch.last_sent_at_us() != 0);
        CHECK(!pio_interrupt_get(pio0, k_tx_done_irq));   // cleared, so the line cannot re-enter

        CASE("completion: a flag with nothing owed is cleared, not reported");
        raise_pio_irq(pio0, k_tx_done_irq);
        CHECK_EQ(reported, 1);
        CHECK(!pio_interrupt_get(pio0, k_tx_done_irq));

        CASE("completion: the RX flag still works with the TX flag wired");
        const int sem_before = g_sem_releases;
        deliver_rx(pio0, 1, 0x5Au | (1u << 8), 9);
        end_burst();
        CHECK_EQ(g_sem_releases, sem_before + 1);   // a published burst wakes a waiting receive()
        uint8_t got[2] = {};
        CHECK_EQ(ch.receive_for(got, sizeof(got), 5000), 1);
        CHECK_EQ(got[0], 0x5Au);
    }
}
