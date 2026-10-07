#include "platform/Placement.h"

#include <cstdint>

#include "JsonLine.h"

using brainscape::golden::JsonHex32;
using brainscape::golden::JsonObj;

namespace brainscape::fw {

namespace {

// .bss.brainscape_* names: the compiler emits them as NOBITS, and the linker script routes
// them to their regions ahead of the catch-all .bss.
alignas(32) __attribute__((section(".bss.brainscape_dtcm_hot"))) unsigned char g_hot[kHotArenaBytes];
alignas(kEngineSlotAlign) __attribute__((section(".bss.brainscape_dtcm_engine")))
unsigned char g_engine[kEngineSlotBytes];
alignas(32) __attribute__((section(".bss.brainscape_axi_warm"))) unsigned char g_warm[kWarmArenaBytes];
alignas(32) __attribute__((section(".bss.brainscape_sdram_bulk"))) unsigned char g_bulk[kBulkArenaBytes];

}  // namespace

extern "C" {
// firmware/linker/seed_h750.ld.in
extern char _sdata, _edata, _sbss, _ebss, _sidata, _stext, _etext;
extern char __dtcm_bss_start, __dtcm_bss_end, __axi_bss_start, __axi_bss_end;
extern char _ssdram_bss, _esdram_bss, _ssram1_bss, _esram1_bss;
extern char __heap_start, __brainscape_heap_end, _estack;
extern char __brainscape_image_start, __brainscape_image_end;
uint32_t BrainscapeHeapUsed(void);  // Syscalls.c
}

EnginePlacement Placement() {
  EnginePlacement p;
  p.arenas.base[static_cast<size_t>(Tier::Hot)]   = g_hot;
  p.arenas.bytes[static_cast<size_t>(Tier::Hot)]  = sizeof g_hot;
  p.arenas.base[static_cast<size_t>(Tier::Warm)]  = g_warm;
  p.arenas.bytes[static_cast<size_t>(Tier::Warm)] = sizeof g_warm;
  p.arenas.base[static_cast<size_t>(Tier::Bulk)]  = g_bulk;
  p.arenas.bytes[static_cast<size_t>(Tier::Bulk)] = sizeof g_bulk;
  p.engine                                        = g_engine;
  return p;
}

namespace {

uint32_t Addr(const void* p) { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p)); }

std::string Span(const void* start, const void* end) {
  return JsonObj()
      .Raw("start", JsonHex32(Addr(start)))
      .UInt("bytes", Addr(end) - Addr(start))
      .Done();
}

}  // namespace

std::string MemoryMapJson() {
  return JsonObj()
      .Raw("image", Span(&__brainscape_image_start, &__brainscape_image_end))
      .Raw("text", Span(&_stext, &_etext))
      .Raw("data", Span(&_sdata, &_edata))
      .Raw("bss", Span(&_sbss, &_ebss))
      .Raw("dtcmArenas", Span(&__dtcm_bss_start, &__dtcm_bss_end))
      .Raw("axiArenas", Span(&__axi_bss_start, &__axi_bss_end))
      .Raw("d2Dma", Span(&_ssram1_bss, &_esram1_bss))
      .Raw("sdramArenas", Span(&_ssdram_bss, &_esdram_bss))
      .Raw("heap", JsonObj()
                       .Raw("start", JsonHex32(Addr(&__heap_start)))
                       .UInt("bytes", Addr(&__brainscape_heap_end) - Addr(&__heap_start))
                       .UInt("used", BrainscapeHeapUsed())
                       .Done())
      .Raw("stackTop", JsonHex32(Addr(&_estack)))
      .Raw("hot", Span(g_hot, g_hot + sizeof g_hot))
      .Raw("engine", Span(g_engine, g_engine + sizeof g_engine))
      .Raw("warm", Span(g_warm, g_warm + sizeof g_warm))
      .Raw("bulk", Span(g_bulk, g_bulk + sizeof g_bulk))
      .Done();
}

}  // namespace brainscape::fw
