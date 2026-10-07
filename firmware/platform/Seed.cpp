#include <cstddef>
#include <cstring>

#include "JsonLine.h"
#include "brainscape/SoundRevision.h"
#include "platform/Placement.h"
#include "platform/Platform.h"
#include "platform/SeedHw.h"
#include "platform/UsbSerial.h"

using brainscape::golden::JsonHex32;
using brainscape::golden::JsonString;
using brainscape::golden::JsonUInt;

// This file is compiled with -fno-tree-loop-distribute-patterns (firmware/CMakeLists.txt):
// the preinit hook below runs before the copy to ITCM, so its loops must stay loops instead
// of becoming calls to memcpy, which an ITCM image keeps there (platform/MemFunctions.c).

namespace brainscape::fw {

namespace {

daisy::DaisySeed g_seed;
const char*      g_image = "unknown";

volatile uint32_t g_cycHigh = 0;  // CYCCNT wraps counted by SysTick
volatile uint32_t g_cycLast = 0;
volatile LedMode  g_ledMode = LedMode::Idle;
volatile uint32_t g_ms      = 0;
volatile uint32_t g_pulseUntil = 0;

// The SysTick hook runs from the image's first instruction: the Daisy bootloader hands over
// with SysTick running (its HAL_Init started it), before the startup code has copied .data
// and zeroed .bss, so every variable here may still hold whatever the previous image left at
// its address. The hook does nothing until BoardInit has stored this 32-bit value, and it
// drives the LED by writing its GPIO directly rather than through the DaisySeed object.
constexpr uint32_t kArmed   = 0x4C454421u;  // "LED!"
volatile uint32_t  g_armed  = 0;
constexpr uint32_t kLedPin  = 7;  // PC7, the Seed's user LED (libDaisy src/daisy_seed.cpp)

void ExtendCycles() {
  const uint32_t now = DWT->CYCCNT;
  if (now < g_cycLast) g_cycHigh = g_cycHigh + 1u;
  g_cycLast = now;
}

void LedWrite(bool on) {
  if (g_armed == kArmed) GPIOC->BSRR = on ? (1u << kLedPin) : (1u << (kLedPin + 16u));
}

// The LED pattern for the current mode, at 1 kHz from SysTick.
void LedTick(uint32_t ms) {
  switch (g_ledMode) {
    case LedMode::Idle: LedWrite(ms % 1000u < 60u); break;
    case LedMode::Busy: LedWrite(ms % 200u < 100u); break;
    case LedMode::Done: LedWrite(true); break;
    case LedMode::Fault:
      // Three quick flashes, then a pause: the Daisy bootloader's error signal is SOS, and a
      // hardware fault blinks two long flashes (platform/Fault.cpp).
      LedWrite((ms % 1500u) < 750u && (ms % 250u) < 80u);
      break;
    case LedMode::Manual: break;
    case LedMode::Pulse: LedWrite(static_cast<int32_t>(g_pulseUntil - ms) > 0); break;
  }
}

daisy::System::MemoryRegion ImageRegion() { return daisy::System::GetMemoryRegion(ImageVectorTable()); }

const char* BootRegionName() {
  switch (ImageRegion()) {
    case daisy::System::MemoryRegion::INTERNAL_FLASH: return "internal flash";
    case daisy::System::MemoryRegion::ITCMRAM: return "ITCM";
    case daisy::System::MemoryRegion::DTCMRAM: return "DTCM";
    case daisy::System::MemoryRegion::SRAM_D1: return "AXI SRAM";
    case daisy::System::MemoryRegion::SRAM_D2: return "D2 SRAM";
    case daisy::System::MemoryRegion::SRAM_D3: return "D3 SRAM";
    case daisy::System::MemoryRegion::SDRAM: return "SDRAM";
    case daisy::System::MemoryRegion::QSPI: return "QSPI flash";
    default: return "unknown";
  }
}

const char* BootloaderName() {
  // libDaisy reads the bootloader's handshake unless the program runs from internal flash,
  // which it decides from SCB->VTOR; the vector table lives in DTCM here (Fault.cpp).
  if (ImageRegion() == daisy::System::MemoryRegion::INTERNAL_FLASH) return "none";
  switch (daisy::System::GetBootloaderVersion()) {
    case daisy::System::BootInfo::Version::NONE: return "none";
    case daisy::System::BootInfo::Version::LT_v6_0: return "Daisy bootloader < v6.0";
    case daisy::System::BootInfo::Version::v6_0: return "Daisy bootloader v6.0";
    case daisy::System::BootInfo::Version::v6_1: return "Daisy bootloader >= v6.1";
    default: return "unknown";
  }
}

daisy::DaisySeed::BoardVersion g_board = daisy::DaisySeed::BoardVersion::DAISY_SEED;

// The audio path libDaisy configured for this board (SAI1 and the codec), read back from it.
std::string AudioJson() {
  const daisy::SaiHandle::Config& sai = g_seed.AudioSaiHandle().GetConfig();
  const uint32_t bits = sai.bit_depth == daisy::SaiHandle::Config::BitDepth::SAI_16BIT   ? 16u
                        : sai.bit_depth == daisy::SaiHandle::Config::BitDepth::SAI_24BIT ? 24u
                                                                                         : 32u;
  return "{\"sampleRate\":" + JsonUInt(static_cast<uint32_t>(g_seed.AudioSampleRate())) +
         ",\"saiRate48k\":" + (sai.sr == daisy::SaiHandle::Config::SampleRate::SAI_48KHZ ? "true" : "false") +
         ",\"blockSize\":" + JsonUInt(g_seed.AudioBlockSize()) + ",\"bitDepth\":" + JsonUInt(bits) +
         ",\"codec\":" +
         JsonString(g_board == daisy::DaisySeed::BoardVersion::DAISY_SEED_1_1 ? "WM8731 (I2C)"
                    : g_board == daisy::DaisySeed::BoardVersion::DAISY_SEED_2_DFM
                        ? "PCM3060 (hardware mode, de-emphasis off)"
                        : g_board == daisy::DaisySeed::BoardVersion::DAISY_SEED_1_2
                              ? "PCM3060 (hardware mode, reset on PB11)"
                              : "AK4556-compatible path (reset on PB11)") +
         "}";
}

extern "C" {
// firmware/linker/seed_h750.ld.in: the engine's code and constants (and the firmware's mem*
// functions), when an image places them in ITCM (profile §7.1).
extern uint32_t __itcm_text_start, __itcm_text_end, __itcm_text_load;
extern char     __brainscape_image_end;
void BrainscapeHeapReady(void);  // Syscalls.c
}

// Runs from .preinit_array, before main and before any static constructor (the startup code
// calls __libc_init_array, which runs .preinit_array first): SysTick off (the bootloader's;
// HAL_Init restarts it), FPSCR = 0 and FPDSCR = 0, the engine's code into ITCM, and the
// platform's fault vectors. CP10/CP11 access is granted here too, in case the code that ran
// before (SystemInit, or the Daisy bootloader for a bootloaded app) did not.
extern "C" void BrainscapePreinit() {
  SysTick->CTRL = 0u;
  SCB->CPACR |= (3UL << 20) | (3UL << 22);
  __DSB();
  __ISB();
  SetBootFpWord();
  // Before any constructor, so no engine code can run from ITCM before it is there. Words:
  // the section starts 64 bytes into ITCM and ends 8-aligned (linker script).
  const uint32_t* src = &__itcm_text_load;
  for (uint32_t* dst = &__itcm_text_start; dst != &__itcm_text_end; ++dst, ++src) *dst = *src;
  __DSB();
  __ISB();
  PrepareFaultVectors();
  UseFaultVectors(true);
}
__attribute__((section(".preinit_array"), used)) void (*const g_preinit)() = BrainscapePreinit;

}  // namespace

void SetBootFpWord() {
  __set_FPSCR(0u);
  FPU->FPDSCR = 0u;
  __DSB();
  __ISB();
}

uint32_t ReadFpscr() { return __get_FPSCR(); }
void     WriteFpscr(uint32_t word) { __set_FPSCR(word); }
uint32_t ReadFpdscr() { return FPU->FPDSCR; }
uint32_t ReadFpccr() { return FPU->FPCCR; }

uint32_t Cycles() { return DWT->CYCCNT; }

uint64_t Cycles64() {
  const uint32_t primask = __get_PRIMASK();
  __disable_irq();
  ExtendCycles();
  const uint64_t v = (static_cast<uint64_t>(g_cycHigh) << 32) | g_cycLast;
  if (primask == 0u) __enable_irq();
  return v;
}

uint32_t SysClkHz() { return daisy::System::GetSysClkFreq(); }

uint32_t UptimeMs() { return g_ms; }

// The fault handler copies it, possibly before the startup code has initialized g_image: only
// a pointer into this image's own flash is followed.
const char* ImageName() {
  const auto p = reinterpret_cast<uintptr_t>(g_image);
  return p >= ImageVectorTable() && p < reinterpret_cast<uintptr_t>(&__brainscape_image_end) ? g_image : "unknown";
}

void SetLedMode(LedMode mode) { g_ledMode = mode; }
void SetLed(bool on) {
  if (g_ledMode == LedMode::Fault) return;
  g_ledMode = LedMode::Manual;
  LedWrite(on);
}

void PulseLed(uint32_t ms) {
  if (g_ledMode == LedMode::Fault) return;
  g_pulseUntil = g_ms + ms;
  g_ledMode    = LedMode::Pulse;
}

const char* BoardVersionName() {
  switch (g_board) {
    case daisy::DaisySeed::BoardVersion::DAISY_SEED: return "Daisy Seed rev4 (AK4556; no strap)";
    case daisy::DaisySeed::BoardVersion::DAISY_SEED_1_1: return "Daisy Seed 1.1 / rev5 (WM8731; PD3 strap)";
    case daisy::DaisySeed::BoardVersion::DAISY_SEED_2_DFM: return "Daisy Seed2 DFM (PCM3060; PD4 strap)";
    case daisy::DaisySeed::BoardVersion::DAISY_SEED_1_2: return "Daisy Seed 1.2 / rev7 (PCM3060; PD5 strap)";
    case daisy::DaisySeed::BoardVersion::DAISY_SEED_3: return "Daisy Seed3 (PH6 strap)";
    default: return "unknown";
  }
}

bool IsRev7() { return g_board == daisy::DaisySeed::BoardVersion::DAISY_SEED_1_2; }

daisy::DaisySeed& Seed() { return g_seed; }

void BoardInit(const char* image) {
  g_image = image;
  // 480 MHz ("boost"), caches on, SDRAM, QSPI (memory-mapped unless the app runs from it),
  // LED, and the audio path of the detected revision: on a Rev7 the PD5 strap selects
  // DAISY_SEED_1_2, whose PCM3060 runs in hardware mode on SAI1 (A: transmit on PE6, B:
  // receive on PE3, MCLK PE2, FS PE4, SCK PE5; reset on PB11), 24-bit at 48 kHz, 48-frame
  // blocks, postgain 1 (libDaisy src/daisy_seed.cpp, ConfigureAudio). libDaisy reads
  // SCB->VTOR to tell where the program runs, so the image's own vector table is installed
  // for the call.
  UseFaultVectors(false);
  g_seed.Init(true);
  UseFaultVectors(true);
  BrainscapeHeapReady();  // the heap lives in the SDRAM Init just brought up
  g_board = g_seed.CheckBoardVersion();
  TakeLastFault();
  SetBootFpWord();

  // DWT cycle counter. The Cortex-M7's DWT needs its lock access register unlocked when no
  // debugger has done it.
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->LAR = 0xC5ACCE55u;
  DWT->CYCCNT = 0u;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
  g_cycLast = 0u;
  g_cycHigh = 0u;
  g_ms      = 0u;
  g_ledMode = LedMode::Idle;
  __DSB();
  g_armed = kArmed;  // the LED pin is an output now (DaisySeed::Init)

  Serial().Init();
}

std::string CpuStateJson() {
  const uint32_t ccr = SCB->CCR;
  std::string    mpu = "[";
  const uint32_t regions = (MPU->TYPE & MPU_TYPE_DREGION_Msk) >> MPU_TYPE_DREGION_Pos;
  const uint32_t primask = __get_PRIMASK();
  __disable_irq();
  const uint32_t savedRnr = MPU->RNR;
  bool           first    = true;
  for (uint32_t r = 0; r < regions; ++r) {
    MPU->RNR            = r;
    const uint32_t rbar = MPU->RBAR;
    const uint32_t rasr = MPU->RASR;
    if ((rasr & MPU_RASR_ENABLE_Msk) == 0u) continue;
    mpu += std::string(first ? "" : ",") + "{\"region\":" + JsonUInt(r) + ",\"rbar\":" + JsonHex32(rbar) +
           ",\"rasr\":" + JsonHex32(rasr) + "}";
    first = false;
  }
  MPU->RNR = savedRnr;
  if (primask == 0u) __enable_irq();
  mpu += "]";
  const uint32_t memsetAt = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&::memset));
  return "{\"ccr\":" + JsonHex32(ccr) + ",\"icache\":" + ((ccr & SCB_CCR_IC_Msk) ? "true" : "false") +
         ",\"dcache\":" + ((ccr & SCB_CCR_DC_Msk) ? "true" : "false") + ",\"mpuCtrl\":" + JsonHex32(MPU->CTRL) +
         ",\"mpu\":" + mpu + ",\"qspi\":{\"cr\":" + JsonHex32(QUADSPI->CR) + ",\"dcr\":" + JsonHex32(QUADSPI->DCR) +
         ",\"ccr\":" + JsonHex32(QUADSPI->CCR) + "}" +
         ",\"memFunctions\":{\"source\":\"firmware/platform/MemFunctions.c (word-wise)\",\"memset\":" +
         JsonHex32(memsetAt) + ",\"in\":" + JsonString(memsetAt < 0x10000u ? "ITCM" : "QSPI") + "}}";
}

