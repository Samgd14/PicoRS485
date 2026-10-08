# Two boards, one firmware each. Build the same main.cpp twice with different
# roles, flash one board with each, and read both boards' USB serial output.
#
# Wiring between the boards: A/B in parallel, termination enabled, and the two
# transceivers' DE and /RE tied together on the same pin as the driver expects.
# Per board: RX = GP5, TX = GP8, DE = GP9, RX_ACT = GP17 (local, unconnected).

# Bit rate the hardware test firmware runs at. A cache variable rather than
# CMAKE_CXX_FLAGS: overriding that replaces the toolchain's own -mcpu/-mthumb flags
# and the build fails on PRIMASK instructions it then compiles for plain ARM.
set(PICO_RS485_BAUD 115200 CACHE STRING "Bit rate for the hardware test firmware")

function(rs485_hw_firmware name role)
    add_executable(${name} main.cpp)
    target_link_libraries(${name} PicoRS485 pico_stdlib)
    target_compile_definitions(${name} PRIVATE ${role}=1 RS485_BAUD=${PICO_RS485_BAUD})
    # USB serial only: the boards talk to each other over RS-485, so UART0 is free
    # and its pins are not wanted.
    pico_enable_stdio_usb(${name} 1)
    pico_enable_stdio_uart(${name} 0)
    pico_add_extra_outputs(${name})
endfunction()

rs485_hw_firmware(rs485_hw_master RS485_ROLE_MASTER)
rs485_hw_firmware(rs485_hw_slave  RS485_ROLE_SLAVE)

# The UART bisect: the same pins and the same transceivers driven by the hardware
# UART with no library code at all, to tell a library fault from a bench fault.
# It links only pico_stdlib on purpose. stdio stays on USB so UART1 is free for the
# test, and RX=GP5 / TX=GP8 are UART1's pins on RP2040.
option(PICO_RS485_UART_BISECT "Build the UART bisect firmware under test/hw" ON)
if (PICO_RS485_UART_BISECT)
    function(rs485_uart_bisect name role)
        add_executable(${name} uart_pingpong.cpp)
        target_link_libraries(${name} pico_stdlib)
        target_compile_definitions(${name} PRIVATE ${role}=1)
        pico_enable_stdio_usb(${name} 1)
        pico_enable_stdio_uart(${name} 0)
        pico_add_extra_outputs(${name})
    endfunction()

    rs485_uart_bisect(rs485_uart_master RS485_ROLE_MASTER)
    rs485_uart_bisect(rs485_uart_slave  RS485_ROLE_SLAVE)
endif()

# The pin probe: no UART, no library. It watches GP5 as a plain input, with no pull
# and then with each internal pull, to find out whether anything is actually driving
# it. One firmware, flashed on both boards; the readings are per board.
add_executable(rs485_pin_probe pin_probe.cpp)
target_link_libraries(rs485_pin_probe pico_stdlib)
pico_enable_stdio_usb(rs485_pin_probe 1)
pico_enable_stdio_uart(rs485_pin_probe 0)
pico_add_extra_outputs(rs485_pin_probe)

# The stress bench: the master sends a 1-byte request and the slave answers with 16
# bytes, once per period, forever. Its own baud cache variable rather than
# PICO_RS485_BAUD so it can sit at the PIO divider's floor without dragging the echo
# bench up with it: 15625000 is exactly 1.0 at the default 125 MHz clk_sys, eight
# cycles per bit. The period is a cache variable too, 0 meaning "as fast as a round
# trip allows", so the same firmware can be rebuilt to pace differently.
set(PICO_RS485_STRESS_BAUD 15625000 CACHE STRING "Bit rate for the stress firmware")
set(PICO_RS485_STRESS_PERIOD_US 83 CACHE STRING "Stress master period in us (0 = back to back)")
set(PICO_RS485_STRESS_REQ_BYTES 1 CACHE STRING "Stress master request size in bytes")
set(PICO_RS485_STRESS_REPLY_BYTES 16 CACHE STRING "Stress slave reply size in bytes")
set(PICO_RS485_STRESS_GUARD_US 0 CACHE STRING "Guard period between any two bursts, either side, in us")
set(PICO_RS485_STRESS_GAP_BITS 1 CACHE STRING "Inter-character gap the transmitter leaves between characters, in bit times")
set(PICO_RS485_STRESS_PACKET 0 CACHE STRING "Stress framing: 0 = 8N1, 1 = 8E1, 2 = 8N2")
set(PICO_RS485_STRESS_PAIRS 1 CACHE STRING "Exchanges sent back to back inside one period")
# System clock for the stress firmware. 0 leaves the SDK's own clock alone; 200000 runs
# RP2040 at 200 MHz, where PICO_RS485_STRESS_BAUD of 25000000 derives a PIO divider of
# exactly 1.0 and a 40 ns bit time.
set(PICO_RS485_STRESS_SYS_CLK_KHZ 0 CACHE STRING "Stress firmware system clock in kHz (0 = SDK default)")

function(rs485_stress_firmware name role)
    add_executable(${name} stress.cpp)
    target_link_libraries(${name} PicoRS485 pico_stdlib)
    target_compile_definitions(${name} PRIVATE ${role}=1
                               RS485_BAUD=${PICO_RS485_STRESS_BAUD}
                               RS485_PERIOD_US=${PICO_RS485_STRESS_PERIOD_US}
                               RS485_REQ_BYTES=${PICO_RS485_STRESS_REQ_BYTES}
                               RS485_REPLY_BYTES=${PICO_RS485_STRESS_REPLY_BYTES}
                               RS485_GUARD_US=${PICO_RS485_STRESS_GUARD_US}
                               RS485_GAP_BITS=${PICO_RS485_STRESS_GAP_BITS}
                               RS485_PACKET_PROFILE=${PICO_RS485_STRESS_PACKET}
                               RS485_PAIRS=${PICO_RS485_STRESS_PAIRS}
                               RS485_SYS_CLK_KHZ=${PICO_RS485_STRESS_SYS_CLK_KHZ})
    pico_enable_stdio_usb(${name} 1)
    # UART0 as well, so the run can be driven from a plain serial adapter on GP0/GP1
    # (TX/RX) while the USB console stays shut - which is the whole point of the
    # command protocol: nothing on either console while the run is in progress.
    pico_enable_stdio_uart(${name} 1)
    pico_add_extra_outputs(${name})
endfunction()

rs485_stress_firmware(rs485_stress_master RS485_ROLE_MASTER)
rs485_stress_firmware(rs485_stress_slave  RS485_ROLE_SLAVE)

# The baud-margin firmware: the slave with its bit rate deliberately off, to see
# whether the receiver's stop-bit margin holds. Build with
#   cmake -DPICO_RS485_MARGIN_PPM=20000 ...        (2% fast)
# and flash that slave instead of the nominal one.
set(PICO_RS485_MARGIN_PPM 0 CACHE STRING "Slave baud error in parts per million")
if (NOT PICO_RS485_MARGIN_PPM EQUAL 0)
    add_executable(rs485_hw_slave_offset main.cpp)
    target_link_libraries(rs485_hw_slave_offset PicoRS485 pico_stdlib)
    target_compile_definitions(rs485_hw_slave_offset PRIVATE RS485_ROLE_SLAVE=1
                               RS485_MARGIN_PPM=${PICO_RS485_MARGIN_PPM})
    pico_enable_stdio_usb(rs485_hw_slave_offset 1)
    pico_enable_stdio_uart(rs485_hw_slave_offset 0)
    pico_add_extra_outputs(rs485_hw_slave_offset)
endif()
