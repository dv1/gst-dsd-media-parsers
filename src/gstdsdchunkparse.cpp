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

#include <gst/gst.h>
#include "gstdsdchunkparse.hpp"
#include "scope_guard.hpp"
#include "mapped_buffer.hpp"


GST_DEBUG_CATEGORY_EXTERN(dsdchunkparse_debug);
#define GST_CAT_DEFAULT dsdchunkparse_debug


enum class ChunkParseStage {
	ParsingChunkStart,
	ParsingChunkContent,
	FinishingChunk
};


struct GstDsdChunkParsePrivate
{
	ChunkParseStage chunk_parse_stage;
	ChunkSizeEndianness chunk_size_endianness;
	bool chunk_size_includes_chunk_header;
	ChunkStack *chunk_stack;
};


G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE(
	GstDsdChunkParse,
	gst_dsd_chunk_parse,
	GST_TYPE_DSD_MEDIA_PARSE
);


static void gst_dsd_chunk_parse_finalize(GObject *object);

static bool gst_dsd_chunk_parse_setup(GstDsdMediaParse *parse);
static void gst_dsd_chunk_parse_teardown(GstDsdMediaParse *parse);

static GstFlowReturn gst_dsd_chunk_parse_scan_info(GstDsdMediaParse *parse);

static bool gst_dsd_chunk_parse_verify_advance(GstDsdMediaParse *parse, guint64 byte_position, guint64 advance_amount, const gchar *advance_name);

static GstFlowReturn gst_dsd_chunk_parse_parse_chunk_start(GstDsdChunkParse *self);
static GstFlowReturn gst_dsd_chunk_parse_parse_chunk_content(GstDsdChunkParse *self);
static GstFlowReturn gst_dsd_chunk_parse_finish_chunk_parsing(GstDsdChunkParse *self);


static inline GstDsdChunkParsePrivate* get_private(GstDsdChunkParse *self)
{
	return reinterpret_cast<GstDsdChunkParsePrivate *>(gst_dsd_chunk_parse_get_instance_private(self));
}


static void gst_dsd_chunk_parse_class_init(GstDsdChunkParseClass *klass)
{
	GObjectClass *object_class;
	GstDsdMediaParseClass *dsd_media_parse_class;

	object_class = G_OBJECT_CLASS(klass);
	dsd_media_parse_class = GST_DSD_MEDIA_PARSE_CLASS(klass);

	object_class->finalize = GST_DEBUG_FUNCPTR(gst_dsd_chunk_parse_finalize);

	dsd_media_parse_class->setup = GST_DEBUG_FUNCPTR(gst_dsd_chunk_parse_setup);
	dsd_media_parse_class->teardown = GST_DEBUG_FUNCPTR(gst_dsd_chunk_parse_teardown);
	dsd_media_parse_class->scan_info = GST_DEBUG_FUNCPTR(gst_dsd_chunk_parse_scan_info);
	dsd_media_parse_class->verify_advance = GST_DEBUG_FUNCPTR(gst_dsd_chunk_parse_verify_advance);
}


static void gst_dsd_chunk_parse_init(GstDsdChunkParse *self)
{
	GstDsdChunkParsePrivate *priv = get_private(self);
	priv->chunk_parse_stage = ChunkParseStage::ParsingChunkStart;
	priv->chunk_stack = nullptr;
}


static void gst_dsd_chunk_parse_finalize(GObject *object)
{
	GstDsdChunkParse *self = GST_DSD_CHUNK_PARSE(object);
	GstDsdChunkParsePrivate *priv = get_private(self);

	delete priv->chunk_stack;

	G_OBJECT_CLASS(gst_dsd_chunk_parse_parent_class)->finalize(object);
}


