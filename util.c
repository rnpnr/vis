static uint32_t
utf8_encode(uint8_t out[4], uint32_t cp)
{
	uint32_t result;
	if (cp <= 0x7F) {
		out[0] = cp & 0x7F;
		result = 1;
	} else if (cp <= 0x7FF) {
		result = 2;
		out[0] = ((cp >>  6) & 0x1F) | 0xC0;
		out[1] = ((cp >>  0) & 0x3F) | 0x80;
	} else if (cp <= 0xFFFF) {
		result = 3;
		out[0] = ((cp >> 12) & 0x0F) | 0xE0;
		out[1] = ((cp >>  6) & 0x3F) | 0x80;
		out[2] = ((cp >>  0) & 0x3F) | 0x80;
	} else if (cp <= 0x10FFFF) {
		result = 4;
		out[0] = ((cp >> 18) & 0x07) | 0xF0;
		out[1] = ((cp >> 12) & 0x3F) | 0x80;
		out[2] = ((cp >>  6) & 0x3F) | 0x80;
		out[3] = ((cp >>  0) & 0x3F) | 0x80;
	} else {
		//out[0] = '?';
		result = 0;
	}
	return result;
}

#define zero_struct(s) memset((s), 0, sizeof(*s))

enum { DA_INITIAL_CAP = 16 };

#define da_release(da) free((da)->data)

#define da_unordered_remove(da, index) do { \
	if ((index) < (da)->count - 1) \
		(da)->data[(index)] = (da)->data[(da)->count - 1]; \
	(da)->count--; \
} while(0)

#define da_ordered_remove(da, index) do { \
	if ((index) < (da)->count - 1) \
		memmove((da)->data + (index), (da)->data + (index) + 1, \
		        sizeof(*(da)->data) * ((da)->count - (index) - 1)); \
	(da)->count--; \
} while(0)

#define DA_COMPARE_FN(name) int name(const void *va, const void *vb)
#define da_sort(da, compare) qsort((da)->data, (da)->count, sizeof(*(da)->data), (compare))

#define da_reserve(vis, da, n) \
  (da)->data = da_reserve_(vis, (da)->data, &(da)->capacity, (da)->count + n, sizeof(*(da)->data))

#define da_push(vis, da) \
  ((da)->count == (da)->capacity \
    ? da_reserve(vis, da, 1), \
      (da)->data + (da)->count++ \
    : (da)->data + (da)->count++)

static void *
da_reserve_(Vis *vis, void *data, VisDACount *capacity, VisDACount needed, size_t size)
{
	VisDACount cap = *capacity;

	if (!cap) cap = DA_INITIAL_CAP;
	while (cap < needed) cap *= 2;
	data = realloc(data, size * cap);
	if (unlikely(data == 0))
		longjmp(vis->oom_jmp_buf, 1);

	memset((char *)data + (*capacity * size), 0, size * (cap - *capacity));
	*capacity = cap;
	return data;
}

static void *
memory_scan_reverse(const void *memory, uint8_t byte, ptrdiff_t n)
{
	void *result = 0;
	if (n > 0) {
		const uint8_t *s = memory;
		while (n) if (s[--n] == byte) { result = (void *)(s + n); break; }
	}
	return result;
}

static void *
memory_scan_forward(const void *memory, uint8_t byte, ptrdiff_t n)
{
	const uint8_t *s = memory, *end = s + n;
	while (s != end && *s != byte) s++;
	void *result = (s != end) ? (void *)s : 0;
	return result;
}

static bool
str8_equal(str8 a, str8 b)
{
	bool result = a.length == b.length;
	for (ptrdiff_t i = 0; result && i < a.length; i++)
		result = a.data[i] == b.data[i];
	return result;
}

static bool
str8_case_ignore_equal(str8 a, str8 b)
{
	bool result = a.length == b.length;
	for (ptrdiff_t i = 0; result && i < a.length; i++)
		result = (a.data[i] & ~0x20u) == (b.data[i] & ~0x20u);
	return result;
}

static str8
str8_from_c_str(const char *c_str)
{
	str8 result = {.data = (uint8_t *)c_str};
	if (c_str) while (*c_str) c_str++;
	result.length = (uint8_t *)c_str - result.data;
	return result;
}

static str8
str8_skip_space(str8 s)
{
	str8 result = s;
	for (ptrdiff_t i = 0; i < s.length && IsSpace(*result.data); result.data++);
	result.length -= result.data - s.data;
	return result;
}

static str8
str8_trim_space(str8 s)
{
	str8 result = str8_skip_space(s);
	while (result.length > 0 && IsSpace(result.data[result.length - 1])) result.length--;
	return result;
}

static void
str8_split_at(str8 s, str8 *left, str8 *right, ptrdiff_t n)
{
	if (left)  *left  = s;
	if (right) *right = (str8){0};
	if (n >= 0 && n <= s.length) {
		if (left) *left = (str8){
			.length = n,
			.data   = s.data,
		};
		if (right) *right = (str8){
			.length = MAX(0, s.length - n - 1),
			.data   = s.data + n + 1,
		};
	}
}

static void
str8_split(str8 s, str8 *left, str8 *right, uint8_t byte)
{
	str8_split_at(s, left, right, (uint8_t *)memory_scan_forward(s.data, byte, s.length) - s.data);
}

static str8
sb_to_str8(StringBuffer *sb)
{
	str8 result = {.data = sb->data, .length = sb->count};
	return result;
}

