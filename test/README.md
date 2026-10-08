# Host tests

The driver targets RP2040/RP2350, but most of it is deciding *what* to program rather than talking to silicon. These tests compile the real `src/PicoRS485.cpp` against a small stub of the SDK in `test/stub/`. Validation, resource claiming, failure handling and pin/teardown state therefore run on a desktop, asserting against recorded calls.

The two-board firmware that covers what these cannot lives in `test/hw/`. The host suite needs no board and no SDK.

## Running

    pwsh test/run_tests.ps1              # all five checks
    pwsh test/run_tests.ps1 -OnlySuite   # just the suite, for a faster loop

The script runs under PowerShell 7 (`pwsh`) or Windows PowerShell 5.1 (`powershell -File test/run_tests.ps1`). A C++17 compiler is required on `PATH` (`g++` here), and `doxygen` is optional. Build output lands in `test/build/`, which git ignores.

The suite runs under a 120 s bound, so a hang fails the run instead of blocking it. The script sets the Windows error mode (`NOGPFAULTERRORBOX`) before spawning it, so a crash reports as a failed check rather than raising a dialog that waits for a click.

## The five checks

1. **The host suite** - 2220 checks with the default stub (2224 in the base-0 run below).
2. **The base-0 suite** - the same suite built *and run* with `-DPICO_PIO_USE_GPIO_BASE=0`, so the RP2040/RP2350A branch of the pin handling is executed rather than only compiled.
3. **The RP2040 / RP2350A build** - `src/PicoRS485.cpp` compiled (`-fsyntax-only`) with `PICO_PIO_USE_GPIO_BASE=0`.
4. **The `.pio` helpers** - the four `% c-sdk` blocks are extracted from `src/rs485.pio` and compiled at both pin-mask bases, so the helper code the suite replaces with recorders is still type-checked, including their `int` returns.
5. **Doxygen** - the header is parsed with `WARN_IF_UNDOCUMENTED` and `WARN_NO_PARAMDOC`, so a public declaration losing its documentation fails the run. Skipped if doxygen is not installed.

`-OnlySuite` runs check 1 alone. Both suite binaries run under a 120 s bound.

### The host suite

Every section sets a `CASE` label and every table row re-sets it, so a failure names the category and the scenario it came from as well as the expression.

The categories cover construction, `init()` and each of its error codes, `set_packet_config()` / `get_packet_config()`, the clock and packet validators, the clock derivation against the pre-refactor maths at 18 rates x 6 system clocks through both entry points, the GPIO-base pin windows and the 64-bit pin mask, teardown, the DMA paths (framing table, a burst clocked out and decoded, silence-delimited receives, queue overflow, the length boundary, timeouts, the in-flight refusals), and the transmit-completion contract.

Framing is checked as whole expected words - start bit, data LSB first, parity, stop bits and nothing above - in a table over 8N1/8E1/8O1/7E2/7N1/7E1/5N1/5E1, including parity computed over the masked byte. Decoding has a table of its own over 8N1/8E1/8O1/7N1/7E1/5N1, each row carrying its expected bytes and its expected framing and parity counts. A canary past a short receive buffer pins that a burst is truncated to what the caller offered while the return stays the full length.

The validator's accepted space was also swept exhaustively, outside this suite: data 0-9 x parity 0-2 x stop 0-3 x gap 0-33 x threshold 0-65 x even/odd, 538,560 combinations, of which 44,144 are accepted - 22,072 framing tuples, each counted twice because the parity mode is ignored without a parity bit. The sweep is `test/build/review/rx_idle_sweep.cpp` (git-ignored), so it does not run here.

### Compile-time guardrails

The driver also makes these compile-time checks, none of which this script tests:

- `[[nodiscard]]` on every call that returns a status, so a dropped `init()` or `send()` result warns.
- `constexpr` on `is_packet_config_valid()`, `is_clock_config_valid()` and the 3-argument `compute_clock_config()`, so a caller can `static_assert` its own framing and divider.
- `static_assert`s refusing `PICO_RS485_MAX_PACKET_BYTES=0` and a maximum burst above 65535.
- `static_assert`s pinning the members the interrupt shares with the caller.
- The maximum burst length is a template parameter, which turns a `PICO_RS485_MAX_PACKET_BYTES` mismatch between two translation units into an undefined reference.