static bool gst_dsd_chunk_parse_setup(GstDsdMediaParse *parse)
{
	GstDsdChunkParse *self = GST_DSD_CHUNK_PARSE(parse);
	GstDsdChunkParsePrivate *priv = get_private(self);

	if (G_UNLIKELY(priv->chunk_stack == nullptr)) {
		GST_ELEMENT_ERROR(
			parse,
			LIBRARY,
			SETTINGS,
			("Internal parser error."),
			(
				"%s did not call gst_dsd_chunk_parse_configure() before the NULL->READY state change",
				G_OBJECT_TYPE_NAME(parse)
			)
		);
		return false;
	}

	priv->chunk_stack->clear();
	priv->chunk_parse_stage = ChunkParseStage::ParsingChunkStart;

	return true;
}


static void gst_dsd_chunk_parse_teardown(GstDsdMediaParse *parse)
{
	GstDsdChunkParse *self = GST_DSD_CHUNK_PARSE(parse);
	GstDsdChunkParsePrivate *priv = get_private(self);

	if (priv->chunk_stack != nullptr)
		priv->chunk_stack->clear();
	priv->chunk_parse_stage = ChunkParseStage::ParsingChunkStart;
}


static GstFlowReturn gst_dsd_chunk_parse_scan_info(GstDsdMediaParse *parse)
{
	GstDsdChunkParse *self = GST_DSD_CHUNK_PARSE_CAST(parse);
	GstDsdChunkParsePrivate *priv = get_private(self);

	GstFlowReturn flow_ret = GST_FLOW_OK;

	switch (priv->chunk_parse_stage) {
		case ChunkParseStage::ParsingChunkStart:
			flow_ret = gst_dsd_chunk_parse_parse_chunk_start(self);
			break;

		case ChunkParseStage::ParsingChunkContent:
			flow_ret = gst_dsd_chunk_parse_parse_chunk_content(self);
			break;

		case ChunkParseStage::FinishingChunk:
			break;

		default:
			g_assert_not_reached();
	}

	// This is called separately since gst_dsd_chunk_parse_parse_chunk_content()
	// might return from parsing the very last of the chunks in the chunk based
	// media, meaning that this chain function invocation might be the very last
	// one until EOS is reached. If gst_dsd_chunk_parse_finish_chunk_parsing()
	// were called in the switch-case block, it would actually never get called
	// for the last chunk then when the parser element is running in push mode.
	//
	// Allow for both GST_FLOW_OK and GST_FLOW_NOTHING_TO_READ return codes.
	// The latter can legitimately happen if the chunk has nothing more to read.
	// Also check gst_dsd_media_parse_is_currently_scanning() - if upstream is
	// not seekable, then the base class will immediately switch to the Scanning
	// stage once the payload is reported. The chunk parsing then is cut short,
	// and finishing chunks here no longer makes sense.
	if ((flow_ret == GST_FLOW_OK) || (flow_ret == GST_FLOW_NOTHING_TO_READ)) {
		bool still_scanning = gst_dsd_media_parse_is_currently_scanning(parse);

		if (
			(priv->chunk_parse_stage == ChunkParseStage::FinishingChunk) &&
			still_scanning
		) {
			GstFlowReturn finish_ret = gst_dsd_chunk_parse_finish_chunk_parsing(self);
			// Preserve flow_ret's NOTHING_TO_READ on success, so the base class
			// does not see a GST_FLOW_OK that advanced nothing.
			if (finish_ret != GST_FLOW_OK)
				flow_ret = finish_ret;
		} else if (G_UNLIKELY(!still_scanning)) {
			// Scanning ended mid-invocation - a parse function reported the payload
			// while upstream is not seekable, so the base class switched to the
			// Streaming stage immediately. Chunks still on the stack will never be
			// finished, so their required subchunks were never verified. Do it now.
			if (G_UNLIKELY(priv->chunk_stack->has_missing_required_subchunks())) {
				GST_ELEMENT_ERROR(
					parse,
					STREAM,
					DEMUX,
					("Internal parser error."),
					("a chunk did not contain all of its required subchunks")
				);
				return GST_FLOW_ERROR;
			}
		}
	}

	return flow_ret;
}


static bool gst_dsd_chunk_parse_verify_advance(GstDsdMediaParse *parse, guint64 byte_position, guint64 advance_amount, const gchar *advance_name)
{
	GstDsdChunkParse *self = GST_DSD_CHUNK_PARSE(parse);
	GstDsdChunkParsePrivate *priv = get_private(self);
	return priv->chunk_stack->verify_advance(byte_position, advance_amount, advance_name);
}


