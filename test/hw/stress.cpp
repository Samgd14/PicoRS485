// High-usage bus bench for PicoRS485: one request/reply exchange per period, at a
// rate chosen to leave the wire only a fraction idle.
//
// The master sends a request; the slave answers with a reply that carries the
// request's sequence byte, so each side can check the burst against the sequence it
// claims, not only against the one just sent. That distinction is the point of the
// counters: a burst that is internally consistent but one sequence behind is stale,
// a burst whose pattern is broken is corrupt, and a run that slips once must not be
// reported as corrupt for the rest of its life.
//
// One firmware covers the whole test matrix, because every parameter that used to need
// its own build is named on the command line. The console is silent while a run is in
// progress: nothing is printed until the dump is asked for, so the measurement never
// competes with the USB stack for the bus or the CPU. A report per second blocks for about
// a millisecond inside pico_stdio_usb - twelve times the exchange period at the rates this
// is here to measure - and jitters the exchange timing, which is exactly what a repeatable
// measurement must not do.
//
// Commands, one line each, over USB serial or UART0 (GP0 = TX, GP1 = RX, both enabled):
//   r<seconds> [key=value ...]   snapshot, apply the parameters, run; silent
//   s                            stop the run early
//   z                            reset both boards, to test what a reset changes
//   d                            print the window deltas and the driver's own counters
//   i                            print the configuration in force
//   ?                            list the commands
//
// Parameters for r (defaults are the build's own):
//   period=<us>    master period, 0 for back to back
//   guard=<us>     silence held before either side transmits
//   gap=<bits>     inter-character gap inside a burst, 1-31
//   packet=<0|1|2> 0 = 8N1, 1 = 8E1, 2 = 8N2
//   pairs=<n>      exchanges sent back to back inside one period, 1-4
//   req=<bytes>    request length, 1 or 4..
//   reply=<bytes>  reply length, 1 or 4..
//   reapply=<0|1>  1 rewrites the packet config before every window (an experiment:
//                  the write touches the receive machine's Y and autopush threshold)
//   drain=<0|1>    1 empties the reader's queue before a window, so one window cannot
//                  inherit a burst left by the last one
//   pad=<n>        busy-loop iterations before each request, to shift where in the
//                  period the request goes out, in fractions of a bit
// (The bit rate is not a run parameter: changing it re-initialises the channel, which
// the firmware cannot do for itself.)
//
// Build the same source twice (rs485_stress_master / rs485_stress_slave), flash one
// board with each, open both consoles, send the same `r` line to both, and read the
// dumps afterwards. Closing the consoles for the run is the point: with no host
// polling, the USB stack is idle as well.
//
// Per board: RX = GP5, TX = GP8, DE = GP9, RX_ACT = GP17, the same wiring as main.cpp.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>

#include "hardware/clocks.h"
#include "hardware/watchdog.h"
#include "pico/stdlib.h"
#include "PicoRS485.h"

static const uint PIN_RX = 5;
static const uint PIN_TX = 8;
static const uint PIN_DE = 9;
static const uint PIN_ACT = 17;

// Boot defaults. Every one of these can be overridden per run from the console.
#ifndef RS485_BAUD
#define RS485_BAUD 15625000u
#endif
#ifndef RS485_PERIOD_US
#define RS485_PERIOD_US 83u
#endif
#ifndef RS485_REQ_BYTES
#define RS485_REQ_BYTES 1u
#endif
#ifndef RS485_REPLY_BYTES
#define RS485_REPLY_BYTES 16u
#endif
#ifndef RS485_GUARD_US
#define RS485_GUARD_US 0u
#endif
#ifndef RS485_GAP_BITS
#define RS485_GAP_BITS 1u
#endif
#ifndef RS485_PACKET_PROFILE
#define RS485_PACKET_PROFILE 0u
#endif
#ifndef RS485_PAIRS
#define RS485_PAIRS 1u
#endif
#ifndef RS485_SYS_CLK_KHZ
// 0 leaves the SDK's clock alone. 200000 runs RP2040 at 200 MHz, where RS485_BAUD of
// 25000000 derives a PIO divider of exactly 1.0.
#define RS485_SYS_CLK_KHZ 0u
#endif

