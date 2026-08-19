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

#include <list>
#include <string_view>
#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <gst/gst.h>


enum class ChunkSizeType
{
	ExactSize,
	FirstNumBytes,
};


struct Chunk;


// NOTE: These are implemented as function pointers, not C++ function objects,
// since in this case, the latter actually do not yield significant advantages -
// these parse functions are implemented as free functions, following the
// GLib / GStreamer conventions.

/**
 * Chunk parse function pointer.
 *
 * This is called for when the associated chunk is to be parsed.
 *
 * Should parsing succeed, this must return GST_FLOW_OK. Otherwise, the
 * appropriate flow error return code must be returned.
 *
 * @param user_data the user_data that is passed to the ChunkStack.
 * @chunk A const reference to information about the chunk that this parses.
 *   Usually, if implementations make use of it, they access the chunk size.
 * @return Flow return code.
 */
typedef GstFlowReturn (*ChunkParseFunction)(gpointer user_data, const Chunk &chunk);

/**
 * Chunk finishing function pointer.
 *
 * This is called for when the associated chunk is to be finished.
 * If finishing succeeds, this returns true, otherwise false. false
 * indicates a non-recoverable error. The chunk stack should not be
 * used then anymore.
 *
 * @param user_data The user_data that is passed to the ChunkStack.
 * @chunk A const reference to information about the chunk that this parses.
 *   Usually, if implementations make use of it, they access the chunk size.
 * @return true if finishing succeeds.
 */
typedef bool (*FinishChunkFunction)(gpointer user_data, const Chunk &chunk);

/**
 * Description about a specific chunk.
 *
 * This is used by ChunkStack to correctly handle chunks that it finds.
 */
struct ChunkDescription
{
	/**
	 * If true, the chunk is supposed to be present only once inside its parent chunk.
	 *
	 * This flag is ignored if a root chunk is the one being parsed,
	 * since it has no parent.
	 */
	bool only_once;

	/**
	 * Set containing fourCCs of subchunks that must be present in this chunk.
	 */
	std::unordered_set<guint32> required_subchunk_fourccs;

	/**
	 * How to interpret the size value.
	 *
	 * - ChunkSizeType::ExactSize : size specifies the exact size of the
	 *   chunk's entire payload.
	 * - ChunkSizeType::FirstNumBytes : size specifies the minimum amount
	 *   of bytes that must be present in the payload.
	 *
	 * This does not affect the parsing itself - this is purely for chunk
	 * validation. The base class will compare the actual chunk size against
	 * this and the size value to check for corrupted data.
	 *
	 * Should this be a chunk that solely contains subchunks,
	 * the size_type must be ChunkSizeType::FirstNumBytes.
	 */
	ChunkSizeType size_type;

	/**
	 * Expected exact or minimum size of the chunk, in bytes.
	 *
	 * See size_type above for details.
	 *
	 * Should this be a chunk that solely contains subchunks,
	 * this needs to be set to 0, since such a chunk has no
	 * regular payload.
	 *
	 * \note This can be 1 byte less than the difference between
	 * a chunk's payload start and payload end positions. This is
	 * because there can be a padding byte after the payload. Some
	 * formats exclude the padding byte from the size, but the
	 * payload end position may still include it for parsing reasons.
	 * Also be aware that some format _include_ the padding byte
	 * in this size, and even within the same format, actual media
	 * may specify this inconsistently (DFF is one such example).
	 */
	guint64 size;

	/**
	 * Parse function called when reading the chunk's contents.
	 *
	 * This must be a valid pointer.
	 */
	ChunkParseFunction parse_function;

	/**
	 * Finish function called when a chunk was reported as finished.
	 *
	 * This is optional. If set to null, no such call is made.
	 */
	FinishChunkFunction finish_function = nullptr;
};

/**
 * STL unordered_map made of chunk descriptions.
 *
 * The key is the chunk fourCC.
 */
