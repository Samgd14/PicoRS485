// OWNERSHIP
//
#include "suite.h"

void suite_ownership() {
    // --- 53. ownership: is_pio_valid() is a pure check -----------------------
    CASE("ownership: is_pio_valid() is a pure check");
    reset_stubs();
    {
        PicoRS485 ch(0, 1, 2, 3);
        CHECK_EQ(ch.is_pio_valid(pio0), (int)PicoRS485::ok);
        CHECK_EQ(ch.is_pio_valid(pio1), (int)PicoRS485::ok);
        CHECK(g_claim == 0 && g_add_calls == 0 && g_enabled_n == 0);   // claimed nothing
        CHECK(ch.init(pio1, 921600) == PicoRS485::ok);            // and init still works
    }

    // --- 54. ownership: GPIO base windows ------------------------------------
    CASE("ownership: GPIO base windows");
    // A block reaches 32 pins from its own GPIO base: base 0 covers 0-31, base 16
    // covers 16-47, and the window belongs to the block it was set on. Where the
    // chip has no base the check is compiled out and every pin is accepted.
#if PICO_PIO_USE_GPIO_BASE
    const int out_of_window = PicoRS485::err_gpio_base;
#else
    const int out_of_window = PicoRS485::ok;
#endif
    {
        struct base_case { uint base, tx, rx, de, act; int want; const char *what; };
        const base_case cases[] = {
            { 0, 40, 41, 42, 43, out_of_window, "base 0, pins 40-43"},
            {16,  0,  1,  2,  3, out_of_window, "base 16, pins 0-3"},
            {16, 15, 33, 40, 41, out_of_window, "base 16, pin 15 and above"},
            {16, 20, 21, 22, 23, PicoRS485::ok, "base 16, pins 20-23"},
            {16, 32, 33, 40, 41, PicoRS485::ok, "base 16, pins 32-41"},
            {16, 47, 46, 45, 44, PicoRS485::ok, "base 16, top of the window"},
        };
        for (const base_case &c : cases) {
            CASE("ownership: %s", c.what);
            reset_stubs();
            pio_set_gpio_base(pio0, c.base);       // the caller configures it
            {
                PicoRS485 ch(c.tx, c.rx, c.de, c.act);
                CHECK_EQ(ch.is_pio_valid(pio0), c.want);   // the pure check agrees
                CHECK_EQ(ch.init(pio0, 921600), c.want);
            }
            CHECK_EQ(g_claim, c.want == PicoRS485::ok ? 4 : 0);
            if (c.want == PicoRS485::ok) {
                CHECK(g_function[c.act] == GPIO_FUNC_SIO);   // pads handed back
                CHECK(g_function[c.de] == GPIO_FUNC_SIO);
            }
        }
    }
    // Pins past 31 are only expressible through the 64-bit mask call, so the DE
    // drop at teardown is checked for the mask, the machine and the block it named.
    reset_stubs();
    pio_set_gpio_base(pio0, 16);
    {
        PicoRS485 ch(32, 33, 40, 41);
        bring_up(ch);
    }
#if PICO_PIO_USE_GPIO_BASE
    CHECK_EQ(g_pio_cleared_last, (1ull << 40));            // exactly DE, at pin 40
    CHECK(g_mask_pio == pio0 && g_mask_sm == SM_TX);       // through the right machine
#endif
    CHECK(g_function[33] == GPIO_FUNC_SIO);                // and the RX pin went back
    // A base on one block does not move another block's window.
    reset_stubs();
    pio_set_gpio_base(pio1, 16);
    {
        PicoRS485 ch(0, 1, 2, 3);
        CHECK_EQ(ch.is_pio_valid(pio1), out_of_window);
        CHECK_EQ(ch.is_pio_valid(pio0), (int)PicoRS485::ok);
    }

    // --- 55. ownership: two channels on different PIOs route their own flags ----
    CASE("ownership: two channels on different PIOs route their own flags");
    reset_stubs();
    {
        PicoRS485 a(0, 1, 2, 3);
        CHECK(a.init(pio0, 921600) == PicoRS485::ok);
        PicoRS485 b(8, 9, 10, 11);
        CHECK(b.init(pio1, 921600) == PicoRS485::ok);

        CHECK(g_irq_handler[PIO_IRQ_NUM(pio1, 0)] != nullptr);
        CHECK(PIO_IRQ_NUM(pio1, 0) != PIO_IRQ_NUM(pio0, 0));

        uint8_t got[4] = {};
        // A burst on pio1 has to reach b and nobody else.
        deliver_rx(pio1, 1, 0x7Eu | (1u << 8), 9);
        raise_pio_irq(pio1, k_rx_done_irq);
        CHECK(b.receive_for(got, sizeof(got), 50000) == 1);
        CHECK(got[0] == 0x7Eu);
        CHECK(!a.is_receiving());                // pio0's DMA moved nothing

        // And pio0 still works with pio1 live.
        deliver_rx(pio0, 1, 0x21u | (1u << 8), 9);
        raise_pio_irq(pio0, k_rx_done_irq);
        CHECK(a.receive_for(got, sizeof(got), 50000) == 1);
        CHECK(got[0] == 0x21u);

        // A flag on the other block must not be serviced by this block's interrupt. Both are left
        // set, then only pio0's line is allowed to run: with one handler per block, pio1's flag
        // survives it. Polling every block's flag instead would clear and publish both.
        CASE("ownership: one block's interrupt does not service the other's flag");
        deliver_rx(pio1, 1, 0x33u | (1u << 8), 9);
        pio0->irq |= (1u << k_rx_done_irq);
        pio1->irq |= (1u << k_rx_done_irq);
        raise_pio_irq(pio0, k_rx_done_irq);
        CHECK_EQ(pio0->irq & (1u << k_rx_done_irq), 0u);   // pio0's flag was serviced and cleared
        CHECK_EQ(pio1->irq & (1u << k_rx_done_irq), (1u << k_rx_done_irq));   // pio1's was left alone
        CHECK(!b.has_burst());          // and nothing was published for it
    }
    // Both are gone: neither PIO's line still reaches anything, and the registry
    // entry for the block that was released is not left dangling.
    CHECK(g_irq_handler[PIO_IRQ_NUM(pio0, 0)] == nullptr);
    CHECK(g_irq_handler[PIO_IRQ_NUM(pio1, 0)] == nullptr);
    raise_pio_irq(pio1, k_rx_done_irq);
    CHECK(g_dma_claimed == 0);

    // One released while the other is still live: the survivor keeps routing its
    // own flag, and the dead block's flag reaches nothing.
    reset_stubs();
    {
        PicoRS485 a(0, 1, 2, 3);
        bring_up(a);
        {
            PicoRS485 b(8, 9, 10, 11);
            bring_up(b, pio1);
        }                                                   // b is torn down here
        CASE("ownership: one channel released, one live");
        CHECK(g_irq_handler[PIO_IRQ_NUM(pio1, 0)] == nullptr);
        feed(pio0, SM_RX, 9, {0x21u | (1u << 8)});
        end_burst();
        uint8_t got[4] = {};
        CHECK_EQ(a.receive_for(got, sizeof(got), 50000), 1);
        CHECK_EQ(got[0], 0x21u);
        raise_pio_irq(pio1, k_rx_done_irq);                             // and nothing answers there
    }

    // --- 56. ownership: only the two completion flags are routed to the CPU --
    CASE("ownership: only the two completion flags are routed to the CPU");
    reset_stubs();
    {
        PicoRS485 ch(0, 1, 2, 3);
        bring_up(ch);
        CASE("ownership: while live");
        CHECK_EQ(g_irq_src_set[(int)pis_interrupt0], 1);   // flag 0: the TX machine's IRQ
        CHECK_EQ(g_irq_src_set[(int)pis_interrupt1], 1);   // flag 1: the RX machine's IRQ
        for (int s = 0; s < 16; ++s) {
            if (s == (int)pis_interrupt0 || s == (int)pis_interrupt1) continue;
            CHECK_EQ(g_irq_src_set[s], 0);                 // and nothing else, flags 4/5 included
        }
        CHECK(g_irq_handler[PIO_IRQ_NUM(pio0, 0)] != nullptr);
        CHECK(g_irq_enabled[PIO_IRQ_NUM(pio0, 0)]);
    }
    CASE("ownership: after teardown");
    CHECK_EQ(g_irq_src_cleared[(int)pis_interrupt0], 1);   // unrouted again
    CHECK_EQ(g_irq_src_cleared[(int)pis_interrupt1], 1);
    CHECK(g_irq_src_set[(int)pis_interrupt2] == 0 && g_irq_src_set[(int)pis_interrupt3] == 0);

    // --- 57. ownership: is_pio_valid() refuses an IRQ line it does not own ----
    CASE("ownership: is_pio_valid() refuses an IRQ line that is already owned");
    reset_stubs();
    {
        PicoRS485 ch(0, 1, 2, 3);
        const uint irq = PIO_IRQ_NUM(pio0, 0);

        // Another library holds the block's IRQ 0 line. Taking it exclusively hard_asserts,
        // so the refusal has to come before anything is claimed.
        irq_set_exclusive_handler(irq, other_library_irq_handler);
        CHECK_EQ(ch.is_pio_valid(pio0), (int)PicoRS485::err_irq_in_use);
        CHECK_EQ(ch.init(pio0, 921600), (int)PicoRS485::err_irq_in_use);
        CHECK(!ch.is_valid());
        CHECK(!pio_sm_is_claimed(pio0, 0));      // and it claimed nothing on the way
        CHECK(!pio_sm_is_claimed(pio0, 3));

        // Handing the line back makes the block usable again.
        irq_remove_handler(irq, other_library_irq_handler);
        irq_set_enabled(irq, false);
        CHECK_EQ(ch.is_pio_valid(pio0), (int)PicoRS485::ok);
        CHECK_EQ(ch.init(pio0, 921600), (int)PicoRS485::ok);
        CHECK(ch.is_valid());
    }

    // --- 58. ownership: using the driver from another core asserts -----------
    CASE("ownership: using the driver from another core");
    reset_stubs();
    {
        PicoRS485 ch(0, 1, 2, 3);
        bring_up(ch);                            // init() on the core the test runs on
        CHECK_EQ(g_core_hook_calls, 0);   // nothing before this section tripped it
        const uint8_t      payload[1] = {0x11};
        const pico_rs485_packet_config other{7, 1, 1, 8, 16, true};
        const int violations_before = g_core_hook_calls;

        // The calls that return an error code name the core, and are not also counted.
        CASE("ownership: an error code, where the call has one to return");
        CHECK_EQ(violations_on_core(1, [&] {
            CHECK_EQ(ch.send(payload, 1), (int)PicoRS485::err_wrong_core);
        }), 0);
        CHECK_EQ(violations_on_core(1, [&] {
            CHECK_EQ(ch.set_packet_config(other), (int)PicoRS485::err_wrong_core);
        }), 0);
        CHECK_EQ(violations_on_core(1, [&] {
            CHECK_EQ(ch.receive_for(nullptr, 0, 0), (int)PicoRS485::err_wrong_core);
        }), 0);

        // The calls that cannot return one answer as if the channel were busy, and count it.
        CASE("ownership: a busy answer and a count, where they cannot");
        CHECK_EQ(violations_on_core(1, [&] { CHECK(ch.is_transmitting()); }), 1);
        CHECK_EQ(violations_on_core(1, [&] { CHECK(ch.is_receiving()); }), 1);
        CHECK_EQ(violations_on_core(1, [&] { CHECK(ch.has_burst()); }), 1);
        CHECK_EQ(violations_on_core(1, [&] { CHECK(ch.in_flight()); }), 1);   // short-circuits

        // A burst this core's read can find, so that the wrong-core read has something to refuse.
        deliver_rx(pio0, SM_RX, 0x55u | (1u << 8), 9);
        end_burst();
        CASE("ownership: a queued burst is left where it is");
        CHECK_EQ(violations_on_core(1, [&] {
            uint8_t got[2] = {};
            CHECK_EQ(ch.receive(got, sizeof(got)), (int)PicoRS485::err_wrong_core);
        }), 0);
        CHECK(ch.has_burst());                    // and nothing was unpacked

        // The hook the library declares weak is replaced above, and is now the only count there is:
        // the four predicates that cannot return an error code, and nothing else.
        CHECK_EQ(g_core_hook_calls - violations_before, 4);

        // On the core that called init() the same calls are silent.
        CASE("ownership: and silent on the core that owns the channel");
        CHECK_EQ(violations_on_core(0, [&] { (void)ch.is_transmitting(); }), 0);
        CHECK_EQ(violations_on_core(0, [&] { (void)ch.has_burst(); }), 0);
        CHECK_EQ(violations_on_core(0, [&] { (void)ch.in_flight(); }), 0);

        // init() belongs to whichever core calls it - it records that core rather than checking,
        // and it calls set_packet_config() internally, so the check has to be gated on the handler
        // being routed rather than on pio_ being set.
        const unsigned was = g_core_num;
        g_core_num = 1;
        PicoRS485 second(8, 9, 10, 11);
        CHECK_EQ(violations_on_core(1, [&] { (void)second.init(pio1, 921600); }), 0);
        CHECK(second.is_valid());
        CHECK_EQ(violations_on_core(1, [&] { (void)second.has_burst(); }), 0);
        CHECK_EQ(violations_on_core(0, [&] { (void)second.has_burst(); }), 1);

        // An inert channel shares nothing yet, so it is silent from anywhere.
        PicoRS485 idle(0, 1, 2, 3);
        CHECK_EQ(violations_on_core(1, [&] { (void)idle.is_transmitting(); }), 0);
        CHECK_EQ(violations_on_core(1, [&] { (void)idle.has_burst(); }), 0);
        g_core_num = was;
    }

    // --- 59. ownership: two maximum burst lengths in one program -------------
    CASE("ownership: two maximum burst lengths in one program");
    // The maximum burst length is a template parameter, so two of them are two types. Each
    // carries its own instance table and its own counters, and the handler registered on a
    // block's IRQ line is the thunk of the instantiation that owns that block - which is
    // what this section pins, because it is the part that could not be tested before the
    // length became a type.
    reset_stubs();
    {
        static_assert(PicoRS485_t<32>::max_burst_bytes == 32,
                      "the parameter is the maximum burst length");
        static_assert(sizeof(PicoRS485_t<32>) < sizeof(PicoRS485_t<256>),
                      "a smaller maximum burst is a smaller object");

        PicoRS485_t<32>  small(0, 1, 2, 3);
        PicoRS485_t<256> large(4, 5, 6, 7);
        CHECK_EQ(small.init(pio0, 921600), (int)PicoRS485_t<32>::ok);
        CHECK_EQ(large.init(pio1, 921600), (int)PicoRS485_t<256>::ok);
        CHECK(small.is_valid() && large.is_valid());

        // Each block's handler looks the instance up in its own instantiation's table, so a
        // burst on one block reaches its channel and leaves the other untouched.
        CASE("ownership: a burst on pio0 reaches the 32-byte channel only");
        deliver_rx(pio0, SM_RX, 0x5Au | (1u << 8), 9);
        end_burst(pio0);
        CHECK(small.has_burst());
        CHECK(!large.has_burst());

        CASE("ownership: a burst on pio1 reaches the 256-byte channel only");
        deliver_rx(pio1, SM_RX, 0x33u | (1u << 8), 9);
        end_burst(pio1);
        CHECK(large.has_burst());
        CHECK(small.has_burst());                // and its own burst is still queued
        uint8_t got[2] = {};
        CHECK_EQ(small.receive_for(got, sizeof(got), 0), 1);
        CHECK_EQ(got[0], 0x5Au);
        CHECK_EQ(large.receive_for(got, sizeof(got), 0), 1);
        CHECK_EQ(got[0], 0x33u);

        // The hook is one symbol with one definition, so a wrong-core call through either
        // instantiation reports to the same place. There is no per-type count left to disagree
        // with it.
        CASE("ownership: the core-violation hook is library-wide across both instantiations");
        const int hook_before = g_core_hook_calls;
        CHECK_EQ(violations_on_core(1, [&] { (void)small.in_flight(); }), 1);
        CHECK_EQ(g_core_hook_calls, hook_before + 1);
        CHECK_EQ(violations_on_core(1, [&] { (void)large.in_flight(); }), 1);
        CHECK_EQ(g_core_hook_calls, hook_before + 2);
        CHECK_EQ(violations_on_core(0, [&] { (void)large.is_transmitting(); }), 0);   // the right core

        // Two channels, two types: one being busy says nothing about the other.
        CASE("ownership: the two channels are separate objects");
        CHECK(!small.is_transmitting() && !large.is_transmitting());
        CHECK_EQ(small.max_burst_bytes, 32u);
        CHECK_EQ(large.max_burst_bytes, 256u);
    }
    // Both destructors released their own block; section 60 checks that neither hand-back
    // was of a machine the other owned.
    CASE("ownership: both teardowns were clean");
    CHECK_EQ(g_bad_unclaim, 0);
    CHECK_EQ(g_bad_irq_remove, 0);
}
