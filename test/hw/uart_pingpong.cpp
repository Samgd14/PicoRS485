// UART bisect for PicoRS485.
//
// This firmware deliberately does NOT use the library. It drives the same pins with
// the RP2040's hardware UART instead: same transceivers, same A/B pair, same baud,
// same software DE discipline (assert before the start bit, hold past the stop bit,
// release). So it separates the two halves of the problem:
//
//   - UART byte exchange works  -> pins, transceivers, termination, DE//RE control and
//                                 baud are all good, and the fault is in the library's
//                                 PIO/DMA receive path.
//   - UART fails the same way   -> the fault is on the bench, and the library is
//                                 exonerated.
//
// The pins are the library's pins on purpose. RX = GP5 and TX = GP8 are UART1's
// alternate function on RP2040 (function 2), not UART0's - GP4/GP5 and GP8/GP9 are
// UART1, so this must use uart1 to match what the library drives. DE stays a plain
// SIO output on GP9, exactly as the library side-sets it.
//
// The slave reports the UART's framing/break/overrun flags (read straight from the
// data register) as well as a running byte count, and prints them every three
// seconds even when no burst arrives. That is the point of the exercise: a bus whose
// bias leaves only tens of millivolts across the pair puts the receiver near its
// threshold, so its output can dither and look like an endless stream of framing.
// If those counters climb while the master is silent, the bias is the problem and no
// firmware change will fix it.

#include <cstdint>
#include <cstdio>

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/uart.h"

static const uint PIN_RX = 5;
static const uint PIN_TX = 8;
static const uint PIN_DE = 9;

// Same clock and same pin roles as the library firmware, so the comparison is fair.
static uart_inst_t *const BUS = uart1;
static const uint32_t RS485_BAUD = 115200u;

static const size_t MAX_BURST = 256;
static const uint8_t PATTERN[8] = {0xA5, 0x00, 0x00, 0x00, 0xFF, 0x7E, 0x81, 0x00};

// Inter-byte silence that ends a burst. The library closes a burst on a bit-counted
// silence window; here it only needs to be comfortably longer than one character
// (87 us at 115200) and far shorter than the 500 ms beacon period.
static const uint32_t BURST_GAP_US = 3000u;

// The library asserts DE one bit time before the start bit and holds it one bit time
// past the stop bit; mirror that.
static const uint32_t DE_SETTLE_US = 9u;

// Counters the periodic report is built from. Only this firmware's ISR-free code
// touches them, but they are read while polling, so keep them plain.
static uint32_t g_bytes = 0;
static uint32_t g_dr_errors = 0;
static uint32_t g_framing = 0;
static uint32_t g_break = 0;
static uint32_t g_overrun = 0;

