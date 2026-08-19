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

#include <utility>
#include "mapped_buffer.hpp"


MappedBuffer::MappedBuffer(GstBuffer *buffer, GstMapFlags map_flags)
	: m_buffer{buffer}
{
	g_return_if_fail(m_buffer != nullptr);

	GstMapInfo map_info;
	if (gst_buffer_map(buffer, &map_info, map_flags)) {
		m_map_info = map_info;
	} else {
		gst_buffer_unref(m_buffer);
		m_buffer = nullptr;
	}
}


MappedBuffer::MappedBuffer(GstBuffer *buffer, GstMapInfo map_info)
	: m_buffer{buffer}
{
	g_return_if_fail(m_buffer != nullptr);

	// Asssigning this here, and not in the initializer list, to
	// ensure the bool operator returns false (g_return_if_fail()
	// exits before this is reached).
	m_map_info = map_info;
}


MappedBuffer::~MappedBuffer()
{
	if (m_buffer == nullptr)
		return;

	// Either m_buffer is non-null and m_map_info is valid,
	// or m_buffer is null and m_map_info is invalid.
	g_assert(m_map_info.has_value());
	gst_buffer_unmap(m_buffer, &(*m_map_info));
	gst_buffer_unref(m_buffer);
}


MappedBuffer::MappedBuffer(MappedBuffer &&other)
	: m_buffer{std::exchange(other.m_buffer, nullptr)}
	, m_map_info{std::move(other.m_map_info)}
{
}


MappedBuffer& MappedBuffer::operator = (MappedBuffer &&other)
{
	if (this == &other)
		return *this;

	if (m_buffer != nullptr) {
		// Either m_buffer is non-null and m_map_info is valid,
		// or m_buffer is null and m_map_info is invalid.
		g_assert(m_map_info.has_value());
		gst_buffer_unmap(m_buffer, &(*m_map_info));
		gst_buffer_unref(m_buffer);
	}

	m_buffer = std::exchange(other.m_buffer, nullptr);
	m_map_info = std::move(other.m_map_info);

	return *this;
}

const guint8* MappedBuffer::data() const
{
	return (m_map_info.has_value()) ? m_map_info->data : nullptr;
}

guint64 MappedBuffer::size() const
{
	return (m_map_info.has_value()) ? m_map_info->size : 0;
}

MappedBuffer::operator bool() const
{
	return m_map_info.has_value();
}
