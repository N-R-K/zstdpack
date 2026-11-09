/*
 * zstdpack's platform layer for linux/x86-64: the SYS_* numbers, the syscall
 * instruction and _start. The numbers are the x86-64 ones from syscall(2) and
 * the kernel's arch/x86/entry/syscalls/syscall_64.tbl. A port is another file
 * like this one, named platform_linux_$(ARCH).c.
 */

enum {
	SYS_write        = 1,
	SYS_close        = 3,
	SYS_lseek        = 8,
	SYS_mmap         = 9,
	SYS_munmap       = 11,
	SYS_mremap       = 25,
	SYS_fcntl        = 72,
	SYS_exit_group   = 231,
	SYS_openat       = 257,
	SYS_memfd_create = 319,
	SYS_execveat     = 322,
};

static long syscall1(long n, long a)
{
	long r;

	__asm__ volatile ("syscall"
		: "=a"(r)
		: "a"(n), "D"(a)
		: "rcx", "r11", "memory");
	return r;
}

static long syscall3(long n, long a, long b, long c)
{
	long r;

	__asm__ volatile ("syscall"
		: "=a"(r)
		: "a"(n), "D"(a), "S"(b), "d"(c)
		: "rcx", "r11", "memory");
	return r;
}

static long syscall6(long n, long a, long b, long c, long d, long e, long f)
{
	register long r10 __asm__("r10") = d;
	register long r8  __asm__("r8") = e;
	register long r9  __asm__("r9") = f;
	long r;

	__asm__ volatile ("syscall"
		: "=a"(r)
		: "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
		: "rcx", "r11", "memory");
	return r;
}

/*
 * The kernel enters _start with the process' stack in rsp, and it is
 * 16-aligned there, so one call gives entrypoint() a normal frame.
 */
__asm__ (
	"        .globl _start\n"
	"_start: mov   %rsp, %rdi\n"
	"        call  entrypoint\n"
);
