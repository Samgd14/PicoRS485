// TRANSMIT
//
#include "suite.h"

void suite_transmit() {
    // --- 30. transmit: one burst, framed and streamed end to end -------------
    CASE("transmit: one burst, framed and streamed end to end");
    reset_stubs();
    {
        PicoRS485 ch(0, 1, 2, 3);
        bring_up(ch);
        CHECK_EQ(g_dma_claimed, 2);                // one channel each way

        // Each word is start = 0, the data LSB first at bits 1-8, the stop bit at
        // bit 9 and nothing above it - except the last word of a burst, which also
        // carries the release mark the TX program shifts into X. The expected words
        // are written out rather than recomputed, so a driver wrong in the same way
        // as its own formula cannot pass.
        const uint8_t  payload[4] = {0x55, 0x00, 0xFF, 0x3C};
        const uint32_t framed[4]  = {0x2AA, 0x200, 0x3FE, 0x278};
        const uint32_t release_mark = 1u << 10;   // char_bits, for the default 8N1

        CHECK_EQ(ch.send(payload, 4), (int)PicoRS485::ok);
        CHECK(ch.is_transmitting());               // queued, nothing clocked out yet

        for (int i = 0; i < 4; ++i) {
            CASE("transmit: burst byte %d (0x%02X)", i, payload[i]);
            uint32_t word = 0;
            CHECK(run_tx(pio0, SM_TX, &word));
            CHECK_EQ(word & ~release_mark, framed[i]);
            CHECK(((word & release_mark) != 0) == (i == 3));   // only the last character
        }
        CASE("transmit: the burst is drained");
        uint32_t spare = 0;
        CHECK(!run_tx(pio0, SM_TX, &spare));       // nothing left to send
        raise_pio_irq(pio0, k_tx_done_irq);        // the interrupt reports it
        CHECK(!ch.is_transmitting());              // and nothing is owed
        CHECK_EQ(ch.burst_duration_us(4), 92);     // 4 * (10 + 8 + 3) bits at 921600
    }

    // --- 31. transmit: the length boundary and the pacing --------------------
    CASE("transmit: the length boundary and the pacing");
    reset_stubs();
    {
        PicoRS485 ch(0, 1, 2, 3);
        bring_up(ch);
        uint8_t big[PicoRS485::max_burst_bytes] = {};
        CASE("transmit: a burst of exactly the maximum");
        CHECK_EQ(ch.send(big, PicoRS485::max_burst_bytes), (int)PicoRS485::ok);
        CHECK_EQ(g_dma[SM_RX].size, (int)DMA_SIZE_32);   // the receiver, by init
        CHECK_EQ(g_dma[SM_TX].size, (int)DMA_SIZE_32);   // the transmitter, by send
        drain_tx(pio0, SM_TX);
        // The burst is bigger than the FIFO, so a channel paced on the wrong DREQ
        // would push past it: the model counts that rather than absorbing it.
        CHECK_EQ(g_model_overrun, 0);
        uint32_t spare = 0;
        CHECK(!run_tx(pio0, SM_TX, &spare));             // exactly that many went out
        // send() must not re-target the setters either.
        CHECK(g_set_tx_sm == SM_TX && g_set_rx_sm == SM_RX);
        CHECK(g_set_rxidle_sm == SM_RXIDLE && g_set_gap_sm == SM_GAP);
    }

    // --- 32. transmit: framing across configurations -------------------------
    CASE("transmit: framing across configurations");
    // Whole expected words again: start 0, data LSB first, parity above the data
    // when configured, then the stop bit(s), and nothing above them.
    {
        struct tx_row { const char *what; pico_rs485_packet_config pkt; uint8_t byte; uint32_t word; };
        const tx_row rows[] = {
            {"8N1 0x55",          {8, 0, 1, 8, 16},              0x55, 0x2AA},
            {"8N1 0x00",          {8, 0, 1, 8, 16},              0x00, 0x200},
            {"8N1 0xFF",          {8, 0, 1, 8, 16},              0xFF, 0x3FE},
            {"8N1 0x3C",          {8, 0, 1, 8, 16},              0x3C, 0x278},
            {"8E1 even parity 0", {8, 1, 1, 8, 16, true}, 0x03, 0x406},
            {"8O1 odd parity 1",  {8, 1, 1, 8, 16, false},  0x03, 0x606},
            {"7E2 two stop bits", {7, 1, 2, 8, 16, true}, 0x01, 0x702},
            {"7N1 masks bit 7",   {7, 0, 1, 8, 16},              0x80, 0x100},
            {"5N1 masks bits 5-7",{5, 0, 1, 8, 16},              0xFF, 0x07E},
            // Parity is computed over the masked byte, so a bit above data_bits
            // must not move it.
            {"7E1 masked parity", {7, 1, 1, 8, 16, true}, 0x80, 0x200},
            {"5E1 masked parity", {5, 1, 1, 8, 16, true}, 0xFF, 0x0FE},
        };
        for (const tx_row &r : rows) {
            CASE("transmit: TX %s", r.what);
            reset_stubs();
            PicoRS485 ch(0, 1, 2, 3);
            bring_up(ch, pio0, 9600, r.pkt);
            CHECK_EQ(ch.send(&r.byte, 1), (int)PicoRS485::ok);
            uint32_t word = 0;
            CHECK(run_tx(pio0, SM_TX, &word));
            // A single-character burst: its one character is last, so its word also carries
            // the release mark above the character.
            const uint32_t mark =
                1u << (1 + r.pkt.data_bits + r.pkt.parity_bits + r.pkt.stop_bits);
            CHECK_EQ(word & ~mark, r.word);
            CHECK((word & mark) != 0);
        }
    }

    // --- 33. transmit: refusals ----------------------------------------------
    CASE("transmit: refusals");
    reset_stubs();
    {
        PicoRS485 ch(0, 1, 2, 3);
        const uint8_t payload[1] = {0x00};
        CASE("transmit: send before init");
        CHECK_EQ(ch.send(payload, 1), (int)PicoRS485::err_not_initialized);
        bring_up(ch);
        CASE("transmit: len 0");
        CHECK_EQ(ch.send(payload, 0), (int)PicoRS485::err_argument);
        CASE("transmit: len max+1");
        CHECK_EQ(ch.send(payload, PicoRS485::max_burst_bytes + 1), (int)PicoRS485::err_argument);
        CASE("transmit: null data");
        CHECK_EQ(ch.send(nullptr, 1), (int)PicoRS485::err_argument);
        CHECK_EQ(ch.burst_duration_us(0), 0);
        // The clamp is only observable for a length whose 64-bit product wraps back *below* the
        // saturation point: for smaller absurd lengths the wrapped product still exceeds it, so a
        // check like (size_t)-1 passes with or without the clamp. 17568327689247192016 is
        // 20 * 2^64 / 21 rounded, i.e. len * (10 + 8 + 3) wraps to 16 bits in a uint64.
        CASE("transmit: burst_duration_us saturates rather than wraps");
        CHECK_EQ(ch.burst_duration_us((size_t)17568327689247192016ull), 0xffffffffu);
        CASE("transmit: burst_duration_us saturates for any absurd length");
        CHECK_EQ(ch.burst_duration_us((size_t)-1), 0xffffffffu);

        // A burst arriving blocks a send; so does one already queued.
        CASE("transmit: send while a burst is arriving");
        deliver_rx(pio0, SM_RX, 0x5Au | (1u << 8), 9);
        CHECK(ch.is_receiving());
        CHECK_EQ(ch.send(payload, 1), (int)PicoRS485::err_in_flight);
        end_burst();
        uint8_t got[4] = {};
        CHECK_EQ(ch.receive(got, sizeof(got)), 1);
        CHECK_EQ(ch.send(payload, 1), (int)PicoRS485::ok);
    }

    // --- 34. transmit: a second send while the first is only queued ----------
    CASE("transmit: a second send while the first is only queued");
    reset_stubs();
    {
        PicoRS485 ch(0, 1, 2, 3);
        CHECK(ch.init(pio0, 921600) == PicoRS485::ok);
        const uint8_t payload[2] = {0xAA, 0xBB};
        CHECK(ch.send(payload, 2) == PicoRS485::ok);
        CHECK(ch.send(payload, 2) == PicoRS485::err_in_flight);

        // Move the burst into the FIFO but leave it there: the DMA is finished, so
        // only the FIFO level can say the burst is still going out.
        dma_run();
        CHECK(ch.is_transmitting());
        CHECK(ch.send(payload, 2) == PicoRS485::err_in_flight);

        uint32_t word = 0;
        CHECK(run_tx(pio0, 0, &word));
        CHECK(run_tx(pio0, 0, &word));
        raise_pio_irq(pio0, k_tx_done_irq);      // the interrupt reports it
        CHECK(!ch.is_transmitting());            // drained, and nothing owed
    }
}