typedef std::unordered_map<guint32, ChunkDescription> ChunkDescriptions;


/** Details about a chunk that is being parsed. */
struct Chunk
{
	/** fourCC of the chunk. */
	guint32 fourcc;

	/**
	 * Size of the chunk, in bytes.
	 *
	 * This does not include the 12 bytes of the chunk header. Should
	 * the underlying format include the 12 bytes, callers must first
	 * subtract these 12 bytes before using ChunkStack::push_chunk().
	 *
	 * \important This is _not_ necessarily the same as
	 * (payload_end_pos - payload_begin_pos). If the chunk is followed by
	 * a padding byte that the format excludes from the size, then
	 * payload_end_pos covers that byte while this size field does not,
	 * making the difference 1 byte larger than this value. See the
	 * ChunkDescription size field documentation for details about padding.
	 *
	 * Consequently, parse functions must derive the amount of payload that
	 * is left to read from (payload_end_pos - current byte position), and
	 * never from this size field. Using this field would leave the padding
	 * byte unaccounted for across repeated partial reads.
	 */
	guint64 size;

	/** Position within the file where this chunk's payload begins, in bytes.
	 *
	 * This is inclusive, that is, the byte position it points
	 * to is where the very first payload byte is located.
	 */
	guint64 payload_begin_pos;

	/**
	 * Positions within the file where this chunk's payload ends, in bytes.
	 *
	 * payload_end_pos is exclusive, that is, it is exactly
	 * one byte past the payload. From that it follows that:
	 *
	 *   payload_end_pos = payload_begin_pos + payload_size
	 *
	 * This follows the convention that STL iterator ranges use;
	 * the begin iterator is inclusive, the end iterator is exclusive.
	 *
	 * \note The illustration above assumes that there are no padding
	 * bytes. If so, it is more complicated. See the ChunkDescription
	 * size field documentation for details.
	 */
	guint64 payload_end_pos;

	/**
	 * Description for this chunk.
	 *
	 * This is automatically filled by ChunkStack when this chunk is parsed.
	 */
	ChunkDescription *description;

	/**
	 * The fourCCs of the subchunks encountered during parsing.
	 *
	 * This is important if a subchunk is supposed to be present
	 * only once within a parent chunk. The set then allows for
	 * detecting duplicates.
	 *
	 * This is only used if this chunk contains subchunks.
	 */
	std::unordered_set<guint32> observed_subchunk_fourccs;
};


/**
 * Helper class for keeping track of chunk and subchunk parsing.
 *
 * When parsing chunks with nested subchunks, it is important to keep track of
 * the chunk that is currently being parsed, and whether it is a subchunk that
 * is nested in a parent chunk. If for example a chunk is fully parsed, and thus
 * is finished, it is important to evaluate whether parent chunks are also now
 * finished. And, when a subchunk is located, it is good to verify that its
 * bounds do not exceed those of the parent class (which otherwise indicates
 * corrupted data).
 *
 * An internal stack is used for this purpose. When a chunk is located,
 * push_chunk() is called, with all of the chunk's details. Should a subchunk
 * within this chunk be found, push_chunk() is called, and the subchunk's
 * details are pushed into the stack, which now has depth 2 (the parent chunk
 * and this new subchunk). When a chunk has been fully parsed, pop_finished_chunks()
 * is called. This walks through the stack and determines if finishing a chunk
 * also finishes its parent chunk.
 *
 * When chunks are finished, pop_finished_chunks() will call the finish_function
 * of their descriptions (if the finish_function is set).
 *
 * The ChunkDescription parse_function is not used by this class. It is meant
 * for callers to use.
 *
 * The verify_advance() function can be used by callers to verify that advancing
 * in the current chunk's payload does not exceed its bounds.
 *
 * This class uses the notion of a "byte position" or "current position". This
 * is the current position, in bytes, within the chunk basedf media that is
 * currently being parsed. Position 0 denotes the very beginning of the media.
 */
