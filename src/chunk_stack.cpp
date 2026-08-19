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

#include <string>
#include "chunk_stack.hpp"


GST_DEBUG_CATEGORY_EXTERN(dsdchunkparse_debug);
#define GST_CAT_DEFAULT dsdchunkparse_debug


ChunkStack::ChunkStack(GstObject *gstobject, gpointer user_data, ChunkDescriptions descriptions)
	: m_gstobject{gstobject}
	, m_user_data{user_data}
	, m_descriptions{std::move(descriptions)}
{
}


ChunkStack::~ChunkStack()
{
}


ChunkStack::ChunkStack(ChunkStack &&other)
	: m_gstobject{std::exchange(other.m_gstobject, nullptr)}
	, m_user_data{std::exchange(other.m_user_data, nullptr)}
	, m_descriptions{std::move(other.m_descriptions)}
	, m_stack{std::move(other.m_stack)}
{
}


ChunkStack& ChunkStack::operator = (ChunkStack &&other)
{
	if (this == &other)
		return *this;

	m_gstobject = std::exchange(other.m_gstobject, nullptr);
	m_user_data = std::exchange(other.m_user_data, nullptr);
	m_descriptions = std::move(other.m_descriptions);
	m_stack = std::move(other.m_stack);

	return *this;
}


void ChunkStack::clear()
{
	m_stack.clear();
}


gsize ChunkStack::depth() const
{
	return m_stack.size();
}


ChunkStack::PushReturnCode ChunkStack::push_chunk(
	guint32 fourcc,
	guint64 size,
	guint64 payload_begin_pos,
	guint64 payload_end_pos
)
{
	ChunkDescription *description;

	auto chunk_description_iter = m_descriptions.find(fourcc);
	if (G_LIKELY(chunk_description_iter != m_descriptions.end())) {
		description = &(chunk_description_iter->second);

		switch (description->size_type) {
			case ChunkSizeType::ExactSize: {
				if (size != description->size) {
					GST_ERROR_OBJECT(
						m_gstobject,
						"expected %" G_GUINT64_FORMAT " byte(s) in chunk \"%" GST_FOURCC_FORMAT "\", got %" G_GUINT64_FORMAT,
						description->size,
						GST_FOURCC_ARGS(fourcc),
						size
					);
					return PushReturnCode::IncorrectChunkSize;
				}
				break;
			}

			case ChunkSizeType::FirstNumBytes: {
				if (size < description->size) {
					GST_ERROR_OBJECT(
						m_gstobject,
						"expected a minimum of %" G_GUINT64_FORMAT " byte(s) in chunk \"%" GST_FOURCC_FORMAT "\", got %" G_GUINT64_FORMAT,
						description->size,
						GST_FOURCC_ARGS(fourcc),
						size
					);
					return PushReturnCode::InsufficientChunkSize;
				}
				break;
			}

			default:
				g_assert_not_reached();
		}

		if (!m_stack.empty()) {
			Chunk &parent_chunk = m_stack.front();

			auto &subchunk_fourccs = parent_chunk.observed_subchunk_fourccs;

			if (description->only_once && (subchunk_fourccs.find(fourcc) != subchunk_fourccs.end())) {
				GST_ERROR_OBJECT(
					m_gstobject,
					"a chunk with ID \"%" GST_FOURCC_FORMAT "\" was already seen in parent chunk with ID \"%" GST_FOURCC_FORMAT "\"",
					GST_FOURCC_ARGS(fourcc),
					GST_FOURCC_ARGS(parent_chunk.fourcc)
				);
				return PushReturnCode::AlreadySeen;
			}

			subchunk_fourccs.insert(fourcc);
		}
	} else {
		description = nullptr;
	}

	// Sanity check; check if the chunk's payload area exceeds the payload area of the parent chunk.
	if (!m_stack.empty()) {
		Chunk &parent_chunk = m_stack.front();

		if (payload_begin_pos > parent_chunk.payload_end_pos) {
			GST_ERROR_OBJECT(
				m_gstobject,
				"payload of chunk \"%" GST_FOURCC_FORMAT "\" begins at %" G_GUINT64_FORMAT ", "
				"which lies fully beyond the end of the payload of parent chunk \"%"
				GST_FOURCC_FORMAT "\" (parent's payload ends at %" G_GUINT64_FORMAT ")",
				GST_FOURCC_ARGS(fourcc),
				payload_begin_pos,
				GST_FOURCC_ARGS(parent_chunk.fourcc),
				parent_chunk.payload_end_pos
			);
			return PushReturnCode::ChunkOutOfParentBounds;
		}

		if (G_UNLIKELY(payload_end_pos > parent_chunk.payload_end_pos)) {
			GST_ERROR_OBJECT(
				m_gstobject,
				"chunk \"%" GST_FOURCC_FORMAT "\" size %" G_GUINT64_FORMAT " exceeds the end "
				"of payload of parent chunk \"%" GST_FOURCC_FORMAT "\" by %" G_GUINT64_FORMAT " byte(s)",
				GST_FOURCC_ARGS(fourcc),
				size,
				GST_FOURCC_ARGS(parent_chunk.fourcc),
				payload_end_pos - parent_chunk.payload_end_pos
			);
			return PushReturnCode::ChunkOutOfParentBounds;
		}
	}

	m_stack.push_front({
		fourcc,
		size,
		payload_begin_pos,
		payload_end_pos,
		description,
		{}
	});

	return PushReturnCode::Ok;
}


