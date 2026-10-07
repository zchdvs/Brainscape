#pragma once
// The Daisy Seed bring-up platform layer (firmware/README.md): board and clock bring-up
// through libDaisy, the determinism profile's boot floating-point word, the DWT cycle
// counter, a buffered USB serial line protocol, a status LED, the engine's memory placement
// and the image identity every tool reads back.
#include <cstddef>
#include <cstdint>
#include <string>

// The test-only negative control (cmake/BrainscapeFpProfile.cmake) never builds firmware.
#if defined(BRAINSCAPE_FP_NEGATIVE_CONTROL)
#error "brainscape firmware: BRAINSCAPE_FP_NEGATIVE_CONTROL is test-only and cannot build firmware"
#endif
#if !defined(__arm__) || !defined(__ARM_FP)
#error "brainscape firmware: Cortex-M7 hard-float only"
#endif

namespace brainscape::fw {

// ---- Board ---------------------------------------------------------------------------

// Brings the Seed up: libDaisy's DaisySeed::Init at 480 MHz (clocks, MPU, caches, SDRAM,
// QSPI, the SAI and the codec reset of the detected board revision), then the boot FP word,
// the DWT cycle counter, USB serial on the Seed's own micro-USB port (OTG_FS) at an
// interrupt priority below the audio DMA's, and the LED driver. `image` names the image in
// every hello line.
void BoardInit(const char* image);

// The board revision libDaisy detected (DaisySeed::CheckBoardVersion: a ground strap on
// PD3, PD4, PD5 or PH6) and the codec it implies.
const char* BoardVersionName();
bool        IsRev7();

uint32_t SysClkHz();  // 480 MHz after BoardInit

// ---- Floating point (determinism-profile.md §4.1, §8.4 step 13) ----------------------

// FPSCR = 0 (round to nearest, FZ = DN = AHP = 0, flags clear) and FPDSCR = 0, so every
// interrupt handler's FP context, the audio callback's included, starts from the profile
// word too. Runs before any static constructor (a .preinit_array entry) and again in
// BoardInit; the engine's guard writes FPSCR on every entry regardless.
void     SetBootFpWord();
uint32_t ReadFpscr();
void     WriteFpscr(uint32_t word);
uint32_t ReadFpdscr();
uint32_t ReadFpccr();

// ---- DWT cycle counter ---------------------------------------------------------------

// CYCCNT, 32 bits at the core clock (wraps every 8.9 s at 480 MHz).
uint32_t Cycles();
// CYCCNT extended to 64 bits by the 1 kHz SysTick interrupt.
uint64_t Cycles64();

// ---- LED -------------------------------------------------------------------------------

// Driven at 1 kHz from SysTick by writing the LED's GPIO (PC7) directly, once BoardInit has
// armed it. A hardware fault has a pattern of its own, bit-banged without SysTick (below).
enum class LedMode : uint8_t {
  Idle,   // a short blink every second: alive, waiting for a command
  Busy,   // fast blink: rendering or measuring
  Done,   // on
  Fault,  // three quick flashes, a pause: Fatal() in thread mode (not SOS, the bootloader's)
  Manual, // the image drives it (SetLed)
  Pulse,  // on until the deadline PulseLed set, then off (the live image's onset light)
};
void SetLedMode(LedMode mode);
void SetLed(bool on);
// Lights the LED for `ms` milliseconds; safe from the audio interrupt. Never overrides the
// Fault pattern.
void PulseLed(uint32_t ms);

// Milliseconds since BoardInit (the SysTick count).
uint32_t UptimeMs();

// ---- Faults (platform/Fault.cpp) ----------------------------------------------------------

// Every fault vector (NMI, HardFault, MemManage, BusFault, UsageFault) and every unhandled
// interrupt goes to the platform's handler: it saves a record to the backup SRAM, blinks two
// long flashes and a pause for about 10 s (GPIO written directly, DWT busy-wait), then resets
// the chip. The next boot reports the record once, as the hello line's "lastFault".
enum class FaultKind : uint32_t { Exception = 1, FatalIsr = 2, FatalThread = 3 };
void              SaveFaultRecord(FaultKind kind, const char* message, uint32_t pc, uint32_t lr,
                                  uint32_t xpsr, uint32_t excReturn, uint32_t sp);
[[noreturn]] void FaultBlinkAndReset();
std::string       LastFaultJson();  // the previous boot's record as JSON, or null
// Platform internals: BoardInit and the preinit hook call these.
void     TakeLastFault();
void     PrepareFaultVectors();
void     UseFaultVectors(bool on);
uint32_t ImageVectorTable();  // where the image's own vector table is linked
const char* ImageName();

// The cache, MPU and QSPI state the measurements run under (SCB->CCR, every enabled MPU
// region, QUADSPI CR/DCR/CCR) and where the firmware's mem* functions run, as raw JSON.
std::string CpuStateJson();

// ---- Identity --------------------------------------------------------------------------

struct BuildInfo {
  const char* commit;             // git short hash of the source tree
  bool        dirty;              // uncommitted changes when built
  const char* appType;            // BOOT_NONE or BOOT_QSPI
  const char* libDaisy;           // tag and commit
  const char* toolchain;          // arm-none-eabi-gcc version
  const char* engineArchiveSha256;  // libbrainscape_dsp.a, as CI's parity-m7 job builds it
  const char* hooksArchiveSha256;   // libbrainscape_dsp_fpenv_hooks.a
};
const BuildInfo& Build();

// The hello object every image prints on "info": image, build, board, FP registers, memory.
// `extra` is raw JSON members appended to the object (without a leading comma), or empty.
std::string HelloJson(const char* image, const std::string& extra = "");

// ---- Errors and the bootloader ---------------------------------------------------------

// Saves `message` as the fault record. In thread mode: reports it as {"type":"error"} (best
// effort) and blinks the Fatal pattern forever, still serving USB. From an interrupt, where
// neither SysTick nor the USB interrupt can preempt: the hardware-fault path (no USB, the
// two-long-flash pattern, a reset after about 10 s).
[[noreturn]] void Fatal(const char* message);

// Reboots into the bootloader that flashes this app type, so a new image can be flashed
// without touching the buttons: the Daisy bootloader for BOOT_QSPI (waiting indefinitely),
// the STM32 system DFU bootloader for BOOT_NONE.
[[noreturn]] void RebootToBootloader();

}  // namespace brainscape::fw