class ChunkStack
{
public:
	/**
	 * push_chunk() return code.
	 *
	 * The chunk will be pushed to the stack only when Ok is returned.
	 */
	enum class PushReturnCode
	{
		/** The chunk is OK, and is now pushed on the stack. */
		Ok,

		/**
		 * The chunk is not supposed to occur more than once within its parent.
		 *
		 * This is relevant if there is a parent chunk (that is, this is not
		 * a root chunk), this chunk's description has its only_once flag set,
		 * and another chunk with the same fourCC was already observed within
		 * the parent chunk.
		 */
		AlreadySeen,

		/**
		 * The chunk's actual size does not exactly match the expected value.
		 *
		 * This is returned if the chunk description's size_type is set to
		 * ChunkSizeType::ExactSize, and the chunk's actual size does not
		 * exactly match the chunk description's size.
		 */
		IncorrectChunkSize,

		/**
		 * The chunk's actual size is not the expected minimum.
		 *
		 * This is returned if the chunk description's size_type is set to
		 * ChunkSizeType::FirstNumbytes, and the chunk's actual size is not
		 * at least as large as the chunk description's size.
		 */
		InsufficientChunkSize,

		/**
		 * The chunk exceeds the bounds of its parent.
		 *
		 * This indicates corrupted data. In correct data, subchunks never
		 * reach past the size of their parent chunks.
		 */
		ChunkOutOfParentBounds
	};

	/**
	 * pop_finished_chunks() return code.
	 *
	 * The stack is modified only when Ok or FinishFunctionFailed is returned.
	 * In the latter case it is left in an undefined state. See that code's
	 * description.
	 */
	enum class PopReturnCode
	{
		/**
		 * All chunks that could be finished were finished.
		 *
		 * If the descriptions of the finished chunks have a finish function set,
		 * then these finish functions were called for all finished chunks.
		 *
		 * This return code implies that at least one chunk was finished.
		 * See NoChunksFinished for the case when none were.
		 */
		Ok,

		/** The chunk stack is empty - there are no chunks. */
		NoChunksPushed,

		/**
		 * The chunk stack is not empty, but none of the chunks in the stack could be finished.
		 *
		 * This return code implies that the stack was not modified.
		 */
		NoChunksFinished,

		/**
		 * During the chunk finishing process, one finish function failed.
		 *
		 * A failure is indicated by the finish function returning false. This
		 * is interpreted as a non-recoverable error. pop_finished_chunks() will
		 * then immediately abort and return this code. The chunk stack should
		 * not be used anymore afterwards, since it is in an undefined state.
		 *
		 * One example case is when a finish function is used for validating
		 * the data that was parsed. If this is essential data, and it turns out
		 * to be invalid, that example function returns false. This media is then
		 * invalid, and cannot be used. Stopping the overall parse process then
		 * makes sense; essential data is corrupted/invalid.
		 */
		FinishFunctionFailed,

		/**
		 * The current position is out of bounds of the current chunk.
		 *
		 * This is actually an extra sanity check for catching buggy parse functions.
		 * Normally, they should use verify_advance() to check before reading from
		 * the chunk's payload to see if they are still within the bounds of the
		 * chunk. But, it does not hurt to also check at the end whether the parse
		 * function went past the end of the payload.
		 */
		PositionOutOfChunkBounds,

		/**
		 * When finishing a chunk, it was discovered that at least one required subchunk is missing.
		 *
		 * Some chunks may require certain subchunks to be present in its payload.
		 * If these subchunks aren't there, this is interpreted as a non-recoverable
		 * error caused by invalid or corrupted data.
		 *
		 * This is checked for every chunk that is about to be finished _before_
		 * any of those chunks are actually finished. Consequently, when this code
		 * is returned, no finish function has been called and no chunk has been
		 * popped - the stack is left exactly as it was. This matters because a
		 * finish function must never observe a chunk whose required subchunks are
		 * missing; such a chunk's contents cannot be relied upon.
		 *
		 * The offending chunks and their missing subchunks are logged by
		 * pop_finished_chunks() itself.
		 */
		RequiredSubchunksMissing
	};

