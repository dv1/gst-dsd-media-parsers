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

#include <gst/gst.h>
#include "gstdsdmediaparse.hpp"
#include "chunk_stack.hpp"


// NOTE: Not using G_BEGIN_DECLS and G_END_DECLS since the API is C++ only


#define GST_TYPE_DSD_CHUNK_PARSE (gst_dsd_chunk_parse_get_type())
G_DECLARE_DERIVABLE_TYPE(GstDsdChunkParse, gst_dsd_chunk_parse, GST, DSD_CHUNK_PARSE, GstDsdMediaParse)
#define GST_DSD_CHUNK_PARSE_CAST(obj) ((GstDsdChunkParse *)(obj))


enum class ChunkSizeEndianness {
	BigEndian,
	LittleEndian,
};


/**
 * Base class for parsing DSD media that is based on chunks.
 *
 * Overview:
 *
 * This builds upon GstDsdMediaParse and implements chunk parsing for the Scanning
 * Info stage. The ChunkStack helper class is used for this purpose. This parser's
 * scanning implementation is compatible with formats that use fourCCs for chunk IDs
 * and 64-bit unsigned integer for chunk sizes. If also automatically skips a pad
 * byte at the end if there is one single byte left by the time a chunk is marked
 * as finished. (Chunk based formats usually align their content to even positions.)
 *
 * During the Scanning Info stage, this base class will move through its own
 * internal chunk parsing stages. First, it will start parsing chunk headers -
 * the fourCC and chunk size. Based on that, it will pick a suitable chunk parse
 * function from a ChunkDescriptions STL unordered_map that must have been
 * provided in a setup step (more on that below). With this function, the next
 * stage commences - reading the chunk content. The parse function takes care
 * of this. Should the chunk solely consist of its own payload, the parse function
 * must report manually when it deems the chunk content to be fully parsed. Should
 * the chunk instead be made of subchunks, or first contain its own payload and
 * then have subchunks, the base class will recognize the end of that chunk on its
 * own when all of its subchunks have been parsed.
 *
 * Optionally, a ChunkDescription can also contain a finish function. That one
 * is invoked when a chunk is reported to be finished. Post-chunk-parse steps
 * like parsed content validation can take place there, for example.
 *
 * The subclass implements these parse and finish functions. They are also the
 * ones that call the gst_dsd_media_parse_report_payload_found() and
 * gst_dsd_media_parse_scanning_finished() functions (see the GstDsdMediaParse
 * documentation for more).
 *
 * Once the Scanning Info stage is over, the Streaming stage continues as usual -
 * there is nothing chunk specific happening there, and this base class does not
 * concern itself with that stage.
 *
 * During the chunk parsing, this base class performs various checks to ensure
 * that the byte position does not exceed the bounds of the chunk that is being
 * read. It also performs checks to see whether a subchunk exceeds the bounds
 * of a parent chunk (in which case the media is considered to be corrupted).
 *
 * This base class implements these GstDsdMediaParse vmethods:
 * - setup()
 * - teardown()
 * - scan_info()
 * - verify_advance()
 *
 * Subclasses must at least implement these GstDsdMediaParse vmethods:
 * - to_index()
 * - from_index()
 * - produce_output()
 *
 * When a chunk is made of subchunks, it must inform the base class when the
 * subchunks start by calling gst_dsd_chunk_parse_begin_parsing_subchunks().
 * The subclass will automatically detect based on the chunk sizes when the
 * parent chunk ends. In the edge case where that chunk turns out to be empty
 * (that is, the number of available payload bytes indicates that no subchunks
 * can be present), the base class automatically detects this and finishes
 * the empty chunk.
 *
 * \important Chunks that have custom payload before _and_ after subchunks
 * are _not_ supported. If a chunk has subchunks and its own payload, the
 * payload must come before the subchunks.
 *
 * As mentioned above, a ChunkDescriptions map must be provided to this base
 * class. Subclasses must call gst_dsd_chunk_parse_configure() before the
 * NULL->READY stage change occurs. A good place for this is the init function.
 * See the ChunkDescription documentation for details about what to place in
 * the descriptions, and see gst_dsd_chunk_parse_configure() for details about
 * its arguments. A compact method of calling this function is for example:
 *
 * \code{.cpp}
 * gst_dsd_chunk_parse_configure(
 *     GST_DSD_CHUNK_PARSE(self),
 *     // 64-bit chunk size integer is stored in big endian order
 *     ChunkSizeEndianness::BigEndian,
 *     // chunk sizes do not include the 12-byte chunk header
 *     false,
 *     {
 *         { GST_MAKE_FOURCC('C', 'H', 'N', 'K'), { true,  true,  true,  ChunkSizeType::FirstNumBytes, 20, parse_chunk_chnk, finish_chunk_chnk } },
 *         { GST_MAKE_FOURCC('F', 'R', 'M', 'T'), { true,  true,  false, ChunkSizeType::ExactSize,     8,  parse_chunk_frmt } },
 *     }
 * );
 * \endcode
 *
 * This sets up the base class to parse the "CHNK" and "FRMT" chunks. The
 * CHNK chunk is described as occurring only once, as being required, and
 * as being a container. Also, the 20 here means that its own payload makes
 * up the first 20 bytes of the chunk (this is what FirstNumBytes indicates).
 * Since it is a container, it means that the rest of the chunk is made of
 * subchunks. Next comes the "FRMT" chunk - it is required, it is supposed
 * to occur only once (within its parent; see ChunkDescription for more),
 * is not a container, and in its case, the 8 means that its entire payload
 * is the exact size of its entire payload (this is what ExactSize indicates).
 *
 * The base class will then call the specified parse functions when the
 * associated chunks are found. It will also validate the chunk sizes, check
 * for out of bound errors as mentioned above etc.
 *
 * Notes about implementing parse and finish functions:
 *
 * (Read the ChunkParseFunction and FinishChunkFunction first for context.)
 *
 * As the documentation of ChunkParseFunction and FinishChunkFunction state,
 * they have a user_data pointer. The base class has its internal ChunkStack
 * instance, which is given as user_data a pointer to the base class instance.
 * In other words, a parse function that is used with this base class gets
 * a GstDsdChunkParse* pointer via user_data:
 *
 * \code{.cpp}
 * static GstFlowReturn my_parse_function(gpointer user_data, const Chunk &chunk)
 * {
 *     // Get a pointer to the base class instance out of user_data
 *     GstDsdChunkParse *parse = GST_DSD_CHUNK_PARSE_CAST(user_data);
 *     ...
 * }
 * \endcode
 *
 * Within the parse function, use gst_dsd_media_parse_read_data_during_scan()
 * and gst_dsd_media_parse_skip_data_during_scan() to read/skip bytes from the
 * chunk's payload. Should these functions return anything other than GST_FLOW_OK,
 * the flow error return code must be immediately returned by this parse function.
 *
 * \important Should the parse function read no payload (usually because the
 * associated chunk only contains subchunks), it must return GST_FLOW_NOTHING_TO_READ.
 * Otherwise, the GstDsdMediaParse base class will incorrectly interpret the lack
 * of reading or skipping activity as a bug.
 */
