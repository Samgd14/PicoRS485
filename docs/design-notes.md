# PicoRS485 design notes

Rationale behind the driver's public interface. The header carries the API contract only (doxygen); `docs/architecture.md` says what the code does, and this file says why.

## Naming: character and burst

A *character* is one start/data/parity/stop group on the wire. A *burst* is the run of characters between two windows of line silence. Those are the only two nouns for wire events.

`frame` is a modifier only (`framing_error_count()`, a "framed" table entry), and `word` means the 32-bit PIO/DMA FIFO entry only. A character carries 5-8 data bits, so it is not a byte. One character occupying one FIFO word is a layout detail, not a name for it.

## PIO program shape

Every program runs on the same clock, so one bit time is exactly 8 PIO cycles and a PIO cycle is `1/8` of a bit. The optional side-set takes two of the five delay/side-set bits, which is why the largest delay is `[7]` rather than `[8]`.

The clock divider scales `clk_sys` down to that PIO clock. It has 8 fractional bits, so `clkdiv = clk_sys / (baudrate * pio_cycles_per_bit)`, and its fractional increment is `1/clkdiv_frac_scale`.

## The receive-activity pin

The channel takes the three bus pins plus a receive-activity pin. The idle timer needs a level rather than an event, and the receiver's side-set is free. So the receiver drives that pin while a character is in flight and the timer polls it; nothing external connects to it.

## The GPIO base and pin reachability

The SDK compiles `PICO_PIO_USE_GPIO_BASE` to 0 on RP2040 and RP2350A, and to `NUM_BANK0_GPIOS > 32` - that is, 1 - on RP2350B. A PIO can address only 32 pins. On RP2350B, which 32 is selected by the block's GPIO base: base 0 covers GPIO0-31, and base 16 covers GPIO16-47. No single block can reach pins 0-15 and 32-47 at the same time.

`is_pio_valid()` compares all four pins against the window of the block it is given, and returns `err_gpio_base` on a mismatch. Per-pin checks cover the machine configurations too, because a configuration can only straddle two windows if one of its pins is unreachable anyway. A pin past 31 needs the 64-bit mask form - `pio_sm_set_pins_with_mask64`, since `1u << 40` is undefined - while the `sm_config_set_*_pins` calls take absolute pin numbers and subtract the base internally. The driver never changes the base. `pio_set_gpio_base()` reprograms the whole block and fails if any program is loaded in it, so a caller who needs pins above 31 calls it before `init()`.

The pin window is not the only thing the SDK can refuse. `pio_sm_init()` and `pio_sm_set_consecutive_pindirs()` return error codes of their own, so the four `% c-sdk` init helpers return the first error they see and `init()` maps it to `err_sm_config` rather than discarding it. The up-front check only predicts which refusals are possible; returning the SDK's own error is what keeps an unpredicted one from being silent.

## init(): failure codes, release and retry

`init()` claims the PIO the caller names, one machine and one program per role, then configures them. It returns `ok` or a negative `error`, and on any failure it releases whatever it managed to take. The object returns to its pre-init state, so `init()` can simply be retried.

The baudrate form derives the divider, and reports `err_baudrate` if the rate cannot be represented. The explicit-clock form takes a config the caller built or restored, and checks it with `is_clock_config_valid()`.

Every way `init()` can fail is a negative return code, so a caller can tell a bad pin from a busy bus.

`init()`'s `set_packet_config()` branch is unreachable, and stays as a net for a future failure path there: the config is validated a few lines earlier, `pio_` is already set, no DMA channel is claimed and the queue is empty, so none of its codes can be returned.

## Error precedence

When more than one thing is wrong, the first check to run decides the code. Both forms check `err_busy` first, so a live channel always reports that. The baudrate form then derives the divider, so an unrepresentable rate reports `err_baudrate` before anything the explicit form shares.

After that both forms run the same checks in this order:

1. the pins
2. the packet config
3. the divider
4. the PIO: `err_pio`, `err_gpio_base`, `err_state_machine`, `err_irq_in_use`
5. the programs: `err_program_load`
6. the two DMA claims: `err_no_dma`
7. the four program-init helpers, which also program the pins and the dividers: `err_sm_config`

The pin argument checks stay in `init()` rather than moving into `is_pio_valid()`: they are properties of the pin arguments rather than of the block, and moving them would change this order.

## Instruction memory

