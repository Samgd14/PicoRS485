// CONFIG
//
#include "suite.h"

void suite_config() {
    // --- 21. config: set_packet_config() and get_packet_config() -------------
    CASE("config: set_packet_config() and get_packet_config()");
    // Before init: refused, and nothing is programmed or recorded.
    reset_stubs();
    {
        PicoRS485 ch(0, 1, 2, 3);
        CHECK(same_packet(ch.get_packet_config(), pico_rs485_packet_config{}));   // default framing
        CHECK(ch.set_packet_config({7, 1, 2, 1, 6}) == PicoRS485::err_not_initialized);
        CHECK(g_set_tx_calls == 0 && g_set_rx_calls == 0 &&
              g_set_rxidle_calls == 0 && g_set_gap_calls == 0);
        CHECK(same_packet(ch.get_packet_config(), pico_rs485_packet_config{}));
    }
    // init() records the config it configured the machines from.
    reset_stubs();
    {
        PicoRS485 ch(0, 1, 2, 3);
        CHECK(ch.init(pio0, 921600, {7, 1, 2, 1, 6}) == PicoRS485::ok);
        CHECK(same_packet(ch.get_packet_config(), {7, 1, 2, 1, 6}));
        // A brace config written without a mode keeps meaning even parity.
        CHECK(ch.get_packet_config().even_parity);
        CHECK(pico_rs485_packet_config{}.even_parity);
        // init() goes through set_packet_config(), so the setters ran once for
        // the config being initialised
        CHECK(g_set_tx_calls == 1 && g_set_rx_calls == 1 &&
              g_set_rxidle_calls == 1 && g_set_gap_calls == 1);
        CHECK(g_set_tx_bits == 11 && g_set_rx_bits == 9 &&
              g_set_rxidle_steps == 3 && g_set_gap_bits == 1);
    }
    // A refused config changes nothing: neither the machines nor the record.
    reset_stubs();
    {
        PicoRS485 ch(0, 1, 2, 3);
        CHECK(ch.init(pio0, 921600, {7, 1, 2, 1, 6}) == PicoRS485::ok);
        int tx_before = g_set_tx_calls, rx_before = g_set_rx_calls;
        CHECK(ch.set_packet_config({4, 0, 1, 8, 16}) == PicoRS485::err_packet);
        CHECK(g_set_tx_calls == tx_before && g_set_rx_calls == rx_before);
        CHECK(same_packet(ch.get_packet_config(), {7, 1, 2, 1, 6}));
    }
    // An accepted config is recorded and pushed to all four machines.
    reset_stubs();
    {
        PicoRS485 ch(0, 1, 2, 3);
        CHECK(ch.init(pio0, 921600) == PicoRS485::ok);
        CHECK(same_packet(ch.get_packet_config(), pico_rs485_packet_config{}));
        CHECK(g_set_tx_calls == 1);   // init() applied the default config

        CHECK(ch.set_packet_config({7, 1, 2, 1, 6}) == PicoRS485::ok);
        CHECK(g_set_tx_calls == 2 && g_set_rx_calls == 2 &&
              g_set_rxidle_calls == 2 && g_set_gap_calls == 2);
        CHECK(g_set_tx_bits == 1 + 7 + 1 + 2);   // 11, recomputed not copied
        CHECK(g_set_rx_bits == 7 + 1 + 1);       // 9
        CHECK(g_set_rxidle_steps == 3);
        CHECK(g_set_gap_bits == 1);
        CHECK(same_packet(ch.get_packet_config(), {7, 1, 2, 1, 6}));

        // and it is repeatable
        CHECK(ch.set_packet_config({7, 1, 2, 1, 6}) == PicoRS485::ok);
        CHECK(g_set_tx_calls == 3 && g_set_gap_bits == 1);
        CHECK(ch.is_valid());   // still live, nothing re-claimed
        CHECK(g_claim == 4);
    }

    // --- 22. config: is_clock_config_valid() ---------------------------------
    CASE("config: is_clock_config_valid()");
    {
        struct clock_row { pico_rs485_clock_config clk; bool want; const char *what; };
        const clock_row rows[] = {
            {{},                              false, "default-constructed"},
            {{125000000, 921600, 16, 244},    true,  "the divider that rate derives"},
            {{125000000, 921600, 0, 0},       false, "zero divider"},
            {{125000000, 921600, 0, 1},       false, "a divider the rate does not derive"},
            {{125000000, 921600, 65535, 255}, false, "a divider no rate could derive"},
            {{125000000, 921600, 16, 245},    false, "right integer, wrong fraction"},
            {{125000000, 921600, 17, 0},      false, "a plain wrong divider"},
            {{0, 921600, 16, 244},            false, "unfilled sys_hz"},
            {{125000000, 0, 16, 244},         false, "unfilled baudrate"},
        };
        for (const clock_row &r : rows) {
            CASE("clock config: %s", r.what);
            CHECK_EQ(PicoRS485::is_clock_config_valid(r.clk), r.want);
        }
    }

    // --- 23. config: is_packet_config_valid() ranges -------------------------
    CASE("config: is_packet_config_valid() ranges");
    {
        struct packet_row { pico_rs485_packet_config pkt; bool want; const char *what; };
        const packet_row rows[] = {
            {{},                                  true,  "the defaults"},
            {{5, 0, 1, 1, 5},                     true,  "narrowest framing"},
            {{8, 1, 2, 27, 32},                   true,  "widest framing the window allows"},
            {{8, 1, 1, 8, 16, false},        true,  "odd parity"},
            {{4, 0, 1, 8, 16},                    false, "data_bits 4"},
            {{9, 0, 1, 8, 16},                    false, "data_bits 9"},
            {{8, 2, 1, 8, 16},                    false, "parity_bits 2"},
            {{8, 0, 0, 8, 16},                    false, "stop_bits 0"},
            {{8, 0, 3, 8, 16},                    false, "stop_bits 3"},
            {{8, 0, 1, 8, 0},                     false, "rx_idle_bits_thresh 0"},
            {{8, 0, 1, 8, 65},                    false, "rx_idle_bits_thresh 65"},
            // The gap timer counts down from tx_gap_bits - 1, so it cannot express a zero.
            {{8, 0, 1, 0, 16},                    false, "tx_gap_bits 0"},
            {{8, 0, 1, 32, 8},                    false, "tx_gap_bits 32"},
            {{8, 0, 1, 8, 8},                     false, "threshold equal to the gap"},
            {{8, 0, 1, 12, 4},                    false, "gap wider than the threshold"},
            // The threshold clears the gap and the stop bits by three bit times: 8 + 1 + 3.
            {{8, 0, 1, 8, 12},                    true,  "the exact margin"},
            {{8, 0, 1, 8, 11},                    false, "one bit short of the margin"},
            // gap 31 with two stop bits needs 31 + 2 + 3 = 36 bit times; 32 is refused.
            {{8, 1, 2, 31, 32},                   false, "gap 31 with two stop bits"},
            // The bench split every burst at gap 16 against a 16 bit threshold, so it is refused.
            {{8, 1, 1, 16, 16},                   false, "the gap the bench split on"},

        };
        for (const packet_row &r : rows) {
            CASE("packet config: %s", r.what);
            CHECK_EQ(PicoRS485::is_packet_config_valid(r.pkt), r.want);
        }
    }

    // --- 24. config: clock derivation against the pre-refactor reference -----
    CASE("config: clock derivation against the pre-refactor reference");
    // The reference is the maths as it stood before the refactor, so this compares
    // two implementations rather than restating one. Every rate is put through the
    // public helper and through init(), where the program-init helpers record what
    // they were handed.
    const uint32_t rates[] = {50, 300, 1200, 9600, 19200, 38400, 57600, 115200,
                              230400, 460800, 921600, 1000000, 1500000, 4000000,
                              8000000, 12000000, 15625000, 100000000};
    const uint32_t sys_clocks[] = {1000000, 48000000, 125000000, 133000000,
                                   150000000, 200000000};
    for (uint32_t rate : rates) {
        for (uint32_t hz : sys_clocks) {
            CASE("config: %u baud at %u Hz", (unsigned)rate, (unsigned)hz);
            uint32_t ref_int = 0; uint8_t ref_frac = 0;
            const bool ref_ok = old_clock(rate, hz, ref_int, ref_frac);

            pico_rs485_clock_config clk{};
            const bool ok = PicoRS485::compute_clock_config(rate, hz, clk);
            CHECK_EQ(ok, ref_ok);
            // A refused rate leaves the caller's struct exactly as it was.
            if (!ok) CHECK(clk.sys_hz == 0 && clk.clkdiv_int == 0 && clk.clkdiv_frac8 == 0);
            if (ok) {
                CHECK_EQ(clk.clkdiv_int, ref_int);
                CHECK_EQ(clk.clkdiv_frac8, ref_frac);
                CHECK(clk.sys_hz == hz && clk.baudrate == rate);
                CHECK(PicoRS485::is_clock_config_valid(clk));   // init() would take it
            }

            g_sys_hz = hz;
            reset_stubs();
            {
                PicoRS485 ch(0, 1, 2, 3);
                const int rc = ch.init(pio0, rate);
                CHECK_EQ(rc == PicoRS485::ok, ref_ok);
                if (ref_ok) {
                    CHECK_EQ(g_tx.clkdiv_int, ref_int);
                    CHECK_EQ(g_tx.clkdiv_frac8, ref_frac);
                    CHECK_EQ(g_rx.clkdiv_int, ref_int);
                    CHECK_EQ(g_rx.clkdiv_frac8, ref_frac);
                    CHECK_EQ(g_rxidle.clkdiv_int, ref_int);
                    CHECK_EQ(g_rxidle.clkdiv_frac8, ref_frac);
                    CHECK_EQ(g_txgap.clkdiv_int, ref_int);
                    CHECK_EQ(g_txgap.clkdiv_frac8, ref_frac);
                }
            }
        }
    }
    g_sys_hz = 125000000;
    // The 2-arg overload derives against clock_get_hz(clk_sys), and a failed call
    // leaves the caller's struct untouched.
    {
        CASE("config: both overloads at 921600");
        pico_rs485_clock_config a{}, b{};
        CHECK_EQ(PicoRS485::compute_clock_config(921600, a),
                 PicoRS485::compute_clock_config(921600, 125000000, b));
        CHECK(a.clkdiv_int == b.clkdiv_int && a.clkdiv_frac8 == b.clkdiv_frac8);
    }
    {
        CASE("config: a refused rate writes nothing");
        pico_rs485_clock_config c{};
        c.clkdiv_int = 1234; c.clkdiv_frac8 = 56; c.baudrate = 7;
        CHECK(!PicoRS485::compute_clock_config(0, 125000000, c));
        CHECK(c.clkdiv_int == 1234 && c.clkdiv_frac8 == 56 && c.baudrate == 7);
        // The overflow input the reference refuses, with sys_hz checked too.
        pico_rs485_clock_config d{};
        CHECK(!PicoRS485::compute_clock_config(1, 134217729u, d));
        CHECK_EQ(d.sys_hz, 0);
    }
    // the PIO shape constants are public library facts, not function locals
    CHECK_EQ(PicoRS485::pio_cycles_per_bit, 8);
    CHECK_EQ(PicoRS485::clkdiv_frac_scale, 256);
    CHECK(PicoRS485::clkdiv_frac_scale == (1u << 8));   // 8 fractional bits

    // The range test multiplies instead of dividing, which shows only when clk_sys is not a whole
    // multiple of the cycle count: 12,000,001 / 8 truncates to 1,500,000, so the dividing form
    // refused this rate although the divider it needs is exactly 1.0. The reference above is the
    // old maths, so this cannot be compared against it.
    {
        CASE("config: a rate at the divider's floor with clk_sys not a multiple of the count");
        pico_rs485_clock_config clk{};
        CHECK(PicoRS485::compute_clock_config(1500000, 12000001u, clk));
        CHECK_EQ(clk.clkdiv_int, 1);
        CHECK_EQ(clk.clkdiv_frac8, 0);
        CHECK(PicoRS485::is_clock_config_valid(clk));
    }

    // --- 25. config: worst-case divider error stays inside the UART budget ----
    CASE("config: worst-case divider error stays inside the UART budget");
    for (uint32_t rate : rates) {
        pico_rs485_clock_config clk{};
        if (!PicoRS485::compute_clock_config(rate, 125000000, clk)) continue;
        double div = clk.clkdiv_int + clk.clkdiv_frac8 / 256.0;
        double actual = 125000000.0 / (div * 8.0);
        double err = (actual - rate) / rate * 100.0;
        if (err < 0) err = -err;
        CHECK(err < 2.0);
    }

    // --- 26. config: set_packet_config() refuses while anything is in flight ----
    CASE("config: set_packet_config() refuses while anything is in flight");
    reset_stubs();
    {
        PicoRS485 ch(0, 1, 2, 3);
        CHECK(ch.init(pio0, 921600) == PicoRS485::ok);

        // A queued burst counts as in flight until the interrupt reports it
        const uint8_t going_out[1] = {0x5A};
        CHECK(ch.send(going_out, 1) == PicoRS485::ok);
        CHECK(ch.is_transmitting());
        CHECK(ch.set_packet_config({7, 1, 1, 8, 16}) == PicoRS485::err_in_flight);
        raise_pio_irq(pio0, k_tx_done_irq);         // ended, and reported
        drain_tx(pio0, SM_TX);                     // and its word is gone
        CHECK(!ch.is_transmitting());

        deliver_rx(pio0, 1, 0x10u | (1u << 8), 9);  // mid-burst
        CHECK(ch.set_packet_config({7, 1, 1, 8, 16}) == PicoRS485::err_in_flight);
        raise_pio_irq(pio0, k_rx_done_irq);                     // complete, but still queued
        CHECK(ch.set_packet_config({7, 1, 1, 8, 16}) == PicoRS485::err_in_flight);

        uint8_t got[4] = {};
        CHECK(ch.receive(got, sizeof(got)) == 1);
        CHECK(ch.set_packet_config({7, 1, 1, 8, 16}) == PicoRS485::ok);
        CHECK(same_packet(ch.get_packet_config(), {7, 1, 1, 8, 16}));

        // The table was rebuilt with the new framing, not the old.
        const uint8_t payload[1] = {0x01};
        CHECK(ch.send(payload, 1) == PicoRS485::ok);
        uint32_t word = 0;
        CHECK(run_tx(pio0, 0, &word));
        CHECK(((word >> 1) & 0x7Fu) == 0x01u);
        CHECK(((word >> 8) & 1u) == 1u);            // parity sits at bit 8 for 7 data bits
        CHECK(((word >> 10) & 1u) == 1u);           // nothing above the stop bit except
                                                    // the release mark of a last character
        CHECK(((word >> 11) == 0u));
    }

    // --- 27. config: a refused config change leaves everything as it was -----
    CASE("config: a refused config change leaves everything as it was");
    reset_stubs();
    {
        PicoRS485 ch(0, 1, 2, 3);
        CHECK(ch.init(pio0, 921600) == PicoRS485::ok);
        const pico_rs485_packet_config before = ch.get_packet_config();
        CHECK(g_set_tx_calls == 1 && g_set_rx_calls == 1);   // applied once, by init

        const uint8_t payload[1] = {0x5A};
        CHECK(ch.send(payload, 1) == PicoRS485::ok);   // a burst is going out
        CHECK(ch.set_packet_config({7, 1, 2, 1, 6}) == PicoRS485::err_in_flight);
        CHECK(ch.set_packet_config({7, 1, 2, 1, 6}) == PicoRS485::err_in_flight);

        // The refusal must not have recorded the config, rebuilt the table or
        // touched any machine: the contract says nothing changes without ok.
        CHECK(same_packet(ch.get_packet_config(), before));
        CHECK(g_set_tx_calls == 1 && g_set_rx_calls == 1);
        CHECK(g_set_rxidle_calls == 1 && g_set_gap_calls == 1);
    }

    // The threshold reaches the PIO in two-bit steps, rounded up.
    reset_stubs();
    {
        PicoRS485 ch(0, 1, 2, 3);
        bring_up(ch);
        pico_rs485_packet_config pkt{};
        pkt.rx_idle_bits_thresh = 17;
        CASE("config: an odd threshold rounds up to the next step");
        CHECK_EQ(ch.set_packet_config(pkt), (int)PicoRS485::ok);
        CHECK_EQ(g_set_rxidle_steps, 9);                  // 17 bit times -> 18, so 9 steps
        pkt.rx_idle_bits_thresh = 64;
        CASE("config: the top of the range is 32 steps");
        CHECK_EQ(ch.set_packet_config(pkt), (int)PicoRS485::ok);
        CHECK_EQ(g_set_rxidle_steps, 32);
    }

    // --- 28. config: the idle threshold must clear the driver's own gap ----
    CASE("config: the idle threshold must clear the driver's own gap");
    reset_stubs();
    {
        PicoRS485 ch(0, 1, 2, 3);
        const pico_rs485_packet_config def;
        CASE("config: the idle threshold must clear the driver's own gap");
        CHECK(def.rx_idle_bits_thresh > def.tx_gap_bits);      // the shipped defaults hold it
        bring_up(ch);
        CHECK(ch.get_packet_config().rx_idle_bits_thresh > ch.get_packet_config().tx_gap_bits);

        // Equal windows race; a gap wider than the window splits a burst the driver
        // itself sent.
        pico_rs485_packet_config equal = def;
        equal.rx_idle_bits_thresh = 8;               // the pair that used to ship
        equal.tx_gap_bits  = 8;
        CASE("config: equal windows are refused");
        CHECK_EQ(ch.set_packet_config(equal), (int)PicoRS485::err_packet);
        pico_rs485_packet_config inverted = def;
        inverted.rx_idle_bits_thresh = 4;            // a gap wider than the threshold
        inverted.tx_gap_bits  = 12;
        CASE("config: a gap wider than the window is refused");
        CHECK_EQ(ch.set_packet_config(inverted), (int)PicoRS485::err_packet);

        // The gap timer counts down from tx_gap_bits - 1, so a zero gap would wrap the
        // counter rather than end the gap. It is refused rather than clamped.
        pico_rs485_packet_config no_gap = def;
        no_gap.tx_gap_bits = 0;
        CASE("config: a zero gap is refused");
        CHECK_EQ(ch.set_packet_config(no_gap), (int)PicoRS485::err_packet);

        // A refused config is not recorded: the last good one still stands.
        CASE("config: a refused window is not recorded");
        CHECK(same_packet(ch.get_packet_config(), def));

        // The window clears the gap and the stop bits by three bit times: 8 + 1 + 3 = 12.
        pico_rs485_packet_config minimal = def;
        minimal.rx_idle_bits_thresh = 12;
        minimal.tx_gap_bits  = 8;
        CASE("config: the exact margin is accepted and one bit less is not");
        CHECK_EQ(ch.set_packet_config(minimal), (int)PicoRS485::ok);
        CHECK_EQ(ch.get_packet_config().rx_idle_bits_thresh, 12);
        minimal.rx_idle_bits_thresh = 11;
        CHECK_EQ(ch.set_packet_config(minimal), (int)PicoRS485::err_packet);

        // tx_gap_bits really is what the burst estimate follows.
        CASE("config: the estimate follows the live gap");
        const uint32_t narrow = ch.burst_duration_us(100);
        pico_rs485_packet_config wider = def;
        wider.rx_idle_bits_thresh = 16;
        wider.tx_gap_bits  = 12;
        CHECK_EQ(ch.set_packet_config(wider), (int)PicoRS485::ok);
        CHECK(ch.burst_duration_us(100) > narrow);
    }

    // --- 29. config: set_packet_config() refuses a character that is already arriving ----
    CASE("config: set_packet_config() refuses a character that is already arriving");
    reset_stubs();
    {
        PicoRS485 ch(0, 1, 2, 3);
        bring_up(ch);
        const pico_rs485_packet_config other{7, 1, 1, 8, 16, true};

        // A character on the wire is being received even before a word has moved, so the pin alone
        // makes is_receiving() true - it is the same state the refusal below comes from.
        g_value[3] = true;                        // pin_rx_act
        CHECK(ch.is_receiving());
        CHECK_EQ(ch.set_packet_config(other), (int)PicoRS485::err_in_flight);
        CHECK_EQ((int)ch.get_packet_config().data_bits, 8);   // nothing was applied

        g_value[3] = false;
        CHECK(!ch.is_receiving());
        CHECK_EQ(ch.set_packet_config(other), (int)PicoRS485::ok);
        CHECK_EQ((int)ch.get_packet_config().data_bits, 7);

        // The same state holds a send() off: driving DE into a burst the peer is sending would
        // corrupt both.
        const uint8_t payload[1] = {0x11};
        g_value[3] = true;
        CHECK_EQ(ch.send(payload, 1), (int)PicoRS485::err_in_flight);
        g_value[3] = false;
        CHECK_EQ(ch.send(payload, 1), (int)PicoRS485::ok);
    }
}
