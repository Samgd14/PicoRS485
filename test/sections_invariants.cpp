// INVARIANTS
//
#include "suite.h"

void suite_invariants() {
    // --- 60. invariants: accumulated over the run ----------------------------
    CASE("invariants: accumulated over the run");
    // These counters are never reset, so this really does cover the run.
    CHECK_EQ(g_double_claim, 0);   // pio_sm_claim() never hit an owned machine
    CHECK_EQ(g_bad_unclaim, 0);    // and no machine was handed back unowned
    CHECK_EQ(g_bad_irq_remove, 0); // and every handler removed was the one installed
    CHECK_EQ(g_add_dirty, 0);      // and nothing was loaded into un-emptied memory
    // The hook is the only report the driver makes, so the sections above having reached it is what
    // says the diagnostic works end to end.
    CHECK(g_core_hook_calls > 0);
}