A channel needs one machine per program, and a block has exactly four machines, so it takes the whole block. Nothing else can be running there. A block is an exclusive resource, so a channel is not copyable: the destructor already suppresses the implicit moves, and deleting them too states the intent rather than leaving it to be inferred.

`init()` therefore empties the instruction memory before loading the four programs, and `release()` empties it again while the machines are still claimed. Starting from empty memory also makes the load offsets deterministic, since the SDK allocates downward from the top of the memory.

The clear erases every slot, so a program another library loaded into the block without claiming a machine is discarded. That is the counterpart of taking all four machines, and it makes the block's ownership unambiguous rather than shared.

There is deliberately no program-space pre-check. `pio_can_add_program()` answers whether one program fits in the free space as it stands, and nothing is added while the question is asked, so all four programs are asked against the same free space. Four programs of four words would each "fit" in ten free words without fitting together. The only reliable report is `pio_add_program()` itself, and a refusal there becomes `err_program_load`.

## Achievable rate and the fractional divider

Every PIO program uses 8 PIO cycles per bit time, so the clock divider supplies a
whole bit as `clkdiv = clk_sys / (baudrate * 8)` and the divider needs its
fractional part. Truncating to an integer puts a large quantisation error on the
bit time: at 125 MHz, 921600 lands on 16.954 and comes out 6.0% fast, 4 Mbps on
3.906 and comes out 30% fast, and 8 Mbps on 1.953 and comes out 95% fast - all
well outside the +-2% per character a UART tolerates. (Rounding instead of
truncating is kinder but still leaves 4 and 8 Mbps about 2.3% slow.) The same rates
are exact once 8 fractional bits are used, which is what the driver does: 3.90625
and 1.953125 are representable exactly, and 921600 lands within 0.01%.

`compute_clock_config()` derives the divider against the current `clk_sys`. It
returns false if the rate cannot be represented; `out` is untouched unless the
call succeeds. The two-argument form against an explicit `clk_sys` is the core of
the pair, and the one that can be exercised without hardware.

## Clock config validation

`is_clock_config_valid()` checks both that the divider can be programmed and that it follows from the `sys_hz` and `baudrate` it carries. The two fields are exactly what the PIO holds - 16 bits whole and 8 bits fractional - so the only value the hardware cannot take is zero, and a divider that does not follow would make `burst_duration_us()` describe a bit rate the channel does not run at. The check rebuilds the divider from the clock and the rate and compares, which refuses both.

`sys_hz` and `baudrate` are checked for zero as well. Neither reaches the hardware, but a zero in either means the struct was not filled in.

## Packet config validation

`is_packet_config_valid()` answers whether the PIO programs can express the requested framing and the two timing windows. It is the single place those ranges are checked, so a caller can check a config before using it.

The bounds named on the class are per-field limits of the programs. The cross-rule between them narrows what is usable: `tx_gap_bits + stop_bits + rx_idle_margin_bits <= rx_idle_bits_thresh` leaves a gap of 1..31 and a threshold of 5..64. A caller that raises the gap without raising the threshold is refused, and a gap near its maximum needs a threshold near its own maximum.

That is why the two are never derived from each other: the accepted set is the same either way, and deriving one would alter two published values for nothing.

## Applying a new packet config

`set_packet_config()` changes the framing and both windows on a live channel. The machines read their timings when they start a character, gap or idle window, so the change lands on the next one. The receiver's update also rewrites the autopush threshold, which is why the call refuses while anything is going out, arriving or still queued (`err_in_flight`).

The check is approximate on the receive side. The activity pin drops about half a bit time before the character ends, and the write pointer only moves once the DMA's write retires, so a change can land in that window - the last half bit of a character. That is why the header still asks the caller to leave the bus alone.

`pkt_`, `char_bits_` and `isr_bits_` are written only inside `set_packet_config()`, which programs the machines from that same config, so nothing can change one copy without the other. A separate recorder would be callable from anywhere in the class and would leave the state machines describing the previous config, which is why there isn't one. `init()` goes through it as well: it calls `set_packet_config()` before the program init helpers, so those configure each machine from the recorded values and a single piece of arithmetic produces both copies. `get_packet_config()` reports the recorded config.

The check and the writes are one critical section: the block's interrupt is masked across both. Otherwise the handler could publish a burst in between, and a burst sampled under the old framing would be queued for the new masks. Masking is enough because `is_receiving()` covers a burst that is merely arriving and the flags are sticky, so anything raised while masked is serviced once the line goes back.

