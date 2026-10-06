/* Minimal newlib syscall layer + startup so the Cortex-M7 (Thumb-2/FPv5) golden
 * harness ELF runs under qemu-arm USER mode (Linux EABI: svc 0, number in r7).
 * From tools/parity/oracle/arm_sys.c, the shim behind the profile's M7 measurements.
 * libgloss-linux in GNU Arm 10.3 encodes the number in the SVC immediate (OABI-style
 * Thumb), which modern Linux / qemu-user reject, hence this file. The dsp/ objects
 * are untouched: only the harness platform layer differs from the firmware's. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/times.h>

#undef errno
extern int errno;

static long sys3(long nr, long a, long b, long c) {
  register long r0 __asm__("r0") = a;
  register long r1 __asm__("r1") = b;
  register long r2 __asm__("r2") = c;
  register long r7 __asm__("r7") = nr;
  __asm__ volatile("svc 0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r7) : "memory");
  return r0;
}
static int ret(long r) {
  if (r < 0 && r > -4096) { errno = (int)-r; return -1; }
  return (int)r;
}

int _write(int fd, const void* p, int n) { return ret(sys3(4, fd, (long)p, n)); }
int _read(int fd, void* p, int n) { return ret(sys3(3, fd, (long)p, n)); }
int _close(int fd) { return ret(sys3(6, fd, 0, 0)); }
int _lseek(int fd, int off, int whence) { return ret(sys3(19, fd, off, whence)); }
int _getpid(void) { return (int)sys3(20, 0, 0, 0); }
int _kill(int pid, int sig) { return ret(sys3(37, pid, sig, 0)); }
int _isatty(int fd) { return fd <= 2; }
int _fstat(int fd, struct stat* st) { st->st_mode = fd <= 2 ? S_IFCHR : S_IFREG; st->st_blksize = 4096; return 0; }
int _open(const char* path, int flags, int mode) {
  int lf = flags & 3;                     /* O_RDONLY/O_WRONLY/O_RDWR identical */
  if (flags & O_CREAT)  lf |= 0100;       /* translate newlib -> Linux ARM bits */
  if (flags & O_TRUNC)  lf |= 01000;
  if (flags & O_APPEND) lf |= 02000;
  if (flags & O_EXCL)   lf |= 0200;
  return ret(sys3(5, (long)path, lf, mode));
}
void _exit(int code) { for (;;) sys3(248, code, 0, 0); }
int _gettimeofday(struct timeval* tv, void* tz) { (void)tz; return ret(sys3(78, (long)tv, 0, 0)); }
clock_t _times(struct tms* t) { (void)t; return (clock_t)-1; }

/* The 2^22 ring alone is 16 MiB; qemu-user maps this lazily. */
static unsigned char heap[160u << 20] __attribute__((aligned(64)));
static unsigned heapUsed;
void* _sbrk(int incr) {
  if (heapUsed + (unsigned)incr > sizeof heap) { errno = ENOMEM; return (void*)-1; }
  void* p = heap + heapUsed;
  heapUsed += (unsigned)incr;
  return p;
}

void _init(void) {}
void _fini(void) {}
extern int main(int, char**);
extern void exit(int);
extern void __libc_init_array(void);
void start_c(uint32_t* sp) {
  int argc = (int)sp[0];
  char** argv = (char**)(sp + 1);
  __libc_init_array();
  exit(main(argc, argv));
}
__attribute__((naked)) void _start(void) {
  __asm__ volatile("mov r0, sp\n bl start_c\n");
}
