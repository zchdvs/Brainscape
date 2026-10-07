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

namespace brainscape::fw {

namespace {

daisy::DaisySeed g_seed;
const char*      g_image = "unknown";

volatile uint32_t g_cycHigh = 0;  // CYCCNT wraps counted by SysTick
volatile uint32_t g_cycLast = 0;
volatile LedMode  g_ledMode = LedMode::Idle;
volatile uint32_t g_ms      = 0;
volatile uint32_t g_pulseUntil = 0;
bool              g_ledReady = false;

void ExtendCycles() {
  const uint32_t now = DWT->CYCCNT;
  if (now < g_cycLast) g_cycHigh = g_cycHigh + 1u;
  g_cycLast = now;
}

void LedWrite(bool on) {
  if (g_ledReady) g_seed.SetLed(on);
}

// The LED pattern for the current mode, at 1 kHz from SysTick.
void LedTick(uint32_t ms) {
  switch (g_ledMode) {
    case LedMode::Idle: LedWrite(ms % 1000u < 60u); break;
    case LedMode::Busy: LedWrite(ms % 200u < 100u); break;
    case LedMode::Done: LedWrite(true); break;
    case LedMode::Fault:
      // Three quick flashes, then a pause: the Daisy bootloader's error signal is SOS, so
      // the images use another one.
      LedWrite((ms % 1500u) < 750u && (ms % 250u) < 80u);
      break;
    case LedMode::Manual: break;
    case LedMode::Pulse: LedWrite(static_cast<int32_t>(g_pulseUntil - ms) > 0); break;
  }
}

const char* BootRegionName() {
  switch (daisy::System::GetProgramMemoryRegion()) {
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

// FPSCR = 0 and FPDSCR = 0 before main and before any static constructor: the startup code
// calls __libc_init_array, which runs .preinit_array first. CP10/CP11 access is granted
// here too, in case the code that ran before (SystemInit, or the Daisy bootloader for a
// bootloaded app) did not.
extern "C" {
// firmware/linker/seed_h750.ld.in: the engine's code and constants, when an image places
// them in ITCM (profile §7.1).
extern char __itcm_text_start, __itcm_text_end, __itcm_text_load;
void BrainscapeHeapReady(void);  // Syscalls.c
}

extern "C" void BrainscapePreinitFp() {
  SCB->CPACR |= (3UL << 20) | (3UL << 22);
  __DSB();
  __ISB();
  SetBootFpWord();
  // Before any constructor, so no engine code can run from ITCM before it is there.
  const size_t itcm = static_cast<size_t>(&__itcm_text_end - &__itcm_text_start);
  if (itcm != 0) {
    std::memcpy(&__itcm_text_start, &__itcm_text_load, itcm);
    __DSB();
    __ISB();
  }
}
__attribute__((section(".preinit_array"), used)) void (*const g_preinitFp)() = BrainscapePreinitFp;

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

void SetLedMode(LedMode mode) { g_ledMode = mode; }
void SetLed(bool on) {
  g_ledMode = LedMode::Manual;
  LedWrite(on);
}

void PulseLed(uint32_t ms) {
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
  // blocks, postgain 1 (libDaisy src/daisy_seed.cpp, ConfigureAudio).
  g_seed.Init(true);
  BrainscapeHeapReady();  // the heap lives in the SDRAM Init just brought up
  g_board = g_seed.CheckBoardVersion();
  g_ledReady = true;
  SetBootFpWord();

  // DWT cycle counter. The Cortex-M7's DWT needs its lock access register unlocked when no
  // debugger has done it.
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->LAR = 0xC5ACCE55u;
  DWT->CYCCNT = 0u;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
  g_cycLast = 0u;
  g_cycHigh = 0u;

  Serial().Init();
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
  s += ",\"memory\":" + MemoryMapJson();
  if (!extra.empty()) s += "," + extra;
  s += "}";
  return s;
}

void Fatal(const char* message) {
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
  if (daisy::System::GetProgramMemoryRegion() == daisy::System::MemoryRegion::INTERNAL_FLASH) {
    daisy::System::ResetToBootloader(daisy::System::BootloaderMode::STM);
  } else {
    daisy::System::ResetToBootloader(daisy::System::BootloaderMode::DAISY_INFINITE_TIMEOUT);
  }
  for (;;) {
  }
}

}  // namespace brainscape::fw

// libDaisy's SysTick_Handler calls HAL_IncTick, then HAL_SYSTICK_IRQHandler, which calls
// this weak HAL hook at 1 kHz: it extends CYCCNT to 64 bits and drives the LED.
extern "C" void HAL_SYSTICK_Callback(void) {
  using namespace brainscape::fw;
  ExtendCycles();
  g_ms = g_ms + 1u;
  LedTick(g_ms);
}

// abort(), an allocation failure under -fno-exceptions and exit() end here (Syscalls.c).
extern "C" void BrainscapeFatalExit(int code) {
  brainscape::fw::Fatal(code == 0 ? "exit called" : "abort called (allocation failure or assert)");
}
