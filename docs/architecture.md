# Architecture

A description of what this library does with the hardware, written from `include/PicoRS485.h`, `src/PicoRS485.cpp`, `src/rs485.pio` and the design notes.

## Shape of the library

One `PicoRS485` object is one half-duplex RS-485 channel. It owns a **whole PIO block**; all four state machines, the block's 32-word instruction memory, the block's IRQ 0 line - and **two DMA channels**. Nothing else can share the block while a channel is live.

It is a burst-oriented character pipe. `send()` takes characters; `receive()` hands back the characters of the received burst, where a burst is delimited by line silence.

Construction claims nothing and cannot fail; all resources are taken by `init()` and given back by the destructor (or by `release()` on any `init()` failure).

## The four PIO programs (`src/rs485.pio`)

| Machine | Program | Role | Y register | FIFO | Flags |
|---|---|---|---|---|---|
| `sm_tx` = 0 | `rs485_tx` | shifts framed characters out, drives DE | `char_bits - 1` | 8-deep TX (`PIO_FIFO_JOIN_TX`) | sets 4 per character, waits on 5 |
| `sm_rx` = 1 | `rs485_rx` | samples the line, drives the activity pin | `isr_bits - 2` | 8-deep RX (`PIO_FIFO_JOIN_RX`) | none |
| `sm_rxidle` = 2 | `rs485_rxidle` | watches the activity pin, times the idle window | `steps - 1` | none | sets 1 per burst |
| `sm_txgap` = 3 | `rs485_txgap` | times the inter-character gap | `tx_gap_bits - 1` | none | waits on 4, sets 5 |

The roles are the `sm_role` enum in the header, and a `static_assert` ties the enum to `NUM_PIO_STATE_MACHINES`: the channel takes every machine, so the role order is also the claim order.

Every program is clocked by the same divider, so one PIO cycle is `1/8` of a bit time at the configured baud (see `pio_cycles_per_bit`). The four programs come to 30 of the block's 32 instruction slots - tx 9, rx 9, idle 8, txgap 4 - so there is no room for a fifth.

### TX PIO program

```
   pull block           ; block until a word is available
   mov x, y side 1 [7]  ; reset bit counter and assert DE 1 bit time ahead of the start bit
bitloop:
   out pins, 1 [6]      ; send the next bit
   jmp x-- bitloop      ; loop until all bits are sent
   irq set 4 [7]        ; character complete (stop bit just ended) -> start gap timer, wait one bit time
   out x, 1             ; set x to end-of-burst marker bit
   jmp !x not_last      ; not the last character -> keep DE high
   irq set 0 side 0     ; last character -> burst over, DE low, the TX machine's IRQ to the CPU
not_last:
   wait 1 irq 5         ; block until the gap timer is done
```

Because the driver pre-frames each character, the PIO does no framing of its own: `out pins, 1` walks the bits of a word the driver already assembled. The bit above the character is the driver's own signal that it is the last of its burst, which is what lets DE be held from the first character to the last: the bus is never released between two characters of one burst, and the release point is decided in PIO cycles rather than by the CPU. `tx_gap_bits` is therefore internal timing between characters, not a window on the bus, and it cannot be zero - the timer counts down from `tx_gap_bits - 1`.

### RX PIO program

```
reset:
   nop side 0          ; no character in flight
   wait 0 pin 0        ; wait for start-bit edge
   nop [1] side 1      ; 2 cycles: puts the start-bit check below mid-bit; activity goes high
   jmp pin reset       ; if still high -> not a start bit -> reset
   mov x, y [5]        ; reset per-character bit counter
bitloop:
   in pins, 1          ; sample the next data bit
   jmp x-- bitloop [6] ; loop until all bits are sampled
   nop side 0          ; character done: drop activity BEFORE the last sample
   in pins, 1          ; sample last bit; autopush stalls here if the FIFO is full
```

`sm_config_set_in_shift(&c, true, true, isr_bits)` is the other half of the contract: **shift right** with **autopush at exactly `isr_bits`**. One character means one word pushed. The activity pin is side-set on the character's second instruction and goes low about half a bit time before the final stop bit ends; the idle timer uses that edge.