static GstFlowReturn gst_dsd_chunk_parse_parse_chunk_start(GstDsdChunkParse *self)
{
	GstDsdMediaParse *parse = GST_DSD_MEDIA_PARSE_CAST(self);
	GstDsdChunkParsePrivate *priv = get_private(self);

	// This checks for an edge case: A chunk whose payload solely consists
	// of subchunks, but which turns out to have no subchunks. When the
	// parse function calls gst_dsd_chunk_parse_begin_parsing_subchunks(),
	// and no subchunks are found, there is nothing to trigger the checks
	// in gst_dsd_chunk_parse_finish_chunk_parsing().
	//
	// To handle this case, check how many payload bytes are left to read
	// in the current chunk. If it is <= 1, then this chunk has no subchunks.
	// (<= 1 instead of == 0 to account for any present padding byte). Here,
	// we are at the _start_ of the chunk parsing, so having <= 1 byte
	// available makes no sense otherwise.
	const Chunk *current_chunk = priv->chunk_stack->get_current_chunk();
	if (current_chunk != nullptr) {
		guint64 current_byte_position = gst_dsd_media_parse_get_current_byte_position(parse);
		guint64 num_remaining_bytes = current_chunk->payload_end_pos - current_byte_position;

		if (num_remaining_bytes <= 1) {
			priv->chunk_parse_stage = ChunkParseStage::FinishingChunk;
			return GST_FLOW_NOTHING_TO_READ;
		}
	}

	// Try reading the 12-byte chunk header:
	// 4 bytes for the fourCC chunk ID , then 8 bytes for the length
	// (64-bit big endian unsigned integer).

	constexpr static guint64 CHUNK_HEADER_SIZE = 12;

	GstBuffer *buffer = nullptr;
	GstFlowReturn flow_ret = gst_dsd_media_parse_read_data_during_scan(parse, CHUNK_HEADER_SIZE, &buffer);
	switch (flow_ret) {
		case GST_FLOW_NOT_ENOUGH_DATA: {
			GST_LOG_OBJECT(
				self,
				"need at least %" G_GUINT64_FORMAT" bytes to parse chunk header",
				CHUNK_HEADER_SIZE
			);
			return GST_FLOW_NOT_ENOUGH_DATA;
		}

		case GST_FLOW_OK:
			break;

		default:
			return flow_ret;
	}

	MappedBuffer mapped_chunk_header_data{buffer, GST_MAP_READ};
	RETURN_IF_GSTBUFFER_MAPPING_FAILED(
		self,
		mapped_chunk_header_data,
		GST_FLOW_ERROR,
		"Could not map the %" G_GUINT64_FORMAT " chunk header byte(s)", CHUNK_HEADER_SIZE
	);

	guint32 chunk_fourcc = GST_MAKE_FOURCC(
		mapped_chunk_header_data.data()[0],
		mapped_chunk_header_data.data()[1],
		mapped_chunk_header_data.data()[2],
		mapped_chunk_header_data.data()[3]
	);
	guint64 chunk_size = (priv->chunk_size_endianness == ChunkSizeEndianness::BigEndian)
		? GST_READ_UINT64_BE(mapped_chunk_header_data.data() + 4)
		: GST_READ_UINT64_LE(mapped_chunk_header_data.data() + 4);

	if (G_UNLIKELY(priv->chunk_size_includes_chunk_header && (chunk_size < CHUNK_HEADER_SIZE))) {
		GST_ELEMENT_ERROR(
			self,
			STREAM,
			DEMUX,
			("Invalid chunk size."),
			(
				"chunk \"%" GST_FOURCC_FORMAT "\" has size %" G_GUINT64_FORMAT ", and this "
				"should include the %" G_GUINT64_FORMAT " bytes of the fourcc and size "
				"integer, but the size is less than %" G_GUINT64_FORMAT,
				GST_FOURCC_ARGS(chunk_fourcc),
				chunk_size,
				CHUNK_HEADER_SIZE,
				CHUNK_HEADER_SIZE
			)
		);
		return GST_FLOW_ERROR;
	}

	if (priv->chunk_size_includes_chunk_header)
		chunk_size -= CHUNK_HEADER_SIZE;

	guint64 chunk_payload_begin_pos = gst_dsd_media_parse_get_current_byte_position(parse);

	// Sometimes, chunk sizes do not include the padding byte at the end, and
	// then they are odd. But sometimes, they do, and then they are even. To
	// deal with both, increment the chunk size if it is odd. This is used
	// in several places below.
	guint64 padded_chunk_size = chunk_size + (chunk_size & 1);

	// padded_chunk_size < chunk_size detects the wraparound that occurs when
	// chunk_size is G_MAXUINT64 - that value is odd, so the padding increment
	// above overflows to 0.
	if (G_UNLIKELY(
		(padded_chunk_size < chunk_size) ||
		(padded_chunk_size > (G_MAXUINT64 - chunk_payload_begin_pos))
	)) {
		GST_ELEMENT_ERROR(
			self,
			STREAM,
			DEMUX,
			("Invalid chunk size."),
			(
				"chunk \"%" GST_FOURCC_FORMAT "\" has size %" G_GUINT64_FORMAT ", and "
				"its payload starts at %" G_GUINT64_FORMAT "; adding the size (and any "
				"potentially present padding byte) to the payload start position "
				"exceeds the range of a 64-bit unsigned integer",
				GST_FOURCC_ARGS(chunk_fourcc),
				chunk_size,
				chunk_payload_begin_pos
			)
		);
		return GST_FLOW_ERROR;
	}

	// To factor in the padding byte in the parsing process, compute
	// chunk_payload_end_pos by adding the padded_chunk_size instead of
	// the original one. Then, when the chunk gets finished, if there
	// is one more byte in the payload left unread (which is determined
	// by looking at the payload end position), this is understood to
	// be the padding byte, and gets automatically skipped.
	guint64 chunk_payload_end_pos = chunk_payload_begin_pos + padded_chunk_size;

	if (chunk_size & 1) {
		GST_DEBUG_OBJECT(
			self,
			"chunk \"%" GST_FOURCC_FORMAT "\" has odd length %" G_GUINT64_FORMAT "; "
			"incremented chunk_payload_end_pos for proper padding byte skipping",
			GST_FOURCC_ARGS(chunk_fourcc),
			chunk_size
		);
	}

	// The "nesting level" specifies how deep the current chunk stack is, that
	// is, how nested this chunk is. A root level chunk is not nested at all,
	// so "nesting level" is 0.
	GST_DEBUG_OBJECT(
		self,
		"started to parse chunk \"%" GST_FOURCC_FORMAT "\" with payload size %" G_GUINT64_FORMAT " byte(s); "
		"payload start / end: %" G_GUINT64_FORMAT " / %" G_GUINT64_FORMAT "; nesting level: %" G_GSIZE_FORMAT,
		GST_FOURCC_ARGS(chunk_fourcc),
		chunk_size,
		chunk_payload_begin_pos, chunk_payload_end_pos,
		priv->chunk_stack->depth()
	);

	// Get the ChunkDescription for this chunk. If the chunk is unknown,
	// no ChunkDescription can be assigned. If it is known, assign it,
	// and record in the ChunkDescription that the chunk was already seen
	// by its parent chunk. This is important for those chunks that are
	// supposed to be unique. Also do sanity check on the chunk size to
	// see if it corresponds to what is expected. Also see the code in
	// gst_dsd_chunk_parse_finish_chunk_parsing().

	auto push_ret = priv->chunk_stack->push_chunk(
		chunk_fourcc,
		chunk_size,
		chunk_payload_begin_pos,
		chunk_payload_end_pos
	);

	switch (push_ret) {
		case ChunkStack::PushReturnCode::Ok:
			break;

		case ChunkStack::PushReturnCode::AlreadySeen:
			GST_ELEMENT_ERROR(
				self,
				STREAM,
				DEMUX,
				("Invalid duplicate chunk."),
				("chunk \"%" GST_FOURCC_FORMAT "\" has already been seen, but is supposed to be unique within its parent", GST_FOURCC_ARGS(chunk_fourcc))
			);
			return GST_FLOW_ERROR;

		case ChunkStack::PushReturnCode::IncorrectChunkSize:
			GST_ELEMENT_ERROR(
				self,
				STREAM,
				DEMUX,
				("Incorrect chunk size."),
				("chunk \"%" GST_FOURCC_FORMAT "\" has invalid size %" G_GUINT64_FORMAT, GST_FOURCC_ARGS(chunk_fourcc), chunk_size)
			);
			return GST_FLOW_ERROR;

		case ChunkStack::PushReturnCode::InsufficientChunkSize:
			GST_ELEMENT_ERROR(
				self,
				STREAM,
				DEMUX,
				("Chunk with insufficient data found."),
				("chunk \"%" GST_FOURCC_FORMAT "\" has insufficient bytes", GST_FOURCC_ARGS(chunk_fourcc))
			);
			return GST_FLOW_ERROR;

		case ChunkStack::PushReturnCode::ChunkOutOfParentBounds:
			GST_ELEMENT_ERROR(
				self,
				STREAM,
				DEMUX,
				("Chunk is out of bounds of parent chunk."),
				("chunk \"%" GST_FOURCC_FORMAT "\" exceeds the bounds of its parent chunk", GST_FOURCC_ARGS(chunk_fourcc))
			);
			return GST_FLOW_ERROR;
	}

	// A chunk with no payload is finished the moment its header has been read.
	// Going through the ParsingChunkContent stage would mean reaching
	// FinishingChunk in a scan_info() invocation that read nothing, which the
	// base class rejects. Finishing it here keeps it in the same invocation as
	// the 12-byte header read.

	if (chunk_size == 0) {
		GST_DEBUG_OBJECT(
			self,
			"will immediately finish chunk \"%" GST_FOURCC_FORMAT "\" since it has no payload",
			GST_FOURCC_ARGS(chunk_fourcc)
		);
		priv->chunk_parse_stage = ChunkParseStage::FinishingChunk;
	} else {
		priv->chunk_parse_stage = ChunkParseStage::ParsingChunkContent;
	}

	return GST_FLOW_OK;
}


