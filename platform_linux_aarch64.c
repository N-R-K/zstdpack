/*
 * zstdpack's platform layer for linux/aarch64: the SYS_* numbers, the syscall
 * instruction and _start. The numbers are the generic ones from the kernel's
 * include/uapi/asm-generic/unistd.h, which arm64 uses. A port is another file
 * like this one, named platform_linux_$(ARCH).c.
 */

enum {
	SYS_openat       = 56,
	SYS_close        = 57,
	SYS_lseek        = 62,
	SYS_write        = 64,
	SYS_exit_group   = 94,
	SYS_munmap       = 215,
	SYS_mremap       = 216,
	SYS_mmap         = 222,
	SYS_memfd_create = 279,
	SYS_execveat     = 281,
};

/*
 * The kernel takes the syscall number in x8, the arguments in x0-x5 and
 * returns in x0. It keeps every other register, so the wrappers name only the
 * registers they set; the "cc" and "memory" clobbers are the same ones musl
 * uses around svc.
 */
static long syscall1(long n, long a)
{
	register long x8 __asm__("x8") = n;
	register long x0 __asm__("x0") = a;

	__asm__ volatile ("svc #0" : "+r"(x0) : "r"(x8) : "cc", "memory");
	return x0;
}

static long syscall3(long n, long a, long b, long c)
{
	register long x8 __asm__("x8") = n;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;

	__asm__ volatile ("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2)
	                  : "cc", "memory");
	return x0;
}

static long syscall6(long n, long a, long b, long c, long d, long e, long f)
{
	register long x8 __asm__("x8") = n;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	register long x3 __asm__("x3") = d;
	register long x4 __asm__("x4") = e;
	register long x5 __asm__("x5") = f;

	__asm__ volatile ("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2),
	                  "r"(x3), "r"(x4), "r"(x5) : "cc", "memory");
	return x0;
}

/*
 * The kernel enters _start with the process' stack in sp, and it is
 * 16-aligned there, which is what entrypoint() needs. The bl does not move sp,
 * so one instruction hands entrypoint() the kernel's stack.
 */
__asm__ (
	"        .globl _start\n"
	"_start: mov   x0, sp\n"
	"        bl    entrypoint\n"
);
