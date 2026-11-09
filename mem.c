/*
 * The libc-shaped symbols zstd and the compiler call by name: memcpy, memmove
 * and memset.
 *
 * A separate translation unit, compiled -fno-builtin: the compiler knows these
 * names and would otherwise fold a definition into a builtin, or turn a body
 * into a call to itself.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __GNUC__
typedef uint32_t __attribute__((__may_alias__, __aligned__(1))) mem32;
#endif

void *memcpy(void *restrict dest, const void *restrict src, size_t n)
{
	unsigned char *d = dest;
	const unsigned char *s = src;

#ifdef __GNUC__
	mem32 *d32 = dest;
	const mem32 *s32 = src;
	for (; n >= 16; n -= 16, d += 16, s += 16, d32 += 4, s32 += 4) {
		mem32 w0 = s32[0], w1 = s32[1], w2 = s32[2], w3 = s32[3];
		d32[0] = w0; d32[1] = w1;
		d32[2] = w2; d32[3] = w3;
	}
	for (; n >= 4; n -= 4, d += 4, s += 4) {
		*d32++ = *s32++;
	}
#endif

	for (; n; n--) *d++ = *s++;
	return dest;
}

void *memmove(void *dest, const void *src, size_t n)
{
	unsigned char *d = dest;
	const unsigned char *s = src;

	/* Compare addresses, not pointers: comparing pointers into different
	 * objects is undefined. */
	if ((uintptr_t)d == (uintptr_t)s) return dest;

	if ((uintptr_t)d < (uintptr_t)s) {
		/* Forward: correct where the ranges overlap. */
#ifdef __GNUC__
		mem32 *d32 = dest;
		const mem32 *s32 = src;
		for (; n >= 16; n -= 16, d += 16, s += 16, d32 += 4, s32 += 4) {
			mem32 w0 = s32[0], w1 = s32[1], w2 = s32[2], w3 = s32[3];
			d32[0] = w0; d32[1] = w1;
			d32[2] = w2; d32[3] = w3;
		}
		for (; n >= 4; n -= 4, d += 4, s += 4) {
			*d32++ = *s32++;
		}
#endif
		for (; n; n--) *d++ = *s++;
	} else {
		/* Backward, from the far end: each word is read before the words
		 * it is written over, so an overlap stays correct. */
		d += n;
		s += n;
#ifdef __GNUC__
		for (; n >= 16; n -= 16, d -= 16, s -= 16) {
			const mem32 *s32 = (const mem32 *)(s - 16);
			mem32 w0 = s32[0], w1 = s32[1], w2 = s32[2], w3 = s32[3];
			mem32 *d32 = (mem32 *)(d - 16);
			d32[0] = w0; d32[1] = w1;
			d32[2] = w2; d32[3] = w3;
		}
		for (; n >= 4; n -= 4, d -= 4, s -= 4) {
			mem32 w = *(const mem32 *)(s - 4);
			*(mem32 *)(d - 4) = w;
		}
#endif
		for (; n; n--) *--d = *--s;
	}
	return dest;
}

void *memset(void *dest, int c, size_t n)
{
	unsigned char *s = dest;

#ifdef __GNUC__
	mem32 c32 = (unsigned char)c * (mem32)0x01010101;
	for (; n >= 16; n -= 16, s += 16) {
		mem32 *d32 = (mem32 *)s;
		d32[0] = c32; d32[1] = c32;
		d32[2] = c32; d32[3] = c32;
	}
	for (; n >= 4; n -= 4, s += 4) {
		*(mem32 *)s = c32;
	}
#endif

	for (; n; n--, s++) *s = c;
	return dest;
}
