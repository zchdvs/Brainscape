/* memcpy, memmove and memset for the firmware images (firmware/README.md, "Memory map").
 *
 * The images link newlib-nano (--specs=nano.specs), whose mem* functions move one byte per
 * loop iteration. The engine calls memset in every Process (Engine::Impl::Process and
 * GranularCore::Process clear their block buffers), and Restart, Init and ClearHistory clear
 * the 16 MiB ring and the post-chain buffers in SDRAM with it, so the byte loops would be
 * what the DWT pass measures (profile §8.3 Q9). These move 32 bytes per iteration in
 * integer registers, which preserves every bit (no floating-point register is touched).
 *
 * Defined here, they replace newlib's: this object is linked into every image directly, so
 * libc.a's members are never pulled in. An ITCM image places them in ITCM with the engine's
 * code (firmware/linker/seed_h750.ld.in), which is why libDaisy's startup code is built so
 * that it does not call them (firmware/cmake/LibDaisy.cmake): it copies .data and zeroes
 * .bss before the preinit hook has copied ITCM's contents in.
 *
 * Compiled with -ffreestanding -fno-tree-loop-distribute-patterns (firmware/CMakeLists.txt),
 * so the compiler cannot turn these loops back into calls to themselves. Unaligned word
 * loads use LDR, which Armv7-M allows on normal memory (SCB->CCR.UNALIGN_TRP is clear); LDM,
 * LDRD and their stores are only used on aligned addresses.
 */
#include <stddef.h>
#include <stdint.h>

typedef uint32_t __attribute__((may_alias)) word;
typedef struct {
  uint32_t v;
} __attribute__((packed, may_alias)) uword; /* an unaligned word */

/* Forward copy, safe for overlap when dst < src: each step loads its words before it stores
 * any, and stores only below the next step's loads. */
static void copy_forward(unsigned char* d, const unsigned char* s, size_t n) {
  if (n >= 16) {
    while (((uintptr_t)d & 3u) != 0u) {
      *d++ = *s++;
      --n;
    }
    word* dw = (word*)d;
    if (((uintptr_t)s & 3u) == 0u) {
      const word* sw = (const word*)s;
      while (n >= 32) {
        const uint32_t a = sw[0], b = sw[1], c = sw[2], e = sw[3], f = sw[4], g = sw[5], h = sw[6], i = sw[7];
        dw[0] = a;
        dw[1] = b;
        dw[2] = c;
        dw[3] = e;
        dw[4] = f;
        dw[5] = g;
        dw[6] = h;
        dw[7] = i;
        dw += 8;
        sw += 8;
        n -= 32;
      }
      while (n >= 4) {
        *dw++ = *sw++;
        n -= 4;
      }
      s = (const unsigned char*)sw;
    } else {
      const uword* su = (const uword*)s;
      while (n >= 16) {
        const uint32_t a = su[0].v, b = su[1].v, c = su[2].v, e = su[3].v;
        dw[0] = a;
        dw[1] = b;
        dw[2] = c;
        dw[3] = e;
        dw += 4;
        su += 4;
        n -= 16;
      }
      while (n >= 4) {
        *dw++ = (su++)->v;
        n -= 4;
      }
      s = (const unsigned char*)su;
    }
    d = (unsigned char*)dw;
  }
  while (n != 0u) {
    *d++ = *s++;
    --n;
  }
}

/* Backward copy from the ends, safe for overlap when dst > src. */
static void copy_backward(unsigned char* d, const unsigned char* s, size_t n) {
  d += n;
  s += n;
  if (n >= 16) {
    while (((uintptr_t)d & 3u) != 0u) {
      *--d = *--s;
      --n;
    }
    word* dw = (word*)d;
    if (((uintptr_t)s & 3u) == 0u) {
      const word* sw = (const word*)s;
      while (n >= 32) {
        dw -= 8;
        sw -= 8;
        const uint32_t a = sw[0], b = sw[1], c = sw[2], e = sw[3], f = sw[4], g = sw[5], h = sw[6], i = sw[7];
        dw[0] = a;
        dw[1] = b;
        dw[2] = c;
        dw[3] = e;
        dw[4] = f;
        dw[5] = g;
        dw[6] = h;
        dw[7] = i;
        n -= 32;
      }
      while (n >= 4) {
        *--dw = *--sw;
        n -= 4;
      }
      s = (const unsigned char*)sw;
    } else {
      const uword* su = (const uword*)s;
      while (n >= 16) {
        dw -= 4;
        su -= 4;
        const uint32_t a = su[0].v, b = su[1].v, c = su[2].v, e = su[3].v;
        dw[0] = a;
        dw[1] = b;
        dw[2] = c;
        dw[3] = e;
        n -= 16;
      }
      while (n >= 4) {
        *--dw = (--su)->v;
        n -= 4;
      }
      s = (const unsigned char*)su;
    }
    d = (unsigned char*)dw;
  }
  while (n != 0u) {
    *--d = *--s;
    --n;
  }
}

void* memcpy(void* restrict dst, const void* restrict src, size_t n) {
  copy_forward((unsigned char*)dst, (const unsigned char*)src, n);
  return dst;
}

void* memmove(void* dst, const void* src, size_t n) {
  unsigned char*       d = (unsigned char*)dst;
  const unsigned char* s = (const unsigned char*)src;
  if (d == s || n == 0u) return dst;
  /* Forward unless the destination starts inside the source. */
  if ((uintptr_t)d - (uintptr_t)s >= n) {
    copy_forward(d, s, n);
  } else {
    copy_backward(d, s, n);
  }
  return dst;
}

void* memset(void* dst, int c, size_t n) {
  unsigned char*      d = (unsigned char*)dst;
  const unsigned char b = (unsigned char)c;
  if (n >= 16) {
    while (((uintptr_t)d & 7u) != 0u) {
      *d++ = b;
      --n;
    }
    const uint32_t w  = 0x01010101u * b;
    word*          dw = (word*)d;
    while (n >= 32) {
      dw[0] = w;
      dw[1] = w;
      dw[2] = w;
      dw[3] = w;
      dw[4] = w;
      dw[5] = w;
      dw[6] = w;
      dw[7] = w;
      dw += 8;
      n -= 32;
    }
    while (n >= 4) {
      *dw++ = w;
      n -= 4;
    }
    d = (unsigned char*)dw;
  }
  while (n != 0u) {
    *d++ = b;
    --n;
  }
  return dst;
}