static uint32_t g_baud       = RS485_BAUD;
static uint32_t g_period_us  = RS485_PERIOD_US;
static uint32_t g_guard_us   = RS485_GUARD_US;
static uint32_t g_gap_bits   = RS485_GAP_BITS;
static uint32_t g_packet     = RS485_PACKET_PROFILE;
static uint32_t g_pairs      = RS485_PAIRS;
static size_t   g_req_len    = RS485_REQ_BYTES;
static size_t   g_reply_len  = RS485_REPLY_BYTES;
static uint32_t g_reapply    = 0;   // 1: rewrite the packet config before every window
static uint32_t g_drain      = 1;   // 1: empty the reader's queue before a window
static uint32_t g_pad        = 0;   // busy-loop iterations before each request

// The driver reports a wrong-core call to this hook and keeps no count of its own, so the bench
// keeps the count. It is what the dump's "driver:" line reports.
static uint32_t g_core_violations = 0;
extern "C" void pico_rs485_core_violation(void) { ++g_core_violations; }
static int      g_init_rc    = 0;
static int      g_packet_rc  = 0;

static std::optional<PicoRS485> ch;

// One byte carries the sequence alone; anything longer carries the marker, the
// sequence, its complement, and a ramp offset by the sequence, so every byte of the
// burst is checkable and a misaligned or merged burst cannot pass.
static void build_frame(uint8_t *out, size_t len, uint8_t seq) {
    if (len == 1) { out[0] = seq; return; }
    out[0] = 0xA5u;
    out[1] = seq;
    out[2] = (uint8_t)(seq ^ 0xFFu);
    for (size_t i = 3; i < len; ++i) out[i] = (uint8_t)(seq + i);
}

static uint8_t frame_seq(const uint8_t *in, size_t len) {
    return len == 1 ? in[0] : in[1];
}

static bool check_frame(const uint8_t *in, size_t len, uint8_t seq) {
    if (len == 1) return in[0] == seq;
    if (len < 4) return false;
    if (in[0] != 0xA5u || in[1] != seq || in[2] != (uint8_t)(seq ^ 0xFFu)) return false;
    for (size_t i = 3; i < len; ++i) {
        if (in[i] != (uint8_t)(seq + i)) return false;
    }
    return true;
}

// Everything the dump needs, so a window is reported as a difference rather than as a
// lifetime total. Maxima and the last-burst snapshot are whole-run values: printed as
// they stand, not subtracted.
struct counters {
    uint32_t cycles = 0, replied = 0, good = 0, stale = 0, corrupt = 0;
    uint32_t refused = 0, short_bursts = 0, timeouts = 0, errors = 0;
    uint32_t late_cycles = 0, max_late_us = 0, rtt_sum = 0, rtt_max = 0;
    // Where the first late cycle of this boot landed in its window, and the round trip of the
    // exchange that overran, so a startup artefact can be told from a one-off stall.
    uint32_t late_first_at = 0, late_first_rtt = 0;
    uint32_t slot_good[4] = {}, slot_stale[4] = {}, slot_corrupt[4] = {},
             slot_timeouts[4] = {};
    uint32_t dropped = 0, framing = 0, parity = 0;
    uint32_t requests = 0, replies = 0, wrong_len = 0, bad_request = 0, gaps = 0;
};

static counters g_c;       // live
static counters g_base;    // taken when a run starts
static bool     g_running = false;
static bool     g_restart_schedule = false;   // resync the schedule on the next exchange
static uint32_t g_last_rtt = 0;               // round trip of the previous exchange
static uint32_t g_run_end_ms = 0;
static uint32_t g_run_ms = 0;

static uint32_t delta(uint32_t now, uint32_t before) { return now - before; }

static void take_snapshot() {
    g_c.dropped   = ch->dropped_burst_count();
    g_c.framing   = ch->framing_error_count();
    g_c.parity    = ch->parity_error_count();
}

