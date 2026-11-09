/*
 * zstdpack's Linux platform layer, with no architecture-specific code in it:
 * the kernel's constants, the stderr messages, the mmap allocator and
 * entrypoint(). The architecture file supplies the arch specific
 * SYS_* numbers, the syscall wrappers and _start. See doc/design.md.
 */

#include <stddef.h>

/* Defined by the architecture file, which includes this file first */
static long syscall1(long n, long a);
static long syscall3(long n, long a, long b, long c);
static long syscall6(long n, long a, long b, long c, long d, long e, long f);
static long os_write(long fd, const void *p, size_t n); /* all of it */

enum {
	/* Linux values; these do not vary by architecture. */
	AT_FDCWD          = -100,
	AT_EMPTY_PATH     = 0x1000,
	O_RDONLY          = 0,
	SEEK_END          = 2,
	PROT_READ         = 1,
	PROT_WRITE        = 2,
	MAP_PRIVATE       = 2,
	MAP_ANONYMOUS     = 0x20,
	MREMAP_MAYMOVE    = 1,
	MFD_CLOEXEC       = 1,

	/* The errnos the calls below can return, named for the messages. */
	EPERM = 1, ENOENT = 2, EINTR = 4, EIO = 5, E2BIG = 7, ENOEXEC = 8, ENOMEM = 12, EACCES = 13,
	EBUSY = 16, ENODEV = 19, ENOTDIR = 20, EISDIR = 21, EINVAL = 22,
	ENFILE = 23, EMFILE = 24, ETXTBSY = 26, ENOSPC = 28, EROFS = 30,
	ENAMETOOLONG = 36, ENOSYS = 38, ELOOP = 40, EOVERFLOW = 75,

	/* Room for the length zp_free() needs; keeps the payload 16-byte aligned. */
	ALLOC_HEADER = 16,
};

/*
 * Buffered output: [buf, off) holds the bytes waiting to be written; out_flush()
 * writes them with one full write and rewinds off. A failed flush sticks in err,
 * after which every put is a no-op, so a caller assembles a message and deals
 * with failure once, at the end. A negative fd means a memory buffer: nothing is
 * written, the error sticks instead.
 */
struct out { unsigned char *buf, *off, *end; long fd; int err; };

static struct out
out_make(long fd, unsigned char *buf, size_t n)
{
	struct out o = { buf, buf, buf + n, fd, 0 };
	return o;
}

static void
out_flush(struct out *o)
{
	size_t n = (size_t)(o->off - o->buf);

	o->off = o->buf;
	if (n == 0)
		return;
	/* fd < 0: a memory buffer, record the error instead of writing. */
	o->err = o->err || o->fd < 0 || os_write(o->fd, o->buf, n) < 0;
}

static void
out_str(struct out *o, struct str s)
{
	const unsigned char *p = (const unsigned char *)s.p;
	const unsigned char *end = s.n ? p + s.n : p;

	while (!o->err && p < end) {
		if (o->off == o->end)
			out_flush(o);
		while (o->off < o->end && p < end)
			*o->off++ = *p++;
	}
}

static void
out_num(struct out *o, unsigned long v)
{
	unsigned char digits[20], *p = digits + sizeof digits;

	do
		*--p = (unsigned char)('0' + v % 10);
	while (v /= 10);
	out_str(o, (struct str){ (const char *)p, (size_t)((digits + sizeof digits) - p) });
}

/* strerror(3), reduced to the errnos the calls below can report. */
static void
out_errno(struct out *o, long e)
{
	switch (e) {
	case EPERM:        out_str(o, S("operation not permitted")); return;
	case ENOENT:       out_str(o, S("no such file or directory")); return;
	case EIO:          out_str(o, S("input/output error")); return;
	case E2BIG:        out_str(o, S("argument list too long")); return;
	case ENOEXEC:      out_str(o, S("exec format error")); return;
	case ENOMEM:       out_str(o, S("cannot allocate memory")); return;
	case EACCES:       out_str(o, S("permission denied")); return;
	case EBUSY:        out_str(o, S("device or resource busy")); return;
	case ENODEV:       out_str(o, S("no such device")); return;
	case ENOTDIR:      out_str(o, S("not a directory")); return;
	case EISDIR:       out_str(o, S("is a directory")); return;
	case EINVAL:       out_str(o, S("invalid argument")); return;
	case ENFILE:       out_str(o, S("too many open files in system")); return;
	case EMFILE:       out_str(o, S("too many open files")); return;
	case ETXTBSY:      out_str(o, S("text file busy")); return;
	case ENOSPC:       out_str(o, S("no space left on device")); return;
	case EROFS:        out_str(o, S("read-only file system")); return;
	case ENAMETOOLONG: out_str(o, S("file name too long")); return;
	case ENOSYS:       out_str(o, S("function not implemented")); return;
	case ELOOP:        out_str(o, S("too many levels of symbolic links")); return;
	case EOVERFLOW:    out_str(o, S("value too large for defined data type")); return;
	}
	out_str(o, S("error "));
	out_num(o, (unsigned long)e);
}