static StringBuffer
sb_from_buffer(void *buffer, VisDACount capacity)
{
	StringBuffer result = {.data = buffer, .capacity = capacity};
	return result;
}

static void
sb_reset(StringBuffer *sb, VisDACount count)
{
	sb->errors = sb->capacity <= count;
	if (!sb->errors)
		sb->count = count;
}

static void
sb_push(StringBuffer *sb, void *data, VisDACount length)
{
	sb->errors |= (sb->capacity - sb->count) < length;
	if (!sb->errors) {
		memcpy(sb->data + sb->count, data, length);
		sb->count += length;
	}
}

static void
sb_pad(StringBuffer *sb, uint8_t byte, VisDACount count)
{
	sb->errors |= (sb->capacity - sb->count) < count;
	if (!sb->errors) {
		memset(sb->data + sb->count, byte, count);
		sb->count += count;
	}
}

// NOTE(rnp): forces termination, will overwrite last byte if necessary
static void
sb_terminate(StringBuffer *sb, uint8_t byte)
{
	sb->data[MIN(sb->count, sb->capacity - 1)] = byte;
}

#define sb_push_str8s(sb, ...) sb_push_str8_list(sb, arg_list(str8, __VA_ARGS__))
static void
sb_push_str8_list(StringBuffer *sb, str8 *strs, size_t count)
{
	for (size_t i = 0; i < count; i++)
		sb_push(sb, strs[i].data, strs[i].length);
}

static void
sb_push_str8(StringBuffer *sb, str8 str)
{
	sb_push(sb, str.data, str.length);
}

// NOTE(rnp): trick to force format string to be compile time constant
#define sb_push_fv(s, f, ...) sb_push_format_vector(s, ""f, __VA_ARGS__)
static void __attribute__((format(printf, 2, 3)))
sb_push_format_vector(StringBuffer *sb, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int length = vsnprintf((char *)(sb->data + sb->count), sb->capacity - sb->count, fmt, ap);
	sb->errors |= (sb->capacity - sb->count) < length;
	if (!sb->errors) sb->count += length;
	va_end(ap);
}

static void
path_split(str8 path, str8 *directory, str8 *basename)
{
	str8 left;
	ptrdiff_t at = (uint8_t *)memory_scan_reverse(path.data, '/', path.length) - path.data;
	str8_split_at(path, &left, basename, at);

	if (basename && basename->length == 0 && at <= 0) {
		*basename = left;
		left      = (str8){0};
	}

	if (directory) *directory = left.length == 0 ? str8(".") : left;
}

static char *
absolute_path(const char *name)
{
	if (!name)
		return 0;

	str8 base, dir, string = str8_from_c_str((char *)name);
	path_split(string, &dir, &base);

	char path_resolved[PATH_MAX];
	char path_normalized[PATH_MAX];

	memcpy(path_normalized, dir.data, dir.length);
	path_normalized[dir.length] = 0;

	char *result = 0;
	if (realpath(path_normalized, path_resolved) == path_resolved) {
		if (strcmp(path_resolved, "/") == 0)
			path_resolved[0] = 0;

		StringBuffer sb = sb_from_buffer(path_normalized, (VisDACount)sizeof(path_normalized));
		sb_push_str8s(&sb, str8_from_c_str(path_resolved), str8("/"), base);
		sb_terminate(&sb, 0);
		result = strdup(path_normalized);
	}
	return result;
}

static IntegerConversion
integer_conversion(str8 raw, bool no_hex)
{
	/* NOTE: place this on its own cacheline */
	static alignas(64) int8_t lut[64] = {
		 0,  1,  2,  3,  4,  5,  6,  7,  8,  9, -1, -1, -1, -1, -1, -1,
		-1, 10, 11, 12, 13, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1,
		-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
		-1, 10, 11, 12, 13, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	};

	IntegerConversion result = {.unparsed = raw};

	ptrdiff_t i     = 0;
	int64_t   scale = 1;
	if (raw.length > 0 && raw.data[0] == '-') {
		scale = -1;
		i     =  1;
	}

	bool hex = 0;
	if (!no_hex) {
		if (raw.length - i > 2 && raw.data[i] == '0' && (raw.data[1] == 'x' || raw.data[1] == 'X')) {
			hex  = 1;
			i   += 2;
		} else if (raw.length - i > 1 && raw.data[i] == '#') {
			hex  = 1;
			i   += 1;
		}
	}

	#define integer_conversion_body(radix, clamp) do {\
		for (; i < raw.length; i++) {\
			int64_t value = lut[MIN((uint8_t)(raw.data[i] - (uint8_t)'0'), clamp)];\
			if (value >= 0) {\
				if (result.as.U64 > (U64_MAX - (uint64_t)value) / radix) {\
					result.result = IntegerConversionResult_OutOfRange;\
					result.as.U64 = U64_MAX;\
					return result;\
				} else {\
					result.as.U64 = radix * result.as.U64 + (uint64_t)value;\
				}\
			} else {\
				break;\
			}\
		}\
	} while (0)

	if (hex) integer_conversion_body(16u, 63u);
	else     integer_conversion_body(10u, 15u);

	#undef integer_conversion_body

	result.unparsed = (str8){.length = raw.length - i, .data = raw.data + i};
	result.result   = IntegerConversionResult_Success;
	if (scale < 0) result.as.U64 = 0 - result.as.U64;

	return result;
}
