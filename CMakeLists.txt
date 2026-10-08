cmake_minimum_required(VERSION 3.13)
project(PicoRS485 VERSION 0.1.0 LANGUAGES C CXX ASM)

# The SDK is normally found through PICO_SDK_PATH in the environment (the VS Code
# extension and the SDK's own setup script both set it); -DPICO_SDK_PATH=... works
# too for a one-off configure.
if (NOT DEFINED PICO_SDK_PATH AND DEFINED ENV{PICO_SDK_PATH})
    set(PICO_SDK_PATH $ENV{PICO_SDK_PATH})
endif()
if (NOT DEFINED PICO_SDK_PATH)
    message(FATAL_ERROR "Set PICO_SDK_PATH, or -DPICO_SDK_PATH=<path to pico-sdk>")
endif()
include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(PicoRS485 C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

pico_sdk_init()

# The driver. rs485.pio becomes rs485.pio.h in this directory's build tree, which
# is what src/PicoRS485.cpp includes; pioasm runs as part of building the target.
#
# PICO_RS485_MAX_PACKET_BYTES sizes members of the class, so it is a build-wide setting:
# it is set here once and propagated PUBLIC. The class is a template on the value, so a
# translation unit that overrides it instantiates a different type and fails to link,
# naming the value it asked for. It also sets what a channel costs in RAM: the receive
# staging buffers and the transmit scratch scale with it.
set(PICO_RS485_MAX_PACKET_BYTES 256 CACHE STRING "Largest burst the driver handles, in bytes (rebuilds the layout)")
# Where the end-of-burst handler's code lives. In flash it is fetched through XIP while the bus is
# live - the handler is 928 bytes and its callees stay in flash either way - and in RAM it costs that
# much RAM and skips that fetch. Which is better depends on the traffic: a long period leaves the CPU
# idle enough to hide the fetch, a short one does not. So it is a switch, off by default. To price it,
# build the stress firmware both ways and compare `r10 period=0`, where the exchange rate is CPU-bound
# rather than paced.
option(PICO_RS485_ISR_IN_RAM "Place the end-of-burst interrupt handler in RAM" OFF)
# Extra maximum burst lengths to export, beyond PICO_RS485_MAX_PACKET_BYTES. The class is a template
# on that length, so a second one is a second instantiation and the library has to export it before
# a consumer can name PicoRS485_t<that size>. Each costs a second copy of the driver's code as weak
# symbols, which the linker discards unless something calls them.
set(PICO_RS485_BURST_SIZES "" CACHE STRING
    "Extra maximum burst lengths the library exports, semicolon separated, e.g. 32;64")

add_library(PicoRS485 STATIC src/PicoRS485.cpp)
target_include_directories(PicoRS485 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_definitions(PicoRS485 PUBLIC PICO_RS485_MAX_PACKET_BYTES=${PICO_RS485_MAX_PACKET_BYTES})

# The instantiations the library exports: the configured length, then any extras, one per line.
set(_rs485_sizes ${PICO_RS485_MAX_PACKET_BYTES} ${PICO_RS485_BURST_SIZES})
list(REMOVE_DUPLICATES _rs485_sizes)
set(_rs485_instantiate "")
foreach (_size IN LISTS _rs485_sizes)
    string(APPEND _rs485_instantiate "template class PicoRS485_t<${_size}>;\n")
endforeach ()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/pico_rs485_instantiations.h"
     "// Written by CMake from PICO_RS485_MAX_PACKET_BYTES and PICO_RS485_BURST_SIZES.\n${_rs485_instantiate}")
# src/PicoRS485.cpp includes that header, and PRIVATE keeps the build directory off a consumer's
# include path.
target_include_directories(PicoRS485 PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
if (PICO_RS485_ISR_IN_RAM)
    target_compile_definitions(PicoRS485 PUBLIC PICO_RS485_ISR_IN_RAM=1)
endif()
pico_generate_pio_header(PicoRS485 ${CMAKE_CURRENT_SOURCE_DIR}/src/rs485.pio)
target_compile_options(PicoRS485 PRIVATE -Wall -Wextra)
target_link_libraries(PicoRS485 PUBLIC
    pico_stdlib
    hardware_pio
    hardware_dma
    hardware_irq
    hardware_clocks
    hardware_gpio
    )

# The two-board hardware tests. Their firmware is host-independent, so they are only
# built when a board target is configured; the host suite lives in test/ and is run
# by test/run_tests.ps1.
option(PICO_RS485_HW_TESTS "Build the two-board firmware under test/hw" ON)
if (PICO_RS485_HW_TESTS)
    add_subdirectory(test/hw)
endif()

# Deliberately no pico_add_extra_outputs(PicoRS485) here: it produces hex/bin/uf2,
# which need a linked executable, and this target is a static library. Each firmware
# under test/hw/ asks for its own outputs.
