// Two-board hardware test for PicoRS485.
//
// Build the same source twice (rs485_hw_master / rs485_hw_slave), flash one board
// with each, and watch both USB serial consoles. The tests are chosen for the
// things the host suite cannot reach: the receiver's word layout, the silence
// delimiter of a real multi-character burst, and the over-long burst that used to
// wedge the receiver until re-init.
//
// Per board: RX = GP5, TX = GP8, DE = GP9, RX_ACT = GP17. The activity pin is local
// to the board - the receiver's state machine drives it through the PIO and the
// idle timer reads it back - so it needs no external connection. The two boards'
// A/B lines are wired in parallel with termination, and each transceiver has DE and
// /RE tied to the same pin, which is what keeps a board from hearing itself.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>

#include "pico/stdlib.h"
#include "PicoRS485.h"

static const uint PIN_RX = 5;
static const uint PIN_TX = 8;
static const uint PIN_DE = 9;
static const uint PIN_ACT = 17;

#ifndef RS485_BAUD
#define RS485_BAUD 115200u
#endif

// A slave built with RS485_MARGIN_PPM transmits at that many parts per million off
// the nominal rate, which is the empirical version of the stop-bit margin question.
#ifndef RS485_MARGIN_PPM
#define RS485_MARGIN_PPM 0
#endif

static const uint32_t effective_baud = (uint32_t)
    ((uint64_t)RS485_BAUD * (1000000u + RS485_MARGIN_PPM) / 1000000u);

static std::optional<PicoRS485> ch;

static void bring_up() {
    ch.emplace(PIN_TX, PIN_RX, PIN_DE, PIN_ACT);
    const int rc = ch->init(pio0, effective_baud);
    // /RE is tied to DE on these boards, so the receiver is disabled and RO goes
    // high-impedance for the whole of every transmission, leaving the MCU's RX pin
    // floating. A floating input collects noise, the receiver frames it as garbage,
    // and a board ends up answering its own garbage. The internal pull-up holds the
    // pin at idle while RO is not driving; an external pull-up on RO is the proper
    // fix at the next hardware revision. Set after init() so nothing clears it.
    gpio_pull_up(PIN_RX);
    printf("init -> %d (0 = ok) at %u baud%s\n", rc, (unsigned)effective_baud,
           RS485_MARGIN_PPM ? " (deliberately offset)" : "");
}

static void tear_down() {
    ch.reset();
}

// Waits for the wire to go quiet without blocking on a receive.
static void wait_sent() {
    const uint32_t start = to_ms_since_boot(get_absolute_time());
    while (ch->is_transmitting()) {
        if (to_ms_since_boot(get_absolute_time()) - start > 500) break;
    }
}

#ifdef RS485_ROLE_MASTER

static const uint8_t pattern[] = {0xA5, 0x01, 0xFE, 0x00, 0xFF, 0x7E, 0x81, 0x00};

// Sends a burst and checks the slave echoed it byte for byte. A failure here is the
// receiver's word layout being wrong, or the burst being split by the delimiter.
static bool round_trip(size_t n, const char *name) {
    uint8_t echo[PicoRS485::max_burst_bytes] = {};

    if (ch->send(pattern, n) != PicoRS485::ok) {
        printf("FAIL %s: send refused\n", name);
        return false;
    }
    const int got = ch->receive_for(echo, sizeof(echo), 300000);
    if (got != (int)n) {
        printf("FAIL %s: got %d bytes, want %u (delimiter or layout)\n",
               name, got, (unsigned)n);
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        if (echo[i] != pattern[i]) {
            printf("FAIL %s: byte %u is 0x%02X, want 0x%02X\n",
                   name, (unsigned)i, echo[i], pattern[i]);
            return false;
        }
    }
    printf("PASS %s: %u bytes round trip, framing errors %u, parity errors %u\n",
           name, (unsigned)n, (unsigned)ch->framing_error_count(),
           (unsigned)ch->parity_error_count());
    return true;
}