The last two instructions are ordered deliberately: activity drops *before* the final sample, which is the autopush. If the RX FIFO is full that push stalls the machine, and with the drop left until the wrap the activity pin would stay high, the idle timer would sit in `wait 0 pin 0` for good, and flag 1 would never be raised again - a receiver wedged until the block was re-initialised. Dropping first costs one cycle before the final `in`, and the `[5]` on the counter reload above hands it back: the start-bit validation and the stop sample keep one intra-bit offset, and the eight data bits land one cycle earlier in their bit. The absolute phase follows from when `wait 0 pin 0` catches the start edge, and no scope has confirmed it.

### RX idle timer

```
start:
    wait 1 pin 0        ; a character started (RX activity high)
    wait 0 pin 0        ; ...and finished (RX activity low)
    mov x, y            ; reset per-window step counter
count:
    jmp pin start       ; activity high again -> new character -> re-arm
    nop [6]             ; 7 cycles
    jmp pin start       ; second sample, one bit time after the first
    jmp x-- count [6]   ; 7 cycles: one step of two bit times
    irq 1               ; the window elapsed with no character -> end of burst, raise IRQ
```

The timer counts in steps of two bit times, so `Y + 1` steps is the window, and an odd `rx_idle_bits_thresh` is rounded up to the next step.

Two imprecisions belong with that listing. The window starts as the RX activity pin drops, which the receiver makes about half a bit before the stop bit ends, so it is that much shorter than the silence on the wire. `jmp pin` samples the pin once per bit time, the last sample a full bit time before `irq 1`, so activity that returns inside that final bit time is not seen and `irq 1` fires with a character already in flight. `rx_idle_bits_thresh` is therefore a threshold with about a bit time of slack rather than an exact measurement - the default of 16 leaves room for both, and `tx_gap_bits` has to stay below it by some margin.

Two flags are routed to the CPU: flag 0 is the TX machine's own IRQ, raised as DE drops at the end of a burst, and flag 1 is the RX machine's, raised by the receive-idle timer. Flags 4 and 5 are a private handshake between the transmitter and the gap timer, and they fire once per character, so routing them would be a per-character interrupt: on RP2040 flags 4-7 surface on the block's *IRQ 1* line rather than IRQ 0, which the driver leaves alone, routing flags 0 and 1 to IRQ 0 instead. It clears all four flags (0, 1, 4, 5) in both `init()` and `release()`, so a teardown that lands mid-handshake cannot leave one set for whoever takes the block next.

## Transmit path

1. `send(data, len)` is the entry point. It returns an error if the channel is inert, the caller is on the wrong core, `data` is null, `len` is 0 or over `max_burst_bytes`, or if a burst is in flight.
2. Each byte is looked up in `char_table_[]` and written to `tx_scratch_`.
3. One DMA channel is configured and triggered to move `len` words from `tx_scratch_` to `&pio->txf[sm_tx]`, paced by the transmitter's TX DREQ.
4. The state machine pulls a word and shifts it out. The FIFO is 8 deep and the DMA refills it as it drains, so the burst streams without the CPU.
5. Once the word is done sending, the TX PIO raises flag 4. The gap timer sees flag 4, counts `tx_gap_bits` steps of one bit time each, and raises flag 5, which releases the transmitter for the next word.
6. Once the last word is reached, the PIO releases DE, and raises flag 0. This triggers `tx_done_isr()`, which marks the burst as sent and calls the user callback.

The `char_table_[]` is prebuilt in `set_packet_config()`, and holds one word per data byte: bit 0 start, then data LSB first, then parity as configured (even or odd, `even_parity`), then the stop bit(s).

`send()` returns as soon as the words are **queued**, and the CPU is free from then on. DMA completion is deliberately not the event the driver uses: the channel
finishes while up to eight characters are still in the transmit FIFO, so "DMA done" means "queued", not "on the wire". The bus-release moment is reported instead, by the PIO's flag: `send()` latches the burst, and the end-of-burst interrupt clears the latch and calls the callback.

`is_transmitting()` reads that latch: one flag, set by `send()` and cleared by the interrupt as DE drops on the last character. It replaced an OR of three hardware terms - DMA busy, TX FIFO non-empty, DE high - which covered the same period but had a theoretical one-to-two-cycle window where all three read false at the start of a burst. The latch has no such window.

## Receive path