struct _GstDsdChunkParseClass {
	GstDsdMediaParseClass parent_class;
};

/**
 * Configures the base class for chunk processing.
 *
 * This must be called exactly once, before the NULL->READY state change.
 * Without this, the base class cannot parse chunks.
 *
 * chunk_size_endianness specifies whether the 64-bit unsigned integers
 * that contain the chunk sizes are in little endian or big endian format.
 *
 * If chunk_size_includes_chunk_header is true, the chunk sizes include
 * the 12 bytes (= the 4 bytes for the chunk ID and the 8 bytes for the
 * chunk size itself) of the chunk header.
 *
 * For more about chunk_descriptions, see the ChunkDescription documentation.
 */
void gst_dsd_chunk_parse_configure(
	GstDsdChunkParse *parse,
	ChunkSizeEndianness chunk_size_endianness,
	bool chunk_size_includes_chunk_header,
	ChunkDescriptions chunk_descriptions
);

/**
 * Informs the base class that the rest of a chunk's payload is made of subchunks.
 *
 * Chunk parse functions must call this once subchunks start within the currently
 * parsed chunk.
 */
void gst_dsd_chunk_parse_begin_parsing_subchunks(GstDsdChunkParse *parse);

/**
 * Informs the base class that the current chunk has been finished (= its payload fully parsed).
 *
 * This only needs to be called when past the chunk's normal payload there are
 * no subchunks. If there are, the base class will automatically detect the end
 * (by detecting that the current byte position is at the end of this chunk,
 * or at most there's a padding byte left).
 */
void gst_dsd_chunk_parse_finish_current_chunk(GstDsdChunkParse *parse);