int main() {
    stdio_init_all();
    sleep_ms(2500);   // let USB enumerate so the output is not lost
    printf("\n=== PicoRS485 master, %u baud ===\n", (unsigned)effective_baud);
    bring_up();

    // 1. Framing and the left-justified word layout, on a burst short enough to be
    //    one burst if the silence window is above the inter-character gap.
    round_trip(sizeof(pattern), "round trip 8 bytes");

    // 2. A burst long enough to span several characters and the DMA's refills.
    uint8_t big[64];
    for (size_t i = 0; i < sizeof(big); ++i) big[i] = (uint8_t)(i * 7u + 3u);
    uint8_t echo[PicoRS485::max_burst_bytes] = {};
    if (ch->send(big, sizeof(big)) == PicoRS485::ok &&
        ch->receive_for(echo, sizeof(echo), 400000) == (int)sizeof(big)) {
        bool same = true;
        for (size_t i = 0; i < sizeof(big); ++i) if (echo[i] != big[i]) same = false;
        printf("%s round trip 64 bytes\n", same ? "PASS" : "FAIL");
    } else {
        printf("FAIL round trip 64 bytes: no matching echo\n");
    }

    // 3. The over-long burst. No conforming sender through this driver can produce
    //    one - send() caps at max_burst_bytes - so the bus is held dominant by
    //    hand for longer than the receiver's buffer, which is what a stuck driver
    //    or a babbling peer looks like. The receiver must drop it, count it, and
    //    still be listening afterwards; before the activity-pin fix it was deaf
    //    until re-init, with send() and set_packet_config() refusing forever.
    wait_sent();
    tear_down();                                   // hands the pins back to SIO
    gpio_set_dir(PIN_DE, GPIO_OUT);
    gpio_put(PIN_DE, true);                        // take the bus
    gpio_set_dir(PIN_TX, GPIO_OUT);
    gpio_put(PIN_TX, false);                       // and hold it dominant
    const uint32_t hold_us = (uint32_t)((uint64_t)4000u * 1000000u / effective_baud);
    printf("holding the bus dominant for %u us (~%u characters)\n",
           (unsigned)hold_us, (unsigned)(4000u / 10u));
    sleep_us(hold_us);
    gpio_put(PIN_DE, false);
    gpio_put(PIN_TX, true);                        // idle high again
    sleep_ms(30);                                  // let the slave recover
    bring_up();
    round_trip(sizeof(pattern), "recovery after an over-long burst");

    // 4. Burst timing. The driver predicts len * (char_bits + tx_gap_bits + 3) bit times per
    //    burst, which is deliberately an upper bound: the last character of a burst pays no
    //    gap, and the per-character allowance it charges is larger than the program spends.
    //    The slope across lengths is the true per-character cost; the intercept is the
    //    per-burst front end. Best of five, so a late exchange cannot inflate a row.
    printf("=== burst timing: measured against predicted, best of 5 ===\n");
    for (size_t len : {1u, 2u, 4u, 8u, 16u}) {
        uint8_t payload[16] = {};
        for (size_t i = 0; i < len; ++i) payload[i] = (uint8_t)(0xA5u + i);
        uint32_t best = 0;
        int rc = 0;
        for (int run = 0; run < 5; ++run) {
            uint8_t echo[PicoRS485::max_burst_bytes];
            while (ch->receive_for(echo, sizeof(echo), 0) >= 0) {}   // drop the slave's reply
            const uint32_t t0 = time_us_32();
            rc = ch->send(payload, len);
            if (rc == PicoRS485::ok) {
                while (ch->is_transmitting() && (time_us_32() - t0) < 50000u) {
                    tight_loop_contents();
                }
                const uint32_t took = time_us_32() - t0;
                if (best == 0 || took < best) best = took;
            }
            sleep_ms(20);
        }
        const uint32_t predicted = ch->burst_duration_us(len);
        printf("  len %2u: rc %2d  took %5u us  predicted %5u us  = %3u%% of measured\n",
               (unsigned)len, rc, (unsigned)best, (unsigned)predicted,
               (unsigned)((best != 0 && predicted != 0) ? (predicted * 100u) / best : 0u));
    }
    {
        // Leave the queue empty, so the beacon's first exchange is its own
        uint8_t echo[PicoRS485::max_burst_bytes];
        while (ch->receive_for(echo, sizeof(echo), 0) >= 0) {}
    }

    // 5. Beacon. The tests above are over in about four seconds, so their output is
    //    lost unless a console is already attached when the board boots - which makes
    //    a wiring fault very hard to see. From here on the master sends a numbered
    //    8-byte burst twice a second, forever. Watch both consoles while probing DE
    //    and RO with a meter, or while reseating A/B: the effect shows up within half
    //    a second instead of requiring a reset at exactly the right moment.
    printf("=== beacon: one numbered burst every 500 ms ===\n");
    uint32_t seq = 0;
    while (true) {
        const uint32_t t0 = to_ms_since_boot(get_absolute_time());

        uint8_t frame[sizeof(pattern)];
        memcpy(frame, pattern, sizeof(pattern));
        frame[1] = (uint8_t)(seq & 0xFFu);
        frame[2] = (uint8_t)((seq >> 8) & 0xFFu);

        // The end-of-burst interrupt reports the burst that just finished. Until it runs, the
        // channel believes a completion is owed and refuses the next send with
        // err_incomplete_tx (-16), which is easy to mistake for a wire fault.
        const uint32_t tx_start = time_us_32();
        const int rc = ch->send(frame, sizeof(frame));
        // How long the burst actually took on the wire, against what the driver predicts
        // for it. Eight characters at 115200 baud is about 700 us. Tens of milliseconds
        // would mean the transmit path put far more on the bus than it was given, which
        // is what the slave's one-drop-per-beacon pattern points at.
        uint32_t tx_us = 0;
        if (rc == PicoRS485::ok) {
            while (ch->is_transmitting() && (time_us_32() - tx_start) < 50000u) {
                tight_loop_contents();
            }
            tx_us = time_us_32() - tx_start;
        }
        printf("tx %u: rc %d  took %u us (driver predicts %u)", (unsigned)seq, rc,
               (unsigned)tx_us, (unsigned)ch->burst_duration_us(sizeof(frame)));
        if (rc == PicoRS485::ok) {
            uint8_t echo[PicoRS485::max_burst_bytes] = {};
            const int got = ch->receive_for(echo, sizeof(echo), 250000);
            if (got == (int)sizeof(frame)) {
                bool same = true;
                for (size_t i = 0; i < sizeof(frame); ++i) {
                    if (echo[i] != frame[i]) same = false;
                }
                printf("  echo %d bytes %s", got, same ? "match" : "MISMATCH");
            } else {
                printf("  echo %d (want %u)", got, (unsigned)sizeof(frame));
            }
        }
        printf("  | dropped %u framing %u parity %u recv %d\n",
               (unsigned)ch->dropped_burst_count(),
               (unsigned)ch->framing_error_count(),
               (unsigned)ch->parity_error_count(),
               ch->is_receiving() ? 1 : 0);

        // Hold the period at 500 ms even when the echo wait ran its full 250 ms.
        const uint32_t spent = to_ms_since_boot(get_absolute_time()) - t0;
        if (spent < 500u) sleep_ms(500u - spent);
        ++seq;
    }
}

