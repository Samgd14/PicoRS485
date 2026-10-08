// LIFECYCLE
//
#include "suite.h"

void suite_lifecycle() {
    // --- 1. lifecycle: construction claims nothing ---------------------------
    CASE("lifecycle: construction claims nothing");
    reset_stubs();
    {
        PicoRS485 ch(0, 1, 2, 3);
        CHECK(!ch.is_valid());                  // not initialized yet
        CHECK(g_claim == 0 && g_add_calls == 0 && g_enabled_n == 0);
        CHECK(g_tx.calls == 0 && g_rx.calls == 0 && g_rxidle.calls == 0 && g_txgap.calls == 0);
    }
    CHECK(g_disabled_n == 0 && g_clear_mem == 0 && g_unclaim == 0);

    // --- 2. init: the defaults reach the four machines -----------------------
    CASE("init: the defaults reach the four machines");
    reset_stubs();
    {
        PicoRS485 ch(0, 1, 2, 3);
        CHECK(ch.init(pio0, 921600) == PicoRS485::ok);
        CHECK(ch.is_valid());
        CHECK(g_tx.calls == 1 && g_rx.calls == 1 && g_rxidle.calls == 1 && g_txgap.calls == 1);
        // clkdiv = 125e6 / (921600 * 8) = 16.9547 -> 16 + 244/256 = 16.953125
        CHECK(g_tx.clkdiv_int == 16 && g_tx.clkdiv_frac8 == 244);
        CHECK(g_rx.clkdiv_int == 16 && g_rx.clkdiv_frac8 == 244);
        CHECK(g_rxidle.clkdiv_int == 16 && g_rxidle.clkdiv_frac8 == 244);
        CHECK(g_txgap.clkdiv_int == 16 && g_txgap.clkdiv_frac8 == 244);
        // framing from the defaults: TX 1+8+0+1 = 10, RX 8+0+1 = 9,
        // idle threshold 16 bit times = 8 steps, gap 8
        CHECK(g_tx.count == 10 && g_rx.count == 9 && g_rxidle.count == 8 && g_txgap.count == 8);
        // pins actually reach the helpers
        CHECK(g_tx.pin_a == 0 && g_tx.pin_b == 2);
        CHECK(g_rx.pin_a == 1 && g_rx.pin_b == 3);
        CHECK(g_rxidle.pin_a == 3);
        // program load offsets are threaded through, not left at 0
        CHECK(g_tx.offset == 0 && g_rx.offset == 8 && g_rxidle.offset == 16 && g_txgap.offset == 24);
        CHECK(g_claim == 4);
        CHECK(saw_all_four(g_enabled, g_enabled_n));
        // Every setting reached its own machine, on the block that was named.
        CHECK(g_set_tx_sm == SM_TX && g_set_rx_sm == SM_RX);
        CHECK(g_set_rxidle_sm == SM_RXIDLE && g_set_gap_sm == SM_GAP);
        CHECK(g_set_tx_pio == pio0 && g_set_rx_pio == pio0);
        CHECK(g_set_rxidle_pio == pio0 && g_set_gap_pio == pio0);
    }

    // --- 3. teardown: the machines, the memory and the pads ------------------
    CASE("teardown: the machines, the memory and the pads");
    CHECK(saw_all_four(g_disabled, g_disabled_n));
    CHECK(g_unclaim == 4);
    CHECK(g_clear_mem == 2);   // emptied on init and again on release

    // The driver enable (pin 2 above) is dropped through the PIO and then handed
    // back driven low, rather than left floating as an input.
    CHECK(g_pio_cleared_last == (1u << 2));   // exactly DE, dropped while the PIO owned it
    CHECK(g_mask_pio == pio0 && g_mask_sm == SM_TX);   // through the transmitter
    CHECK(g_dir_out[2] && !g_value[2]);      // SIO now drives it low
    CHECK(g_function[2] == GPIO_FUNC_SIO);   // with the pad handed back to SIO
    CHECK(!g_dir_out[0]);                    // TX goes back as an input
    CHECK(g_function[1] == GPIO_FUNC_SIO);   // and so do RX and the activity pin
    CHECK(g_function[3] == GPIO_FUNC_SIO);

    // --- 4. init: an explicit packet config ----------------------------------
    CASE("init: an explicit packet config");
    reset_stubs();
    {
        PicoRS485 ch(0, 1, 2, 3);
        CHECK(ch.init(pio0, 9600, {7, 1, 2, 1, 6}) == PicoRS485::ok);
        CHECK(g_tx.count == 1 + 7 + 1 + 2);   // 11
        CHECK(g_rx.count == 7 + 1 + 1);       // 9
        CHECK(g_rxidle.count == 3);           // 6 bit times, in 2-bit steps
        CHECK(g_txgap.count == 1);
    }

    // --- 5. init: the explicit-clock overload --------------------------------
    CASE("init: the explicit-clock overload");
    reset_stubs();
    {
        PicoRS485 ch(0, 1, 2, 3);
        pico_rs485_clock_config clk{125000000, 921600, 16, 244};
        bring_up(ch, pio1, clk);
        CHECK(g_tx.clkdiv_int == 16 && g_tx.clkdiv_frac8 == 244);
        CHECK(g_txgap.clkdiv_int == 16 && g_txgap.clkdiv_frac8 == 244);
    }
    // deriving and supplying agree for the same rate
    reset_stubs();
    {
        PicoRS485 ch(0, 1, 2, 3);
        pico_rs485_clock_config clk{};
        CHECK(PicoRS485::compute_clock_config(921600, clk));
        bring_up(ch, pio0, clk, {8, 0, 1, 8, 16});
        CHECK(g_tx.clkdiv_int == 16 && g_tx.clkdiv_frac8 == 244);
    }

    // --- 6. lifecycle: instruction memory is emptied before every load -------
    CASE("lifecycle: instruction memory is emptied before every load");
    reset_stubs();
    {
        PicoRS485 ch(0, 1, 2, 3);
        bring_up(ch);
        CHECK(g_add_dirty == 0);   // every load saw memory init() had just emptied
        CHECK(g_clear_mem == 1);   // emptied once on the way in
    }
    CHECK(g_clear_mem == 2);       // and emptied again on the way out

    // --- 7. lifecycle: init is retryable after a failure ---------------------
    CASE("lifecycle: init is retryable after a failure");
    reset_stubs();
    {
        PicoRS485 ch(0, 1, 2, 3);
        CASE("lifecycle: retry after a packet refusal");
        CHECK_EQ(ch.init(pio0, 921600, {4, 0, 1, 8, 16}), (int)PicoRS485::err_packet);
        CHECK(!ch.is_valid());
        bring_up(ch);                                   // retry succeeds
    }
    // ...including after a mid-init resource failure, and on another block.
    reset_stubs();
    g_add_fail_at = 3;
    {
        PicoRS485 ch(0, 1, 2, 3);
        CASE("lifecycle: retry on another block after a load failure");
        CHECK_EQ(ch.init(pio0, 921600), (int)PicoRS485::err_program_load);
        bring_up(ch, pio1);
        CHECK(g_tx.calls == 1 && g_rx.calls == 1 && g_rxidle.calls == 1 && g_txgap.calls == 1);
    }

    // --- 8. lifecycle: teardown gives the DMA and the IRQ back ---------------
    CASE("lifecycle: teardown gives the DMA and the IRQ back");
    reset_stubs();
    {
        PicoRS485 ch(0, 1, 2, 3);
        CHECK(ch.init(pio0, 921600) == PicoRS485::ok);
        CHECK(g_irq_handler[PIO_IRQ_NUM(pio0, 0)] != nullptr);
        CHECK(g_irq_enabled[PIO_IRQ_NUM(pio0, 0)]);
    }
    CHECK(g_dma_claimed == 0);
    CHECK(g_irq_handler[PIO_IRQ_NUM(pio0, 0)] == nullptr);
    CHECK(!g_irq_enabled[PIO_IRQ_NUM(pio0, 0)]);
    raise_pio_irq(pio0, k_rx_done_irq);                        // nothing routed any more

    // --- 9. lifecycle: stale flags cannot survive init or release ------------
    CASE("lifecycle: stale flags cannot survive init or release");
    reset_stubs();
    {
        pio_hw_of(0)->irq = (1u << 0) | (1u << 4) | (1u << 5);   // left by a previous user
        PicoRS485 ch(0, 1, 2, 3);
        CHECK(ch.init(pio0, 921600) == PicoRS485::ok);
        CHECK(!pio_interrupt_get(pio0, k_rx_done_irq));      // a stale idle flag would end
        CHECK(!pio_interrupt_get(pio0, 4));      // the first burst instantly, and a
        CHECK(!pio_interrupt_get(pio0, 5));      // stale gap flag would skip a gap

        pio_hw_of(0)->irq |= (1u << 4) | (1u << 5);   // torn down mid-handshake
    }
    CHECK(!pio_interrupt_get(pio0, 4));
    CHECK(!pio_interrupt_get(pio0, 5));

    // A flag raised at the instant the receiver is armed is the window init()'s
    // final clear covers. It must not survive as a stale flag, and it must not
    // publish anything: the DMA has only just been armed.
    reset_stubs();
    g_raise_idle_on_arm = true;
    {
        PicoRS485 ch(0, 1, 2, 3);
        bring_up(ch);
        CASE("lifecycle: a flag raised while the receiver was armed");
        CHECK(!pio_interrupt_get(pio0, k_rx_done_irq));
        CHECK_EQ(ch.dropped_burst_count(), 0);
        // And the channel still works afterwards.
        feed(pio0, SM_RX, 9, {0x5Au | (1u << 8)});
        end_burst();
        uint8_t got[4] = {};
        CHECK_EQ(ch.receive_for(got, sizeof(got), 5000), 1);
        CHECK_EQ(got[0], 0x5A);
    }
}
