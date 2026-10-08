# PicoRS485

A half-duplex RS-485 channel for RP2040 and RP2350, driven by four PIO state machines and two DMA channels.

One `PicoRS485` object is one channel, and it owns a whole PIO block. It moves bursts of characters: `send()` frames a burst and hands it to the DMA, and `receive()` / `receive_for()` hands back the characters of the next burst, where a burst ends at line silence. It uses UART framing for the characters.

## Wiring

A channel needs four GPIOs:

| Pin | Connect to |
|---|---|
| TX | Transceiver `DI` |
| RX | Transceiver `RO` |
| DE | Transceiver `DE` and `/RE` |
| RX activity | Nothing |

- **`/RE` must be tied to `DE`.** The library has no provision for a separate `/RE` pin.
- **The receive-activity needs a reserved GPIO.** It's used as an inter-SM signal for end-of-burst timing.
- **RX floats while the driver transmits,** because the receiver is off. The driver enables the internal pull-up on that pin to hold it idle, and an external, stronger pull-up is recommended. Otherwise, the receiver sees random noise it might interpret as packets.
- **All four pins have to reach one PIO block.** On RP2350B, a PIO block addresses 32 pins selected by its GPIO base, so all used pins have to be within the same 32-pin range.

## Adding to a Pico SDK/CMake project

In a Pico SDK project with `PICO_SDK_PATH` set:

```cmake
set(PICO_RS485_HW_TESTS OFF) # avoid adding the test firmware
add_subdirectory(path/to/PicoRS485)
target_link_libraries(my_firmware PicoRS485)
```

The target carries its own include directory, its sources and the generated PIO header. Pico SDK 2.x, CMake 3.13 and C++17 are required.

This repository is also a top-level CMake project, which is how its own firmware is built.

## Adding to a PlatformIO project

The `raspberrypi` platform in the PlatformIO registry is not up to date. Instead, use the one specified in the example `platformio.ini` below.

```ini
[env:pico]
platform  = https://github.com/maxgerhardt/platform-raspberrypi.git
board     = pico
framework = picosdk

; This library is not in the PlatformIO registry, so get it directly from GitHub
lib_deps = https://github.com/Samgd14/PicoRS485_dev2.git

; Use build flags to change library compile settings
build_flags =
    -DPICO_RS485_MAX_PACKET_BYTES=256
```

## Getting started

Initializing the PicoRS485 object:

```cpp
#include "PicoRS485.h"
#include "pico/stdlib.h"

#define PIN_TX 0
#define PIN_RX 1
#define PIN_DE 2
#define PIN_RX_ACT 3

PicoRS485 rs485(PIN_TX, PIN_RX, PIN_DE, PIN_RX_ACT);

int main() {
    // Framing and the two timing windows
    // (default values for example purposes)
    pico_rs485_packet_config pck{};
    pck.data_bits           = 8;    // 5-8
    pck.parity_bits         = 0;    // 0-1, none when 0
    pck.stop_bits           = 1;    // 1-2
    pck.tx_gap_bits         = 8;    // 1-31
    pck.rx_idle_bits_thresh = 16;   // 1-64, rounded up to a step of 2
    pck.even_parity         = true; // even when true, odd when false

    // Initialize the RS485 driver
    const int err = rs485.init(pio0, 921600, pck);
    if (err != PicoRS485::ok) {
        // do something with the error code
    }
    // rest of the code
}
```

Sending a message:

```cpp
    const uint8_t request[] = {0x01, 0x03, 0x00, 0x00, 0x00, 0x02};

    while (true) {
        // send() refuses while a burst is being processed or received,
        // so this example retries until it succeeds.
        if (rs485.send(request, sizeof(request)) != PicoRS485::ok) continue;
        while (rs485.is_transmitting()) sleep_ms(1); // true until the burst is done sending
        sleep_ms(100);
    }
```

Receiving a message:

```cpp
    uint8_t in[PicoRS485::max_burst_bytes];

    while (true) {
        // has_burst() polls the completed-burst queue, so receive() then returns without
        // waiting. Without the poll it blocks until a burst arrives.
        if (rs485.has_burst()) {
            const int n = rs485.receive(in, sizeof(in));
            if (n > 0) {
                // The burst's bytes are in in[]. A burst longer than the buffer is truncated
                // to sizeof(in) while n stays the true length.
            }
        }
        // other work
    }
```

## `PicoRS485` constants

| name | value | what it is |
|---|---|---|
| `default_baudrate` | 921600 | the bit rate `init()` uses when the call does not give one |
| `max_burst_bytes` | 256 | the largest burst this instance takes, being the `PICO_RS485_MAX_PACKET_BYTES` the library was built with |
| `pio_cycles_per_bit` | 8 | PIO cycles in one bit time, for deriving a divider yourself |
| `clkdiv_frac_scale` | 256 | fractional steps in the PIO divider |

## Defaults and configuration

Framing and the two timing windows are the `pico_rs485_packet_config` given to `init()` or `set_packet_config()`. Every field has a default, so a default-constructed config is 8N1 with a 16-bit-time idle threshold and an 8-bit-time inter-character gap.

### `pico_rs485_packet_config`

