// Fault handling for the bring-up images (firmware/README.md, "LED patterns" and "lastFault").
//
// libDaisy leaves every fault invisible on a board without a debugger: its HardFault_Handler
// (src/sys/system.cpp, a strong symbol) ends in BKPT, which locks the core up, and MemManage,
// BusFault, UsageFault, NMI and every unhandled interrupt go to core/startup_stm32h750xx.c's
// Default_Handler, an endless loop at the exception's priority that also stops SysTick. Both
// freeze the LED in whatever state it was in, indistinguishable from a hang or a long render.
//
// So the platform copies the image's vector table to DTCM and points the fault vectors, and
// every vector that was Default_Handler, at its own handler, which runs on a stack of its
// own, saves a fault record to the backup SRAM (kept across a reset), blinks the LED in a
// pattern of its own by writing the GPIO directly with a DWT busy-wait (no SysTick, no HAL
// tick, no USB), and resets the chip after about 10 s. The next boot reports the record as
// "lastFault" in its hello line. Fatal() called from an interrupt takes the same path.
//
// The relocated table is installed by the preinit hook (before any constructor) and again
// after DaisySeed::Init; during DaisySeed::Init the image's own table is the vector table,
// because libDaisy decides from SCB->VTOR where the program runs (System::
// GetProgramMemoryRegion: QSPI memory-mapped or not, the bootloader handshake).
#include <cstddef>
#include <cstdint>

#include "JsonLine.h"
#include "platform/Platform.h"
#include "stm32h7xx.h"
#include "stm32h7xx_hal.h"

extern "C" {
// libDaisy core/startup_stm32h750xx.c: the image's vector table (0xa6 entries) and the
// handler every unhandled interrupt aliases.
extern void* g_pfnVectors[];
void         Default_Handler(void);
// Below: the naked entry every fault vector points at.
void BrainscapeFaultEntry(void);
}

