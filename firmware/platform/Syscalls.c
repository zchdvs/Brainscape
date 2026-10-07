/* newlib system calls the bring-up images need beyond libnosys (linked by
 * --specs=nosys.specs): a heap that stops at the end of its linker region instead of
 * growing into whatever follows (and that refuses to grow before BoardInit has brought up
 * the SDRAM it lives in), and an exit that shows itself on the LED. */
#include <errno.h>
#include <reent.h>
#include <stddef.h>
#include <stdint.h>

#include "stm32h7xx.h"

#undef errno
extern int errno;

/* firmware/linker/seed_h750.ld.in */
extern char __heap_start;
extern char __brainscape_heap_end;

static char* g_heapTop;
static int   g_heapReady;

void BrainscapeHeapReady(void) { g_heapReady = 1; } /* platform/Seed.cpp, after the SDRAM init */

void* _sbrk(ptrdiff_t increment) {
  if (!g_heapReady) {
    errno = ENOMEM;
    return (void*)-1;
  }
  if (g_heapTop == NULL) g_heapTop = &__heap_start;
  char* const previous = g_heapTop;
  if (increment < 0 || (size_t)(&__brainscape_heap_end - g_heapTop) < (size_t)increment) {
    errno = ENOMEM;
    return (void*)-1;
  }
  g_heapTop += increment;
  return previous;
}

uint32_t BrainscapeHeapUsed(void) {
  return g_heapTop == NULL ? 0u : (uint32_t)(g_heapTop - &__heap_start);
}

/* malloc is not reentrant, and libDaisy's USB device stack allocates its CDC state with
 * malloc inside the USB interrupt when the host configures the device (src/usbd/usbd_conf.h:
 * USBD_malloc is malloc), while the main loop allocates too (the golden harness, reply
 * strings). newlib calls these around every heap operation: they mask interrupts for its
 * few microseconds, nesting safely. */
static uint32_t g_mallocPrimask;
static int      g_mallocDepth;

void __malloc_lock(struct _reent* r) {
  (void)r;
  const uint32_t primask = __get_PRIMASK();
  __disable_irq();
  if (g_mallocDepth++ == 0) g_mallocPrimask = primask;
}

void __malloc_unlock(struct _reent* r) {
  (void)r;
  if (--g_mallocDepth == 0 && g_mallocPrimask == 0u) __enable_irq();
}

/* abort(), a failed allocation under -fno-exceptions and a failed assert end here. */
void BrainscapeFatalExit(int code); /* platform/Seed.cpp */
void _exit(int code) {
  BrainscapeFatalExit(code);
  for (;;) {
  }
}