static std::string fourcc_to_string(guint32 fourcc)
{
	// Matches GST_MAKE_FOURCC's byte order: first character in the low byte.
	return std::string{
		char((fourcc >>  0) & 0xFF),
		char((fourcc >>  8) & 0xFF),
		char((fourcc >> 16) & 0xFF),
		char((fourcc >> 24) & 0xFF)
	};
}


bool ChunkStack::report_missing_required_subchunks(const Chunk &chunk)
{
	if (chunk.description == nullptr)
		return false;

	std::unordered_set<guint32> missing_required_chunk_fourccs;

	for (guint32 required_subchunk_fourcc : chunk.description->required_subchunk_fourccs) {
		if (G_UNLIKELY(chunk.observed_subchunk_fourccs.find(required_subchunk_fourcc) ==
		               chunk.observed_subchunk_fourccs.end())) {
			missing_required_chunk_fourccs.insert(required_subchunk_fourcc);
		}
	}

	if (G_LIKELY(missing_required_chunk_fourccs.empty()))
		return false;

	std::string missing_chunks_str;

	for (guint32 missing_fourcc : missing_required_chunk_fourccs) {
		if (!missing_chunks_str.empty())
			missing_chunks_str += ", ";
		missing_chunks_str += '"' + fourcc_to_string(missing_fourcc) + '"';
	}

	GST_ERROR_OBJECT(
		m_gstobject,
		"these required subchunks were never seen within chunk \"%" GST_FOURCC_FORMAT "\": %s",
		GST_FOURCC_ARGS(chunk.fourcc),
		missing_chunks_str.c_str()
	);

	return true;
}


