#!/usr/bin/env pwsh
# The mutation corpus: a maintainer tool, not part of run_tests.ps1.
#
#     pwsh test/mutation_corpus.ps1
#
# Each mutation is applied to a COPY of src/PicoRS485.cpp and compiled against the
# UNMODIFIED suite. "SURVIVED" means no assertion in the suite can see the wrong driver.
# "NOT-APPLIED" means the anchor no longer matches the code, which is the corpus's own
# debt rather than a gap in the suite, and a few mutants are equivalent by construction.
# A run that does not terminate is reported as HANG (detected, but not by an assertion).
# The mutants and their executables are written to test/build/mutants, which is git-ignored;
# a full run takes several minutes.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# Mutants fault on purpose (a wild index, a lost interrupt), and on Windows an unhandled
# access violation raises a "program has stopped working" dialog that waits for a click -
# one per crashing mutant, dozens per run. The error mode is inherited by child processes,
# so set it before spawning any: FAILCRITICALERRORS | NOGPFAULTERRORBOX, the same pair
# run_tests.ps1 uses for the same reason.
try {
    Add-Type -Namespace Native -Name ErrMode -MemberDefinition `
        '[System.Runtime.InteropServices.DllImport("kernel32.dll")] public static extern uint SetErrorMode(uint uMode);' `
        -ErrorAction Stop
    [void][Native.ErrMode]::SetErrorMode(0x0003)
} catch {
    Write-Host 'note: could not suppress crash dialogs in this shell'
}

$root   = Split-Path $PSScriptRoot -Parent
$src    = Join-Path $root 'src/PicoRS485.cpp'
$suite  = @(Join-Path $root 'test/test_pico_rs485.cpp'
            Join-Path $root 'test/suite_model.cpp') +
          @(Get-ChildItem (Join-Path $root 'test/sections_*.cpp') | Sort-Object Name | ForEach-Object { $_.FullName })
$mutDir = Join-Path $root 'test/build/mutants'
New-Item -ItemType Directory -Force -Path $mutDir | Out-Null

$orig = ((Get-Content $src -Raw) -replace "`r`n", "`n")

