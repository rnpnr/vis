#ifndef BUFFER_H
#define BUFFER_H

#include <stddef.h>
#include <stdbool.h>
#include "text.h"

/**
 * @file
 * A dynamically growing buffer storing arbitrary data.
 * @rst
 * .. note:: Used for Register, *not* Text content.
 * @endrst
 */

/** A dynamically growing buffer storing arbitrary data. */
typedef struct {
	char *data;    /**< Data pointer, ``NULL`` if empty. */
	size_t len;    /**< Current length of data. */
	size_t size;   /**< Maximal capacity of the buffer. */
} Buffer;

/** Release all resources, reinitialize buffer. */
void buffer_release(Buffer*);
/** Reserve space to store at least ``size`` bytes.*/
bool buffer_reserve(Buffer*, size_t size);
/** Reserve space for at least ``len`` *more* bytes. */
bool buffer_grow(Buffer*, size_t len);
/** If buffer is non-empty, make sure it is ``NUL`` terminated. */
bool buffer_terminate(Buffer*);
/** Set buffer content, growing the buffer as needed. */
bool buffer_put(Buffer*, const void *data, size_t len);
/** Set buffer content to ``NUL`` terminated data. */
bool buffer_put0(Buffer*, const char *data);
/** Remove ``len`` bytes starting at ``pos``. */
bool buffer_remove(Buffer*, size_t pos, size_t len);
/** Insert NUL-terminated data at pos. */
bool buffer_insert0(Buffer*, size_t pos, const char *data);
/** Append further content to the end. */
bool buffer_append(Buffer*, const void *data, size_t len);
/** Append NUL-terminated data. */
bool buffer_append0(Buffer*, const char *data);
/** Append formatted buffer content, ensures NUL termination on success. */
bool buffer_appendf(Buffer*, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/** Append variadic arguments to buffer with format string */
bool buffer_vappendf(Buffer *, const char *fmt, va_list) __attribute__((format(printf, 2, 0)));
/** Return length of a buffer without trailing NUL byte. */
size_t buffer_length0(Buffer*);
/**
 * Get pointer to buffer data.
 * Guaranteed to return a NUL terminated string even if buffer is empty.
 */
const char *buffer_content0(Buffer*);

/** ``read(3p)`` like interface for reading into a Buffer (``context``) */
ssize_t read_into_buffer(void *context, char *data, size_t len);

#define buffer_to_s8(buf) (s8){.data = (u8 *)(buf)->data, .len = (buf)->len}

#endif