// Times a loop of the same shape as a delay unit, so any sweep of one can be read in
// nanoseconds and as a fraction of a bit rather than in iterations.
static void calibrate_delay_unit() {
    const uint32_t n = 200000;
    volatile uint32_t sink = 0;
    const uint32_t t0 = time_us_32();
    for (uint32_t i = 0; i < n; ++i) sink = i;
    const uint32_t dt = time_us_32() - t0;
    const uint32_t ns = dt ? (uint32_t)((uint64_t)dt * 1000u / n) : 0;
    printf("calibration: one loop iteration is %u ns; one bit is %u ns at %u baud\n",
           (unsigned)ns, (unsigned)((uint64_t)1000000000u / g_baud), (unsigned)g_baud);
}

static void print_config() {
    printf("config: baud %u  period %u us  guard %u us  gap %u  packet %u  pairs %u  "
           "req %u  reply %u  reapply %u  drain %u  pad %u  (init %d packet %d)\n",
           (unsigned)g_baud, (unsigned)g_period_us, (unsigned)g_guard_us,
           (unsigned)g_gap_bits, (unsigned)g_packet, (unsigned)g_pairs,
           (unsigned)g_req_len, (unsigned)g_reply_len, (unsigned)g_reapply,
           (unsigned)g_drain, (unsigned)g_pad, g_init_rc, g_packet_rc);
}

static void dump_run() {
#ifdef RS485_ROLE_MASTER
    const char *role = "master";
#else
    const char *role = "slave";
#endif
    const counters &a = g_base, &b = g_c;
    printf("\n=== dump: %s  %u ms window ===\n", role, (unsigned)g_run_ms);
    print_config();
    printf("cycles %u  good %u  stale %u  corrupt %u  refused %u  short %u  "
           "timeouts %u  errors %u\n",
           (unsigned)delta(b.cycles, a.cycles), (unsigned)delta(b.good, a.good),
           (unsigned)delta(b.stale, a.stale), (unsigned)delta(b.corrupt, a.corrupt),
           (unsigned)delta(b.refused, a.refused),
           (unsigned)delta(b.short_bursts, a.short_bursts),
           (unsigned)delta(b.timeouts, a.timeouts), (unsigned)delta(b.errors, a.errors));
    const uint32_t cycles = delta(b.cycles, a.cycles);
    printf("rate %u exchanges/s over %u ms\n",
           (unsigned)(g_run_ms ? (uint32_t)((uint64_t)cycles * 1000u / g_run_ms) : 0u),
           (unsigned)g_run_ms);
    const uint32_t replied = delta(b.replied, a.replied);
    const uint32_t rtt_sum = delta(b.rtt_sum, a.rtt_sum);
    printf("late %u (max %u us whole run)  first since boot at cycle %u, rtt %u us\n",
           (unsigned)delta(b.late_cycles, a.late_cycles), (unsigned)b.max_late_us,
           (unsigned)b.late_first_at, (unsigned)b.late_first_rtt);
    printf("rtt max %u (whole run) mean %u us\n",
           (unsigned)b.rtt_max, (unsigned)(replied ? rtt_sum / replied : 0));
    if (g_pairs > 1) {
        printf("slots:");
        for (uint32_t s = 0; s < g_pairs; ++s) {
            printf("  [%u] good %u stale %u corrupt %u timeouts %u;", (unsigned)s,
                   (unsigned)delta(b.slot_good[s], a.slot_good[s]),
                   (unsigned)delta(b.slot_stale[s], a.slot_stale[s]),
                   (unsigned)delta(b.slot_corrupt[s], a.slot_corrupt[s]),
                   (unsigned)delta(b.slot_timeouts[s], a.slot_timeouts[s]));
        }
        printf("\n");
    }
    printf("driver: violations %u  dropped %u  framing %u  parity %u\n",
           (unsigned)g_core_violations,
           (unsigned)delta(b.dropped, a.dropped), (unsigned)delta(b.framing, a.framing),
           (unsigned)delta(b.parity, a.parity));
    printf("slave: requests %u  replies %u  wrong len %u  bad request %u  gaps %u\n",
           (unsigned)delta(b.requests, a.requests), (unsigned)delta(b.replies, a.replies),
           (unsigned)delta(b.wrong_len, a.wrong_len),
           (unsigned)delta(b.bad_request, a.bad_request),
           (unsigned)delta(b.gaps, a.gaps));
    printf("=== end dump ===\n");
}

