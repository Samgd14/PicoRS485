# Bench evidence

This is the record of the hardware and software tests behind the driver's design.

## How these measurements were taken

- Two RP2040 boards, one master and one slave, linked by RS-485. Unless a figure says otherwise: a 1-byte request and a 16-byte reply, 15,625,000 baud (divider 1.0 at the default clk_sys), one exchange every 83 µs - 12,048 exchanges/s.
- The run is quiet: the console stays **open** on the host for the whole window, and the board communicates nothing until the window is over. An earlier firmware reported its results during the test, and that output alone skewed the figures heavily. Closing or reopening the port during the test collapses the master's rate: 311 exchanges/s, with a third of the replies stale and 126 timeouts, against 12,047/s and no errors while it stays open.
- A figure is trusted only if the same binary reproduces it. Six consecutive flashes of one binary gave identical results. A result that moves between runs is therefore evidence about the instrument, not about the driver.
- Every new host test is checked by mutation: reintroduce the fault, then watch the test fail with the bytes the bench produced.
- The bring-up probe was not neutral. Adding or removing it changed whether bursts decoded at all, so code that never executed could flip the outcome. Any measurement taken with it present therefore says nothing about a build without it. The probe has since been removed from the library.

## Floating RX pin

A board with the transceiver's `/RE` tied to `DE` and no pull-up on `RO` produced a continuous break flood. With the receiver in high impedance for the whole of every transmission, the floating pin framed noise as traffic. The driver's silence delimiter and stop-bit checks saw it all as characters. A build that removes the driver's internal pull-up on the RX pin floods the FIFO the same way.

## Receive mis-decode: shift counter issue

In an early build of the library, every burst was decoded with roughly half its characters wrong. With 8N1 framing, an average of 7.5 of 16 characters failed the stop-bit check, and the payload pattern was broken. The count was identical to within 0.1% at every bit rate and period tested (500 Hz, 6 kHz, 12 kHz), which is what ruled out anything transient.

For a character 0xA5:

| state | first word | decoded byte | stop slot |
|---|---|---|---|
| correct | `0xD2800000` | 0xA5 | reads the stop bit |
| mis-decoded | `0xA5800000` | 0x4B | reads d7 |

The mis-decoded word is the correct one rotated one place: `received = ((byte << 1) | 1) & 0xFF`. The word boundary was one sample early, so the stop slot held the data MSB. That bit is set in about half of any ramp, which gives the 7.5-of-16 average. A character pushes exactly as many samples as it consumes, so the phase was preserved from the first burst to the last. The phase stayed fixed until the channel was re-initialised.

The cause was the receiver's autopush shift counter, left one sample out of phase by the recovery sequence. `pio_sm_restart()` clears the counter and the input shift register, but it cannot cancel a push that is stalled because the FIFO is full. The flush is what releases that instruction. The driver restarted *before* flushing, in `init()` and in `rx_done_isr()`. In `init()` the receiver was also enabled eight characters before the DMA was armed, so with no consumer the FIFO filled and a push genuinely stalled. The retried push then re-incremented the counter the restart had just cleared.

The fix was to drain the FIFO before restarting, and to arm the RX channel before the machines are enabled.

After the fix, the probe-free build ran 96,376 of 96,376 exchanges correct at 12 kHz (twice) and 4,001 of 4,001 at 500 Hz, with dropped, framing, parity and timeout counters all zero. The host suite guards the ordering: the stalled-autopush section (43) fails on the old order with 0x4B for a transmitted 0xA5.

## Envelope after the fix

- 500 Hz and 12 kHz both run clean, at divider 1.0 with 1-byte requests and 16-byte replies, for as long as the windows tested.
- Boot costs one or two framing errors' worth of fragment if a burst is already arriving while `init()` runs. The counters show it, and payloads are unaffected.
- The `rs485_hw_*` pair's own boot sequence exercises the recovery path directly, and passes. It runs a round trip of 8 bytes, one of 64 bytes, and one after a deliberately over-long burst, each with the framing and parity counters still zero.

## The burst length: write pointer against transfer count

A bring-up probe read both sources at the same decision, on an eight-character burst. The write pointer sat 32 bytes past the base, 8 words, the transfer count read 249 before the stop with 8 taken, and the FIFO was empty.

The count cannot serve the same purpose, because the stop that ends a burst is what invalidates it. `dma_channel_cleanup()` clears the enable bit and then aborts, and the abort leaves `TRANS_COUNT` reading zero. The read taken before the stop said 249 transfers remaining. The same read taken afterwards said 0. A count read after the stop is the full arm count every time, so every good burst looks over-long and is dropped, twice a second at this pacing. Nothing is unpacked either, so no framing or parity error is counted, which leaves the dropped-burst counter as the only symptom.

The register map does not say that an abort clears the count, and the measurement did not record which of RP2040 or RP2350 the board carried. The errata that carry the RP2350-E5 enable-before-abort workaround document no such behaviour either, so the zero is measured behaviour rather than a documented property of either part.

## RP2040-E13: one-word-short burst