| field | default | what it is |
|---|---|---|
| `data_bits` | 8 | data bits per character (5-8) |
| `parity_bits` | 0 | parity bits per character (0-1); zero is no parity |
| `stop_bits` | 1 | stop bits per character (1-2) |
| `tx_gap_bits` | 8 | bit times left between the characters of a burst (1-31) |
| `rx_idle_bits_thresh` | 16 | bit times of line silence that end a receive burst (1-64) |
| `even_parity` | `true` | even parity when true, odd when false; ignored with no parity bit |

### Bounds

| name | value | what it bounds | associated field |
|---|---|---|---|
| `data_bits_min`, `data_bits_max` | 5, 8 | data bits per character | `data_bits` |
| `parity_bits_max` | 1 | parity bits per character | `parity_bits` |
| `stop_bits_min`, `stop_bits_max` | 1, 2 | stop bits per character | `stop_bits` |
| `tx_gap_bits_min`, `tx_gap_bits_max` | 1, 31 | inter-character gap, in bit times | `tx_gap_bits` |
| `rx_idle_bits_thresh_min`, `rx_idle_bits_thresh_max` | 1, 64 | idle threshold, in bit times | `rx_idle_bits_thresh` |
| `rx_idle_bits_per_step` | 2 | bit times per step the idle timer counts in | `rx_idle_bits_thresh` |
| `rx_idle_margin_bits` | 3 | the bit times the idle threshold must clear the gap and the stop bits by | `tx_gap_bits`, `stop_bits` |

A config outside these is refused with `err_packet`, by `init()` and by `set_packet_config()`. `is_packet_config_valid()` validates the config.

Each field of `pico_rs485_packet_config` is checked on its own, and then the three timing values together: `rx_idle_bits_thresh` has to be at least `tx_gap_bits + stop_bits + rx_idle_margin_bits`. The default threshold of 16 therefore caps the gap at 12 bit times, or 11 with two stop bits, and covering the whole gap range takes a threshold of 35, or 36 with two stop bits.

### `pico_rs485_clock_config`

| field | default | what it is |
|---|---|---|
| `sys_hz` | none | the `clk_sys` the divider was derived from |
| `baudrate` | none | the bit rate the divider was derived for |
| `clkdiv_int` | none | the whole part of the divider (1-65535) |
| `clkdiv_frac8` | none | the fractional part, in 1/256 of a divider step (0-255) |

Only the two divider fields reach the hardware, but all four have to be filled: `is_clock_config_valid()` derives the divider from `sys_hz` and `baudrate` again, and refuses a config whose divider does not follow from them.

## Build settings

| variable | default | action |
|---|---|---|
| `PICO_RS485_MAX_PACKET_BYTES` | 256 | the largest burst, and therefore the size of a channel |
| `PICO_RS485_BURST_SIZES` | empty | extra maximum burst lengths the library exports, for a program that holds channels of different sizes |
| `PICO_RS485_ISR_IN_RAM` | OFF | whether the interrupt handlers run from RAM instead of being fetched through XIP |
| `PICO_RS485_HW_TESTS` | ON | whether this repository's two-board firmware is built |

A channel costs about 5.2 KB of RAM at the default maximum burst, and about 1.7 KB at 32. The staging buffers and the transmit scratch both scale with `PICO_RS485_MAX_PACKET_BYTES`, which is why the library and every consumer have to agree on it; a mismatch makes two distinct types, and the link fails naming the value that was wanted.

## Tests

```powershell
pwsh test/run_tests.ps1            # the host suite on both pin-mask bases, the RP2040/RP2350A build, the PIO helpers, doxygen
pwsh test/run_tests.ps1 -OnlySuite # the suite alone, for faster execution
```

The suite needs no board and no SDK: it compiles `src/PicoRS485.cpp` against a stub of the SDK. A C++17 compiler on `PATH` is required, and `doxygen` is used when it is installed.
[`test/README.md`](test/README.md) covers what the suite can and cannot reach, the two-board firmware under `test/hw`, and the bench.

## Non-goals

- **Hear a second talker.** The receiver is off while the driver transmits, so a collision is invisible and the protocol's own timeout is the only detector.
- **Ensure data integrity.** Framing and parity errors are counted, but never rejected. A malformed burst is returned as-is; any kind of data integrity check must be handled by the caller.
- **Hide an over-long burst.** A burst longer than `max_burst_bytes` is dropped whole and counted, never returned truncated. A burst longer than the buffer is not an error: `receive()` fills what fits and returns the true length.
- **Automatically separate two bursts.** Bursts closer together than `rx_idle_bits_thresh` merge into one. The sender has to manage that timing.
- **Synchronize across cores.** The library is not thread-safe. A call from the wrong core will fail; see [`docs/architecture.md`](docs/architecture.md) for the details.

"Deliberate limits" in [`docs/architecture.md`](docs/architecture.md) covers these in full.

## More documentation

| | |
|---|---|
| Public API | `include/PicoRS485.h` |
| Implementation | `src/PicoRS485.cpp`, `src/rs485.pio` |
| What it does to the hardware | [`docs/architecture.md`](docs/architecture.md) |
| Why each decision is what it is | [`docs/design-notes.md`](docs/design-notes.md) |
| What was measured, and what was ruled out | [`docs/bench-evidence.md`](docs/bench-evidence.md) |
| Host suite and the bench | [`test/README.md`](test/README.md) |

## License

MIT. See [`LICENSE`](LICENSE).