#endif  // RS485_ROLE_MASTER

#ifdef RS485_ROLE_SLAVE

int main() {
    stdio_init_all();
    sleep_ms(2500);
    printf("\n=== PicoRS485 slave, %u baud ===\n", (unsigned)effective_baud);
    bring_up();

    uint8_t buf[PicoRS485::max_burst_bytes] = {};
    uint32_t last_report = 0;
    uint32_t bursts = 0;

    while (true) {
        const int n = ch->receive_for(buf, sizeof(buf), 100000);
        if (n > 0) {
            ++bursts;
            // The first bytes are printed in hex so the master's beacon number can be
            // read straight off this console and matched against what it sent.
            printf("burst %u: %d bytes:", (unsigned)bursts, n);
            for (int i = 0; i < n && i < 8; ++i) printf(" %02X", buf[i]);
            printf("\n");
            // Echo it back as one burst, so the master's round trip measures both
            // directions and a burst the driver split shows up as extra replies.
            if (ch->send(buf, (size_t)n) != PicoRS485::ok) {
                printf("  echo refused\n");
            }
            wait_sent();
        } else if (n != PicoRS485::err_timeout) {
            printf("receive -> %d\n", n);
            sleep_ms(100);
        }

        const uint32_t now = to_ms_since_boot(get_absolute_time());
        if (now - last_report > 3000) {
            last_report = now;
            printf("idle: %u bursts, %u dropped, %u framing errors, %u parity errors\n",
                   (unsigned)bursts, (unsigned)ch->dropped_burst_count(),
                   (unsigned)ch->framing_error_count(),
                   (unsigned)ch->parity_error_count());
        }
    }
}

#endif  // RS485_ROLE_SLAVE