	/**
	 * Constructor.
	 *
	 * Sets up the chunk stack.
	 *
	 * The provided descriptions are stored inside and used in push_chunk() to
	 * associate a parsed chunk with a description. This is also used to detect
	 * unknown chunks - those are chunks that have no associated description.
	 * The description is matched with the chunk via its fourCC.
	 *
	 * @param gstobject Pointer to a GstObject, purely used for logging
	 *   with the GStreamer logger. Must not be null.
	 * @param user_data User defined pointer that is passed to the parse
	 *   and finish functions in the descriptions.
	 * @param descriptions Chunk descriptions this stack shall use to identify
	 *   and associate chunks with.
	 */
	explicit ChunkStack(GstObject *gstobject, gpointer user_data, ChunkDescriptions descriptions);

	/**
	 * Destructor.
	 *
	 * Note that this does _not_ automatically pop finished chunks.
	 */
	~ChunkStack();

	// Disallow copying.
	ChunkStack(const ChunkStack &) = delete;
	ChunkStack& operator = (const ChunkStack &) = delete;

	// ChunkStack can be moved.
	ChunkStack(ChunkStack &&other);
	ChunkStack& operator = (ChunkStack &&other);

	/** Clears all chunks from the stack, resetting the stack to its initial state. */
	void clear();

	/**
	 * Returns the current stack depth.
	 *
	 * This is the number of chunks that are currently present on the stack.
	 */
	gsize depth() const;

	/**
	 * Pushes a newly found chunk on the stack.
	 *
	 * This is called during the parse process when a chunk is found. Its
	 * information is entered here, a Chunk instance is created out of that
	 * information, and that instance is pushed on the stack. To access that
	 * instance, call get_current_chunk() right after this.
	 *
	 * @param fourcc The Chunk's fourCC.
	 * @param size The Chunk's size, in bytes. This does not include the
	 *   size of the chunk header (= the fourcc and the size integer itself).
	 * @param payload_begin_pos Byte position in the media where the payload
	 *   begins. This usually is right after the chunk header. This position
	 *   is inclusive; it points to the first byte of the payload.
	 * @param payload_end_pos Byte position in the media of the first byte
	 *   that comes after the payload. This means that position is exclusive;
	 *   it does not point to the last byte within the payload, but to the
	 *   first byte past the payload. If the format places a padding byte
	 *   after the payload and excludes it from the chunk size, this position
	 *   must include that padding byte, and is then one byte past
	 *   (payload_begin_pos + size). See the Chunk size field documentation.
	 * @return Outcome of the push. See PushReturnCode for details.
	 */
	PushReturnCode push_chunk(
		guint32 fourcc,
		guint64 size,
		guint64 payload_begin_pos,
		guint64 payload_end_pos
	);

	/**
	 * Pops all chunks that are now finished.
	 *
	 * A chunk is considered to be finished if the current position equals
	 * the chunk's payload_end_pos position. In case of chunks that contain
	 * subchunks, it is possible that the current position also matches
	 * their payload_end_pos position. This is the case when the parent
	 * chunk's very last subchunk just got finished as well. For that reason,
	 * this function walks through the entire stack to see what chunks are
	 * finished.
	 *
	 * Finished chunks are removed from the stack; depth() then returns
	 * a lower value than before the finishing. Should the description
	 * associated with a finished chunk contain a finish function, it
	 * is invoked right before the chunk is removed from the stack.
	 *
	 * @param current_position Current byte position in the media, in bytes.
	 * @return Outcome of the pop. See PopReturnCode for details.
	 */
	PopReturnCode pop_finished_chunks(guint64 current_position);