Of those, the template parameter is the only one that reaches these builds as a link requirement, and even then only as a symbol that has to resolve. The mismatch itself is not produced here.

The suite also compiles the library with `PICO_RS485_EXTRA_SIZE=32`, so a second instantiation of the class exists to link against. The two-lengths section (59) brings up a 32-byte channel and a 256-byte one on separate blocks and checks that each block's interrupt reaches its own channel. Without that define the second type has no out-of-line members and the section would not link. It is the same rule a consumer meets.

## What this cannot cover

- The stub is sized like an **RP2350B**: 48 GPIOs, a per-PIO GPIO base, and `PICO_PIO_USE_GPIO_BASE` defaulting to 1. `NUM_PIOS` is 2 as on RP2040, and no third block exists here to test. The base-0 run exercises the RP2040 code path, but `NUM_BANK0_GPIOS` is still 48, so the 30-pin RP2040 boundary is never exercised.
- The `.pio` programs are **not assembled or run** - no pioasm and no hardware. The IRQ 4/5 handshake, bit timing, the activity pin and the DE side-set are all outside what these tests can see. A bit-level model in the suite groups received samples the way the autopush counter does, which is enough to reach the word boundary (the stalled-autopush and retirement-lag sections, 43 and 44). No sample *timing* is modelled, so the sampling phase itself stays out of reach.
- **The activity pin is a stub pin.** `is_receiving()` reads it, and the tests set that value by hand rather than watching the PIO drive it. The point at which it goes high and low within a character is a `.pio` property, not a driver one. DE is written by the driver and never read back.
- The **DMA is serviced by the test**, not autonomous: `dma_run()` moves a word when a channel's DREQ says its FIFO has room or data, and transfers touching a PIO FIFO go through a queue. Bus contention, priorities and real transfer latency are not modelled. One window that is real on hardware is modelled deliberately. `g_retire_lag` leaves a transfer's write unretired, so a single read of the pointer comes back one word short - RP2040-E13 - which is what the retirement-lag section (44) pins. The double's abort also clears the transfer count, as the hardware's does. A driver that read the count after the stop therefore fails here the same way it failed on silicon.
- The stub **records calls**. It does not model PIO or pad semantics beyond the assertions that read it back. A test passing here means the driver asked for the right things, not that the hardware did the right thing with them. In particular `pio_set_gpio_base()` returns `PICO_OK` unconditionally. The SDK's rule that a block with loaded programs refuses a base change, which [`docs/design-notes.md`](../docs/design-notes.md) relies on, is therefore not checked here.
- **Optimisation-sensitive bugs are out of reach here.** A wait loop that polls interrupt-written state can be deleted by the optimiser (see [`docs/design-notes.md`](../docs/design-notes.md)), and no host test can catch that. Catching it would mean running the handler from inside the wait, and any call able to do that is itself a compiler barrier that protects the loop. That one is pinned by a `static_assert` in the header instead, and the stub for `pico/platform.h` is kept faithful (an empty always-inline `tight_loop_contents`) so it cannot mask such a bug a second time.

### Known gaps in the assertions

These are places where a wrong driver could pass, or where the model is simpler than the hardware. They are listed so the suite is not read as stronger than it is. The list is measured, not guessed. The suite is run against a corpus of single-line mutations of `src/PicoRS485.cpp`, and every survivor is either closed or recorded here.