1. `init()` arms the RX DMA once; it stays armed. `arm_receive(buffer)` requires a **stopped** channel.
2. The PIO samples the line and autopushes one word per character into its RX FIFO.
3. The channel moves words from `&pio->rxf[sm_rx]` into `rx_staging_[buffer]`, paced by the receiver's RX DREQ, with a count of `max_burst_bytes + 1`.
4. When the activity pin has been idle for `rx_idle_bits_thresh` bit times, the timer raises flag 1, which is routed to `PIO_IRQ_NUM(pio, 0)`. This runs `service_irq_()`.
5. The handler stops the channel, reads `write_addr` until it stops moving, derives `chars` from how far it moved into the active staging buffer, and appends that buffer's index to `rx_ready_`.
6. It then arms a buffer that is **not queued** and returns.
7. `receive()` / `receive_for()` wait on `rx_ready_count_`, then `take_burst()` gives the filled buffer to the higher layer.

Step 5 reads the pointer until it stops moving because RP2040-E13 lets the abort return with a write still in flight, which can leave a single read one word short. The stop itself is the SDK's `dma_channel_cleanup()`, which clears the enable bit before aborting.

`take_burst()` masks the channel's own interrupt while it pops the queue and reads the staging buffer, so the handler cannot re-arm the buffer underneath it. That mask does not stop the RX DMA, so an idle flag raised inside the window is serviced with whatever the DMA has taken by then: a burst that started in the meantime has its first characters counted with the burst the flag belonged to, and the rest of it is published as another burst. The window is the length of the unpack (about 20 us at 125 MHz for a full buffer) so it takes back-to-back traffic to hit, and nothing here detects it. One flag marks the end of the burst it belongs to; it is not a promise that one flag is one published burst.

## Buffers and the queue

| Buffer | Size | Written by | Read by |
|---|---|---|---|
| `char_table_[256]` | 1 KB | `set_packet_config()` | `send()` |
| `tx_scratch_[max_burst_bytes]` | 1 KB | `send()` | TX DMA |
| `rx_staging_[3][max_burst_bytes + 1]` | ~3 KB | RX DMA, and the handler | `take_burst()` |
| `rx_ready_[2]` + `rx_length_[3]` | bytes | handler | `take_burst()` |

`max_burst_bytes` is 256 by default (`PICO_RS485_MAX_PACKET_BYTES`), matching Modbus RTU's address + PDU + CRC. Three staging buffers against a two-deep queue is the minimum that works: while the DMA fills one, up to two can be queued, and the handler always arms a buffer that is not queued. A full queue drops its **oldest** burst, and that drop is exactly what frees the buffer the next arm needs, so the receiver never has to stop listening.

Two bursts can be queued; a third that completes before the caller drains drops the oldest and increments `dropped_burst_count()`. A burst longer than `max_burst_bytes`, or one the DMA somehow did not finish, is dropped whole rather than published truncated, and counted there too. An idle flag with no character behind it is counted by `empty_burst_count()` instead, so a burst that ended empty can be told apart from one that never arrived.

## What is shared with the interrupt, and how

The receive queue crosses between the handler and the caller: the queue state `rx_ready_`, `rx_ready_count_`, `rx_active_`, `rx_length_`, `rx_armed_base_`, and the `dropped_bursts_` and `empty_bursts_` counters the getters read. All seven are `volatile`, pinned by three `static_assert`s in the header - the five queue members by one, either counter by another - because the handler writes them and the caller reads them, and a reader's loop body need not contain a call: the predicates a caller polls - `has_burst()`, `in_flight()`, `tx_completion_pending()`, `last_sent_at_us()` - read them directly. See "Optimisation-sensitive state" in the design notes. `__compiler_memory_barrier()` appears on both sides of the publication: the handler writes a buffer's length and then the queue entry, the caller reads the entry and then the length.

Everything else follows from two rules: `set_packet_config()` is the only function that writes `pkt_`, `char_bits_`, `isr_bits_` and the character table, so they cannot drift from each other or from the machines; and the DMA is the only thing that reads the RX FIFO *for data* - `init()` and the recovery path do flush it, and throw away what they find - just as it is the only producer into the TX FIFO.

The sent-completion state is written by the interrupt and by the caller: `tx_pending_`, `tx_callback_`, `tx_user_` and `tx_burst_id_` are written by `send()`, read and cleared by the end-of-burst interrupt, and discarded by `release()`. The latch and the timestamp are `volatile` under the rule above, since the inline predicates `tx_completion_pending()` and `last_sent_at_us()` read them from the caller; `tx_user_` and `tx_burst_id_` cross the same boundary but no accessor reads them, so they need neither volatile nor a barrier. The burst id counts sends, which is what lets a report be matched to the burst that owes it.