namespace brainscape::fw {

namespace {

constexpr uint32_t kNumVectors = 0xa6;  // libDaisy's g_pfnVectors
constexpr uint32_t kFaultMagic = 0xFA017BADu;

// The relocated vector table: VTOR needs the table's size rounded up to a power of two as
// its alignment (166 entries: 1 KiB). DTCM, so no cache maintenance; not zeroed by the
// startup code, and filled whole before use.
alignas(1024) __attribute__((section(".bss.brainscape_dtcm_vectors"))) void* g_ramVectors[256];

struct FaultRecord {
  uint32_t magic;
  uint32_t kind;  // FaultKind
  uint32_t ipsr, cfsr, hfsr, mmfar, bfar, pc, lr, xpsr, excReturn, sp, uptimeMs;
  char     image[16];
  char     message[64];
  uint32_t check;
};

// In the backup SRAM, after libDaisy's boot_info (the Daisy bootloader's handshake, which
// the linker script keeps at the start of the region): the reset this handler ends with
// does not clear it, and the startup code never touches it. libDaisy's MPU setup makes the
// region non-cacheable; before that the D-cache is off.
__attribute__((section(".backup_sram.brainscape_fault"))) volatile FaultRecord g_record;

FaultRecord g_lastFault;  // this boot's copy of the record the previous boot left, if any
bool        g_haveLastFault = false;

uint32_t RecordCheck(const volatile FaultRecord& r) {
  const volatile uint32_t* w = reinterpret_cast<const volatile uint32_t*>(&r);
  uint32_t                 sum = 0x5EED1234u;
  for (size_t i = 0; i < offsetof(FaultRecord, check) / 4u; ++i) sum = (sum ^ w[i]) * 16777619u;
  return sum;
}

void EnableBackupSram() {
  RCC->AHB4ENR |= RCC_AHB4ENR_BKPRAMEN;
  (void)RCC->AHB4ENR;
  PWR->CR1 |= PWR_CR1_DBP;
  for (uint32_t i = 0; i < 100000u && (PWR->CR1 & PWR_CR1_DBP) == 0u; ++i) {
  }
}

void CopyText(volatile char* dst, size_t size, const char* src) {
  size_t i = 0;
  if (src != nullptr) {
    for (; i + 1u < size && src[i] != '\0'; ++i) dst[i] = src[i];
  }
  for (; i < size; ++i) dst[i] = '\0';
}

// The core clock from the RCC registers (SystemCoreClock is only as fresh as the last HAL
// clock call, and a fault can come before DaisySeed::Init).
uint32_t CoreHz() {
  const uint32_t sys = HAL_RCC_GetSysClockFreq();
  const uint32_t pre = D1CorePrescTable[(RCC->D1CFGR & RCC_D1CFGR_D1CPRE) >> RCC_D1CFGR_D1CPRE_Pos] & 0x1Fu;
  const uint32_t hz  = sys >> pre;
  return hz != 0 ? hz : 64000000u;
}

void BusyWaitMs(uint32_t ms, uint32_t cyclesPerMs) {
  for (uint32_t m = 0; m < ms; ++m) {
    const uint32_t t0 = DWT->CYCCNT;
    while (DWT->CYCCNT - t0 < cyclesPerMs) {
    }
  }
}

}  // namespace

void SaveFaultRecord(FaultKind kind, const char* message, uint32_t pc, uint32_t lr, uint32_t xpsr,
                     uint32_t excReturn, uint32_t sp) {
  EnableBackupSram();
  volatile FaultRecord& r = g_record;
  r.magic                 = 0u;
  __DSB();
  r.kind      = static_cast<uint32_t>(kind);
  r.ipsr      = __get_IPSR();
  r.cfsr      = SCB->CFSR;
  r.hfsr      = SCB->HFSR;
  r.mmfar     = SCB->MMFAR;
  r.bfar      = SCB->BFAR;
  r.pc        = pc;
  r.lr        = lr;
  r.xpsr      = xpsr;
  r.excReturn = excReturn;
  r.sp        = sp;
  r.uptimeMs  = UptimeMs();
  CopyText(r.image, sizeof r.image, ImageName());
  CopyText(r.message, sizeof r.message, message);
  r.check = RecordCheck(r);
  __DSB();
  r.magic = kFaultMagic;
  __DSB();
  if ((SCB->CCR & SCB_CCR_DC_Msk) != 0u) SCB_CleanDCache();
}

[[noreturn]] void FaultBlinkAndReset() {
  __disable_irq();
  // The cycle counter, enabled here too in case the fault came before BoardInit.
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->LAR = 0xC5ACCE55u;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
  const uint32_t perMs = CoreHz() / 1000u;
  // The Seed's user LED, PC7 (libDaisy src/daisy_seed.cpp), as a push-pull output.
  RCC->AHB4ENR |= RCC_AHB4ENR_GPIOCEN;
  (void)RCC->AHB4ENR;
  GPIOC->MODER  = (GPIOC->MODER & ~(3u << 14)) | (1u << 14);
  GPIOC->OTYPER = GPIOC->OTYPER & ~(1u << 7);
  // Two long flashes, then a pause (2 s), five times: neither the Fatal() pattern (three
  // quick flashes), the idle blink nor the bootloader's SOS.
  for (int cycle = 0; cycle < 5; ++cycle) {
    for (int flash = 0; flash < 2; ++flash) {
      GPIOC->BSRR = 1u << 7;
      BusyWaitMs(400, perMs);
      GPIOC->BSRR = 1u << (7 + 16);
      BusyWaitMs(flash == 0 ? 200 : 1000, perMs);
    }
  }
  NVIC_SystemReset();
  for (;;) {
  }
}

std::string LastFaultJson() {
  if (!g_haveLastFault) return "null";
  const FaultRecord& r    = g_lastFault;
  const char*        kind = r.kind == static_cast<uint32_t>(FaultKind::Exception)    ? "exception"
                            : r.kind == static_cast<uint32_t>(FaultKind::FatalIsr)   ? "fatal in an interrupt"
                            : r.kind == static_cast<uint32_t>(FaultKind::FatalThread) ? "fatal"
                                                                                       : "unknown";
  return "{\"kind\":" + golden::JsonString(kind) + ",\"image\":" + golden::JsonString(r.image) +
         ",\"message\":" + golden::JsonString(r.message) + ",\"uptimeMs\":" + golden::JsonUInt(r.uptimeMs) +
         ",\"ipsr\":" + golden::JsonUInt(r.ipsr) + ",\"cfsr\":" + golden::JsonHex32(r.cfsr) +
         ",\"hfsr\":" + golden::JsonHex32(r.hfsr) + ",\"mmfar\":" + golden::JsonHex32(r.mmfar) +
         ",\"bfar\":" + golden::JsonHex32(r.bfar) + ",\"pc\":" + golden::JsonHex32(r.pc) +
         ",\"lr\":" + golden::JsonHex32(r.lr) + ",\"xpsr\":" + golden::JsonHex32(r.xpsr) +
         ",\"excReturn\":" + golden::JsonHex32(r.excReturn) + ",\"sp\":" + golden::JsonHex32(r.sp) + "}";
}

void TakeLastFault() {
  EnableBackupSram();
  const volatile FaultRecord& r = g_record;
  if (r.magic == kFaultMagic && r.check == RecordCheck(r)) {
    const volatile uint8_t* src = reinterpret_cast<const volatile uint8_t*>(&r);
    uint8_t*                dst = reinterpret_cast<uint8_t*>(&g_lastFault);
    for (size_t i = 0; i < sizeof g_lastFault; ++i) dst[i] = src[i];
    g_lastFault.image[sizeof g_lastFault.image - 1u]     = '\0';
    g_lastFault.message[sizeof g_lastFault.message - 1u] = '\0';
    g_haveLastFault                                      = true;
  }
  g_record.magic = 0u;  // reported once: by this boot's hello lines
  __DSB();
}

uint32_t ImageVectorTable() { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_pfnVectors)); }

