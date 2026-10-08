// The instantiations the library exports when the build does not write its own list.
//
// The class is a template on the maximum burst length, so a consumer that names
// PicoRS485_t<size> needs the library to have compiled the out-of-line members for that
// size. CMake writes a build-directory copy of this header from PICO_RS485_MAX_PACKET_BYTES
// and PICO_RS485_BURST_SIZES; src/PicoRS485.cpp prefers that one and falls back to this.
//
// One line per exported size. A second one costs a second copy of the driver's code as weak
// symbols, which the linker discards unless something calls them.
template class PicoRS485_t<PICO_RS485_MAX_PACKET_BYTES>;

// Builds without CMake name a second size with this, which is how the host suite exercises
// two instantiations of one class.
#ifdef PICO_RS485_EXTRA_SIZE
template class PicoRS485_t<PICO_RS485_EXTRA_SIZE>;
#endif