- **`take_burst()`'s interrupt mask is unobservable here.** The handler only runs when the test calls it, so nothing can arrive between the mask going on and off. Section 46 (receive: leaves the interrupt line as it found it) reads the line's state back, which pins that it is restored rather than forced, but not that it excludes anything.
- **The model does not dispatch a pending flag when the IRQ is enabled.** It runs the handler only from `raise_pio_irq()`. On hardware, enabling the line with a flag already set takes the interrupt immediately; here the flag has to be cleared by the driver. The "flag raised while the receiver was armed" check therefore pins the driver's final `pio_interrupt_clear()`, which is a tidy-up rather than the mechanism that protects the first burst.
- **A driver that relied on an SDK default instead of programming it would pass.** `dma_channel_get_default_config()` already returns 32-bit transfers with read increment on, so dropping the driver's explicit calls to set those changes nothing. The DREQ is the term that must be programmed, and that one is pinned by the model's FIFO-overrun counter.
- The clock sweep compares the driver against a copy of the same arithmetic, so it is a regression guard rather than verification. The independent evidence is the hand-pinned divider and the +/-2% budget check.
- `receive()` waits without a bound, so a driver that loses a burst can fail the run by hanging rather than by a failed check. `receive_for()`'s timeout boundary is pinned exactly, and every table row uses the bounded call. The unbounded call appears where a burst is already queued, and a null output buffer is checked with a burst queued, so a missing guard fails fast rather than spinning.
- `release()`'s registry entry (`instances_`) is not read directly. The suite observes that the dead block's handler is gone and that its flag reaches nothing, but not the entry itself.
- The `init()` order - drain, restart, rewind, arm, route - is pinned by call counts, by the machine and block each call names, and by the jump target, not by their interleaving. The drain-before-restart half is pinned more strongly, because the stalled-autopush section (43) models the stalled push that makes the order matter.
- **A defensive guard that no input can reach is invisible here.** `take_burst()` refuses a queue index the handler cannot have written, and the mutation that removes that check survives. It is recorded as defensive rather than as tested. `burst_duration_us()`'s length clamp is the other kind: only a 64-bit host can reach it, and section 33 pins it there with two lengths chosen so that a wrapped product would show.
- **`release()`'s teardown ordering is not observable.** The registry entry is cleared before the flags and the handler, so an interrupt arriving mid-teardown cannot still service the instance. The suite, however, runs the handler only when the test calls it, so a mutation that moves the clear to the end of the block is not caught.
- The `.pio` check is a compile check: an emptied helper body still passes. Doxygen parses `include/PicoRS485.h` only.

## Layout

    test_pico_rs485.cpp   the entry point: the table of contents, then main(), which calls the categories in order and prints the total
    suite.h               what every unit shares: the stub state and the models' types as inline variables, the CHECK/CASE macros, and declarations for the model's functions
    suite_model.cpp       the stubbed SDK, the DMA/FIFO/IRQ model - including the bit-level receive model and the retirement lag - and the scenario helpers. The helpers are `bring_up()`, `expect_refused_clean()`, `expect_refused_released()`, `feed()`, `end_burst()` and `same_packet()`, plus `deliver_sample()`, `deliver_char_bits()` and `release_stalled()` for the bit-level model. Each checks what it sets up, so a scenario cannot silently skip its precondition.
    sections_*.cpp        one file per category, each a single function the dispatcher calls: lifecycle, failure, config, transmit, receive, completion, ownership, invariants
    stub/                 minimal SDK surface, sized to what src/PicoRS485.cpp uses
      hardware/pio.h      PIO API, real instance objects, claim map, FIFO registers
      hardware/dma.h      DMA channels: configured by the driver, driven by the test
      hardware/irq.h      handler registration and the line's state, enough to route the idle flag and to ask whether a line is already owned
      hardware/clocks.h   clock_get_hz()
      hardware/gpio.h     pin state, modelled on the SDK's ordering
      hardware/sync.h     the interrupt save/restore pair
      hardware/timer.h    the test-owned clock receive_for() waits on
      pico/time.h         re-exports hardware/timer.h, as the SDK's does
      pico/platform.h     tight_loop_contents(), and get_core_num() over a global the test moves
      pico/sem.h          the semaphore the handler signals: releases counted, acquires return at once
      pico/sync.h         re-exports hardware/sync.h, as the SDK's does
      rs485.pio.h         what pioasm would emit, with the helper signatures
    Doxyfile              the header comment check
    run_tests.ps1         builds and runs everything above

## The two-board firmware