`dma_channel_abort()` waits on BUSY, and per the errata that wait can return while a transfer is still in flight - "an individual read has taken place but the corresponding write has not". A single read of the write pointer can therefore be one word short and publish an N-character burst as N-1. The shortfall is silent, because a short burst raises no framing or parity error.

The driver reads the pointer until it stops moving instead. The errata's own workaround concerns a spurious completion interrupt, which this driver never routes, because it uses no DMA interrupt at all. Only the pointer half of the errata applies here.

The host suite models the window with a one-word retirement lag. The retirement-lag section (44) fails without the re-read, with exactly `got 15, want 16`.

## Transmitter's completion window

`is_transmitting()` was DMA-busy **or** TX-FIFO-non-empty **or** DE-high. The driver's own comments claimed all three can read false for a couple of PIO cycles between the state machine's `pull` retiring and DE being asserted - "a report can in principle arrive about one character time early". Phase D's item C2 existed to close that window. It is now closed by construction rather than by measurement: `is_transmitting()` became a single latch, which cannot read false mid-burst. The sampling below is the record of why that was safe to conclude.

A probe sampled the three terms together with the transmitter's program counter in a tight loop (~23 cycles per sample) across six one-byte bursts, and recorded every sample in which all three read false *after* the burst was under way. It ran at **115200**, not at 15,625,000, because the window is 1-2 state-machine cycles wide. One SM cycle is ~136 CPU cycles at that divider, and the bit time is eight of them, 1085 CPU cycles, against 1 CPU cycle per SM cycle at divider 1.0. At 15.6 Mbaud no CPU loop can sample the window at all, which is why an earlier 4000-burst histogram taken at that rate settled nothing.

In all six bursts the state was true from the first sample to sample 467-551. Every all-false sample came *after* the last true one, at the end of the burst and never at the start.

```
burst 0: send 0, samples 60000, first-true 0, last-true 467, false runs 8
    false at 468 pc 31
    ...
```

One burst is ~10,850 cycles of wire time and came to 468 samples, so the loop did begin at the burst's start. A start window would have been six to twelve samples wide. The program counter reads 31 at the end, which is the `wait 1 irq 5` that the earlier 4000-burst histogram also recorded - the tenth instruction of the TX program as it stood then, the eighth of today's.

Thus, C2 is withdrawn: there is no window to close. The header, `architecture.md` and this file say what was measured rather than what was assumed. The interval that does exist is inside `send()` itself, between the DMA being triggered and the word landing in the FIFO. No caller and no poll can run there, so it is not part of the contract.

## Tried and refuted

Each of these was tried before the cause was known, most of them with the probe present. They are therefore evidence about the state they were taken in, not about the cause. None of them is worth retrying:

| intervention | outcome |
|---|---|
| RX DMA priority | no change |
| inter-exchange guard (`guard`), 0-200 µs | no reliable change |
| slave reply delay, 0-10 µs | no reliable change |
| edge-qualified start (`wait 1` then `wait 0`) | no change |
| DE held across a burst (the release mark) | no change in the failure; kept |
| sampling offset `[3]` / `[5]` / `[7]` | no change |
| 38 µs of executed delay in the handler | no change |
| zero-instruction compiler barrier | no change |
| staging reads gated on and off at runtime | no change |
| request timing across two bit times (`pad`) | no change |

RX DMA priority cannot matter in any case: nothing else contends for the bus at that moment. Holding DE across a burst stays despite the null result: dropping it between two characters would release the bus for the length of the gap, and another talker could take it. The common factor in the rest is that none of them changes the *order* of the flush and the restart, which is the only thing that does.

## The paced soak, and `period=0`

Both boards were flashed from this machine with `picotool`, addressed by USB bus so that only the bench pair was touched, and driven over their USB consoles. The stress pair soaked for 20 s at the setting above: 15,625,000 baud, `tx_gap_bits` 1, a 1-byte request and a 16-byte reply, one exchange every 83 µs:

    cycles 240955  good 240955  stale 0  corrupt 0  refused 0  short 0  timeouts 0  errors 0
    rate 12047 exchanges/s over 20000 ms
    late 1 (max 163 us whole run)
    rtt max 218 (whole run) mean 36 us
    driver: violations 0  dropped 0  framing 0  parity 0

The one late cycle, at 163 µs against the recorded run's zero, is most likely the host reading that console during the run. The recorded rate is 12048 exchanges/s and this run made 12047.

`late` is **1 in the first window after a flash and 0 in every window after it**, always at cycle 1. An instrumented firmware shows exactly one slow exchange per boot, the first one, at 292 µs against a 38 µs mean; after that the schedule resyncs and the remaining cycles are paced to the microsecond. `max ... us whole run` is a whole-run field, not a window one: a single occurrence keeps printing in every later dump until the next flash, which makes it look like a standing fault when it is one event at boot.

**`period=0` is not a soak setting.** Running the master back to back reached 21855 exchanges/s with the driver still untouched: dropped, framing and parity zero, and no wrong-core report. That is worth having: the receiver neither loses nor mis-frames a burst under the hardest pacing the bench can produce. The bench's own protocol, however, does not survive it. The master re-sends before the slave's reply has landed, so `stale` rose to 240012 of 240955, and the slave was left a reply behind. The *next* run at the recorded setting then reported `good 941` until the slave was reset. That is a property of an unpaced exchange rather than of the driver, and the recorded configuration is the paced one. Its one use is pricing a CPU-side change, because there the exchange rate is CPU-bound rather than paced. That is how the RAM question below was priced, and the slave wants a reset afterwards.

