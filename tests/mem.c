/* An alignment or overlap mistake in mem.c corrupts data silently, so check its
 * three functions byte for byte against libc's. mem.c is included with its
 * names changed to shim_*, so the libc_* calls above stay libc's. */
#include <stdio.h>
#include <string.h>

/* libc's own, under names the defines below do not touch. */
static void *libc_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
static void *libc_memset(void *d, int c, size_t n) { return memset(d, c, n); }
static void *libc_memmove(void *d, const void *s, size_t n) { return memmove(d, s, n); }

#define memcpy shim_memcpy
#define memset shim_memset
#define memmove shim_memmove
#include "../mem.c"

#define N 4096
static unsigned char got[N], want[N];
static unsigned long fails;

static void seed(void)
{
	size_t i;

	for (i = 0; i < N; i++)
		got[i] = want[i] = (unsigned char)(i * 7 + i / 97);
}

/* The whole buffer is compared, so a write outside the range shows up too. */
static void cmp(const char *what, size_t n, unsigned d, unsigned s)
{
	size_t i;

	for (i = 0; i < N; i++)
		if (got[i] != want[i]) {
			printf("FAIL  %s len=%zu d=%u s=%u: byte %zu differs\n",
			       what, n, d, s, i);
			fails++;
			return;
		}
}

int main(void)
{
	size_t n;
	unsigned d, s;

	for (n = 0; n <= 64; n++)
		for (d = 0; d < 8; d++)
			for (s = 0; s < 8; s++) {
				seed();
				shim_memcpy(got + 2048 + d, got + s, n);
				libc_memcpy(want + 2048 + d, want + s, n);
				cmp("memcpy", n, d, s);

				seed();
				shim_memset(got + 2048 + d, (int)s, n);
				libc_memset(want + 2048 + d, (int)s, n);
				cmp("memset", n, d, s);

				seed();
				shim_memmove(got + 2048 + d, got + s, n);
				libc_memmove(want + 2048 + d, want + s, n);
				cmp("memmove", n, d, s);
			}

	/* Overlapping ranges, each way round. */
	for (d = 1; d <= 16; d++) {
		seed();
		shim_memmove(got + d, got, 200);
		libc_memmove(want + d, want, 200);
		cmp("memmove overlap", 200, d, 0);

		seed();
		shim_memmove(got, got + d, 200);
		libc_memmove(want, want + d, 200);
		cmp("memmove overlap", 200, 0, d);
	}

	if (fails)
		printf("FAIL  mem.c against libc\n");
	return fails != 0;
}