ChunkStack::PopReturnCode ChunkStack::pop_finished_chunks(guint64 current_position)
{
	if (G_UNLIKELY(m_stack.empty()))
		return PopReturnCode::NoChunksPushed;

	const Chunk &current_chunk = m_stack.front();

	if (G_UNLIKELY(current_position > current_chunk.payload_end_pos)) {
		GST_ERROR_OBJECT(m_gstobject,
			"byte position %" G_GUINT64_FORMAT " exceeds bounds of chunk \"%" GST_FOURCC_FORMAT "\" by %" G_GUINT64_FORMAT " byte(s)",
			current_position,
			GST_FOURCC_ARGS(current_chunk.fourcc),
			(current_position - current_chunk.payload_end_pos)
		);
		return PopReturnCode::PositionOutOfChunkBounds;
	}

	// Verify the required subchunks of every chunk that is about to be finished
	// _before_ any of them is finished. A finish function must never observe a
	// chunk whose required subchunks are missing, since that chunk's contents
	// cannot be relied upon. Doing this as a separate pass also means that
	// nothing has been modified yet when this check fails, so the stack is left
	// untouched in that case.
	//
	// The loop deliberately continues after the first offending chunk, so that
	// all of them end up in the log instead of just the first one.
	{
		bool required_chunks_are_missing = false;

		for (const Chunk &chunk : m_stack) {
			if (current_position == chunk.payload_end_pos) {
				if (G_UNLIKELY(report_missing_required_subchunks(chunk)))
					required_chunks_are_missing = true;
			}
		}

		if (G_UNLIKELY(required_chunks_are_missing))
			return PopReturnCode::RequiredSubchunksMissing;
	}

	gsize num_popped = 0;

	// NOTE: The loop below mixes finish function invocation with
	// chunk removal from the stack. It is possible that several
	// chunks have been removed by the time a finish function
	// is called and returns false. In that case, the stack is
	// in an undefined state, since some other chunks might be
	// finished as well.
	//
	// However, a failed finish function is understood to indicate
	// a non-recoverable error. Thus, this is okay - it is not
	// necessary to separate finishing and chunk removal.

	// Walk the stack front to back, to finish subchunks
	// first, and their parent chunks afterwards.
	for (auto iter = m_stack.begin(); iter != m_stack.end();) {
		Chunk &chunk = *iter;

		if (current_position == chunk.payload_end_pos) {
			GST_DEBUG_OBJECT(
				m_gstobject,
				"finished reading/skipping chunk \"%" GST_FOURCC_FORMAT "\" with payload size %" G_GUINT64_FORMAT " byte(s) (nesting level: %" G_GSIZE_FORMAT ")",
				GST_FOURCC_ARGS(chunk.fourcc),
				chunk.size,
				m_stack.size() - 1
			);

			// The required subchunks were already verified in the pass above.
			if ((chunk.description != nullptr) && chunk.description->finish_function) {
				bool success = chunk.description->finish_function(m_user_data, chunk);
				if (G_UNLIKELY(!success))
					return PopReturnCode::FinishFunctionFailed;
			}

			// The chunk is done, so remove it from the chunk stack.
			iter = m_stack.erase(iter);
			num_popped++;
		} else {
			++iter;
		}
	}

	return (num_popped > 0) ? PopReturnCode::Ok : PopReturnCode::NoChunksFinished;
}


bool ChunkStack::has_missing_required_subchunks()
{
	bool retval = false;

	// Deliberately not stopping at the first offending chunk,
	// so that all of them end up in the log.
	for (const Chunk &chunk : m_stack) {
		if (G_UNLIKELY(report_missing_required_subchunks(chunk)))
			retval = true;
	}

	return retval;
}


const Chunk* ChunkStack::get_current_chunk() const
{
	return m_stack.empty() ? nullptr : &(m_stack.front());
}


bool ChunkStack::verify_advance(guint64 position, guint64 advance_amount, std::string_view advance_name)
{
	if (m_stack.empty())
		return true;

	guint64 advanced_position = position + advance_amount;
	const Chunk &current_chunk = m_stack.front();

	if (G_UNLIKELY(advanced_position > current_chunk.payload_end_pos)) {
		GST_ERROR_OBJECT(m_gstobject,
			"attempt to advance byte position by %" G_GUINT64_FORMAT " byte(s) for the "
			"\"%.*s\" operation exceeds bounds of chunk \"%" GST_FOURCC_FORMAT "\" by %"
			G_GUINT64_FORMAT " byte(s)",
			advance_amount,
			int(advance_name.length()),
			advance_name.data(),
			GST_FOURCC_ARGS(current_chunk.fourcc),
			(advanced_position - current_chunk.payload_end_pos)
		);
		return false;
	}

	return true;
}
