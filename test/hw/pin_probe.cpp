// Pin probe for PicoRS485 bring-up.
//
// The driver and the UART both report a receive line that is active more or less
// continuously, yet the transceiver's RO pin measures a healthy 3V3 at idle. Those
// two cannot both be true of the same node, so this firmware looks at the node
// itself: it takes GP5 away from the UART, makes it a plain SIO input, and reports
// what the pin actually does with no internal pull, with a pull-up, and with a
// pull-down.
//
//   follows the pull (pull-up HIGH, pull-down LOW)  -> nothing is driving GP5
//   HIGH in all three                               -> RO really is driving it
//   LOW in all three                                -> something holds it down
//   SWITCHING in any case                           -> the line is glitching
//
// GP8 (TX) and GP9 (DE) are reported as well, for context. Flash the same UF2 on
// both boards: every reading is per board and needs no role, so comparing the two
// consoles compares the two boards.
//
// No UART and no library here on purpose. The UART is what we are trying to explain,
// so it must not be part of the measurement.

#include <cstdint>
#include <cstdio>

#include "pico/stdlib.h"
#include "hardware/gpio.h"

static const uint PIN_RX = 5;
static const uint PIN_TX = 8;
static const uint PIN_DE = 9;

static const uint32_t SAMPLE_MS = 150;

struct reading {
    uint32_t highs;
    uint32_t lows;
    uint32_t edges;
};

enum { K_LOW = 0, K_HIGH = 1, K_SWITCH = 2 };

static const char *kname(int k) {
    return k == K_HIGH ? "HIGH" : (k == K_LOW ? "LOW" : "SWITCHING");
}

// Samples the pin for a fixed window after letting the pad settle, so the pull
// configuration is in effect for the whole measurement.
static reading watch(uint pin, bool pull_up, bool pull_down) {
    gpio_set_pulls(pin, pull_up, pull_down);
    sleep_ms(20);

    reading r = {0, 0, 0};
    int prev = gpio_get(pin);
    const absolute_time_t end = make_timeout_time_ms(SAMPLE_MS);
    while (!time_reached(end)) {
        const int v = gpio_get(pin);
        if (v) ++r.highs; else ++r.lows;
        if (v != prev) { ++r.edges; prev = v; }
    }
    return r;
}

static int kind(const reading &r) {
    if (r.edges != 0) return K_SWITCH;
    return r.highs != 0 ? K_HIGH : K_LOW;
}

int main() {
    stdio_init_all();
    sleep_ms(2000);   // let USB enumerate so the banner is not lost

    // The UART is never brought up. GP5 becomes a plain input with no function muxed
    // onto it, which is the cleanest possible view of what the pin is doing.
    gpio_init(PIN_RX);
    gpio_set_dir(PIN_RX, GPIO_IN);

    // Also watch the two pins that drive the transceiver, as plain inputs. Their
    // levels say whether this board believes it is transmitting.
    gpio_init(PIN_TX);
    gpio_set_dir(PIN_TX, GPIO_IN);
    gpio_init(PIN_DE);
    gpio_set_dir(PIN_DE, GPIO_IN);

    printf("\n=== PicoRS485 pin probe (RX=GP%u TX=GP%u DE=GP%u, no UART) ===\n",
           (unsigned)PIN_RX, (unsigned)PIN_TX, (unsigned)PIN_DE);

    while (true) {
        const reading none = watch(PIN_RX, false, false);
        const reading up   = watch(PIN_RX, true,  false);
        const reading down = watch(PIN_RX, false, true);

        const int kn = kind(none), ku = kind(up), kd = kind(down);
        printf("GP5: float %-9s pull-up %-9s pull-down %-9s | edges %u/%u/%u\n",
               kname(kn), kname(ku), kname(kd),
               (unsigned)none.edges, (unsigned)up.edges, (unsigned)down.edges);

        const char *verdict = "UNCLEAR - read the three readings directly";
        if (ku == K_HIGH && kd == K_LOW) {
            verdict = "FLOATING - nothing is driving GP5 (RO-to-GP5 path is open)";
        } else if (kn == K_HIGH && ku == K_HIGH && kd == K_HIGH) {
            verdict = "DRIVEN HIGH - RO is driving GP5, so the line is genuinely idle";
        } else if (kn == K_LOW && ku == K_LOW && kd == K_LOW) {
            verdict = "DRIVEN LOW - something is holding GP5 down";
        } else if (kn == K_SWITCH || ku == K_SWITCH || kd == K_SWITCH) {
            verdict = "SWITCHING - GP5 is moving, the line is glitching";
        }
        printf("  verdict: %s\n", verdict);
        printf("  GP8(TX)=%d  GP9(DE)=%d  (both read as inputs, so this is the wire)\n",
               gpio_get(PIN_TX), gpio_get(PIN_DE));

        sleep_ms(2000);
    }
}