Applying the packet config before the program-init helpers has one consequence: if a helper then fails, the record holds a config that only some of the machines were configured from. That state is unreachable from outside, because `is_valid()` is false and a retry records and programs again.

## Word grouping and the recovery order

The receiver's autopush fires once per character. The counter that decides when it fires is cleared by `pio_sm_restart()` and by a completed push, and nothing else in the driver touches it, so the driver's obligation is to leave it at zero whenever the receiver starts listening. It meets that by draining the FIFO *before* restarting, never after.

A push stalled because the FIFO is full is a *program* instruction. `pio_sm_restart()` clears the counter and the input shift register, but it does not cancel that instruction, and the flush that finally makes room retires it against the counter the restart had just cleared. From then on every word holds the previous character's stop sample in front of its data - one sample early, for the life of the channel - because a character pushes exactly as many samples as it consumes. A fault that produced the same rotation at every bit rate ruled out anything transient. Draining first, then restarting, leaves the counter and the FIFO both empty.

Flushing twice is deliberate. The first flush releases the stalled push into the FIFO it just emptied, so the second clears what that left behind.

`init()` closes the same window from the other side: the RX channel is armed *before* the program init helpers enable the machines, so there is never a receiver without a consumer and a push can never block in the first place. What remains of a burst arriving mid-init is a fragment the counters show and the payloads do not care about.

The flush is unconditional, and it lives inside `rx_rewind()` rather than at the call sites. `dma_fell_behind` is sampled *before* the burst is published, so gating the drain on it left a character arriving in between reaching `pio_sm_restart()` with its push unflushed - the one path that could reach a restart with no flush at all. Folding the two in closes that window.

The two raw words that fingerprint the fault, and the measurements behind all of this, are
in `bench-evidence.md`.

## Bursts, DMA and the two paths

The driver is a burst-oriented character pipe: `send()` takes characters, and `receive()` hands back the characters of the next burst. It knows nothing about Modbus, so any delimiter-framed protocol sits above it unchanged.

**Transmit.** Each character is framed by a table that `set_packet_config()` builds, and one DMA channel streams it into the TX FIFO, paced by the FIFO's DREQ. The PIO adds no framing of its own, which is why the table exists at all.

`send()` returns when the words are queued, and the transmit FIFO holds eight of them, so "queued" is not "on the wire". What the driver reports instead is the bus release, and the report is exact: the interrupt costs one comparison when there is nothing to report.

A burst that owes a callback holds the slot until it has been reported, so a report can never be mistaken for another burst's. A send that asks for nothing owes no report, so it cannot leave a later send failing as `err_incomplete_tx`; the burst is still on the wire until the interrupt reports it, and a send in that window is refused as `err_in_flight` either way. The callback carries the burst's id, which counts up across the instance's life, so a caller retrying a burst can tell which attempt was acknowledged. Teardown drops an unreported completion rather than delivering it.

**Receive.** One DMA channel stays armed with the staging buffer's capacity, and it is the only thing that reads the receive FIFO for data. In neither direction does anything per character touch the CPU. The idle timer's flag is the delimiter: the handler stops the channel, publishes what arrived and re-arms, and `receive()` does the unpacking, so the interrupt stays short. A flag with nothing behind it is noise; a character still in the FIFO means the DMA fell behind, and that is a lost burst rather than a truncated one.

The receiver shifts right, so the sampled bits are left-justified and the byte is `word >> (32 - isr_bits_)`. The SDK's own UART RX example reads the byte from the uppermost byte of the FIFO, which is where that bit layout is pinned down. Each character's stop bit and parity bit are checked on the way past and counted, and neither rejects the character. The expected parity is not recomputed: it is read back out of the same framing table the transmitter built, so the two directions cannot disagree about the convention.

## Why the length comes from the write pointer

The burst length is how far the RX channel's write pointer moved into the staging buffer: `rx_done_isr()` stops the channel, reads `write_addr`, and divides the span by the word size. The pointer is the one piece of the channel's state that survives that stop. It moves only when a write retires, and `dma_channel_cleanup()` - enable low, then abort - does not return until the transfer in flight has retired, so the read finds it past the last word the burst put in the buffer.

