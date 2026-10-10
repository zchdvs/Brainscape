#pragma once

// Code placement on the pedal (docs/design/clock.md §9.6, firmware/README.md "Sizes"). The ITCM
// images place the engine archive's code by object (firmware/CMakeLists.txt) and, inside the
// objects placed in ITCM, by function: a function marked BRAINSCAPE_COLD is main-thread-only
// (construction, Init, Exact loads and Restart, the accessors the console reads), and goes to the
// section .text_cold, which the ITCM input patterns (`.text`, `.text.*`) do not match and the
// QSPI `.text` output section (`*(.text*)`) does. A cold function runs from QSPI behind the
// I-cache; a call into it from code in ITCM needs a veneer, which ItcmCheck refuses outside its
// allow-list, so a hot path cannot come to call one unnoticed.
//
// Only on bare-metal Arm (the firmware and the emulated Cortex-M7 oracle, whose engine archives
// must stay byte-identical); everywhere else the mark is empty and changes nothing. Placement
// never changes what the code computes.
#if defined(__arm__) && defined(__ELF__) && !defined(__linux__) && !defined(__APPLE__)
#define BRAINSCAPE_COLD __attribute__((section(".text_cold")))
#else
#define BRAINSCAPE_COLD
#endif