$M = @(
  # ---- prior batches, re-expressed against the current source -----------------
  @{ n='m01-tx-table-ignores-data-mask'; o=@'
        uint32_t word = (byte & data_mask) << 1;
        if (pkt.parity_bits != 0) {
            uint32_t parity = (uint32_t)__builtin_parity(byte & data_mask);
'@; e=@'
        uint32_t word = (uint32_t)byte << 1;
        if (pkt.parity_bits != 0) {
            uint32_t parity = (uint32_t)__builtin_parity(byte);
'@ }
  @{ n='m02-rx-masks-hardcoded-8bit'; o=@'
    const uint32_t data_mask   = (1u << data_bits) - 1u;
    const uint32_t parity_mask = 1u << data_bits;
    const uint32_t stop_mask   = 1u << (data_bits + parity_bits);
'@; e=@'
    const uint32_t data_mask   = 0xFFu;
    const uint32_t parity_mask = 1u << 8;
    const uint32_t stop_mask   = 1u << data_bits;
'@ }
  @{ n='m03-receive_for-always-times-out'; o=@'
int PicoRS485::receive_timed(uint8_t *out, size_t max, uint32_t timeout_us, bool bounded) {
    // Returns if the channel is not initialized
    if (pio_ == nullptr) return err_not_initialized;
    // Returns if the out buffer is invalid
    if (out == nullptr && max != 0) return err_argument;
'@; e=@'
int PicoRS485::receive_timed(uint8_t *out, size_t max, uint32_t timeout_us, bool bounded) {
    // Returns if the channel is not initialized
    if (pio_ == nullptr) return err_not_initialized;
    // Returns if the out buffer is invalid
    if (out == nullptr && max != 0) return err_argument;
    return err_timeout;
'@ }
  @{ n='m05-arm-rx-without-the-extra-count'; o='max_packet_bytes + 1, true);'; e='max_packet_bytes, true);' }
  @{ n='m05b-arm-rx-count-but-words-without-extra'; o='        (max_packet_bytes + 1) - dma_channel_hw_addr(rx_dma_)->transfer_count;'; e='        max_packet_bytes - dma_channel_hw_addr(rx_dma_)->transfer_count;' }
  @{ n='m06-send-guard-drops-is_transmitting'; o='    if (is_transmitting() || is_receiving()) return err_in_flight;'; e='    if (is_receiving()) return err_in_flight;' }
  @{ n='m07-tx-dma-transfer-size-8'; o=@'
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
'@; e=@'
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, true);
'@ }
@{ n='m08-parity-check-runs-without-parity'; o=@'
        if (want_parity) {
'@; e=@'
        if (true) {
'@ }
  @{ n='m09-send-rejects-len-equal-max'; o='    if (data == nullptr || len == 0 || len > max_packet_bytes) return err_argument;'; e='    if (data == nullptr || len == 0 || len >= max_packet_bytes) return err_argument;' }
  @{ n='m10-compute-clock-writes-sys_hz-on-failure'; o='        if (baudrate == 0 || (uint64_t)baudrate * pio_cycles_per_bit > sys_hz) return false;'; e=@'
    out.sys_hz = sys_hz;
        if (baudrate == 0 || (uint64_t)baudrate * pio_cycles_per_bit > sys_hz) return false;
'@; v='    out.sys_hz = sys_hz;' }
  @{ n='m11-receive_for-strict-greater-timeout'; o='        if (bounded && (uint32_t)(time_us_32() - start) >= timeout_us) return err_timeout;'; e='        if (bounded && (uint32_t)(time_us_32() - start) > timeout_us) return err_timeout;' }
  @{ n='m12-publish-newest-burst-first'; o=@'
        if (rx_ready_count_ >= rx_queue_depth) {  // make room by dropping the oldest
            rx_ready_[0] = rx_ready_[1];
            --rx_ready_count_;
            ++dropped_bursts_;
        }
        rx_ready_[rx_ready_count_++] = active;
'@; e=@'
        if (rx_ready_count_ >= rx_queue_depth) {  // make room by dropping the oldest
            --rx_ready_count_;
            ++dropped_bursts_;
        } else {
            rx_ready_[1] = rx_ready_[0];
        }
        rx_ready_[0] = active;
        ++rx_ready_count_;
'@ }
  @{ n='m13-tx-byte-order-reversed'; o='        tx_scratch_[i] = frame_table_[data[i]];'; e='        tx_scratch_[len - 1 - i] = frame_table_[data[i]];' }
  @{ n='m14-take-burst-returns-words-plus-one'; o='    return (int)words;'; e='    return (int)words + 1;' }
  @{ n='m15-parity-check-skipped'; o='            if (((bits & parity_mask) != 0) != (expected != 0)) ++parity;'; e='            (void)expected;' }
  @{ n='m16-is-transmitting-drops-de'; o=@'
           !pio_sm_is_tx_fifo_empty(pio_, sm_tx) ||
           gpio_get(pin_de_);
'@; e=@'
           !pio_sm_is_tx_fifo_empty(pio_, sm_tx) ||
           false;
'@ }
  @{ n='m17-start-bit-inverted'; o='        frame_table_[byte] = word;'; e='        frame_table_[byte] = word ^ 1u;' }
  @{ n='m18-instances-entry-not-cleared-on-release'; o='        instances_[PIO_NUM(pio_)] = nullptr;'; e='        /* instances_ entry left behind */' }
  @{ n='m19-overflow-burst-not-counted'; o=@'
    if (dma_fell_behind || words > max_packet_bytes) {
        ++dropped_bursts_;
    } else if (words != 0) {
'@; e=@'
    if (dma_fell_behind || words > max_packet_bytes) {
    } else if (words != 0) {
'@ }
@{ n='m20-is-receiving-wrong-staging-buffer'; o=@'
    return dma_channel_hw_addr(rx_dma_)->write_addr != rx_armed_base_;
'@; e=@'
    return dma_channel_hw_addr(rx_dma_)->write_addr == rx_armed_base_;
'@ }
  @{ n='m24-rx-dma-write-increment-off'; o=@'
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
'@; e=@'
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, false);
'@ }
  @{ n='m25-tx-dma-count-len-plus-one'; o='    dma_channel_configure(tx_dma_, &c, &pio_->txf[sm_tx], tx_scratch_, len, true);'; e='    dma_channel_configure(tx_dma_, &c, &pio_->txf[sm_tx], tx_scratch_, len + 1, true);' }
  @{ n='m26-three-stop-bits-for-two'; o='        for (uint32_t stop = 0; stop < pkt.stop_bits; ++stop) {'; e='        for (uint32_t stop = 0; stop <= pkt.stop_bits; ++stop) {' }
  @{ n='m27-parity-bit-shifted-one-high'; o='            word |= parity << (1u + pkt.data_bits);'; e='            word |= parity << (2u + pkt.data_bits);' }
  @{ n='m28-is_pio_valid-checks-only-sm0'; o=@'
    for (uint sm = 0; sm < NUM_PIO_STATE_MACHINES; ++sm) {
        if (pio_sm_is_claimed(pio, sm)) return err_state_machine;
'@; e=@'
    for (uint sm = 0; sm < 1; ++sm) {
        if (pio_sm_is_claimed(pio, sm)) return err_state_machine;
'@ }
  @{ n='m29-take-burst-ignores-max'; o='        if (i < max) out[i] = (uint8_t)(bits & data_mask);'; e='        out[i] = (uint8_t)(bits & data_mask);' }
  @{ n='m30-no-dma-failure-check-for-rx'; o='    if (tx_dma_ < 0 || rx_dma_ < 0) {'; e='    if (tx_dma_ < 0) {' }
  @{ n='m31-record-config-before-in-flight-check'; o=@'
    if (in_flight()) {
        irq_set_enabled(irq, irq_was_enabled);
        return err_in_flight;
    }

    // Record the new config to the class
    pkt_        = pkt;
'@; e=@'
    // Record the new config to the class
    pkt_        = pkt;

    if (in_flight()) {
        irq_set_enabled(irq, irq_was_enabled);
        return err_in_flight;
    }
'@ }
@{ n='m31b-setters-removed'; o=@'
    rs485_tx_set_frame_bits(pio_, sm_tx, frame_bits_);
    rs485_rx_set_isr_bits(pio_, sm_rx, isr_bits_);
    rs485_idle_set_silence_bits(pio_, sm_idle, pkt_.silence_bits);
    rs485_txgap_set_gap_bits(pio_, sm_txgap, pkt_.gap_bits);
'@; e=@'
    /* the machines keep the old framing */
'@ }
  @{ n='m32-setters-target-the-wrong-state-machine'; o=@'
    rs485_tx_set_frame_bits(pio_, sm_tx, frame_bits_);
    rs485_rx_set_isr_bits(pio_, sm_rx, isr_bits_);
    rs485_idle_set_silence_bits(pio_, sm_idle, pkt_.silence_bits);
    rs485_txgap_set_gap_bits(pio_, sm_txgap, pkt_.gap_bits);
'@; e=@'
    rs485_tx_set_frame_bits(pio_, sm_rx, frame_bits_);
    rs485_rx_set_isr_bits(pio_, sm_tx, isr_bits_);
    rs485_idle_set_silence_bits(pio_, sm_txgap, pkt_.silence_bits);
    rs485_txgap_set_gap_bits(pio_, sm_idle, pkt_.gap_bits);
'@ }
  @{ n='m33-no-receiver-restart-after-overflow'; o='    if (dma_fell_behind || words > max_packet_bytes) rx_rewind();'; e='    if (dma_fell_behind || words > max_packet_bytes) { }' }
  @{ n='m34-no-rx-fifo-clear-in-handler'; o=@'
    if (dma_fell_behind) rx_drain_fifo();

    // A burst that ran too long, or one the DMA did not finish, left the receiver mid-frame.
    if (dma_fell_behind || words > max_packet_bytes) rx_rewind();
'@; e=@'
    // A burst that ran too long, or one the DMA did not finish, left the receiver mid-frame.
    if (dma_fell_behind || words > max_packet_bytes) rx_rewind();
'@ }
  @{ n='m34b-init-does-not-restart-or-flush-rx'; o=@'
    rx_drain_fifo();
    rx_rewind();
'@; e=@'
    /* recovery omitted */
'@ }
  @{ n='m35-instances-always-index-zero'; o='    instances_[PIO_NUM(pio_)] = this;'; e='    instances_[0] = this;' }
  @{ n='m36-irq-handler-routes-only-instances-0'; o=@'
void PicoRS485::irq_handler() {
    for (uint i = 0; i < NUM_PIOS; ++i) {
        if (instances_[i] != nullptr) instances_[i]->service_silence();
    }
}
'@; e=@'
void PicoRS485::irq_handler() {
    if (instances_[0] != nullptr) instances_[0]->service_silence();
}
'@ }
  @{ n='m37-rearm-the-buffer-just-published'; o='    if (next < rx_buffer_count) arm_receive(next);'; e='    if (next < rx_buffer_count) arm_receive(active);' }
  @{ n='m38-take-burst-does-not-reenable-irq'; o=@'
    irq_set_enabled(irq, irq_was_enabled);
    return (int)words;
'@; e=@'
    return (int)words;
'@ }
  @{ n='m39-take-burst-does-not-pop-the-queue'; o=@'
    --rx_ready_count_;
    if (rx_ready_count_ != 0) rx_ready_[0] = rx_ready_[1];
'@; e=@'
    if (rx_ready_count_ != 0) rx_ready_[0] = rx_ready_[1];
'@ }
@{ n='m40-de-pin-mask-wrong-in-release'; o=@'
    pio_sm_set_pins_with_mask64(pio_, sm_tx, 0u, 1ull << pin_de_);
'@; e=@'
    pio_sm_set_pins_with_mask64(pio_, sm_tx, 0u, 1ull << pin_tx_);
'@ }
  @{ n='m42-empty-bursts-also-queued'; o='    } else if (words != 0) {'; e='    } else if (words != 0 || words == 0) {' }
  @{ n='m43-refusal-applies-everything-first'; o=@'
    if (in_flight()) {
        irq_set_enabled(irq, irq_was_enabled);
        return err_in_flight;
    }
'@; e=@'
    // Record the new config to the class
'@; extra=@(@{o=@'
    rs485_txgap_set_gap_bits(pio_, sm_txgap, pkt_.gap_bits);

    return ok;
'@; e=@'
    rs485_txgap_set_gap_bits(pio_, sm_txgap, pkt_.gap_bits);

    if (in_flight()) {
        return err_in_flight;
    }

    return ok;
'@}) }
  @{ n='m44-rx-data-mask-all-ones'; o='    const uint32_t data_mask   = (1u << data_bits) - 1u;'; e='    const uint32_t data_mask   = 0xFFFFFFFFu;' }
  @{ n='m45-rx-parity-mask-hardcoded-8bit'; o='    const uint32_t parity_mask = 1u << data_bits;'; e='    const uint32_t parity_mask = 1u << 8;' }

  # ---- new mutations ---------------------------------------------------------
  @{ n='n01-service-ignores-owed-slot'; o='    if (pio_ == nullptr || !tx_pending_) return;   // the common case: nothing owed'; e='    if (pio_ == nullptr) return;   // the common case: nothing owed' }
  @{ n='n02-service-never-clears-tx-pending'; o=@'
    tx_pending_      = false;
    last_sent_at_us_ = time_us_32();
'@; e=@'
    last_sent_at_us_ = time_us_32();
'@ }
  @{ n='n03-service-reports-twice'; o=@'
    tx_pending_      = false;
    last_sent_at_us_ = time_us_32();
'@; e=@'
    last_sent_at_us_ = time_us_32();
'@; extra=@(@{o=@'
    tx_callback_ = nullptr;
    tx_user_     = nullptr;
    if (cb != nullptr) cb(user, id);
'@; e=@'
    if (cb != nullptr) cb(user, id);
'@}) }
  @{ n='n04-service-reports-before-wire-clear'; o='    if (is_transmitting()) return;                 // still going out'; e='    /* reports early */' }
  @{ n='n05-release-delivers-unreported-completion'; o=@'
    tx_pending_  = false;
    tx_callback_ = nullptr;
    tx_user_     = nullptr;
'@; e=@'
    if (tx_callback_ != nullptr) tx_callback_(tx_user_, tx_burst_id_);
    tx_pending_  = false;
    tx_callback_ = nullptr;
    tx_user_     = nullptr;
'@ }
  @{ n='n06-burst-id-never-incremented'; o='    ++tx_burst_id_;'; e='    /* id never increments */' }
  @{ n='n07-burst-id-reported-plus-one'; o='    const uint32_t id = tx_burst_id_;'; e='    const uint32_t id = tx_burst_id_ + 1;' }
  @{ n='n08-last-sent-at-recorded-at-send'; o=@'
    tx_pending_  = true;
    tx_callback_ = cb;
'@; e=@'
    tx_pending_  = true;
    last_sent_at_us_ = time_us_32();
    tx_callback_ = cb;
'@ }
  @{ n='n09-send-accepts-null-data'; o='    if (data == nullptr || len == 0 || len > max_packet_bytes) return err_argument;'; e='    if (len == 0 || len > max_packet_bytes) return err_argument;' }
  @{ n='n10-packet-and-clock-validation-swapped'; o=@'
    if (!is_packet_config_valid(pkt)) return err_packet;
    if (!is_clock_config_valid(clk)) return err_clock;
'@; e=@'
    if (!is_clock_config_valid(clk)) return err_clock;
    if (!is_packet_config_valid(pkt)) return err_packet;
'@ }
  @{ n='n11-pin-check-after-packet-check'; o=@'
    if (pin_tx_ >= NUM_BANK0_GPIOS || pin_rx_ >= NUM_BANK0_GPIOS ||
        pin_de_ >= NUM_BANK0_GPIOS || pin_rx_act_ >= NUM_BANK0_GPIOS) {
        return err_pin;
    }

    // Validate packet/clock configurations
    if (!is_packet_config_valid(pkt)) return err_packet;
'@; e=@'
    if (!is_packet_config_valid(pkt)) return err_packet;

    // Check the pin numbers are valid
    if (pin_tx_ >= NUM_BANK0_GPIOS || pin_rx_ >= NUM_BANK0_GPIOS ||
        pin_de_ >= NUM_BANK0_GPIOS || pin_rx_act_ >= NUM_BANK0_GPIOS) {
        return err_pin;
    }
'@ }
  @{ n='n12-gpio-window-excludes-top-pin'; o=@'
    if (pin_tx_ < base || pin_tx_ >= base + 32 ||
        pin_rx_ < base || pin_rx_ >= base + 32 ||
        pin_de_ < base || pin_de_ >= base + 32 ||
        pin_rx_act_ < base || pin_rx_act_ >= base + 32) {
'@; e=@'
    if (pin_tx_ < base || pin_tx_ >= base + 30 ||
        pin_rx_ < base || pin_rx_ >= base + 30 ||
        pin_de_ < base || pin_de_ >= base + 30 ||
        pin_rx_act_ < base || pin_rx_act_ >= base + 30) {
'@ }
@{ n='n13-release-leaves-activity-pin-muxed'; o=@'
        gpio_init(pin_rx_act_);
'@; e=@'
        /* left muxed to the PIO */
'@ }
@{ n='n14-arm-receive-does-not-stop-the-channel'; o=@'
    dma_channel_cleanup(rx_dma_);
'@; e=@'
    /* the channel is left enabled */
'@ }
  @{ n='n15-init-does-not-clear-handshake-flags'; o=@'
    pio_interrupt_clear(pio_, tx_done_irq);
    pio_interrupt_clear(pio_, rx_done_irq);
    pio_interrupt_clear(pio_, tx_gap_irq);

    // Initialize each state machine
'@; e=@'
    // Initialize each state machine
'@ }
  @{ n='n16-release-does-not-clear-handshake-flags'; o=@'
        pio_interrupt_clear(pio_, tx_done_irq);
        pio_interrupt_clear(pio_, rx_done_irq);
        pio_interrupt_clear(pio_, tx_gap_irq);
        irq_set_enabled(PIO_IRQ_NUM(pio_, 0), false);
'@; e=@'
        irq_set_enabled(PIO_IRQ_NUM(pio_, 0), false);
'@ }
  @{ n='n17-take-burst-clamps-its-return'; o='    return (int)words;'; e='    return (int)(words < max ? words : max);' }
  @{ n='n18-framing-error-inverted'; o='        if ((bits & stop_mask) == 0) ++framing;'; e='        if ((bits & stop_mask) != 0) ++framing;' }
  @{ n='n19-framing-errors-never-counted'; o='        if ((bits & stop_mask) == 0) ++framing;'; e='        ;' }
  @{ n='n20-rx-parity-ignores-mode'; o='            const uint32_t expected = (table[bits & data_mask] >> (1u + data_bits)) & 1u;'; e='            const uint32_t expected = (uint32_t)__builtin_parity(bits & data_mask) & 1u;' }
  @{ n='n21-tx-parity-mode-inverted'; o='            if (pkt.mode == parity_mode::odd) parity ^= 1u;'; e='            if (pkt.mode != parity_mode::odd) parity ^= 1u;' }
  @{ n='n22-rx-bits-not-shifted'; o='        const uint32_t bits = staged[i] >> shift;'; e='        const uint32_t bits = staged[i];' }
  @{ n='n23-handler-publishes-a-wrong-buffer'; o='        rx_ready_[rx_ready_count_++] = active;'; e='        rx_ready_[rx_ready_count_++] = (uint8_t)((active + 1) % rx_buffer_count);' }
  @{ n='n24-is-transmitting-ignores-dma'; o=@'
    return dma_channel_is_busy(tx_dma_) ||
           !pio_sm_is_tx_fifo_empty(pio_, sm_tx) ||
'@; e=@'
    return false ||
           !pio_sm_is_tx_fifo_empty(pio_, sm_tx) ||
'@ }
@{ n='n25-receive-does-not-check-liveness'; o=@'
    if (pio_ == nullptr) return err_not_initialized;
'@; e=@'
    /* no liveness check */
'@ }
  @{ n='n26-framing-error-counted-once-per-burst'; o='        if ((bits & stop_mask) == 0) ++framing;'; e='        if ((bits & stop_mask) == 0 && i == 0) ++framing;' }
  @{ n='n27-release-does-not-restore-rx-pin'; o=@'
        gpio_init(pin_tx_);
        gpio_init(pin_rx_act_);
        gpio_init(pin_rx_);
'@; e=@'
        gpio_init(pin_tx_);
        gpio_init(pin_rx_act_);
'@ }

  # ---- phase C ---------------------------------------------------------------
  @{ n='p01-pio-valid-ignores-the-irq-line'; o=@'
    if (irq_has_handler(PIO_IRQ_NUM(pio, 0))) return err_irq_in_use;
'@; e=@'
'@ }
  # The pin lives inside is_receiving() now, so "ignores a frame arriving" is one mutation and not
  # two: it was p02 and p05 separately while the same check was written at both gates.
  @{ n='p02-rx-ignores-a-frame-arriving'; o='    if (irq_registered_ && gpio_get(pin_rx_act_)) return true;'; e='    /* the pin is ignored */' }
  @{ n='p03-take-burst-trusts-a-queued-index'; o='    if (queued == 0 || buffer >= rx_buffer_count) {'; e='    if (queued == 0) {' }
  @{ n='p04-core-check-never-fails'; o='    return !irq_registered_ || get_core_num() == core_;'; e='    return true;' }
  @{ n='p04b-core-violation-not-counted'; o='    ++core_violations_;'; e='    /* not counted */' }
  @{ n='p04c-core-violation-hook-not-called'; o='    pico_rs485_core_violation();'; e='    /* no hook */' }
  @{ n='p06-release-resets-pins-anyway'; o='    if (pins_claimed_) {'; e='    if (true) {' }
  @{ n='p07-is-transmitting-ignores-the-latch'; o='    return tx_pending_;'; e='    return false;' }
  @{ n='m21-burst-duration-drops-gap'; o=@'
    const uint64_t bits  = bytes * (frame_bits_ + pkt_.gap_bits + pio_overhead_bits);
'@; e=@'
    const uint64_t bits  = bytes * (frame_bits_ + pio_overhead_bits);
'@ }
  @{ n='m21b-burst-duration-rounds-down'; o=@'
    const uint64_t us = (bits * 1000000u + baudrate_ - 1) / baudrate_;
'@; e=@'
    const uint64_t us = (bits * 1000000u) / baudrate_;
'@ }
  @{ n='m22-take-burst-wrong-length-array'; o=@'
    const uint16_t words  = rx_length_[buffer];
'@; e=@'
    const uint16_t words  = rx_length_[rx_active_];
'@ }
  @{ n='m23-queue-full-drops-newest'; o=@'
            rx_ready_[0] = rx_ready_[1];
'@; e=@'

'@ }
  @{ n='m41-release-leaves-rx-dma-claimed'; o=@'
        dma_channel_unclaim(rx_dma_);
'@; e=@'

'@ }
)
# Obsolete: the code these mutated is gone (the hardware terms, the poll, the in-header
# thunk, the precedence guard - see test/README.md).
$M = $M | Where-Object { $_.n -notin @('m05b-arm-rx-count-but-words-without-extra','m16-is-transmitting-drops-de','n24-is-transmitting-ignores-dma','m36-irq-handler-routes-only-instances-0','n01-service-ignores-owed-slot','n04-service-reports-before-wire-clear','n11-pin-check-after-packet-check') }

$rows = @()

# Runs a program and captures both streams, with a hard timeout.
function Invoke-Capture {
    param([string]$File, [string[]]$Argv, [int]$TimeoutMs)
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName               = $File
    $psi.Arguments              = ($Argv -join ' ')
    $psi.UseShellExecute        = $false
    $psi.CreateNoWindow         = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError  = $true
    $p = [System.Diagnostics.Process]::Start($psi)
    $oTask = $p.StandardOutput.ReadToEndAsync()
    $eTask = $p.StandardError.ReadToEndAsync()
    $done = $p.WaitForExit($TimeoutMs)
    if (-not $done) { try { $p.Kill() } catch {} }
    return [pscustomobject]@{
        code = $(if ($done) { $p.ExitCode } else { -999 })
        out  = $oTask.Result
        err  = $eTask.Result
        hang = (-not $done)
    }
}

foreach ($m in $M) {
    $text = $orig
    $applied = $text.Contains($m.o)
    if ($applied) { $text = $text.Replace($m.o, $m.e) }
    if ($applied -and $m.ContainsKey('extra')) {
        foreach ($x in $m.extra) {
            if (-not $text.Contains($x.o)) { $applied = $false; break }
            $text = $text.Replace($x.o, $x.e)
        }
    }
    if (-not $applied) {
        $rows += [pscustomobject]@{ name=$m.n; result='NOT-APPLIED'; detail='old string not found' }
        continue
    }
    $mf  = Join-Path $mutDir ($m.n + '.cpp')
    Set-Content -Path $mf -Value $text -Encoding utf8 -NoNewline
    # Prove the edit landed, not just that the replace was attempted.
    $needle = if ($m.ContainsKey('v')) { $m.v } else { $m.e.Trim() }
    $onDisk = Get-Content $mf -Raw
    if ($needle -and -not $onDisk.Contains($needle)) {
        $rows += [pscustomobject]@{ name=$m.n; result='NOT-APPLIED'; detail="verify needle missing: $needle" }
        continue
    }
    $exe = Join-Path $mutDir ($m.n + '.exe')
    $gxxArgs = @('-std=c++17','-w','-O1',
                 '-I', (Join-Path $root 'include'), '-I', (Join-Path $root 'test/stub'),
                 '-o', $exe) + $suite + @($mf)
    $b = Invoke-Capture -File 'g++' -Argv $gxxArgs -TimeoutMs 120000
    if ($b.code -ne 0) {
        $rows += [pscustomobject]@{ name=$m.n; result='BUILD-FAILED'; detail=(($b.err -split "`n") | Select-Object -First 2) -join ' | ' }
        continue
    }
    $r = Invoke-Capture -File $exe -Argv @() -TimeoutMs 20000
    if ($r.hang) {
        $rows += [pscustomobject]@{ name=$m.n; result='HANG'; detail='no termination within 20 s' }
        continue
    }
    $out  = $r.out
    $last = (($out -split "`n") | Where-Object { $_ -match 'checks,' } | Select-Object -Last 1)
    if ($r.code -eq 0 -and $last -match '0 failures') {
        $rows += [pscustomobject]@{ name=$m.n; result='SURVIVED'; detail=$last }
    } else {
        $firstFail = (($out -split "`n") | Where-Object { $_ -match '^FAIL ' } | Select-Object -First 1)
        $rows += [pscustomobject]@{ name=$m.n; result=("KILLED($($r.code))"); detail=("$firstFail") }
    }
}
$rows | ForEach-Object { "{0}`t{1}`t{2}" -f $_.name, $_.result, $_.detail }