The transfer count cannot serve the same purpose, because the stop this handler has to perform is what invalidates it. `dma_channel_cleanup()` clears the enable bit and then aborts the channel, and the abort leaves `TRANS_COUNT` reading zero. So `words = (max_burst_bytes + 1) - transfer_count` taken after the stop is the full arm count every time. Every correctly received burst then looks longer than a maximum burst and is dropped instead of published. Because nothing was unpacked, no framing or parity error was counted either, which left the dropped-burst counter as the only symptom.

Reading the count before the stop instead leaves a narrower hole. A transfer already in flight when the pause landed retired during the abort, and decremented the count the read had already taken, so that burst was published one byte short, silently, because a truncated burst is never unpacked and so raises no framing or parity error. The pointer's window is narrower still, since the retired write is what moved it - but not empty.

RP2040-E13 closes the remaining gap: aborting a channel with a transfer in flight lets the ABORT status bit clear prematurely, so the BUSY wait can return before that write retires, and a single read of the pointer can be one word short. `rx_done_isr()` therefore reads the pointer until it stops moving - the pointer only ever advances, so a stable value means the transfer has landed. The errata's own workaround concerns a spurious completion interrupt, and this driver routes no DMA interrupt at all, so only the pointer half applies here.

The register map does not say that an abort clears the count, so the zero should be read as measured behaviour rather than as a documented property. The pointer was measured the same way, but the design leans on something narrower and mechanical: a write that retires moves the pointer, and the abort waits for the write in flight, so the pointer is past every word the channel took. The count can only offer that while the channel is armed, which is exactly the state the handler cannot leave it in.

The arm count stays one more than a maximum burst: an exactly-full 256-byte burst leaves the pointer at `base + 256` words and is published, while a 257-byte burst runs the channel to completion, leaves it at `base + 257` words, and is dropped as over-long. Anything the pointer reports past a maximum burst is dropped the same way rather than published, so a wild register cannot be believed any further than a wild count could.

## The RX pull-up

Tying the transceiver's `/RE` to `DE` - the wiring this driver assumes - disables the receiver and puts its `RO` output into high impedance for the whole of every transmission. That leaves the MCU's RX pin floating, and a floating input is framed as traffic: the noise becomes characters in the FIFO, and most of them fail the stop-bit check.

This is why `rs485_rx_program_init()` sets an internal pull-up on the RX pin. The driver's silence delimiter and its framing checks both assume a defined idle, and the pull-up is what supplies one while the transceiver is not driving. An external pull-up on `RO` holds the line at the transceiver rather than at the pin, which is the better fix at a hardware revision.

## What the driver leaves to the caller

- **When the last bit left, to the microsecond.** The interrupt reports the bus release, which the PIO raises with the DE drop, so `last_sent_at_us()` carries the interrupt's own latency rather than the instant of the drop. `burst_duration_us()` remains the way to bound it.
- **Whether a collision happened.** The receiver is off while we drive, so a second talker is invisible, and only the protocol's own timeout can detect one.
- **A character with bad parity is delivered anyway.** Parity is a link-level check here, not a filter: the mismatch is counted, and RTU integrity belongs to the CRC.

## Optimisation-sensitive state

`receive()` waits on a counter the interrupt handler writes. Nothing in an empty loop body can change a plain object, so with the SDK's `tight_loop_contents()` - an empty always-inline no-op - the optimiser is entitled to hoist that load out of the loop, or to delete the loop outright. At `-O2` the wait disappears and the call returns 0 as if an empty burst had arrived. At `-O1` the load is hoisted and the loop spins on a value it will never re-read.

Both readings above are of a wait that spins. Where the wait calls `sem_acquire_blocking()` or `sem_acquire_timeout_us()`, the count is re-read after a call, so nothing there depends on `volatile`. What does depend on it is the `tight_loop_contents()` spin above, and the predicates a caller polls in a loop of its own: `has_burst()`, `in_flight()`, `tx_completion_pending()` and `last_sent_at_us()` read the shared state directly.

The members the handler shares with the caller are therefore `volatile` - the five queue members, the dropped-burst and empty-burst counters, and the transmit latch with its timestamp - and `static_assert`s pin that, so removing one breaks the build rather than the firmware. `__compiler_memory_barrier()` on both sides of the publication keeps a buffer and its length ordered against the queue entry that publishes them.

The host suite cannot catch that class of bug, and does not pretend to. Detecting it means having the handler run from *inside* the wait, and any call that could do that is itself a compiler barrier, which would protect the loop. The `static_assert` is what guards it instead.