std::string HelloJson(const char* image, const std::string& extra) {
  const BuildInfo& b = Build();
  std::string      s = "{\"type\":\"hello\",\"image\":" + JsonString(image);
  s += ",\"format\":\"brainscape-hil/1\"";
  s += ",\"soundRevision\":" + JsonUInt(kSoundRevision);
  const ToolchainId& id = BuildToolchain();
  s += ",\"build\":{\"commit\":" + JsonString(b.commit) + ",\"dirty\":" + (b.dirty ? "true" : "false") +
       ",\"appType\":" + JsonString(b.appType) + ",\"libDaisy\":" + JsonString(b.libDaisy) +
       ",\"toolchain\":" + JsonString(b.toolchain) +
       ",\"engineArchiveSha256\":" + JsonString(b.engineArchiveSha256) +
       ",\"hooksArchiveSha256\":" + JsonString(b.hooksArchiveSha256) +
       ",\"engineToolchain\":{\"compiler\":" + JsonString(id.compiler) +
       ",\"version\":" + JsonString(id.version) + ",\"target\":" + JsonString(id.target) +
       ",\"fpFlags\":" + JsonString(id.fpFlags) + ",\"fpFlagsHash\":" + JsonString(id.fpFlagsHash) +
       "}}";
  s += ",\"board\":{\"version\":" + JsonString(BoardVersionName()) +
       ",\"rev7\":" + (IsRev7() ? "true" : "false") + ",\"sysclkHz\":" + JsonUInt(SysClkHz()) +
       ",\"hclkHz\":" + JsonUInt(daisy::System::GetHClkFreq()) +
       ",\"bootRegion\":" + JsonString(BootRegionName()) +
       ",\"bootloader\":" + JsonString(BootloaderName()) +
       ",\"audio\":" + AudioJson() +
       ",\"cpuid\":" + JsonHex32(SCB->CPUID) + ",\"idcode\":" + JsonHex32(DBGMCU->IDCODE) + "}";
  s += ",\"fp\":{\"fpscr\":" + JsonHex32(ReadFpscr()) + ",\"fpdscr\":" + JsonHex32(ReadFpdscr()) +
       ",\"fpccr\":" + JsonHex32(ReadFpccr()) + "}";
  s += ",\"cpu\":" + CpuStateJson();
  s += ",\"lastFault\":" + LastFaultJson();
  s += ",\"uptimeMs\":" + JsonUInt(UptimeMs());
  s += ",\"memory\":" + MemoryMapJson();
  if (!extra.empty()) s += "," + extra;
  s += "}";
  return s;
}