## Lifecycle

**`init()`** does this in order, each step released by `release()` on failure:

1. Refuse if already live -> refuse out-of-range pins, or two pins that are the same; validate the packet and clock configs.
2. `is_pio_valid()` -> the block is a real instance, the four pins are inside the block's GPIO window (on chips where that applies), **all four** machines are unclaimed, and the block's IRQ 0 line carries no handler yet.
3. Claim all four machines, then set `pio_` -> a non-null `pio_` means "this block is ours".
4. Clear the block's instruction memory; load all four programs, keeping the offsets.
5. `set_packet_config()` records the config, builds the character table and programs the four machines from it.
6. Clear flags 0, 1, 4 and 5, so nothing stale from a previous PIO use can fire when the machines start.
7. Claim the two DMA channels and arm the receiver on the first staging buffer, before any machine is enabled, so a push is never left without a consumer.
8. Run the four program-init helpers in role order; each enables its own machine.
9. Flush the RX FIFO, then restart the receiver and rewind it to the top of its program (`pio_sm_restart()` clears the machine's counters but does not rewind its program counter). If a burst that began while `init()` was configuring is being received, only what had already reached the FIFO is discarded.
10. Register the exclusive handler for the block's IRQ 0, route flags 0 and 1 to it, enable it, and then clear the idle flag once more so the channel starts with no flag of its own set.

**`release()`** does this in order:
- Unroute and remove the interrupt
- Clear flags 0, 1, 4 and 5
- Stop and unclaim both DMA channels
- Reset the queue state
- Drop an unreported completion
- Disable all four machines
- Drop DE low through the PIO
- Clear the instruction memory
- Unclaim the state machines
- Hand the pins back to SIO
- Clear `pio_` last, so `is_valid()` stays true until teardown has finished

## Deliberate limits

- **One completion interrupt per direction.** `send()` queues and returns; the block's IRQ 0 line reports the bus release when DE drops, and `burst_duration_us()` bounds the wait.
- **Bursts must arrive an idle window apart.** The receiver ends a burst after `rx_idle_bits_thresh` of quiet. A peer that starts its next burst inside that window has the two merged into one staged burst; renewed activity re-arms the timer, so its flag never fires and nothing is reported - while one that starts just after the flag, with a character still in the FIFO, has that burst dropped and counted as over-long. Neither is distinguishable from real traffic, which is why this is a requirement.
- **The idle window is in bit times, not in time.** `rx_idle_bits_thresh` is a bit count - 16 by default, 64 at most - so the silence it defines scales with the baud rate. It expresses Modbus RTU's 3.5 characters, but not the fixed 1.75 ms floor Modbus requires above 19200 baud: 64 bit times falls short of that above about 36.6 kbaud. Treat the burst event as the frame boundary and let the protocol own the rest.
- **No echo suppression.** This library assumes/requires the board ties the transceiver's `/RE` to `DE`, which is what keeps the receiver off while the transmitter is being driven, and stops the receiver from seeing the transmitted data.
- **A line held low looks like traffic.** The receiver resynchronises only on a start bit that is low at the mid-bit sample and high before it, so a line stuck dominant yields a stream of zero-filled bursts, each character counted as a framing error, until the line idles high again. Nothing latches a bus fault or disables the receiver; the protocol's own timeout has to notice.
- **Parity and framing errors are reported, never enforced.** A bad character is still delivered; RTU integrity is an upper layer concern. `framing_error_count()` and `parity_error_count()` count them per character as a burst is unpacked, so a dropped burst's characters are never counted, and parity stays 0 without a parity bit.
- **`receive()` may return a length over the caller's capacity** (the true burst length) and fills what fits.
- **`burst_duration_us()` is an upper bound**, including about three bit times per character of PIO bookkeeping: the DE lead-in, the DE hold where a short gap does not already cover it, and the pull that starts the next character.
- **One core per instance.** Nothing is synchronised across cores, and the handler is registered on the core that called `init()`. Calls that return a status code report `err_wrong_core` if called from the wrong core (`set_packet_config()`, `send()`, `receive()`, `receive_for()` and `try_receive()`). The four functions that cannot return an error code answer busy and call `pico_rs485_core_violation()` (`has_burst()`, `is_transmitting()`, `is_receiving()` and `in_flight()`). The remaining accessors, like `is_valid()` and `framing_error_count()`, are plain reads and need no core gate.