static GstFlowReturn gst_dsd_chunk_parse_parse_chunk_content(GstDsdChunkParse *self)
{
	GstDsdMediaParse *parse = GST_DSD_MEDIA_PARSE_CAST(self);
	GstDsdChunkParsePrivate *priv = get_private(self);

	const Chunk *current_chunk = priv->chunk_stack->get_current_chunk();
	if (G_UNLIKELY(current_chunk == nullptr)) {
		GST_ELEMENT_ERROR(self, STREAM, FAILED,
			("Read error while parsing - no chunks to parse."),
			("%s attempted to parse chunk content while the chunk stack was empty", G_OBJECT_TYPE_NAME(self))
		);
		return GST_FLOW_ERROR;
	}

	if (current_chunk->description != nullptr) {
		// This is a known chunk (it has an assigned description),
		// so it can be parsed.

		return current_chunk->description->parse_function(self, *current_chunk);
	} else {
		// This is an unknown chunk (it has no assigned description).
		// Just skip its contents.

		guint64 num_bytes_to_skip =
			current_chunk->payload_end_pos - gst_dsd_media_parse_get_current_byte_position(parse);

		// The size safety check for unknown chunks in parse_chunk_start()
		// should have caught this.
		g_assert(num_bytes_to_skip != 0);

		guint64 num_actually_skipped_bytes = 0;
		GstFlowReturn flow_ret = gst_dsd_media_parse_skip_data_during_scan(parse, num_bytes_to_skip, &num_actually_skipped_bytes);
		if (flow_ret != GST_FLOW_OK)
			return flow_ret;

		GST_DEBUG_OBJECT(
			self,
			"skipped %" G_GUINT64_FORMAT " byte(s) from unknown chunk \"%" GST_FOURCC_FORMAT "\" with size %" G_GUINT64_FORMAT " byte(s)",
			num_actually_skipped_bytes,
			GST_FOURCC_ARGS(current_chunk->fourcc),
			current_chunk->size
		);

		if (num_actually_skipped_bytes == num_bytes_to_skip) {
			priv->chunk_parse_stage = ChunkParseStage::FinishingChunk;
		} else if (num_actually_skipped_bytes == 0) {
			// gst_dsd_media_parse_skip_data_during_scan() skips fewer
			// bytes than requested if the parser runs in push mode and
			// there is currently insufficient data to skip. In this
			// case, we cannot currently skip any contents of the unknown
			// chunk. Return GST_FLOW_NOT_ENOUGH_DATA to let the base
			// class know that it needs to try again later.
			return GST_FLOW_NOT_ENOUGH_DATA;
		}

	}

	return GST_FLOW_OK;
}


