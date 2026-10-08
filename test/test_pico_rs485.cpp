// Host test suite for PicoRS485: the entry point.
//
// The SDK surface is stubbed in suite_model.cpp, the state and the CHECK/CASE
// machinery are in suite.h, and each category of tests is one file under
// sections_*.cpp. They run in this order: the last one reads the counters the
// others accumulated.
//
// Sections, by category:
//
//   lifecycle
//       1. lifecycle: construction claims nothing
//       2. init: the defaults reach the four machines
//       3. teardown: the machines, the memory and the pads
//       4. init: an explicit packet config
//       5. init: the explicit-clock overload
//       6. lifecycle: instruction memory is emptied before every load
//       7. lifecycle: init is retryable after a failure
//       8. lifecycle: teardown gives the DMA and the IRQ back
//       9. lifecycle: stale flags cannot survive init or release
//
//   failure
//      10. failure: an unusable baudrate
//      11. failure: a pin outside the bank
//      12. failure: an unusable packet or clock config
//      13. failure: not a usable PIO
//      14. failure: a state machine is already claimed
//      15. failure: program load, at each of the four loads
//      16. failure: the SDK refused a machine configuration
//      17. failure: busy wins, and the live channel is untouched
//      18. failure: resource exhaustion
//      19. failure: error precedence
//      20. failure: the four pins must be four pins
//
//   config
//      21. config: set_packet_config() and get_packet_config()
//      22. config: is_clock_config_valid()
//      23. config: is_packet_config_valid() ranges
//      24. config: clock derivation against the pre-refactor reference
//      25. config: worst-case divider error stays inside the UART budget
//      26. config: set_packet_config() refuses while anything is in flight
//      27. config: a refused config change leaves everything as it was
//      28. config: the idle threshold must clear the driver's own gap
//      29. config: set_packet_config() refuses a character that is already arriving
//
//   transmit
//      30. transmit: one burst, framed and streamed end to end
//      31. transmit: the length boundary and the pacing
//      32. transmit: framing across configurations
//      33. transmit: refusals
//      34. transmit: a second send while the first is only queued
//
//   receive
//      35. receive: one silence-delimited burst
//      36. receive: decoding across configurations
//      37. receive: the queue, its overflow and the length limit
//      38. receive: take_burst() writes only what the caller offered
//      39. receive: what the API refuses
//      40. receive: a burst arriving while two are queued cannot overwrite them
//      41. overrun recovery: drain, restart, rewind, clean next burst
//      42. receive: a burst of N characters publishes N bytes and does not drop
//      43. receive: a stalled autopush must not re-phase the grouping
//      44. RP2040-E13: a write still in flight when the channel is stopped
//      45. receive: a silence flag with nothing behind it is counted
//      46. receive: leaves the interrupt line as it found it
//
//   completion
//      47. completion: reported once the wire is clear
//      48. completion: the slot, and what a callback-less send owes
//      49. completion: teardown drops an unobserved one
//      50. completion: the timestamp is latched when observed
//      51. completion: the callback may queue the next burst
//      52. completion: the end-of-burst interrupt reports the transmission
//
//   ownership
//      53. ownership: is_pio_valid() is a pure check
//      54. ownership: GPIO base windows
//      55. ownership: two channels on different PIOs route their own flags
//      56. ownership: only the two completion flags are routed to the CPU
//      57. ownership: is_pio_valid() refuses an IRQ line it does not own
//      58. ownership: using the driver from another core asserts
//      59. ownership: two maximum burst lengths in one program
//
//   invariants
//      60. invariants: accumulated over the run
//
#include "suite.h"
void suite_lifecycle();
void suite_failure();
void suite_config();
void suite_transmit();
void suite_receive();
void suite_completion();
void suite_ownership();
void suite_invariants();

int main() {
    // Unbuffered: a crash still shows the checks that ran before it.
    setvbuf(stdout, nullptr, _IONBF, 0);
    suite_lifecycle();
    suite_failure();
    suite_config();
    suite_transmit();
    suite_receive();
    suite_completion();
    suite_ownership();
    suite_invariants();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