// Applies the framing and the inter-character gap, and only when they actually changed: a
// redundant set_packet_config() writes the receive machine's Y and autopush threshold,
// so applying it before every window would make "the config is the same" a different
// experiment from "the config was rewritten".
static void apply_config() {
    static bool     have_applied = false;
    static uint32_t last_gap = 0, last_packet = 0;
    if (!g_reapply && have_applied && last_gap == g_gap_bits && last_packet == g_packet) {
        return;
    }
    pico_rs485_packet_config pkt = ch->get_packet_config();
    pkt.tx_gap_bits = (uint8_t)g_gap_bits;
    pkt.parity_bits = (g_packet == 1u) ? 1u : 0u;
    pkt.stop_bits   = (g_packet == 2u) ? 2u : 1u;
    pkt.even_parity = true;
    g_packet_rc = ch->set_packet_config(pkt);
    last_gap = g_gap_bits;
    last_packet = g_packet;
    have_applied = true;
}

// Empties whatever the reader still holds, so one window cannot inherit the tail of the
// previous one: with a burst already queued the first exchange of a window is answered
// from the queue, which shows up as a whole window of stale exchanges.
static void drain_queue() {
    if (g_drain == 0) return;
    uint8_t scratch[PicoRS485::max_burst_bytes];
    for (int i = 0; i < 8; ++i) {
        if (ch->receive_for(scratch, sizeof(scratch), 0) <= 0) break;
    }
}

// --- console -----------------------------------------------------------------------

static bool parse_uint(const char *s, uint32_t &out) {
    if (s == nullptr || *s == '\0') return false;
    char *end = nullptr;
    const unsigned long v = strtoul(s, &end, 10);
    if (end == s || *end != '\0') return false;
    out = (uint32_t)v;
    return true;
}

static bool valid_lengths() {
    if (g_req_len < 1 || g_req_len > PicoRS485::max_burst_bytes) return false;
    if (g_reply_len < 1 || g_reply_len > PicoRS485::max_burst_bytes) return false;
    if (g_req_len != 1 && g_req_len < 4) return false;
    if (g_reply_len != 1 && g_reply_len < 4) return false;
    return true;
}

