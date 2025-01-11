#ifndef UTIL_H
#define UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LENGTH(x)  ((int)(sizeof (x) / sizeof *(x)))
#define MIN(a, b)  ((a) > (b) ? (b) : (a))
#define MAX(a, b)  ((a) < (b) ? (b) : (a))

#define BETWEEN(x, a, b) ((x) >= (a) && (x) <= (b))

/* is c the start of a utf8 sequence? */
#define ISUTF8(c)   (((c)&0xC0)!=0x80)
#define ISASCII(ch) ((unsigned char)ch < 0x80)
#define ISDIGIT(c)  (BETWEEN((c), '0', '9'))
#define ISPUNCT(c)  (BETWEEN((c), '!', '~') && !(ISALPHA(c) || ISDIGIT(c)))
#define ISHEX(c)    (ISDIGIT((c)) || BETWEEN((c), 'a', 'f') || BETWEEN((c), 'A', 'F'))
#define ISSPACE(c)  ((c) == ' ' || (c) == '\n' || (c) == '\t' || (c) == '\r')

#define I64_MAX   INT64_MAX

typedef uint8_t   u8;
typedef int32_t   i32;
typedef uint32_t  u32;
typedef uint32_t  b32;
typedef int64_t   i64;
typedef ptrdiff_t ix;

typedef struct { ix len; u8 *data; } s8;
#define s8(s) (s8){.len = (ix)sizeof(s) - 1, .data = (u8 *)s}

#if GCC_VERSION>=5004000 || CLANG_VERSION>=4000000
#define addu __builtin_add_overflow
#else
static inline bool addu(size_t a, size_t b, size_t *c) {
	if (SIZE_MAX - a < b)
		return false;
	*c = a + b;
	return true;
}
#endif

#if !HAVE_MEMRCHR
/* MIT licensed implementation from musl libc */
static void *memrchr(const void *m, int c, size_t n)
{
	const unsigned char *s = m;
	c = (unsigned char)c;
	while (n--) if (s[n]==c) return (void *)(s+n);
	return 0;
}
#endif

/* Needed for building on GNU Hurd */

#ifndef PIPE_BUF
#define PIPE_BUF 4096
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

s8 s8_trim_space(s8);

void s8_split(s8, u8 cut_byte, s8 *lhs, s8 *rhs);

i64 s8_to_i64(s8);
u32 s8_hex_to_u32(s8);

b32 s8_equal(s8, s8);
b32 s8_case_ignore_equal(s8, s8);

s8 c_str_to_s8(const char *);

#endif /* UTIL_H */