`test/hw` builds under the Pico SDK and needs `PICO_SDK_PATH`. `PICO_BOARD=pico` is what the wiring below assumes:

    cmake -S . -B build-hw -G Ninja -DPICO_BOARD=pico
    cmake --build build-hw

- `rs485_hw_master` / `rs485_hw_slave` - flash one board with each and read both USB serial consoles. They round-trip bursts, including one long enough to span several DMA refills, and one deliberately over-long burst that the receiver must drop, count and recover from.
- `rs485_uart_master` / `rs485_uart_slave` - the UART bisect: the same pins and the same transceivers driven by the hardware UART with no library code at all, to tell a library fault from a bench fault.
- `rs485_pin_probe` - no library and no UART. It watches the RX pin as a plain input, first with no pull and then with each internal pull, to find out whether anything is driving it.
- `rs485_stress_master` / `rs485_stress_slave` - the soak.
- `-DPICO_RS485_MARGIN_PPM=<ppm>` builds `rs485_hw_slave_offset`, a slave whose bit rate is deliberately off, to put the receiver's stop-bit margin under load.

Per board: RX = GP5, TX = GP8, DE = GP9, RX activity = GP17 (local to the board and unconnected). The two boards' A/B lines are wired in parallel with termination enabled, and each transceiver's DE and /RE are tied together.

The firmware is configured for RP2040 (`-DPICO_BOARD=pico`). The RP2350 code paths are covered by compilation and by the host suite's base-1 run, not by a recorded RP2350 hardware run.

`-DPICO_RS485_HW_TESTS=OFF` builds the driver alone, with none of the firmware above. It is on by default because this repository is normally the top-level project. A consumer that adds it as a subdirectory wants it off. Each firmware calls `pico_add_extra_outputs()`, which is what brings `picotool` in through `FetchContent`, and the library target avoids that on purpose.

## The stress bench, and what bites

`test/hw/stress.cpp` is flashed on two boards (one master, one slave) and driven over the console by the scripts under `build-hw/`, which are git-ignored scratch. These properties of the harness and of this host are not.

1. **One `key=value` per run line.** The argument parser writes a NUL over the separator and the outer loop then sees a terminator, so `r10 gap=12 period=200` runs gap 12 with the *boot* period.
2. **A config change can silently not apply.** The new value is recorded as applied even when `set_packet_config()` refuses it for being in flight, so retrying the same value is skipped. The reliable sequence is to stop the run, let the wire go quiet, apply a decoy value, then the real one, and confirm `packet` in the dump's config line.
3. **Hold both console handles open for the whole window.** Closing one mid-run collapses the exchange rate and fills the counters with stale and timeout figures.
4. **`gap=` 13 and above is refused here** (`packet -2`): the console leaves `rx_idle_bits_thresh` at its default 16, and validation requires a threshold of at least `gap + stop_bits + 3`, so with one stop bit only gaps 1-12 are reachable, and 1-11 with `packet=2`.
5. **The first window after a flash always shows one late event**, at cycle 1, because the first exchange follows a cold start. The run scripts discard a priming second before measuring.

## The mutation corpus

`test/mutation_corpus.ps1` regenerates mutants by text substitution on the driver and the header, compiles each against the host suite, and reports KILLED, SURVIVED, NOT-APPLIED or HANG. It runs every mutant with the Windows error dialog suppressed. A few of them fault on purpose.

Two things to expect. Some anchors describe code that no longer exists and report NOT-APPLIED. That is the corpus's own debt, not a gap in the suite. A few mutants are equivalent: the suite cannot kill them because the mutation has no observable effect. That is worth checking before treating a survivor as a missing test.

## What to run, and in what order

1. `pwsh test/run_tests.ps1` - the host suite on both pin-mask bases, the RP2040/RP2350A syntax build, the `.pio` helper assembly and the doxygen check. It builds no firmware: the two-board build is the separate `cmake --build build-hw`.
2. The bench, which needs both boards, an idle host and a shell that can run the Pico SDK build.
3. `pwsh test/mutation_corpus.ps1` - the corpus, several minutes.