// Parses "r<seconds> key=value ...", applying what it recognises. Returns false and
// leaves the run unstarted if anything is out of range, so a typo cannot be measured.
static bool apply_run_args(char *line, uint32_t &secs_out, const char *&bad) {
    char *p = line;
    if (*p != 'r') { bad = "not a run command"; return false; }
    ++p;
    uint32_t secs = 0;
    while (*p >= '0' && *p <= '9') { secs = secs * 10u + (uint32_t)(*p - '0'); ++p; }
    if (secs == 0) secs = 10;

    while (*p != '\0') {
        while (*p == ' ' || *p == ',' || *p == '\t') ++p;
        if (*p == '\0') break;
        char *key = p;
        while (*p != '\0' && *p != '=' && *p != ' ' && *p != ',') ++p;
        char *key_end = p;
        if (*p != '=') { bad = "token without a value"; return false; }
        *key_end = '\0';
        ++p;
        char *val = p;
        while (*p != '\0' && *p != ' ' && *p != ',') ++p;
        const char sep = *p;
        *p = '\0';
        uint32_t v = 0;
        if (!parse_uint(val, v)) { bad = "value is not a number"; return false; }
        if (strcmp(key, "period") == 0) {
            if (v > 1000000u) { bad = "period out of range"; return false; }
            g_period_us = v;
        } else if (strcmp(key, "guard") == 0) {
            if (v > 100000u) { bad = "guard out of range"; return false; }
            g_guard_us = v;
        } else if (strcmp(key, "gap") == 0) {
            if (v < 1u || v > 31u) { bad = "gap must be 1-31"; return false; }
            g_gap_bits = v;
        } else if (strcmp(key, "packet") == 0) {
            if (v > 2u) { bad = "packet must be 0-2"; return false; }
            g_packet = v;
        } else if (strcmp(key, "pairs") == 0) {
            if (v < 1u || v > 4u) { bad = "pairs must be 1-4"; return false; }
            g_pairs = v;
        } else if (strcmp(key, "req") == 0) {
            g_req_len = v;
        } else if (strcmp(key, "reply") == 0) {
            g_reply_len = v;
        } else if (strcmp(key, "reapply") == 0) {
            if (v > 1u) { bad = "reapply must be 0 or 1"; return false; }
            g_reapply = v;
        } else if (strcmp(key, "drain") == 0) {
            if (v > 1u) { bad = "drain must be 0 or 1"; return false; }
            g_drain = v;
        } else if (strcmp(key, "pad") == 0) {
            if (v > 200u) { bad = "pad out of range"; return false; }
            g_pad = v;
        } else {
            bad = "unknown parameter";
            return false;
        }
        if (sep == ',') continue;
        // p already sits on a space or the terminator
    }
    if (!valid_lengths()) { bad = "request or reply length out of range"; return false; }
    secs_out = secs;
    return true;
}