static GstFlowReturn gst_dsd_chunk_parse_finish_chunk_parsing(GstDsdChunkParse *self)
{
	GstDsdMediaParse *parse = GST_DSD_MEDIA_PARSE_CAST(self);
	GstDsdChunkParsePrivate *priv = get_private(self);

	const Chunk *current_chunk = priv->chunk_stack->get_current_chunk();
	if (G_UNLIKELY(current_chunk == nullptr)) {
		GST_ELEMENT_ERROR(self, STREAM, FAILED,
			("Read error while parsing - no chunks to finish."),
			("%s finished a chunk while the chunk stack was empty", G_OBJECT_TYPE_NAME(self))
		);
		return GST_FLOW_ERROR;
	}

	guint64 current_byte_position = gst_dsd_media_parse_get_current_byte_position(parse);

	// Check if there is one byte left in the chunk's payload. If so,
	// this is a padding byte.
	//
	// This assumes that the caller correctly parsed all actual payload
	// bytes from the chunk, and did not miss a legitimate last payload
	// byte. If not, the parser has a bug; but then, due to misalignment,
	// no valid chunk IDs will be read, and eventually, the parser will
	// stop with an error. So, while such a bug in the parser is not
	// ideal, it will not cause undefined behavior, just an error.
	if ((current_byte_position < current_chunk->payload_end_pos) && ((current_chunk->payload_end_pos - current_byte_position) == 1)) {
		GST_DEBUG_OBJECT(
			self,
			"skipping pad byte in chunk \"%" GST_FOURCC_FORMAT "\" (chunk size %" G_GUINT64_FORMAT " byte(s))",
			GST_FOURCC_ARGS(current_chunk->fourcc),
			current_chunk->size
		);

		guint64 num_actually_skipped_bytes;
		GstFlowReturn flow_ret = gst_dsd_media_parse_skip_data_during_scan(parse, 1, &num_actually_skipped_bytes);
		if (flow_ret != GST_FLOW_OK)
			return flow_ret;

		// gst_dsd_media_parse_skip_data_during_scan() skips fewer
		// bytes than requested if the parser runs in push mode and
		// there is currently insufficient data to skip. In this
		// case, we cannot currently skip the pad byte. Return
		// GST_FLOW_NOT_ENOUGH_DATA to let the base class know
		// that it needs to try again later.
		if (num_actually_skipped_bytes < 1)
			return GST_FLOW_NOT_ENOUGH_DATA;
	}

	// Record the current chunk's fourCC here, since after the
	// pop_finished_chunks() call below, the current_chunk pointer
	// may no longer be valid.
	guint32 current_chunk_fourcc = current_chunk->fourcc;

	current_byte_position = gst_dsd_media_parse_get_current_byte_position(parse);

	// NOTE: After this pop_finished_chunks() call, current_chunk
	// is no longer to be considered valid.
	auto pop_ret = priv->chunk_stack->pop_finished_chunks(current_byte_position);

	switch (pop_ret) {
		case ChunkStack::PopReturnCode::Ok:
			priv->chunk_parse_stage = ChunkParseStage::ParsingChunkStart;
			break;

		case ChunkStack::PopReturnCode::NoChunksPushed:
			GST_ELEMENT_ERROR(
				self,
				STREAM,
				FAILED,
				("Read error while parsing - no chunks to finish."),
				("the chunk stack is empty when trying to finish chunks.")
			);
			return GST_FLOW_ERROR;

		case ChunkStack::PopReturnCode::NoChunksFinished:
			// Reaching this location means that the current chunk is supposed
			// to be finished, so at least that chunk must have been popped.
			// Every legitimate route into the FinishingChunk stage leaves the
			// byte position exactly at the current chunk's payload_end_pos:
			// a chunk without payload, a chunk whose payload was fully read,
			// a fully skipped unknown chunk, and the padding byte skip above
			// all do. If nothing was popped, the position is somewhere in the
			// middle of the current chunk's payload instead.
			//
			// Continuing would mean parsing a chunk header from the middle of
			// that payload. No valid chunk IDs would be read from there, so
			// the parser would eventually stop with an error anyway - but that
			// error would point at some later, unrelated position. Report it
			// here instead, where the offending chunk is still known.
			GST_ELEMENT_ERROR(
				self,
				STREAM,
				FAILED,
				("Read error while parsing - chunk finished prematurely."),
				(
					"%s declared chunk \"%" GST_FOURCC_FORMAT "\" to be finished at "
					"position %" G_GUINT64_FORMAT ", which is not the end of its payload",
					G_OBJECT_TYPE_NAME(self),
					GST_FOURCC_ARGS(current_chunk_fourcc),
					current_byte_position
				)
			);
			return GST_FLOW_ERROR;

		case ChunkStack::PopReturnCode::FinishFunctionFailed:
			GST_ELEMENT_ERROR(
				self,
				STREAM,
				FAILED,
				("Read error while parsing - chunk could not be fully processed."),
				(
					"finish function for chunk \"%" GST_FOURCC_FORMAT "\" failed",
					GST_FOURCC_ARGS(current_chunk_fourcc)
				)
			);
			return GST_FLOW_ERROR;

		case ChunkStack::PopReturnCode::PositionOutOfChunkBounds:
			GST_ELEMENT_ERROR(
				self,
				STREAM,
				FAILED,
				("Read error while parsing - chunk bounds exceeded."),
				(
					"parse position %" G_GUINT64_FORMAT " exceeds bounds of chunk \"%" GST_FOURCC_FORMAT "\"",
					current_byte_position,
					GST_FOURCC_ARGS(current_chunk_fourcc)
				)
			);
			return GST_FLOW_ERROR;

		case ChunkStack::PopReturnCode::RequiredSubchunksMissing:
			GST_ELEMENT_ERROR(
				self,
				STREAM,
				FAILED,
				("Read error while parsing - required chunks are missing."),
				("a chunk did not contain all of its required subchunks")
			);
			return GST_FLOW_ERROR;
	}

	return GST_FLOW_OK;
}


void gst_dsd_chunk_parse_configure(
	GstDsdChunkParse *parse,
	ChunkSizeEndianness chunk_size_endianness,
	bool chunk_size_includes_chunk_header,
	ChunkDescriptions descriptions
)
{
	GstDsdChunkParsePrivate *priv = get_private(parse);

	g_return_if_fail(priv->chunk_stack == nullptr);

	priv->chunk_size_endianness = chunk_size_endianness;
	priv->chunk_size_includes_chunk_header = chunk_size_includes_chunk_header;
	priv->chunk_stack = new ChunkStack(GST_OBJECT(parse), parse, std::move(descriptions));
}


void gst_dsd_chunk_parse_begin_parsing_subchunks(GstDsdChunkParse *parse)
{
	GstDsdChunkParsePrivate *priv = get_private(parse);
	priv->chunk_parse_stage = ChunkParseStage::ParsingChunkStart;
}


void gst_dsd_chunk_parse_finish_current_chunk(GstDsdChunkParse *parse)
{
	GstDsdChunkParsePrivate *priv = get_private(parse);
	priv->chunk_parse_stage = ChunkParseStage::FinishingChunk;
}