void Fatal(const char* message) {
  if (__get_IPSR() != 0u) {
    // An interrupt handler (the audio callback, USB): SysTick and the USB interrupt cannot
    // preempt it, so nothing could be sent and no SysTick pattern would run.
    __disable_irq();
    SaveFaultRecord(FaultKind::FatalIsr, message, 0u, 0u, 0u, 0u, __get_MSP());
    FaultBlinkAndReset();
  }
  SaveFaultRecord(FaultKind::FatalThread, message, 0u, 0u, 0u, 0u, __get_MSP());
  g_ledMode = LedMode::Fault;
  Serial().WriteLine("{\"type\":\"error\",\"image\":" + JsonString(g_image) +
                         ",\"message\":" + JsonString(message) + "}",
                     UsbSerial::Mode::Block);
  Serial().Flush(500);
  for (;;) Serial().Pump();
}

void RebootToBootloader() {
  Serial().WriteLine("{\"type\":\"rebooting\",\"to\":\"bootloader\"}", UsbSerial::Mode::Block);
  Serial().Flush(200);
  daisy::System::Delay(50);
  if (ImageRegion() == daisy::System::MemoryRegion::INTERNAL_FLASH) {
    daisy::System::ResetToBootloader(daisy::System::BootloaderMode::STM);
  } else {
    daisy::System::ResetToBootloader(daisy::System::BootloaderMode::DAISY_INFINITE_TIMEOUT);
  }
  for (;;) {
  }
}

}  // namespace brainscape::fw

// libDaisy's SysTick_Handler calls HAL_IncTick, then HAL_SYSTICK_IRQHandler, which calls
// this weak HAL hook at 1 kHz: it extends CYCCNT to 64 bits and drives the LED, once
// BoardInit has armed it (see g_armed).
extern "C" void HAL_SYSTICK_Callback(void) {
  using namespace brainscape::fw;
  if (g_armed != kArmed) return;
  ExtendCycles();
  g_ms = g_ms + 1u;
  LedTick(g_ms);
}

// abort(), an allocation failure under -fno-exceptions and exit() end here (Syscalls.c).
extern "C" void BrainscapeFatalExit(int code) {
  brainscape::fw::Fatal(code == 0 ? "exit called" : "abort called (allocation failure or assert)");
}
