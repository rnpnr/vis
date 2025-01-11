#ifndef UTIL_H
#define UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <string.h>
#include <sys/mman.h>

#if defined(__aarch64__)
#define ASSERT(a) if (!(a)) { __asm volatile ("brk 0xf000"); }
#elif defined(__amd64__)
#define ASSERT(a) if (!(a)) { __asm volatile ("int3; nop"); }
#else
#define ASSERT(a) if (!(a)) { (*(int *)0 = 0); }
#endif

#define ARRAY_COUNT(a)  (sizeof(a) / sizeof(*a))

#define LENGTH(x)  ((int)(sizeof (x) / sizeof *(x)))
#define MIN(a, b)  ((a) > (b) ? (b) : (a))
#define MAX(a, b)  ((a) < (b) ? (b) : (a))

#define BETWEEN(x, a, b) ((x) >= (a) && (x) <= (b))

/* is c the start of a utf8 sequence? */
#define ISUTF8(c)   (((c)&0xC0)!=0x80)
#define ISASCII(ch) ((unsigned char)ch < 0x80)
#define ISALPHA(c)  (BETWEEN((c), 'a', 'z') || BETWEEN((c), 'A', 'Z'))
#define ISDIGIT(c)  (BETWEEN((c), '0', '9'))
#define ISPUNCT(c)  (BETWEEN((c), '!', '~') && !(ISALPHA(c) || ISDIGIT(c)))
#define ISHEX(c)    (ISDIGIT((c)) || BETWEEN((c), 'a', 'f') || BETWEEN((c), 'A', 'F'))
#define ISSPACE(c)  ((c) == ' ' || (c) == '\n' || (c) == '\t' || (c) == '\r')

#define GB(n) ((n) << 30ULL)


#define I64_MAX   INT64_MAX
#define I32_MAX   INT32_MAX

typedef uint8_t   u8;
typedef int32_t   i32;
typedef uint32_t  u32;
typedef uint32_t  b32;
typedef int64_t   i64;
typedef uint64_t  u64;
typedef ptrdiff_t ix;

typedef struct { ix len; u8 *data; } s8;
#define s8(s) (s8){.len = (ix)sizeof(s) - 1, .data = (u8 *)s}

typedef struct {
	u8 *start;
	u8 *end;
	u8 *base;
	ix  capacity;
} Arena;

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


static s8 c_str_to_s8(const char *s)
{
	s8 result = {.data = (u8 *)s};
	for (; s && *s; s++, result.len++);
	return result;
}

static s8 s8_trim_space(s8 s)
{
	while (s.len > 0 && ISSPACE(s.data[0])) { s.len--; s.data++; }
	while (s.len > 0 && ISSPACE(s.data[s.len - 1])) s.len--;
	return s;
}

void s8_split(s8, u8 cut_byte, s8 *lhs, s8 *rhs);

i64 s8_to_i64(s8);
u32 s8_hex_to_u32(s8);

b32 s8_equal(s8, s8);
b32 s8_case_ignore_equal(s8, s8);

s8 c_str_to_s8(const char *);

static Arena
arena_new(void)
{
	Arena result = {0};
	/* TODO(rnp): may not be supported everywhere */
	void *store = mmap(0, GB(1), PROT_NONE, MAP_ANON|MAP_PRIVATE, -1, 0);
	if (store != MAP_FAILED) {
		/* NOTE(rnp): start by committing a single page; expand as needed */
		madvise((u8 *)store + 4096, GB(1) - 4096, MADV_DONTNEED);
		mprotect(store, 4096, PROT_READ|PROT_WRITE);
		result.start    = result.base = store;
		result.end      = result.start + 4096;
		result.capacity = GB(1);
	}
	return result;
}

static void
arena_expand(Arena *a, ix needed_size)
{
	ASSERT(needed_size < a->capacity - (a->end - a->start));
	ix committed = a->end - a->base;
	ix used      = a->start - a->base;
	while (committed < a->capacity && (committed - used) < needed_size)
		committed <<= 1;
	ix to_commit = committed - (a->end - a->base);
	madvise(a->end, to_commit, MADV_NORMAL);
	mprotect(a->end, to_commit, PROT_READ|PROT_WRITE);
	a->end += to_commit;
}

#define push_struct(b, t) alloc(b, t, 1)
#define alloc(b, t, n) (t *)alloc_(b, sizeof(t), __alignof__(t), n)
static void *
alloc_(Arena *a, ix len, ix align, ix count)
{
	ix padding   = -(uintptr_t)a->start & (align - 1);
	ix remaining = a->end - a->start - padding;
	if (remaining <= 0 || remaining / len < count) {
		if ((a->capacity - remaining) / len < count) {
			arena_expand(a, len * count + padding);
		} else {
			ASSERT(0);
		}
	}

	void *result = a->start + padding;
	a->start += padding + count * len;
	return memset(result, 0, count * len);
}

static s8
push_s8_zero(Arena *a, s8 s)
{
	s8 result = {0};
	if (s.len) {
		result.data = alloc(a, u8, s.len + 1);
		result.len  = s.len;
		memcpy(result.data, s.data, s.len);
	}
	return result;
}

#endif /* UTIL_H */