static void
os_fail(struct str what, long err)
{
	unsigned char buf[512];
	struct out o = out_make(2, buf, sizeof buf);

	out_str(&o, S("zstdpack: "));
	out_str(&o, what);
	if (err) {
		out_str(&o, S(": "));
		out_errno(&o, -err);
	}
	out_str(&o, S("\n"));
	out_flush(&o);
	syscall1(SYS_exit_group, 127);
	__builtin_unreachable();
}

/* A syscall reports an error as a negative value in [-4095, -1]; anything
 * else it returns is a result, even when a 32-bit long makes it negative. */
static int failed(long r)
{
	return r < 0 && r >= -4095;
}

static long os_open_self(void)
{
	return syscall3(SYS_openat, AT_FDCWD, (long)"/proc/self/exe", O_RDONLY);
}

static long os_size(long fd)
{
	return syscall3(SYS_lseek, fd, 0, SEEK_END);
}

static void *os_map(long fd, size_t n, long *err)
{
	long r = syscall6(SYS_mmap, 0, (long)n, PROT_READ, MAP_PRIVATE, fd, 0);

	if (failed(r)) {
		*err = r;
		return 0;
	}
	return (void *)r;
}

static void os_close(long fd)
{
	syscall1(SYS_close, fd);
}

static void *os_alloc(size_t n)
{
	long r = syscall6(SYS_mmap, 0, (long)n, PROT_READ | PROT_WRITE,
	                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	return failed(r) ? 0 : (void *)r;
}

static void *os_grow(void *p, size_t was, size_t now)
{
	long r = syscall6(SYS_mremap, (long)p, (long)was, (long)now,
	                  MREMAP_MAYMOVE, 0, 0);

	return failed(r) ? 0 : (void *)r; /* on failure p is still mapped */
}

static void os_release(void *p, size_t n)
{
	syscall3(SYS_munmap, (long)p, (long)n, 0);
}

static long os_memfd(void)
{
	return syscall3(SYS_memfd_create, (long)"zstdpack",
	                 MFD_CLOEXEC, 0);
}

static long os_write(long fd, const void *p, size_t n)
{
	const char *b = p;

	while (n) {
		long w = syscall3(SYS_write, fd, (long)b, (long)n);

		if (w < 0) {
			if (w == -EINTR)
				continue;
			return w;
		}
		if (!w) /* no progress; a memfd write cannot block */
			return -EIO;
		b += w;
		n -= (size_t)w;
	}
	return 0;
}

static long os_exec(long fd, char **argv, char **envp)
{
	return syscall6(SYS_execveat, fd, (long)"", (long)argv, (long)envp,
	                AT_EMPTY_PATH, 0);
}

/*
 * zstd's allocator, hooked in zstdpack_unity.c: a length header in front of
 * each block, because ZSTD_free() is not given a size.
 */
static void *zp_alloc(size_t size)
{
	size_t total = size + ALLOC_HEADER;
	size_t *p;

	if (total < size) /* size + header wrapped */
		return 0;
	if (!(p = os_alloc(total)))
		return 0;
	p[0] = total;
	return (char *)p + ALLOC_HEADER;
}

static void zp_free(void *p)
{
	char *base;

	if (!p)
		return;
	base = (char *)p - ALLOC_HEADER;
	os_release(base, *(size_t *)base);
}

/* The architecture file's _start calls this with the process' stack: argc,
 * argv[argc], NULL, envp[]. */
void entrypoint(long *stack)
{
	int argc = (int)stack[0];
	char **argv = (char **)(stack + 1);
	char **envp = argv + argc + 1;

	syscall1(SYS_exit_group, os_main(argv, envp));
	__builtin_unreachable();
}
