/* GStreamer DSD container formats parser plugin
 * Copyright (C) 2026 Carlos Rafael Giani <crg7475@mailbox.org>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <optional>
#include <gst/gst.h>


/**
 * Helper class for handling GStreamer buffer mappings in an RAII-friendly manner.
 *
 * For accessing the data in a GstBuffer, it must first be mapped. This
 * mapping must be undone by calling  gst_buffer_unmap(). This is a
 * task that is suitable for RAII usage. Also, in many cases, once the
 * buffer is unmapped, the buffer itself is no longer needed, and thus
 * can - and should - be unref'd.
 *
 * This class is meant for such cases. It takes a buffer, maps it, and
 * in its destructor, unmaps the buffer, then unrefs it. This means
 * that it takes ownership over the buffer. Alternatively, it can accept
 * an externally performed buffer mapping, and unmap it then, RAII-style.
 *
 * Should the mapping in the constructor have failed, the MappedBuffer's
 * bool operator returns false.
 *
 * \code{.cpp}
 *
 * {
 *     // Map my_buffer with the GST_MAP_READ.
 *     MappedBuffer my_mapped_buffer{my_buffer, GST_MAP_READ};
 *
 *     if (!my_mapped_buffer) {
 *         report_mapping_error();
 *         // Upon exiting here, my_buffer is automatically unref'd.
 *         // (It is not unmapped, since mapping failed.)
 *         return;
 *     }
 *
 *     do_something_with_the_buffer_data(
 *         my_mapped_buffer.data(),
 *         my_mapped_buffer.size()
 *     );
 *
 *     // at the end of the scope, my_buffer is automatically
 *     // unmapped and unref'd.
 * }
 * \endcode
 */
class MappedBuffer
{
public:
	/**
	 * Constructor.
	 *
	 * Maps the given buffer with the specified map flags.
	 *
	 * If mapping fails, the bool operator will return false.
	 *
	 * This takes ownership over the buffer. The destructor unrefs
	 * the buffer. Should the buffer still be needed after this
	 * MappedBuffer instance is destroyed, ref the buffer before
	 * passing it to this constructor.
	 *
	 * @param buffer Buffer to map. Must not be null.
	 * @param map_flags Mapping flags to use.
	 */
	explicit MappedBuffer(GstBuffer *buffer, GstMapFlags map_flags);
	/**
	 * Constructor.
	 *
	 * This is a variant of the constructor above that does not map
	 * the buffer. Instead, it accepts a buffer mapping that was
	 * done by the caller.
	 *
	 * \important The mapping cannot be verified, so the caller must make
	 * sure the mapping is valid and is associated with the given buffer.
	 * Otherwise, undefined behavior occurs.
	 *
	 * This takes ownership over the buffer. The destructor unrefs
	 * the buffer. Should the buffer still be needed after this
	 * MappedBuffer instance is destroyed, ref the buffer before
	 * passing it to this constructor.
	 *
	 * @param buffer Buffer that is mapped. Must not be null.
	 * @param map_info The buffer mapping.
	 */
	explicit MappedBuffer(GstBuffer *buffer, GstMapInfo map_info);

	/**
	 * Destructor.
	 *
	 * Unmaps the buffer (unless the mapping in the constructor
	 * failed), then unrefs the buffer.
	 */
	~MappedBuffer();

	// Disallow copying.
	MappedBuffer(const MappedBuffer &) = delete;
	MappedBuffer& operator = (const MappedBuffer &) = delete;

	// Allow moving.
	MappedBuffer(MappedBuffer &&other);
	MappedBuffer& operator = (MappedBuffer &&other);

	/**
	 * Returns a pointer to the mapped buffer data.
	 *
	 * Should mapping have failed, this returns null.
	 *
	 * @return Pointer to the mapped buffer data.
	 */
	const guint8* data() const;

	/**
	 * Returns the size of the mapped buffer data, in bytes
	 *
	 * Should mapping have failed, this returns 0.
	 *
	 * @return Size of the mapped buffer data, in bytes.
	 */
	guint64 size() const;

	/**
	 * Returns true if mapping in the constructor succeeded, false otherwise.
	 *
	 * Should the constructor that accepts an external mapping have been
	 * used to construct this MappedBuffer instance, this operator will
	 * always return true.
	 */
	explicit operator bool() const;

private:
	GstBuffer *m_buffer;
	std::optional<GstMapInfo> m_map_info;
};


/**
 * Convenience macro for handling failed MappedBuffer mappings.
 *
 * If the bool operator of specified MappedBuffer instance
 * (MAPPED_BUFFER) returns true, this macro does nothing.
 * Otherwise, it reports an element error, and returns
 * from the current scope using RETVAL as the return value.
 *
 * Example usage:
 *
 * \code{.cpp}
 *
 * MappedBuffer my_mapped_buffer{my_buffer, GST_MAP_READ};
 *
 * // If the my_mapped_buffer bool operator returns false,
 * // this will posts an element error to my_element, add
 * // the format string at the end as the debugging information,
 * // and then return error_return_value.
 * RETURN_IF_GSTBUFFER_MAPPING_FAILED(
 *     my_element,
 *     my_mapped_buffer,
 *     error_return_value,
 *     "my buffer could not be mapped; extra information: %d", some_integer_number
 * )
 *
 * \endcode
 *
 * @param ELEMENT GStreamer element to report the error to.
 * @param MAPPED_BUFFER MappedBuffer instance to evaluate.
 * @param RETVAL Value that is used when the current scope
 *   is returned from.
 * @param ERRMSG_FORMAT_STR Format string, used as debugging
 *   information in the reported element error.
 */
#define RETURN_IF_GSTBUFFER_MAPPING_FAILED(ELEMENT, MAPPED_BUFFER, RETVAL, ERRMSG_FORMAT_STR, ...) \
	G_STMT_START { \
		if (!(MAPPED_BUFFER)) { \
			GST_ELEMENT_ERROR( \
				(ELEMENT), \
				STREAM, \
				FAILED, \
				("Buffer mapping error."), \
				(ERRMSG_FORMAT_STR __VA_OPT__(,) __VA_ARGS__) \
			); \
			return (RETVAL); \
		} \
	} G_STMT_END