// Runs from the preinit hook, before the copy to ITCM, so it must not call memcpy, which an
// ITCM image keeps there (platform/MemFunctions.c): this file is compiled with
// -fno-tree-loop-distribute-patterns (firmware/CMakeLists.txt), so the loop stays a loop.
void PrepareFaultVectors() {
  void* const handler = reinterpret_cast<void*>(&BrainscapeFaultEntry);
  void* const unhandled = reinterpret_cast<void*>(&Default_Handler);
  for (uint32_t i = 0; i < 256u; ++i) {
    void* v = i < kNumVectors ? g_pfnVectors[i] : unhandled;
    // 2 NMI, 3 HardFault, 4 MemManage, 5 BusFault, 6 UsageFault, and every unhandled
    // vector (libDaisy aliases them to Default_Handler).
    if ((i >= 2u && i <= 6u) || (i >= 2u && v == unhandled)) v = handler;
    g_ramVectors[i] = v;
  }
  __DSB();
}

void UseFaultVectors(bool on) {
  __DSB();
  SCB->VTOR = on ? static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_ramVectors)) : ImageVectorTable();
  // Bus, usage and memory-management faults to their own vectors (all the fault handler),
  // instead of escalating to HardFault.
  if (on) SCB->SHCSR |= SCB_SHCSR_BUSFAULTENA_Msk | SCB_SHCSR_USGFAULTENA_Msk | SCB_SHCSR_MEMFAULTENA_Msk;
  __DSB();
  __ISB();
}

}  // namespace brainscape::fw

// The fault handler's own stack, in DTCM below the arenas' neighbours: a fault caused by a
// broken main stack must not push onto it again.
extern "C" {
alignas(8) __attribute__((section(".bss.brainscape_dtcm_faultstack"))) uint32_t brainscape_fault_stack[256];

__attribute__((noreturn, used)) void BrainscapeFaultReport(const uint32_t* frame, uint32_t excReturn,
                                                            uint32_t sp) {
  using namespace brainscape::fw;
  uint32_t        pc = 0, lr = 0, xpsr = 0;
  const uintptr_t f  = reinterpret_cast<uintptr_t>(frame);
  // Only a frame inside DTCM or AXI SRAM is read: the stacked registers of a fault taken on
  // a broken stack are not worth a second fault.
  const bool readable = (f & 3u) == 0u && ((f >= 0x20000000u && f <= 0x2001FFE0u) ||
                                            (f >= 0x24000000u && f <= 0x2407FFE0u));
  if (readable) {
    lr   = frame[5];
    pc   = frame[6];
    xpsr = frame[7];
  }
  SaveFaultRecord(FaultKind::Exception, "", pc, lr, xpsr, excReturn, sp);
  FaultBlinkAndReset();
}

// Every fault vector: interrupts off, the stacked frame of whichever stack was in use, then
// onto the fault stack and into BrainscapeFaultReport(frame, EXC_RETURN, sp).
__attribute__((naked, noreturn)) void BrainscapeFaultEntry(void) {
  __asm volatile(
      "cpsid i\n\t"
      "tst lr, #4\n\t"
      "ite eq\n\t"
      "mrseq r0, msp\n\t"
      "mrsne r0, psp\n\t"
      "mov r1, lr\n\t"
      "mov r2, r0\n\t"
      "movw r3, #:lower16:brainscape_fault_stack+1024\n\t"
      "movt r3, #:upper16:brainscape_fault_stack+1024\n\t"
      "mov sp, r3\n\t"
      "b BrainscapeFaultReport\n\t");
}
}
