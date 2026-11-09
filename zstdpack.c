/*
 * zstdpack's stub: decode the zstd frame appended to this executable and exec
 * it out of a memfd. Usage is in README.md, the reasoning in doc/design.md.
 *
 * The kernel is reached only through the os_* calls below, so this file needs
 * neither libc nor system headers.
 */

#include <zstd.h>

/* Sized strings: the length is known up front, and nothing here needs strlen. */
struct str { const char *p; size_t n; };
#define S(s) (struct str){ s, sizeof(s) - 1 }

/* The platform layer's face: an os_* call returns the kernel's result, or
 * -errno on failure. os_fail() does not return at all. */
static long os_open_self(void);                         /* fd */
static long os_size(long fd);                           /* bytes */
static void *os_map(long fd, size_t n, long *err);      /* read-only */
static void os_close(long fd);
static void *os_alloc(size_t n);                        /* zeroed, or 0 */
static void *os_grow(void *p, size_t was, size_t now);  /* 0 on failure */
static void os_release(void *p, size_t n);
static long os_memfd(void);                             /* fd */
static long os_write(long fd, const void *p, size_t n); /* all of it */
static long os_exec(long fd, char **argv, char **envp); /* only returns on failure */
static void os_fail(struct str what, long err)
	__attribute__((noreturn));

/* shortcut: refuse payloads bigger than this instead of allocating forever */
#define MAX_IMAGE ((size_t)2 << 30) /* 2 GiB */

/* The probe is capped: a file made of magic bytes would otherwise turn into
 * one decode attempt per four bytes. The payload is at the offset the ELF
 * header gives, or among the first few hits when that hint does not apply. */
#define MAX_CANDIDATES 32

static const unsigned char MAGIC[4] = { 0x28, 0xb5, 0x2f, 0xfd }; /* zstd frame */

/* The ELF header fields the hint below needs, little endian. */
static size_t le16(const unsigned char *p)
{
	return (size_t)p[0] | (size_t)p[1] << 8;
}

static size_t le64(const unsigned char *p)
{
	size_t v = 0;
	int i;

	for (i = 7; i >= 0; i--)
		v = v << 8 | p[i];
	return v;
}

/* Where the stub's own bytes end, and the payload therefore starts. On GNU
 * ld output the section header table ends the file, which makes the stub's
 * end derivable from the ELF header. The spec does not promise that, so this
 * is only a hint: the probe checks the frame magic there and falls back to
 * scanning the file. */
static size_t payload_hint(const unsigned char *map, size_t size)
{
	size_t shoff, shnum, shent, end;

	if (size < 64 || map[0] != 0x7f || map[1] != 'E' || map[2] != 'L' || map[3] != 'F')
		return 0;
	if (map[4] != 2 || map[5] != 1) /* ELFCLASS64, ELFDATA2LSB */
		return 0;
	shoff = le64(map + 0x28);
	shent = le16(map + 0x3a);
	shnum = le16(map + 0x3c);
	if (!shoff || !shent || !shnum)
		return 0;
	end = shoff + shnum * shent;
	if (end < shoff || end > size || size - end < sizeof MAGIC)
		return 0;
	return end;
}

static int has_magic(const unsigned char *p)
{
	return p[0] == MAGIC[0] && p[1] == MAGIC[1] &&
	       p[2] == MAGIC[2] && p[3] == MAGIC[3];
}

static size_t magic_from(const unsigned char *p, size_t len, size_t from)
{
	size_t i;

	for (i = from; i + sizeof MAGIC <= len; i++)
		if (has_magic(p + i))
			return i;
	return (size_t)-1;
}

/* Why the payload was rejected. The order is the severity order: the probe
 * loop keeps the worst one for the final message. */
enum inflate {
	INFLATE_NOT_STREAM, /* the candidate did not decode: try the next magic */
	INFLATE_CORRUPT,   /* the payload was there, but did not decode cleanly */
	INFLATE_NOMEM,      /* the output buffer could not be grown */
	INFLATE_CAPPED,     /* the output would pass MAX_IMAGE */
	INFLATE_OK
};

/* The decoder and its output buffer live across candidates: a stray magic
 * must not cost an mmap and a fresh decoder each time. */
struct dec {
	ZSTD_DStream *ds;
	unsigned char *buf;
	size_t cap;
};

/* Inflate src[0..len) into the shared buffer. A candidate that is not exactly
 * one whole zstd stream is the common case, not an error: stray magic bytes
 * come through here too. */