## Teardown and claiming

The pins go back to SIO, but DE is left driven low rather than floating. An undefined driver enable lets the transceiver put an equally undefined data input on the bus, and the transmitter can be halted mid-character with DE still asserted by its side-set. So DE is dropped through the PIO first, and then handed to SIO with the enable already set, leaving no window where it floats. TX, RX and the activity pin go back as inputs, which DE being low makes harmless, and the receiver's pull-up on RX goes with them.

`init()` takes the block inline: `is_pio_valid()` runs first, then the four `pio_sm_claim()` calls, then `pio_ = pio` marks the channel live. That ordering is what makes claiming every machine safe, since `pio_sm_claim()` asserts on a machine someone else owns.

## Interrupt masking

`take_burst()` masks the channel's interrupt while it reads the queue and the buffer the DMA filled, and `set_packet_config()` masks it across its in-flight check and its writes. Both put the line back as they found it rather than enabling it, so a caller that masked it deliberately still finds it masked. Nothing else masks the line. `receive()`'s wait does not: the state it waits on is `volatile` instead (see "Optimisation-sensitive state").

That mask is also what makes three staging buffers enough: the buffer being unpacked is a queued one, so without it the handler could re-arm the DMA onto a buffer the caller is reading.

## Guards that cannot be reached today

Three places defend against states the code cannot currently enter, and are kept because
what they protect against fails silently:

- `take_burst()` answers a zero-length burst rather than popping a stale queue entry, if
  it is ever called with nothing queued.
- The handler frees the oldest entry rather than leaving the receiver unarmed, if every
  buffer were ever queued.
- `release()` zeroes the queue count and resets the semaphore, so nothing in the object
  can be mistaken for a staged burst after teardown.

All three are unreachable by construction, which is why none of them has a test.

The second guard has been proposed for removal more than once, and a size argument cannot carry it: the branch costs 140 bytes of `rx_done_isr` on 15.2.1, 120 on 14.3.0 and *saves* 92 on 9.2.1. The direction flips between toolchain versions, and what it guards is silent corruption of a staging buffer that is already queued.

## The maximum packet size is a build setting

`PICO_RS485_MAX_PACKET_BYTES` sizes members of the class, so it is a property of the build
rather than of one translation unit: the library and every consumer have to be compiled
with the same value, or they disagree about where the members live and read each other's
memory. The build sets it once, on the library target, and propagates it PUBLIC.

A header cannot state that politely enough to be safe, so it is enforced by the type. The
class is a template on the maximum burst length, and `PicoRS485` is an alias for the
instantiation this build asks for; the library compiles one explicit instantiation. Two
values give two types, so two translation units do not merely disagree about one class -
they are well-formed code that fails to link, with the value it wanted named in the error.

A value-derived symbol was tried first, and it failed for a reason worth remembering: the name came from the macro's token rather than its value, so `0x100` and `256` were two tags for one number. The reference also had to sit in the accessor's own code to be a guard at all. Moved into a shared object's initialiser, it became a COMDAT group the linker unifies, and whether a mismatch still failed to link depended on link order - caught with the library object first, silently linked with the consumer first. A guard that holds on one link order and not another is worse than none, because it looks fixed.

A second length has to be exported before a consumer can name it: `PICO_RS485_BURST_SIZES` lists the extras and CMake writes one instantiation for each, since only the translation unit holding the member definitions can instantiate them. What stayed library-wide is deliberate. `instances_` is per instantiation, and that is what makes the dispatch work - a block's handler is the thunk of the instantiation that owns the block, so two lengths on two blocks do not look in each other's table. The driver keeps no counter of its own: `pico_rs485_core_violation()` is the one place a wrong-core call is reported, and a host that wants a count keeps it there. One symbol with one definition cannot disagree with itself the way a count inside a class template could.

The parameter also makes a second maximum length in one program expressible, so a small channel no longer pays for the largest one. The staging buffers and the transmit scratch dominate the instance, and both scale with the maximum burst; the sizes are in `bench-evidence.md`. The library object grows when a length is exported - an explicit instantiation emits every member as a weak symbol, the inline ones included - while the linked image barely does, since the SDK builds with `-ffunction-sections --gc-sections` and discards whatever nothing calls.

## Decided, and kept deliberately

