// FAILURE
//
#include "suite.h"

void suite_failure() {
    // --- 10. failure: an unusable baudrate -----------------------------------
    CASE("failure: an unusable baudrate");
    for (uint32_t bad : {(uint32_t)0, (uint32_t)1, (uint32_t)0xFFFFFFFFu,
                         (uint32_t)20000000u}) {
        CASE("failure: baudrate %u", (unsigned)bad);
        reset_stubs();
        {
            PicoRS485 ch(0, 1, 2, 3);
            expect_refused_clean(ch, ch.init(pio0, bad), PicoRS485::err_baudrate);
        }
        CHECK(g_clear_mem == 0 && g_unclaim == 0);   // teardown stayed a no-op too
    }

    // --- 11. failure: a pin outside the bank ---------------------------------
    CASE("failure: a pin outside the bank");
    {   // The constructor accepts anything; the range check belongs to init().
        struct pin_case { uint tx, rx, de, act; const char *which; };
        const pin_case cases[] = {
            {48, 1, 2, 3, "tx"}, {0, 48, 2, 3, "rx"},
            {0, 1, 48, 3, "de"}, {0, 1, 2, 48, "act"},
        };
        for (const pin_case &c : cases) {
            CASE("failure: pin_%s = %d", c.which, NUM_BANK0_GPIOS);
            reset_stubs();
            {
                PicoRS485 ch(c.tx, c.rx, c.de, c.act);
                expect_refused_clean(ch, ch.init(pio0, 921600), PicoRS485::err_pin);
            }
            CHECK(g_claim == 0 && g_enabled_n == 0 && g_disabled_n == 0 && g_unclaim == 0);
        }
    }

    // --- 12. failure: an unusable packet or clock config ---------------------
    CASE("failure: an unusable packet or clock config");
    for (pico_rs485_packet_config p : {pico_rs485_packet_config{4, 0, 1, 8, 16}, pico_rs485_packet_config{8, 0, 1, 32, 8},
                            pico_rs485_packet_config{8, 2, 1, 8, 16}, pico_rs485_packet_config{8, 0, 3, 8, 16}}) {
        CASE("failure: packet %u/%u/%u/%u/%u", p.data_bits, p.parity_bits, p.stop_bits,
             p.rx_idle_bits_thresh, p.tx_gap_bits);
        reset_stubs();
        PicoRS485 ch(0, 1, 2, 3);
        expect_refused_clean(ch, ch.init(pio0, 921600, p), PicoRS485::err_packet);
    }
    reset_stubs();
    {
        PicoRS485 ch(0, 1, 2, 3);
        CASE("failure: empty clock config");
        expect_refused_clean(ch, ch.init(pio0, pico_rs485_clock_config{}), PicoRS485::err_clock);
        CASE("failure: zero divider");
        expect_refused_clean(ch, ch.init(pio0, pico_rs485_clock_config{125000000, 921600, 0, 0}),
                             PicoRS485::err_clock);
        // An unfilled sys_hz or baudrate is refused even though only the divider
        // reaches the hardware.
        CASE("failure: unfilled sys_hz");
        expect_refused_clean(ch, ch.init(pio0, pico_rs485_clock_config{0, 921600, 16, 244}),
                             PicoRS485::err_clock);
        CASE("failure: unfilled baudrate");
        expect_refused_clean(ch, ch.init(pio0, pico_rs485_clock_config{125000000, 0, 16, 244}),
                             PicoRS485::err_clock);
        CHECK(!ch.is_valid());
    }

    // --- 13. failure: not a usable PIO ---------------------------------------
    CASE("failure: not a usable PIO");
    reset_stubs();
    {
        PicoRS485 ch(0, 1, 2, 3);
        CHECK_EQ(ch.is_pio_valid(nullptr), (int)PicoRS485::err_pio);
        CHECK_EQ(ch.is_pio_valid((PIO)(uintptr_t)0xBAD), (int)PicoRS485::err_pio);
        expect_refused_clean(ch, ch.init(nullptr, 921600), PicoRS485::err_pio);
        expect_refused_clean(ch, ch.init((PIO)(uintptr_t)0xBAD, 921600), PicoRS485::err_pio);
        CHECK(g_claim == 0 && g_tx.calls == 0 && g_enabled_n == 0);
    }

    // --- 14. failure: a state machine is already claimed ---------------------
    CASE("failure: a state machine is already claimed");
    reset_stubs();
    pio_sm_claim(pio0, 0);                   // another library owns SM0 here
    int claim_base = g_claim;
    {
        PicoRS485 ch(0, 1, 2, 3);
        CASE("failure: SM0 claimed by another user");
        CHECK_EQ(ch.is_pio_valid(pio0), (int)PicoRS485::err_state_machine);
        CHECK_EQ(ch.init(pio0, 921600), (int)PicoRS485::err_state_machine);
        CHECK(g_claim == claim_base);        // refused before claiming anything
        CHECK(!ch.is_valid());
        // ...and the caller's own search finds the other block
        PIO picked = nullptr;
        for (uint i = 0; i < NUM_PIOS; ++i) {
            PIO candidate = pio_get_instance(i);
            if (ch.is_pio_valid(candidate) == PicoRS485::ok) { picked = candidate; break; }
        }
        CHECK(picked == pio1);
        bring_up(ch, picked);
        CHECK(saw_all_four(g_enabled, g_enabled_n));
    }

    // --- 15. failure: program load, at each of the four loads ----------------
    CASE("failure: program load, at each of the four loads");
    for (int fail_at = 1; fail_at <= 4; ++fail_at) {
        CASE("failure: pio_add_program call %d refuses", fail_at);
        reset_stubs();
        g_add_fail_at = fail_at;
        {
            PicoRS485 ch(0, 1, 2, 3);
            expect_refused_released(ch, ch.init(pio0, 921600),
                                    PicoRS485::err_program_load);
        }
        CHECK_EQ(g_disabled_n, 4);   // the destructor added nothing
    }

    // --- 16. failure: the SDK refused a machine configuration ----------------
    CASE("failure: the SDK refused a machine configuration");
    // A refused pio_sm_init() on the first helper: reported, not discarded.
    reset_stubs();
    g_sm_ok = false;
    {
        PicoRS485 ch(0, 1, 2, 3);
        CASE("failure: pio_sm_init refuses");
        expect_refused_released(ch, ch.init(pio0, 921600), PicoRS485::err_sm_config);
        CHECK_EQ(g_tx.calls, 1);                       // reached the first helper
        CHECK(g_rx.calls == 0 && g_rxidle.calls == 0 && g_txgap.calls == 0);
        CHECK_EQ(g_enabled_n, 0);                      // nothing was enabled
    }
    // A refused pin-direction call is the same failure, one step earlier.
    reset_stubs();
    g_pindir_ok = false;
    {
        PicoRS485 ch(0, 1, 2, 3);
        CASE("failure: pio_sm_set_consecutive_pindirs refuses");
        expect_refused_released(ch, ch.init(pio0, 921600), PicoRS485::err_sm_config);
        CHECK_EQ(g_enabled_n, 0);
    }
    // ...and both are retryable.
    reset_stubs();
    g_sm_ok = false;
    {
        PicoRS485 ch(0, 1, 2, 3);
        CASE("failure: retry after a refused config");
        CHECK_EQ(ch.init(pio0, 921600), (int)PicoRS485::err_sm_config);
        g_sm_ok = true;
        bring_up(ch);
        // tx ran once for the failed attempt and once for the retry; the other
        // three helpers only ever ran on the retry
        CHECK_EQ(g_tx.calls, 2);
        CHECK(g_rx.calls == 1 && g_rxidle.calls == 1 && g_txgap.calls == 1);
    }

    // --- 17. failure: busy wins, and the live channel is untouched -----------
    CASE("failure: busy wins, and the live channel is untouched");
    reset_stubs();
    {
        PicoRS485 ch(0, 1, 2, 3);
        bring_up(ch);
        const int claims_now = g_claim, enables_now = g_enabled_n;
        const int tx_now = g_tx.calls;

        CASE("busy: same call again");
        CHECK_EQ(ch.init(pio0, 921600), (int)PicoRS485::err_busy);
        CASE("busy: unusable baudrate too");
        CHECK_EQ(ch.init(pio0, 0), (int)PicoRS485::err_busy);
        CASE("busy: bad packet too");
        CHECK_EQ(ch.init(pio0, 9600, {4, 0, 1, 8, 16}), (int)PicoRS485::err_busy);
        CASE("busy: another block too");
        CHECK_EQ(ch.init(pio1, 921600), (int)PicoRS485::err_busy);
        CASE("busy: null PIO too");
        CHECK_EQ(ch.init(nullptr, 921600), (int)PicoRS485::err_busy);
        CASE("busy: empty clock config too");
        CHECK_EQ(ch.init(pio0, pico_rs485_clock_config{}), (int)PicoRS485::err_busy);
        CASE("busy: divider 32 too");
        CHECK_EQ(ch.init(pio0, pico_rs485_clock_config{125000000, 921600, 32, 0}),
                 (int)PicoRS485::err_busy);

        CHECK(ch.is_valid());                                       // still live
        CHECK(g_claim == claims_now && g_enabled_n == enables_now);  // untouched
        CHECK_EQ(g_tx.calls, tx_now);                               // not reconfigured
        CHECK(g_disabled_n == 0 && g_clear_mem == 1);   // the one clear is init's own
    }

    // --- 18. failure: resource exhaustion ------------------------------------
    CASE("failure: resource exhaustion");
    // No free DMA channel at all.
    reset_stubs();
    {
        int grabbed[NUM_DMA_CHANNELS];
        for (int i = 0; i < NUM_DMA_CHANNELS; ++i) grabbed[i] = dma_claim_unused_channel(false);
        PicoRS485 ch(0, 1, 2, 3);
        CASE("failure: no DMA channel free");
        CHECK_EQ(ch.init(pio0, 921600), (int)PicoRS485::err_no_dma);
        CHECK(!ch.is_valid());
        CHECK(!g_claimed[pio_slot(pio0)][SM_TX]);       // the block was handed back
        for (int i = 0; i < NUM_DMA_CHANNELS; ++i) dma_channel_unclaim((uint)grabbed[i]);
    }
    // A failure before any state machine is configured must not touch the caller's pins: the caller
    // may have had them for something else, and the header promises only that what was claimed is
    // given back.
    reset_stubs();
    {
        int grabbed[NUM_DMA_CHANNELS];
        for (int i = 0; i < NUM_DMA_CHANNELS; ++i) grabbed[i] = dma_claim_unused_channel(false);
        PicoRS485 ch(0, 1, 2, 3);
        const uint other_function = 7;                  // any function but SIO
        g_function[0] = other_function;                 // the caller's pins, in use
        g_function[2] = other_function;
        g_dir_out[2]  = true;
        g_value[2]    = true;                           // DE held high by whoever had it
        CASE("failure: a failure before the pins are taken over leaves them alone");
        CHECK_EQ(ch.init(pio0, 921600), (int)PicoRS485::err_no_dma);
        CHECK_EQ(g_function[0], other_function);
        CHECK_EQ(g_function[2], other_function);
        CHECK(g_dir_out[2]);
        CHECK(g_value[2]);                              // not driven low by a teardown
        for (int i = 0; i < NUM_DMA_CHANNELS; ++i) dma_channel_unclaim((uint)grabbed[i]);
    }
    // Exactly one free: the transmitter takes it, so the receiver's half is the
    // only thing that can fail, and the channel it took has to go back.
    reset_stubs();
    {
        int grabbed[NUM_DMA_CHANNELS - 1];
        for (int i = 0; i < NUM_DMA_CHANNELS - 1; ++i) grabbed[i] = dma_claim_unused_channel(false);
        PicoRS485 ch(0, 1, 2, 3);
        CASE("failure: one DMA channel free");
        CHECK_EQ(ch.init(pio0, 921600), (int)PicoRS485::err_no_dma);
        CHECK(!ch.is_valid());
        CHECK(!g_claimed[pio_slot(pio0)][SM_TX]);
        CHECK_EQ(g_dma_claimed, NUM_DMA_CHANNELS - 1);  // the one it took went back
        for (int i = 0; i < NUM_DMA_CHANNELS - 1; ++i) dma_channel_unclaim((uint)grabbed[i]);
    }
    // A block whose receive-idle machine is already claimed (SM0 is the case in
    // the section above, so both ends of the role order are covered).
    reset_stubs();
    {
        pio_sm_claim(pio0, SM_RXIDLE);
        PicoRS485 ch(0, 1, 2, 3);
        CASE("failure: SM2 claimed by another user");
        CHECK_EQ(ch.init(pio0, 921600), (int)PicoRS485::err_state_machine);
        CHECK(!g_claimed[pio_slot(pio0)][SM_TX]);       // nothing was taken
        CHECK(!ch.is_valid());
        pio_sm_unclaim(pio0, SM_RXIDLE);
    }

    // --- 19. failure: error precedence ---------------------------------------
    CASE("failure: error precedence");
    CASE("failure: a bad pin is reported before a bad packet");
    reset_stubs();
    {
        PicoRS485 ch(NUM_BANK0_GPIOS, 1, 2, 3);
        CHECK_EQ(ch.init(pio0, 921600, {3, 0, 1, 16, 8}), (int)PicoRS485::err_pin);
    }
    CASE("failure: a bad packet is reported before a bad clock");
    reset_stubs();
    {
        PicoRS485 ch(0, 1, 2, 3);
        CHECK_EQ(ch.init(pio0, pico_rs485_clock_config{}, {3, 0, 1, 16, 8}),
                 (int)PicoRS485::err_packet);
    }

    // --- 20. failure: the four pins must be four pins ------------------------
    // Two state machines on one GPIO is not a channel: the transmitter would drive the
    // pin the receiver watches, or DE would drive the receiver's input, and what follows
    // looks like a bus fault rather than a wiring mistake.
    CASE("failure: two state machines may not share a pin");
    reset_stubs();
    {
        PicoRS485 tx_is_rx(0, 0, 2, 3);
        CHECK(tx_is_rx.init(pio0, 921600) == PicoRS485::err_pin);
        PicoRS485 rx_is_act(0, 1, 2, 1);
        CHECK(rx_is_act.init(pio0, 921600) == PicoRS485::err_pin);
        PicoRS485 de_is_act(0, 1, 3, 3);
        CHECK(de_is_act.init(pio0, 921600) == PicoRS485::err_pin);
        // A refused pin set claims nothing, so the ordinary four still work after it.
        PicoRS485 ok(0, 1, 2, 3);
        CHECK(ok.init(pio0, 921600) == PicoRS485::ok);
    }
}