static void bus_init() {
    uart_init(BUS, RS485_BAUD);
    gpio_set_function(PIN_TX, GPIO_FUNC_UART);
    gpio_set_function(PIN_RX, GPIO_FUNC_UART);
    // DE (and /RE, tied to it on these boards) starts low, which is receive.
    gpio_init(PIN_DE);
    gpio_set_dir(PIN_DE, GPIO_OUT);
    gpio_put(PIN_DE, false);
    uart_set_format(BUS, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(BUS, true);
    // /RE is tied to DE, so RO is high-impedance whenever this board transmits and
    // the RX pin floats. The internal pull-up keeps it at idle between characters; the
    // proper fix is an external pull-up on RO. Set last so nothing clears it.
    gpio_pull_up(PIN_RX);
}

// Reads one byte, harvesting the error flags while the data register is consumed.
// The flags live in the same register as the data and clear on read, so this has to
// be a single raw read rather than uart_getc() plus a separate flag query.
static bool bus_read_byte(uint8_t *out) {
    uart_hw_t *hw = uart_get_hw(BUS);
    if (hw->fr & UART_UARTFR_RXFE_BITS) return false;
    const uint32_t dr = hw->dr;
    if (dr & UART_UARTDR_FE_BITS) { ++g_framing; ++g_dr_errors; }
    if (dr & UART_UARTDR_BE_BITS) { ++g_break; ++g_dr_errors; }
    if (dr & UART_UARTDR_OE_BITS) { ++g_overrun; ++g_dr_errors; }
    *out = (uint8_t)(dr & UART_UARTDR_DATA_BITS);
    ++g_bytes;
    return true;
}

static void wait_tx_idle() {
    while (!(uart_get_hw(BUS)->fr & UART_UARTFR_TXFE_BITS)) tight_loop_contents();
    while (uart_get_hw(BUS)->fr & UART_UARTFR_BUSY_BITS) tight_loop_contents();
}

static void send_burst(const uint8_t *p, size_t n) {
    gpio_put(PIN_DE, true);
    busy_wait_us_32(DE_SETTLE_US);      // let the driver enable before the start bit
    uart_write_blocking(BUS, p, n);
    wait_tx_idle();
    busy_wait_us_32(DE_SETTLE_US);      // hold past the stop bit, then let go
    gpio_put(PIN_DE, false);
}

// Waits up to timeout_ms for the first byte, then keeps going until a gap of
// BURST_GAP_US with nothing arriving. Returns the burst length, or 0 on timeout.
static int read_burst(uint8_t *out, size_t cap, uint32_t timeout_ms) {
    size_t n = 0;
    const uint32_t start = to_ms_since_boot(get_absolute_time());
    while (n == 0 && (to_ms_since_boot(get_absolute_time()) - start) < timeout_ms) {
        uint8_t b;
        if (bus_read_byte(&b)) { out[n++] = b; break; }
        sleep_us(100);
    }
    if (n == 0) return 0;

    uint32_t gap = 0;
    while (n < cap && gap < BURST_GAP_US) {
        uint8_t b;
        if (bus_read_byte(&b)) { out[n++] = b; gap = 0; }
        else { sleep_us(50); gap += 50; }
    }
    return (int)n;
}

static void print_counters(const char *tag, uint32_t bursts) {
    printf("%s: %u bursts, %u bytes, %u uart errors (framing %u break %u overrun %u)\n",
           tag, (unsigned)bursts, (unsigned)g_bytes, (unsigned)g_dr_errors,
           (unsigned)g_framing, (unsigned)g_break, (unsigned)g_overrun);
}

// ---------------------------------------------------------------------------
// Master: sends the numbered pattern every 500 ms and reports what came back.
// ---------------------------------------------------------------------------
#ifdef RS485_ROLE_MASTER

int main() {
    stdio_init_all();
    sleep_ms(2000);   // let USB enumerate so the banner is not lost
    bus_init();
    printf("\n=== UART bisect master, %u baud (TX=GP%u RX=GP%u DE=GP%u, uart%u) ===\n",
           (unsigned)RS485_BAUD, (unsigned)PIN_TX, (unsigned)PIN_RX, (unsigned)PIN_DE,
           (unsigned)uart_get_index(BUS));

    uint32_t seq = 0;
    while (true) {
        const uint32_t t0 = to_ms_since_boot(get_absolute_time());

        uint8_t frame[sizeof(PATTERN)];
        for (size_t i = 0; i < sizeof(frame); ++i) frame[i] = PATTERN[i];
        frame[1] = (uint8_t)(seq & 0xFFu);
        frame[2] = (uint8_t)((seq >> 8) & 0xFFu);

        send_burst(frame, sizeof(frame));

        uint8_t echo[MAX_BURST];
        const int got = read_burst(echo, sizeof(echo), 250u);
        printf("tx %u: rc ok", (unsigned)seq);
        if (got == (int)sizeof(frame)) {
            bool same = true;
            for (size_t i = 0; i < sizeof(frame); ++i) {
                if (echo[i] != frame[i]) same = false;
            }
            printf("  echo %d bytes %s", got, same ? "match" : "MISMATCH");
            if (!same) {
                // Print both sides: a clean 8-byte frame that does not match is
                // either an off-by-one-beacon echo or a shifted payload, and the
                // bytes tell those apart immediately.
                printf(" [sent");
                for (size_t i = 0; i < sizeof(frame); ++i) printf(" %02X", frame[i]);
                printf(" got");
                for (size_t i = 0; i < sizeof(frame); ++i) printf(" %02X", echo[i]);
                printf("]");
            }
        } else {
            printf("  echo %d (want %u)", got, (unsigned)sizeof(frame));
        }
        printf("  | bytes %u errors %u\n", (unsigned)g_bytes, (unsigned)g_dr_errors);

        const uint32_t spent = to_ms_since_boot(get_absolute_time()) - t0;
        if (spent < 500u) sleep_ms(500u - spent);
        ++seq;
    }
}

#endif  // RS485_ROLE_MASTER

// ---------------------------------------------------------------------------
// Slave: echoes every burst it receives, and reports the counters every 3 s even
// when nothing has arrived - that idle report is the bias test.
// ---------------------------------------------------------------------------
#ifdef RS485_ROLE_SLAVE

int main() {
    stdio_init_all();
    sleep_ms(2000);
    bus_init();
    printf("\n=== UART bisect slave, %u baud (TX=GP%u RX=GP%u DE=GP%u, uart%u) ===\n",
           (unsigned)RS485_BAUD, (unsigned)PIN_TX, (unsigned)PIN_RX, (unsigned)PIN_DE,
           (unsigned)uart_get_index(BUS));

    uint8_t buf[MAX_BURST];
    uint32_t bursts = 0;
    uint32_t last_report = 0;

    while (true) {
        const int n = read_burst(buf, sizeof(buf), 100u);
        if (n > 0) {
            ++bursts;
            printf("burst %u: %d bytes:", (unsigned)bursts, n);
            for (int i = 0; i < n && i < 8; ++i) printf(" %02X", buf[i]);
            printf("\n");
            send_burst(buf, (size_t)n);
        }

        const uint32_t now = to_ms_since_boot(get_absolute_time());
        if (now - last_report > 3000u) {
            last_report = now;
            print_counters("idle", bursts);
        }
    }
}

#endif  // RS485_ROLE_SLAVE