## The same bench at 200 MHz

The two boards were rebuilt with the system clock at 200 MHz and the line rate at 25 Mbaud (`PICO_RS485_STRESS_SYS_CLK_KHZ=200000`, `PICO_RS485_STRESS_BAUD=25000000`), which puts the PIO divider at exactly 1.0 and the bit time at 40 ns; the firmware reports `clk_sys 200000000, divider 1.000` at init.

The same 1-byte request and 16-byte reply ran clean at that rate: 240,964 exchanges in the 20 s paced window, still 12,048 exchanges/s because the period paces it, and 338,831 exchanges back to back in 10 s, 33,883 exchanges/s. Every counter was zero in both windows, and the round trip is 24 µs mean against the 36 µs it takes at 15.6 Mbaud.

## The estimator's allowance

Measured by `rs485_hw_master` against `burst_duration_us()` at 115200, 8N1, `tx_gap_bits` 8, best of five. It is a sweep rather than a single length, because the slope across rows separates the per-character allowance from the per-burst overhead and one length cannot. The beacon that preceded it claimed eight characters at 115200 take "about 700 us", which is 80 bit times with the inter-character gap ignored. The default `tx_gap_bits` makes the same burst about 147 bit times, 1.27 ms:

    len  1: took   110 us   predicted   183 us    166% of measured
    len  2: took   281 us   predicted   365 us    129%
    len  4: took   622 us   predicted   730 us    117%
    len  8: took  1304 us   predicted  1459 us    111%
    len 16: took  2667 us   predicted  2917 us    109%

The slope between rows is the true per-character cost: 170.5 µs against a bit time of 8.68 µs, so **19.6 bit times per character** where the formula charges `char_bits + tx_gap_bits + 3` = 21. The allowance is 1.4 bit times too generous per character, 6.9% on the slope. The intercept is about -61 µs, one inter-character gap: the last character of a burst does not pay that gap, and the formula charges it anyway. That is why the error is much larger at short lengths: 66% high for a one-byte burst, where 1.4 bit times of slope error and a whole gap of intercept error are charged against a single character.

The constant could be 2 instead of 3, which would still sit above the measured 19.6 bit times and cut the slope error from 6.9% to under 2%. The one-byte figure would fall only from 66% to 58%. Or the wording, "an upper bound", can stand on its own. The suite pins the current value (`burst_duration_us(4) == 92`), so changing it is a test-visible decision rather than a wording fix.

## What the driver costs

Cortex-M0+, `-O3`, SDK 2.3.1, `arm-none-eabi-g++` 15.2.1, with the flags the committed Release build records, read from the library object's `.text.<symbol>` sections:

| symbol | bytes |
|---|---|
| `rx_done_isr` | 752 |
| `service_irq_` | 104 |
| `tx_done_isr` | 72 |
| the payload `PICO_RS485_ISR_IN_RAM` moves | **928** |
| `take_burst` | 416 |
| `arm_receive` | 132 |
| `burst_duration_us` | 152 |
| `rx_rewind` | 64 |
| `is_core_ok_` | 40 |
| `pico_rs485_core_violation` | 4 |

The object holds every member of the instantiation, including the inline ones, as a weak symbol, so its total is not what a firmware keeps. The SDK builds with `-ffunction-sections --gc-sections` and discards whatever nothing calls.

Nothing on the unpack path computes parity: `take_burst()` calls no parity helper, and `__paritysi2` survives only in `set_packet_config()`, the cold path that builds the character table.

A source-level change prices the same way. Moving the flush into `rx_rewind()` rather than leaving it at its call sites costs 20 bytes net, `rx_rewind()` growing while the handler shrinks.

`sizeof(PicoRS485)` is 1688 bytes at a maximum burst of 32, 3224 at 128 and 5272 at 256, measured against the host stub. The staging buffers and the transmit scratch dominate the instance, and both scale with it.

## What the RAM option buys

`PICO_RS485_ISR_IN_RAM` moves those three handlers into RAM. Measured on the stress firmware, three interleaved arms each with the slave held constant, 15 s windows at the setting above:

| handlers | exchanges/s | cycles per 15 s window |
|---|---|---|
| in flash (the default) | 22525.5 | 337896 / 337887 / 337889 |
| in RAM | 23766.4 | 356495 / 356496 / 356509 |

That is **+5.51%**, or 2.32 µs per exchange. Each handler entry accounts for about 1.16 µs - 145 cycles - since one exchange enters the handler twice. In the image, `.text` falls 37200 to 36336 (-864) and `.data` rises 4864 to 5744 (+880). Every driver counter was 0 and `late` 0 in both.

## What the host suite cannot reach

The suite does not model sample timing, so a fault in the sampling phase has to be identified on hardware. For the rest of what the host suite cannot reach, see [`test/README.md`](../test/README.md).