- **`burst_duration_us()`'s clamp** only matters on a 64-bit host - on the target a `size_t` cannot exceed the 32-bit product - but it keeps the function total for any caller, and the host suite pins it.
- **`dma_channel_cleanup()` rather than a hand-rolled stop.** The SDK call does four or five peripheral accesses this driver never needs, worth 25-40 cycles per burst - under half a percent of one 83 us exchange at the bench's 12 kHz. The narrower stop was the source of earlier bugs, so the SDK call stays.
- **The busy predicates stay split.** The layer above needs to see what the driver is doing, so `is_transmitting()`, `is_receiving()` and `in_flight()` remain public and distinct rather than collapsing into one `is_busy()`.
- **`PICO_RS485_ISR_IN_RAM` stays.** It moves the three end-of-burst handlers - `service_irq_()`, `rx_done_isr()` and `tx_done_isr()` - into RAM, and it is the only lever on where their code is fetched from. What it buys and what it costs are measured in `bench-evidence.md`.
- **`try_receive()` stays.** It is an inline forward to `receive_for(out, max, 0u)` and the two emit identical thunks, so deleting the name would save nothing and cost the one call a polling caller wants.
- **The receive calls return a length or a negative error in one `int`.** A design smell, but the one API break with no cheap migration.

## Considered and rejected

- **`pio_sm_init()` in place of `pio_sm_restart()` + `pio_sm_exec(jmp)`.** A null config applies the
  default state machine config, wiping the divider, shift control, pin mapping and exec control, and
  re-phases the divider.
- **`sm_config_set_clkdiv_int_frac8()` as a validator.** Not the same predicate: the SDK asserts a
  range and truncates, where the library refuses a divider it cannot program.
- **The SDK's `pio_calculate_clkdiv8_from_float()`.** It converts a divider to int + 1/256; deriving a
  divider from a bit rate is the caller's job, and the float version rounds according to the
  consumer-visible `PICO_PIO_CLKDIV_ROUND_NEAREST` and silently truncates out of range. The local
  integer version is stricter, deterministic, and `constexpr`, which the validator needs.
- **`gpio_get_out_level()` instead of `gpio_get(pin_rx_act_)`.** The former reads the SIO latch, which is
  always 0 for a PIO-driven output.
- **`pico/util/queue.h` and `irq_add_shared_handler()`** in place of the staging ring and the per-block
  registry: the queue heap-allocates, takes a spin lock per operation and hides the slot index the
  re-arm needs, and a shared handler cannot route to a per-block owner.
- **Byte-packed RX staging, and dropping the character table.** Both trade RAM for work per character,
  which the byte path must never gain: at 15,625,000 baud one character is 640 ns, and the exception
  entry alone would eat 40% of that.
- **Returning length-or-error in one `int`** from the receive calls. A design smell, but the one API
  break with no cheap migration.
- **A 0.0-divider guard** was removed as provably unreachable: the range test refuses any rate needing
  a divider below 1.0, so the integer part is always at least one.
- **`rx_next_free_` as derived state.** It is redundant in principle - always `rx_next_(rx_active_)` -
  but deriving it grows `rx_done_isr` by 100 bytes.
- **Dropping the two `volatile` that are not load-bearing** (`rx_active_`, `rx_length_`). Measured -4 bytes rather than -8, because `arm_receive()` grows, and it would delete the assertion that pins the invariant the receive wait depends on.
- **Hoisting the per-burst DMA config.** The compiler already folds it: `arm_receive()` contains no calls and four stores at `-O3`.
- **Narrowing `take_burst()`'s critical section.** It would corrupt the buffer being unpacked: the queue entry is popped before the buffer is read, so a narrower window lets the handler re-arm onto it.

## Sleeping in the receive wait

`receive()` sleeps rather than spins, and `receive_for()` sleeps where the platform can wake it at the deadline. The handler signals a semaphore when it publishes a burst, and the wait acquires it: one permit at most, since a signal means "wake up" and the queue count says whether there is a burst to take. A spurious permit simply re-runs the count test, so the semaphore never needs draining.

Two consequences are worth knowing. The wait blocks, so it must not be called from an interrupt, and it cannot make progress while the PIO's line can never be serviced - which is equally true of the spin, because the flag is what changes the count. The bounded wait also needs the SDK's default alarm pool to wake it at its deadline; where `PICO_TIME_DEFAULT_ALARM_POOL_DISABLED` is set it spins instead. That is checked at compile time rather than offered as a build option: the option would be a switch between two mechanisms that fail in the same circumstances.