// Reads whatever the console has, one line at a time. Nothing is echoed, and nothing is
// printed while a run is in progress.
static void poll_console() {
    static char line[96];
    static size_t len = 0;
    int c;
    while ((c = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) {
        if (c == '\r' || c == '\n') {
            line[len] = '\0';
            if (len > 0) {
                if (line[0] == 'r') {
                    uint32_t secs = 0;
                    const char *bad = nullptr;
                    if (apply_run_args(line, secs, bad)) {
                        apply_config();
                        drain_queue();
                        take_snapshot();
                        g_base = g_c;
                        g_run_ms = secs * 1000u;
                        g_run_end_ms = to_ms_since_boot(get_absolute_time()) + g_run_ms;
                        g_restart_schedule = true;   // the idle before this is not lateness
                        g_running = true;            // silent: no acknowledgment
                    } else {
                        printf("run refused: %s\n", bad ? bad : "bad arguments");
                    }
                } else if (line[0] == 's') {
                    g_running = false;
                } else if (line[0] == 'z') {
                    // A firmware reset, not a power cycle: it re-runs init() and the PIO
                    // configuration from scratch, so it tests whether the state that
                    // decides the receive phase is established at reset.
                    watchdog_reboot(0, 0, 10);
                } else if (line[0] == 'd') {
                    dump_run();
                } else if (line[0] == 'i') {
                    print_config();
                } else {
                    printf("commands: r<seconds> [key=value ...] run, s stop, d dump, "
                           "i config, ? help\n"
                           "  keys: period guard gap packet pairs req reply baud\n");
                }
            }
            len = 0;
        } else if (len < sizeof(line) - 1) {
            line[len++] = (char)c;
        }
    }
}

// The guard period, honoured by both roles before either one transmits: wait until the
// bus is quiet by the driver's own view - nothing still going out, nothing still
// arriving - and then hold that quiet for guard_us.
static void guard_before_send() {
    if (g_guard_us == 0) return;
    while (ch->is_transmitting() || ch->is_receiving()) tight_loop_contents();
    const uint32_t quiet = time_us_32();
    while ((uint32_t)(time_us_32() - quiet) < g_guard_us) tight_loop_contents();
}

static void bring_up() {
    ch.emplace(PIN_TX, PIN_RX, PIN_DE, PIN_ACT);
    g_init_rc = ch->init(pio0, g_baud);
    // /RE is tied to DE on these boards, so RO floats for the whole of every
    // transmission and the pin needs a defined idle; see main.cpp.
    gpio_pull_up(PIN_RX);
    printf("init -> %d (0 = ok) at %u baud, clk_sys %u, divider %.3f\n", g_init_rc,
           (unsigned)g_baud, (unsigned)clock_get_hz(clk_sys),
           (double)clock_get_hz(clk_sys) /
               ((double)g_baud * (double)PicoRS485::pio_cycles_per_bit));
    // The boot defaults are applied through the same path a run uses, so the two cannot
    // drift apart.
    pico_rs485_packet_config pkt = ch->get_packet_config();
    pkt.tx_gap_bits = (uint8_t)g_gap_bits;
    pkt.parity_bits = (g_packet == 1u) ? 1u : 0u;
    pkt.stop_bits   = (g_packet == 2u) ? 2u : 1u;
    pkt.even_parity = true;
    g_packet_rc = ch->set_packet_config(pkt);
    calibrate_delay_unit();
    print_config();
    take_snapshot();
}

#ifdef RS485_ROLE_MASTER

int main() {
#if RS485_SYS_CLK_KHZ
    // Before stdio_init_all(), so USB enumerates on the clock the run will use.
    set_sys_clock_khz(RS485_SYS_CLK_KHZ, true);
#endif
    stdio_init_all();
    sleep_ms(2500);   // let USB enumerate so the output is not lost
    bring_up();
    printf("master ready: r<seconds> [period= guard= gap= packet= pairs= req= reply= "
           "baud=], d to dump\n");

    uint8_t  request[PicoRS485::max_burst_bytes] = {};
    uint8_t  reply[PicoRS485::max_burst_bytes] = {};
    uint8_t  seq = 0;
    uint32_t slot = 0;
    uint32_t next = time_us_32();
    uint32_t reply_timeout_us = 1000;

    while (true) {
        poll_console();
        if (!g_running) {
            sleep_ms(2);                   // idle: nothing on the bus, nothing printed
            continue;
        }
        // How long the master waits for a reply. A timeout is the caller's choice, and
        // the pipeline makes a short one expensive: one missed reply leaves a burst
        // queued, and from then on every exchange returns the *previous* reply - a whole
        // window of "stale" that looks like a driver fault and is not. Give it several
        // periods, so only a genuinely lost reply times out. Inside a group the wait
        // stays short, because the next exchange of the group follows immediately and a
        // long wait there would stall the group.
        reply_timeout_us = (g_period_us != 0 && g_pairs > 1)
                               ? (g_period_us / g_pairs)
                               : (g_period_us ? g_period_us * 4u : 1000u);

        if (g_period_us != 0 && slot == 0) {
            // A run starts on a fresh schedule: the idle before it is not lateness and
            // must not be recorded as the worst case.
            if (g_restart_schedule) {
                g_restart_schedule = false;
                next = time_us_32();
            }
            // Absolute schedule, but never build a backlog: a stall would otherwise
            // leave the master sending back to back until it caught up, which is the
            // saturating case rather than the paced one. Past a whole period late, the
            // schedule is reset instead of caught up.
            const uint32_t now = time_us_32();
            if ((int32_t)(next - now) > 0) {
                while ((int32_t)(time_us_32() - next) < 0) tight_loop_contents();
                next += g_period_us;
            } else {
                const uint32_t late = now - next;
                if (late > g_c.max_late_us) g_c.max_late_us = late;
                if (late > g_period_us) {
                    if (g_c.late_cycles == 0) {
                        g_c.late_first_at  = g_c.cycles;
                        g_c.late_first_rtt = g_last_rtt;
                    }
                    ++g_c.late_cycles;
                    next = now + g_period_us;
                } else {
                    next += g_period_us;
                }
            }
        }

        guard_before_send();

        // A deliberate, finely adjustable delay before the request goes out. One iteration
        // is a few cycles, so a scan of this shifts the request by fractions of a bit at the
        // highest rate - a way to move the exchange's timing without changing anything else.
        for (uint32_t i = 0; i < g_pad; ++i) tight_loop_contents();

        const uint8_t want = seq;
        build_frame(request, g_req_len, want);

        const uint32_t t0  = time_us_32();
        const int      rc  = ch->send(request, g_req_len);
        int            got = 0;
        if (rc == PicoRS485::ok) {
            got = ch->receive_for(reply, sizeof(reply), reply_timeout_us);
        }
        const uint32_t rtt = time_us_32() - t0;
        g_last_rtt = rtt;
        ++g_c.cycles;
        ++seq;
        const uint32_t this_slot = slot;
        if (++slot >= g_pairs) slot = 0;

        if (rc != PicoRS485::ok) {
            ++g_c.refused;                 // still transmitting or receiving: too fast
        } else if (got == (int)g_reply_len) {
            ++g_c.replied;
            g_c.rtt_sum += rtt;
            if (rtt > g_c.rtt_max) g_c.rtt_max = rtt;
            // Judge the burst on what it claims to be first, then on whether that claim
            // is the exchange we are waiting for.
            const uint8_t claims = frame_seq(reply, g_reply_len);
            if (!check_frame(reply, g_reply_len, claims)) {
                ++g_c.corrupt;
                ++g_c.slot_corrupt[this_slot];
            } else if (claims != want) {
                ++g_c.stale;
                ++g_c.slot_stale[this_slot];
            } else {
                ++g_c.good;
                ++g_c.slot_good[this_slot];
            }
        } else if (got > 0) {
            ++g_c.short_bursts;            // a split or truncated reply is its own burst
        } else if (got == PicoRS485::err_timeout) {
            ++g_c.timeouts;
            ++g_c.slot_timeouts[this_slot];
        } else {
            ++g_c.errors;
        }

        take_snapshot();
        if ((int32_t)(to_ms_since_boot(get_absolute_time()) - g_run_end_ms) >= 0) {
            g_running = false;             // the window is over; the dump is on request
        }
    }
}

#endif  // RS485_ROLE_MASTER

#ifdef RS485_ROLE_SLAVE

int main() {
#if RS485_SYS_CLK_KHZ
    // Before stdio_init_all(), so USB enumerates on the clock the run will use.
    set_sys_clock_khz(RS485_SYS_CLK_KHZ, true);
#endif
    stdio_init_all();
    sleep_ms(2500);
    bring_up();
    printf("slave ready: answers requests; r<seconds> [keys] marks a window, d to dump\n");

    uint8_t  buf[PicoRS485::max_burst_bytes] = {};
    uint8_t  reply[PicoRS485::max_burst_bytes] = {};
    uint8_t  last_seq = 0;
    bool     have_seq = false;

    while (true) {
        poll_console();
        const int n = ch->receive_for(buf, sizeof(buf), 2000);   // a request every period
        if (n > 0) {
            ++g_c.requests;
            if (n != (int)g_req_len) ++g_c.wrong_len;   // a split request lands here
            // Check the request against the sequence it claims: a burst that is one
            // behind is stale, not corrupt, and only a broken pattern is corruption.
            const uint8_t claimed = frame_seq(buf, (size_t)n);
            if (!check_frame(buf, (size_t)n, claimed)) ++g_c.bad_request;

            if (have_seq) {
                const uint8_t step = (uint8_t)(claimed - last_seq);
                if (step != 1) g_c.gaps += (uint32_t)(uint8_t)(step - 1);
            }
            last_seq = claimed;
            have_seq = true;

            build_frame(reply, g_reply_len, claimed);
            guard_before_send();
            if (ch->send(reply, g_reply_len) == PicoRS485::ok) ++g_c.replies;
            else ++g_c.refused;   // usually still transmitting the previous reply
        } else if (n != PicoRS485::err_timeout) {
            ++g_c.errors;         // silent: an error here must not print during a run
        }
        take_snapshot();
        if (g_running &&
            (int32_t)(to_ms_since_boot(get_absolute_time()) - g_run_end_ms) >= 0) {
            g_running = false;
        }
    }
}

#endif  // RS485_ROLE_SLAVE