static enum inflate inflate(struct dec *d, const unsigned char *src, size_t len,
                            unsigned char **out, size_t *outlen)
{
	ZSTD_inBuffer in;
	ZSTD_outBuffer ob;
	size_t got = 0, r = 1;

	if (!d->ds && !(d->ds = ZSTD_createDStream()))
		return INFLATE_NOMEM;
	if (!d->buf) {
		d->cap = 1u << 20;
		if (!(d->buf = os_alloc(d->cap)))
			return INFLATE_NOMEM;
	}
	ZSTD_initDStream(d->ds);

	in.src = src;
	in.size = len;
	in.pos = 0;
	while (in.pos < in.size) {
		if (got == d->cap) {
			unsigned char *grown;

			/* check before doubling: 2 * MAX_IMAGE overflows a 32-bit size_t */
			if (d->cap > MAX_IMAGE / 2)
				return INFLATE_CAPPED;
			/* mremap moves the pages without copying them */
			grown = os_grow(d->buf, d->cap, d->cap * 2);
			if (!grown)
				return INFLATE_NOMEM;
			d->buf = grown;
			d->cap *= 2;
			continue;
		}
		ob.dst = d->buf;
		ob.size = d->cap;
		ob.pos = got;
		r = ZSTD_decompressStream(d->ds, &ob, &in);
		if (ZSTD_isError(r))
			break;
		got = ob.pos;
	}

	/* The stream has to end exactly at EOF and the frame has to be complete:
	 * a truncated frame makes decompressStream ask for more input instead of
	 * failing. */
	if (in.pos == in.size && r == 0) {
		*out = d->buf;
		*outlen = got;
		return INFLATE_OK;
	}
	return INFLATE_NOT_STREAM; /* the buffer stays: the next candidate reuses it */
}

static int os_main(char **argv, char **envp)
{
	unsigned char *map, *blob = 0;
	size_t size, off, hint, bloblen = 0, tried = 0;
	enum inflate status = INFLATE_NOT_STREAM;
	struct dec dec = { 0, 0, 0 };
	long fd, mfd, e;

	fd = os_open_self();
	if (fd < 0)
		os_fail(S("open /proc/self/exe"), fd);
	e = os_size(fd);
	if (e < 0)
		os_fail(S("size /proc/self/exe"), e);
	size = (size_t)e;
	map = os_map(fd, size, &e);
	if (!map)
		os_fail(S("mmap /proc/self/exe"), e);
	os_close(fd);

	/*
	 * The payload sits behind the stub's own bytes, and the ELF header says
	 * where those end: that offset is the first candidate. The magic is only
	 * a place to start looking, though: the candidate has to decode a whole
	 * stream ending exactly at EOF, which a stray magic cannot also be. The
	 * scan is the fallback for a file whose ELF header does not add up.
	 * See doc/design.md.
	 */
	hint = payload_hint(map, size);
	if (hint && has_magic(map + hint)) {
		enum inflate r = inflate(&dec, map + hint, size - hint, &blob, &bloblen);

		tried++;
		if (r == INFLATE_NOT_STREAM)
			r = INFLATE_CORRUPT; /* the payload is there, and it is broken */
		if (r != INFLATE_OK && r > status) /* remember the worst reason */
			status = r;
	}
	for (off = 0; !blob && tried < MAX_CANDIDATES &&
	                (off = magic_from(map, size, off)) != (size_t)-1; off++) {
		enum inflate r = inflate(&dec, map + off, size - off, &blob, &bloblen);

		tried++;
		if (r == INFLATE_OK)
			break;
		if (r > status) /* remember the worst reason, for the message */
			status = r;
	}
	if (!blob)
		switch (status) {
		case INFLATE_NOMEM:
			os_fail(S("out of memory"), 0);
		case INFLATE_CAPPED:
			os_fail(S("payload is too large (raise MAX_IMAGE)"), 0);
		case INFLATE_CORRUPT:
			os_fail(S("the appended payload is truncated or corrupt"), 0);
		default:
			os_fail(S("no zstd payload appended to this file"), 0);
		}

	mfd = os_memfd();
	if (mfd < 0)
		os_fail(S("memfd_create"), mfd);
	if ((e = os_write(mfd, blob, bloblen)) < 0)
		os_fail(S("write memfd"), e);
	e = os_exec(mfd, argv, envp);
	os_fail(S("exec the payload"), e);
}