	/**
	 * Checks all chunks that are currently on the stack for missing required subchunks.
	 *
	 * Normally, the required subchunks of a chunk are verified by
	 * pop_finished_chunks() when that chunk is finished. Should parsing end
	 * before some chunks could be finished, those chunks never undergo that
	 * check. This function exists for that case: it walks the _entire_ stack,
	 * not just the current chunk, since unfinished chunks can be nested at any
	 * depth. Chunks that were already finished are no longer on the stack and
	 * were checked when they were popped, so this covers exactly the chunks
	 * that pop_finished_chunks() never got to.
	 *
	 * The offending chunks and their missing subchunks are logged by this
	 * function itself. The stack is not modified.
	 *
	 * \note The return value indicates a _problem_, unlike for example
	 * verify_advance(), which returns true when everything is in order.
	 *
	 * @return true if at least one chunk on the stack is missing at least one
	 *   of its required subchunks, false if all of them are complete.
	 */
	bool has_missing_required_subchunks();

	/**
	 * Returns the chunk that is currently being parsed.
	 *
	 * This equals the chunk that is at the top of the stack. This
	 * is important to remember when the media has chunk hierarchies.
	 *
	 * For example, imagine a root chunk R with 3 subchunks inside
	 * (let's call them A, B, C), and subchunk A in turn has its
	 * own subchunks X and Y. While Y is being parsed, this will
	 * return a pointer to the Chunk instance that corresponds to
	 * Y (and this Chunk instance is also the one at the top of the
	 * internal stack). When Y is fully parsed, pop_finished_chunks()
	 * is called. Y is then finished, and thus popped. Since Y is
	 * the last subchunk in A, A is also popped. Consequently, after
	 * the pop_finished_chunks() ends, both Y and A are gone. Calling
	 * get_current_chunk() afterwards will no longer return Y, but
	 * R, since B has not been encountered yet. Once it is found,
	 * push_chunk() is called and B is pushed on the stack. Calling
	 * get_current_chunk() then will return B.
	 *
	 * \caution Do not assume that the Chunk pointer that this function
	 * returned still is valid after a pop_finished_chunks() call,
	 * since that chunk may have been removed by that function.
	 *
	 * @return The current chunk, or null if the stack is empty.
	 */
	const Chunk* get_current_chunk() const;

	/**
	 * Checks whether advancing from the given position by the given amount is okay.
	 *
	 * An advance is okay if it remains within the bounds of the current
	 * chunk. That is, if checks whether (position + advance) is less than
	 * or equal to the payload_end_pos of the chunk. If it is, this returns
	 * true. If it exceeds the end of the payload, it returns false.
	 *
	 * @param position Position that will be advanced, in bytes.
	 * @param advance_amount Advance amount, in bytes.
	 * @advance_name Descriptive name of the advance. This is purely used
	 *   for logging, to give useful context to the advance. Examples are
	 *   "read" (for read operations) and "skip" (for skipping operations).
	 * @return true if the advance stays within chunk bounds, false otherwise.
	 *   Should the stack be empty, this always returns true.
	 */
	bool verify_advance(guint64 position, guint64 advance_amount, std::string_view advance_name);

private:
	/**
	 * Checks one chunk for missing required subchunks, and logs any that are missing.
	 *
	 * @param chunk Chunk to check. Chunks without a description are treated as
	 *   complete, since there is nothing that could specify requirements for them.
	 * @return True if at least one required subchunk is missing, false otherwise.
	 */
	bool report_missing_required_subchunks(const Chunk &chunk);

	GstObject *m_gstobject;
	gpointer m_user_data;
	ChunkDescriptions m_descriptions;
	// Using std::list as the stack data structure to be able to efficiently
	// remove items in the middle of the stack in pop_finished_chunks().
	// The top of the stack equals the front of the list.
	std::list<Chunk> m_stack;
};
