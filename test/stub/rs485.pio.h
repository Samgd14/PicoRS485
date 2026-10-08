// Hand-written stand-in for the pioasm-generated "rs485.pio.h".
// Function signatures are copied from the % c-sdk blocks in src/rs485.pio, so a
// syntax check of PicoRS485.cpp validates the real call signatures - including
// the int returns the helpers now use to report the SDK's own verdict.
#ifndef _STUB_RS485_PIO_H
#define _STUB_RS485_PIO_H

#include "hardware/pio.h"

extern const struct pio_program rs485_tx_program;
extern const struct pio_program rs485_txgap_program;
extern const struct pio_program rs485_rx_program;
extern const struct pio_program rs485_rxidle_program;

pio_sm_config rs485_tx_program_get_default_config(uint offset);
pio_sm_config rs485_txgap_program_get_default_config(uint offset);
pio_sm_config rs485_rx_program_get_default_config(uint offset);
pio_sm_config rs485_rxidle_program_get_default_config(uint offset);

// Declared, not defined: the test provides recording definitions so the values
// and the return codes the helpers hand back can be observed.
int rs485_tx_program_init(PIO pio, uint sm, uint offset,
                          uint pin_tx, uint pin_de,
                          uint clkdiv_int, uint8_t clkdiv_frac8,
                          uint char_bits);

void rs485_tx_set_char_bits(PIO pio, uint sm, uint char_bits);

int rs485_txgap_program_init(PIO pio, uint sm, uint offset,
                             uint clkdiv_int, uint8_t clkdiv_frac8,
                             uint gap_bits);

void rs485_txgap_set_gap_bits(PIO pio, uint sm, uint gap_bits);

int rs485_rx_program_init(PIO pio, uint sm, uint offset,
                          uint pin_rx, uint pin_rx_act,
                          uint clkdiv_int, uint8_t clkdiv_frac8,
                          uint isr_bits);

void rs485_rx_set_isr_bits(PIO pio, uint sm, uint isr_bits);

int rs485_rxidle_program_init(PIO pio, uint sm, uint offset,
                              uint pin_rx_act, uint clkdiv_int,
                              uint8_t clkdiv_frac8, uint steps);

void rs485_rxidle_set_steps(PIO pio, uint sm, uint steps);

#endif
