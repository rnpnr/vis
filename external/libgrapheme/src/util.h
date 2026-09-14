/* See LICENSE file for copyright and license details. */
#ifndef GRAPHEME_UTIL_H
#define GRAPHEME_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../grapheme.h"

#define LEN(a) (sizeof(a) / sizeof((*a)))

/*
 * Herodotus, the ancient greek historian and geographer,
 * was criticized for including legends and other fantastic
 * accounts into his works, among others by his contemporary
 * Thucydides.
 *
 * The Herodotus readers and writers are tailored towards the needs
 * of the library interface, doing all the dirty work behind the
 * scenes. While the reader is relatively faithful in his accounts,
 * the Herodotus writer will never fail and always claim to write the
 * data. Internally, it only writes as much as it can, and will simply
 * keep account of the rest. This way, we can properly signal truncation.
 *
 * In this sense, explaining the naming, the writer is always a bit
 * inaccurate in his accounts.
 *
 */
enum herodotus_status {
	HERODOTUS_STATUS_SUCCESS,
	HERODOTUS_STATUS_END_OF_BUFFER,
	HERODOTUS_STATUS_SOFT_LIMIT_REACHED,
};

enum herodotus_type {
	HERODOTUS_TYPE_CODEPOINT,
	HERODOTUS_TYPE_UTF8,
};

typedef struct herodotus_reader {
	enum herodotus_type type;
	const void *src;
	size_t srclen;
	size_t off;
	bool terminated_by_null;
	size_t soft_limit[10];
} HERODOTUS_READER;

typedef struct herodotus_writer {
	enum herodotus_type type;
	void *dest;
	size_t destlen;
	size_t off;
	size_t first_unwritable_offset;
} HERODOTUS_WRITER;

struct proper {
	/*
	 * prev_prop[1] prev_prop[0] | next_prop[0] next_prop[1]
	 */
	struct {
		uint_least8_t prev_prop[2];
		uint_least8_t next_prop[2];
	} raw, skip;

	HERODOTUS_READER mid_reader, raw_reader, skip_reader;
	void *state;
	uint_least8_t no_prop;
	uint_least8_t (*get_break_prop)(uint_least32_t);
	bool (*is_skippable_prop)(uint_least8_t);
	void (*skip_shift_callback)(uint_least8_t, void *);
};

static void herodotus_reader_init(HERODOTUS_READER *, enum herodotus_type,
                           const void *, size_t);
static void herodotus_reader_copy(const HERODOTUS_READER *, HERODOTUS_READER *);
static void herodotus_reader_push_advance_limit(HERODOTUS_READER *, size_t);
static void herodotus_reader_pop_limit(HERODOTUS_READER *);
static size_t herodotus_reader_number_read(const HERODOTUS_READER *);
static size_t herodotus_reader_next_word_break(const HERODOTUS_READER *);
static size_t herodotus_reader_next_codepoint_break(const HERODOTUS_READER *);
static enum herodotus_status herodotus_read_codepoint(HERODOTUS_READER *, bool,
                                                      uint_least32_t *);

static void herodotus_writer_init(HERODOTUS_WRITER *, enum herodotus_type, void *,
                                  size_t);
static void herodotus_writer_nul_terminate(HERODOTUS_WRITER *);
static size_t herodotus_writer_number_written(const HERODOTUS_WRITER *);
static void herodotus_write_codepoint(HERODOTUS_WRITER *, uint_least32_t);

static void proper_init(const HERODOTUS_READER *, void *, uint_least8_t,
                        uint_least8_t (*get_break_prop)(uint_least32_t),
                        bool (*is_skippable_prop)(uint_least8_t),
                        void (*skip_shift_callback)(uint_least8_t, void *),
                        struct proper *);
static int proper_advance(struct proper *);

#endif /* UTIL_H */
