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

#include <optional>
#include <string>
#include <cstring>
#include <vector>
#include <gst/gst.h>
#include <gst/tag/tag.h>
#include "mapped_buffer.hpp"
#include "gstdsfparse.hpp"
#include "scope_guard.hpp"


GST_DEBUG_CATEGORY_STATIC(dsfparse_debug);
#define GST_CAT_DEFAULT dsfparse_debug


// DSD files are scanned chunk by chunk. The Scanning Info stage
// is finished once the top level chunk is finished.


// NOTE: The first DSF chunk that acts as its header has as
// fourCC "DSD " (note the whitespace). But, since "DSD chunk"
// sounds ambiguous in a DSF parser, this chunk is referred
// to as "DSF header chunk" instead.


// About indices and position boundaries in GstDSFParse
//
// (See the GstDsdMediaParse documentation about these concepts
// if they aren't known already.)
//
// In DSF, channels are interleaved, but at a block level. The grouping
// format is U8. Given a block size B, the first B bytes contain the
// first B*8 bits for channel 1. The next B bytes contain the first
// B*8 bits for channel 2 etc. Then, the next B*8 bits for channel 1
// follow, then the next B*8 bits for channel 2 etc.
//
// Naturally, this maps to the position boundary concept. The alignment
// simply follows the block size. In GstDSFParse, the index -> byte
// bytes conversion then is:
//
//   bytes = index * (num_channels * block_size)
//
// the index -> time conversion is trickier, since the block size has
// no implicit relation to a specific playtime. Thus, the following
// approach is used instead:
//
//   time = gst_util_uint64_scale(index, duration, total_num_indices)
//
// gst_dsfparse_from_index() performs these calculations.
//
// The other way round is:
//
//   index = bytes / (num_channels * block_size)
//   index = gst_util_uint64_scale(time, total_num_indices, duration)
//
// gst_dsfparse_to_index() performs these calculations. However,
// that function has the specialty that it can be requested by
// the caller to round _up_, while the formulas above round _down_.
// the rounding-up variants are:
//
//   index = (bytes + (num_channels * block_size - 1)) / (num_channels * block_size)
//   index = gst_util_uint64_scale_ceil(time, total_num_indices, duration)
//
// total_num_indices is computed alongside the duration, in this manner:
//
//   total_num_indices = ceil(actual_num_dsd_bytes_per_channel / block_size)
//
// (See the comment above actual_num_dsd_bytes_per_channel and
// expected_num_dsd_bytes_per_channel for details about what "actual" means.)
//
// The rounding is upwards because the DSF specification requires the last
// block to be zero padded if there is not enough sample data left to fill
// it. That padded block is present in the medium and is output like any
// other one, so it must have an index of its own.
//
// (num_channels * block_size) is computed once in the fmt chunk parser
// and stored in the "alignment" field.


#define GST_DSFPARSE_FOURCC_CHUNK_DSF_HEADER       GST_MAKE_FOURCC('D', 'S', 'D', ' ')
#define GST_DSFPARSE_FOURCC_CHUNK_fmt              GST_MAKE_FOURCC('f', 'm', 't', ' ')
#define GST_DSFPARSE_FOURCC_CHUNK_data             GST_MAKE_FOURCC('d', 'a', 't', 'a')


static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE(
	"sink",
	GST_PAD_SINK,
	GST_PAD_ALWAYS,
	GST_STATIC_CAPS(GST_DSF_MEDIA_TYPE)
);


static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE(
	"src",
	GST_PAD_SRC,
	GST_PAD_ALWAYS,
	GST_STATIC_CAPS(
		GST_DSD_MEDIA_TYPE ", "
		"format = (string) DSDU8, "
		"rate = " GST_AUDIO_RATE_RANGE ", "
		"layout = (string) non-interleaved, "
		"reversed-bytes = (gboolean) { false, true }, "
		"channels = " GST_AUDIO_CHANNELS_RANGE
	)
);


// This struct contains all the data from the fmt chunk.
struct FmtData {
	guint32 sample_rate;
	guint32 num_channels;
	std::vector<GstAudioChannelPosition> channel_positions;
	bool reversed_bytes;

	// expected_num_dsd_bytes_per_channel contains a value
	// that the DSF spec refers to as "Sample count". That
	// value is originally given in bits - the parser here
	// converts it into bytes.
	// If the DSF media is okay, actual_num_dsd_bytes_per_channel
	// matches expected_num_dsd_bytes_per_channel. But if it
	// is truncated, actual_num_dsd_bytes_per_channel is
	// calculated out of the actual number of available DSD
	// bytes. The expected value is still important though
	// to handle the last-block padding correctly, so both
	// are kept here.
	guint64 expected_num_dsd_bytes_per_channel;
	guint64 actual_num_dsd_bytes_per_channel;

	guint32 block_size;
};


struct _GstDSFParse
{
	GstDsdMediaParse parent;

	// This is set to a valid FmtData instance if the fmt chunk was parsed.
	std::optional<FmtData> fmt_data;

	// Incremented in gst_dsfparse_produce_output()
	// and set in gst_dsfparse_current_index_after_seek().
	guint64 current_index;

	// Calculated out of fmt_data's actual_num_dsd_bytes_per_channel.
	// Only valid once the data chunk has been parsed.
	guint64 total_num_indices;

	// This is set to num_channels * block_size in the fmt chunk parser.
	guint64 alignment;

	// Upstream size in bytes. Determined
	// in the DSF header chunk parser.
	guint64 upstream_size;

	// ID3v2 metadata in GStreamer tag list form.
	GstTagList *id3v2_tags;

	// If true, the DSF header chunk parser is currently
	// actually parsing the ID3v2 metadata.
	bool reading_id3v2_metadata;

	// The ID3v2 metadata is actually outside of the DSF
	// header chunk's bounds. The parser jumps to that
	// position to parse the metadata. Prior to that,
	// it records the pre-skeep byte position to be
	// able to seek back once the metadata is parsed.
	guint64 id3v2_pre_seek_byte_position;

	// The first bytes of the metadata that make up
	// the ID3v2 header. Its size in specified by
	// GST_TAG_ID3V2_HEADER_SIZE, meaning that this
	// buffer is of that size. This separate buffer
	// is needed by the parser because it needs to
	// first know how big the metadata is in total.
	// That size is the recorded as id3v2_size.
	GstBuffer *id3v2_header_buffer;
	guint id3v2_size;
};


G_DEFINE_TYPE(GstDSFParse, gst_dsfparse, GST_TYPE_DSD_CHUNK_PARSE)

GST_ELEMENT_REGISTER_DEFINE(dsfparse, "dsfparse", GST_RANK_PRIMARY + 1, GST_TYPE_DSFPARSE);


static void gst_dsfparse_finalize(GObject *object);

static bool gst_dsfparse_setup(GstDsdMediaParse *parse);
static void gst_dsfparse_teardown(GstDsdMediaParse *parse);

static guint64 gst_dsfparse_to_index(GstDsdMediaParse *parse, GstFormat source_format, guint64 source_value, ToIndexRoundingMode rounding_mode);
static guint64 gst_dsfparse_from_index(GstDsdMediaParse *parse, GstFormat dest_format, guint64 index);

static GstFlowReturn gst_dsfparse_produce_output(GstDsdMediaParse *parse, guint64 byte_position, guint64 end_payload_position, GstBuffer **output);

static GstTagList* gst_dsfparse_fill_tags(GstDsdMediaParse *parse, GstTagList *tag_list);

static void gst_dsfparse_current_index_after_seek(GstDsdMediaParse *parse, guint64 new_current_index);

static bool gst_dsfparse_verify_advance(GstDsdMediaParse *parse, guint64 byte_position, guint64 advance_amount, const gchar *advance_name);

static void gst_dsfparse_reset_all_fields(GstDSFParse *self);

static GstFlowReturn gst_dsfparse_parse_chunk_dsf_header(gpointer user_data, const Chunk &chunk);
static GstFlowReturn gst_dsfparse_parse_chunk_fmt(gpointer user_data, const Chunk &chunk);
static GstFlowReturn gst_dsfparse_parse_chunk_data(gpointer user_data, const Chunk &chunk);


static void gst_dsfparse_class_init(GstDSFParseClass *klass)
{
	GObjectClass *object_class;
	GstElementClass *element_class;
	GstDsdMediaParseClass *dsd_media_parse_class;

	GST_DEBUG_CATEGORY_INIT(dsfparse_debug, "dsfparse", 0, "DSF parser");

	object_class = G_OBJECT_CLASS(klass);
	element_class = GST_ELEMENT_CLASS(klass);
	dsd_media_parse_class = GST_DSD_MEDIA_PARSE_CLASS(klass);

	object_class->finalize = GST_DEBUG_FUNCPTR(gst_dsfparse_finalize);

	dsd_media_parse_class->setup = GST_DEBUG_FUNCPTR(gst_dsfparse_setup);
	dsd_media_parse_class->teardown = GST_DEBUG_FUNCPTR(gst_dsfparse_teardown);
	dsd_media_parse_class->to_index = GST_DEBUG_FUNCPTR(gst_dsfparse_to_index);
	dsd_media_parse_class->from_index = GST_DEBUG_FUNCPTR(gst_dsfparse_from_index);
	dsd_media_parse_class->produce_output = GST_DEBUG_FUNCPTR(gst_dsfparse_produce_output);
	dsd_media_parse_class->fill_tags = GST_DEBUG_FUNCPTR(gst_dsfparse_fill_tags);
	dsd_media_parse_class->current_index_after_seek = GST_DEBUG_FUNCPTR(gst_dsfparse_current_index_after_seek);
	dsd_media_parse_class->verify_advance = GST_DEBUG_FUNCPTR(gst_dsfparse_verify_advance);

	gst_element_class_add_pad_template(element_class, gst_static_pad_template_get(&sink_template));
	gst_element_class_add_pad_template(element_class, gst_static_pad_template_get(&src_template));

	gst_element_class_set_static_metadata(
		element_class,
		"DSF parser",
		"Codec/Parser/Audio",
		"Parses DSF data and outputs DSD content",
		"Carlos Rafael Giani <crg7475@mailbox.org>"
	);
}


static void gst_dsfparse_init(GstDSFParse *self)
{
	new (&(self->fmt_data)) std::optional<FmtData>();

	gst_dsd_chunk_parse_configure(
		GST_DSD_CHUNK_PARSE(self),
		ChunkSizeEndianness::LittleEndian,
		true,
		{
			// NOTE: The chunk lengths are from the DSF spec, but with the
			// 12 bytes for the chunk fourcc and length subtracted, because
			// that's how the chunk stack expects the lengths to be like.

			// Also, in DSF, the chunk hierarchy is very simple - it is flat,
			// all chunks are root chunks, and all chunks occur exactly once.
			// This is why their only_once values are all set to true, and
			// why the required_subchunk_fourccs are all empty.

			{
				GST_DSFPARSE_FOURCC_CHUNK_DSF_HEADER,
				{
					true,
					{},
					ChunkSizeType::ExactSize,
					16,
					gst_dsfparse_parse_chunk_dsf_header
				}
			},

			{
				GST_DSFPARSE_FOURCC_CHUNK_fmt,
				{
					true,
					{},
					ChunkSizeType::ExactSize,
					40,
					gst_dsfparse_parse_chunk_fmt
				}
			},

			{
				GST_DSFPARSE_FOURCC_CHUNK_data,
				{
					true,
					{},
					ChunkSizeType::FirstNumBytes,
					0,
					gst_dsfparse_parse_chunk_data
				}
			},
		}
	);
}


static void gst_dsfparse_finalize(GObject *object)
{
	GstDSFParse *self = GST_DSFPARSE(object);

	using OptionalFmtData = std::optional<FmtData>;
	self->fmt_data.~OptionalFmtData();

	gst_tag_list_replace(&(self->id3v2_tags), nullptr);
	gst_buffer_replace(&(self->id3v2_header_buffer), nullptr);

	G_OBJECT_CLASS(gst_dsfparse_parent_class)->finalize(object);
}


static bool gst_dsfparse_setup(GstDsdMediaParse *parse)
{
	GstDSFParse *self = GST_DSFPARSE(parse);

	gst_dsfparse_reset_all_fields(self);

	return GST_DSD_MEDIA_PARSE_CLASS(gst_dsfparse_parent_class)->setup(parse);
}


static void gst_dsfparse_teardown(GstDsdMediaParse *parse)
{
	GstDSFParse *self = GST_DSFPARSE(parse);

	gst_dsfparse_reset_all_fields(self);

	GST_DSD_MEDIA_PARSE_CLASS(gst_dsfparse_parent_class)->teardown(parse);
}


static guint64 gst_dsfparse_to_index(GstDsdMediaParse *parse, GstFormat source_format, guint64 source_value, ToIndexRoundingMode rounding_mode)
{
	GstDSFParse *self = GST_DSFPARSE_CAST(parse);
	guint64 index;

	switch (source_format) {
		case GST_FORMAT_BYTES: {
			if (rounding_mode == ToIndexRoundingMode::RoundingDown)
				index = source_value / self->alignment;
			else
				index = (source_value + (self->alignment - 1)) / self->alignment;

			break;
		}

		case GST_FORMAT_TIME: {
			if (rounding_mode == ToIndexRoundingMode::RoundingDown)
				index = gst_util_uint64_scale(
					source_value,
					self->total_num_indices,
					gst_dsd_media_parse_get_duration(parse)
				);
			else
				index = gst_util_uint64_scale_ceil(
					source_value,
					self->total_num_indices,
					gst_dsd_media_parse_get_duration(parse)
				);

			break;
		}

		default:
			g_assert_not_reached();
			break;
	}

	// Clamp the index to the first index past the valid range.
	// See the GstDsdMediaParse to_index() documentation for more.
	index = std::min(index, self->total_num_indices);

	return index;
}


static guint64 gst_dsfparse_from_index(GstDsdMediaParse *parse, GstFormat dest_format, guint64 index)
{
	GstDsdMediaParse *dsd_media_parse = GST_DSD_MEDIA_PARSE(parse);
	GstDSFParse *self = GST_DSFPARSE_CAST(parse);
	guint64 dest_value;

	switch (dest_format) {
		case GST_FORMAT_BYTES: {
			dest_value = index * self->alignment;
			dest_value = std::min(dest_value, gst_dsd_media_parse_get_payload_size(dsd_media_parse));

			break;
		}

		case GST_FORMAT_TIME: {
				dest_value = gst_util_uint64_scale(
					index,
					gst_dsd_media_parse_get_duration(parse),
					self->total_num_indices
				);
				dest_value = std::min(dest_value, gst_dsd_media_parse_get_duration(parse));

			break;
		}

		default:
			g_assert_not_reached();
			break;
	}

	return dest_value;
}


static GstFlowReturn gst_dsfparse_produce_output(GstDsdMediaParse *parse, guint64 byte_position, guint64 end_payload_position, GstBuffer **output)
{
	GstDSFParse *self = GST_DSFPARSE(parse);
	guint64 output_buffer_size = self->alignment;

	guint64 num_available_payload_bytes = end_payload_position - byte_position;

	// In DSF, partial blocks do not exist. If there is one, it is caused
	// by truncated data at the end of the payload. This residual data
	// cannot be used reliably, so just ignore it and report EOS.
	// (This _is_ the end of the payload, after all.)
	if (num_available_payload_bytes < output_buffer_size)
		return GST_FLOW_EOS;

	// Since one index corresponds to (num_channels * block_size) bytes,
	// we can output one index length worth's of data in one output buffer.

	GstBuffer *output_buffer;
	GstFlowReturn flow_ret = gst_dsd_media_parse_read_data_during_streaming(parse, output_buffer_size, &output_buffer);
	if (flow_ret != GST_FLOW_OK)
		return flow_ret;

	// Planar DSD content needs the plane offset meta attached to the buffer.

	GstDsdPlaneOffsetMeta *dsd_plane_offset_meta = gst_buffer_add_dsd_plane_offset_meta(
		output_buffer,
		self->fmt_data->num_channels,
		self->fmt_data->block_size,
		nullptr
	);
	for (guint32 channel_index = 0; channel_index < self->fmt_data->num_channels; ++channel_index)
		dsd_plane_offset_meta->offsets[channel_index] = channel_index * self->fmt_data->block_size;

	// Compute the PTS and duration for this buffer out of the current index.

	GstClockTime pts = gst_dsfparse_from_index(parse, GST_FORMAT_TIME, self->current_index);
	GstClockTime next_pts = gst_dsfparse_from_index(parse, GST_FORMAT_TIME, self->current_index + 1);

	GST_BUFFER_PTS(output_buffer) = pts;
	GST_BUFFER_DURATION(output_buffer) = GST_CLOCK_DIFF(pts, next_pts);

	*output = output_buffer;

	// See above. We just output one index length worth's of data.
	self->current_index++;

	return GST_FLOW_OK;
}


static GstTagList* gst_dsfparse_fill_tags(GstDsdMediaParse *parse, GstTagList *tag_list)
{
	GstDSFParse *self = GST_DSFPARSE(parse);
	GstTagList *new_tag_list;

	if (self->id3v2_tags != nullptr) {
		new_tag_list = gst_tag_list_merge(tag_list, self->id3v2_tags, GST_TAG_MERGE_REPLACE);
		gst_tag_list_unref(tag_list);
	} else {
		new_tag_list = tag_list;
	}

	return new_tag_list;
}


static void gst_dsfparse_current_index_after_seek(GstDsdMediaParse *parse, guint64 new_current_index)
{
	GstDSFParse *self = GST_DSFPARSE(parse);

	self->current_index = new_current_index;
}


static bool gst_dsfparse_verify_advance(GstDsdMediaParse *parse, guint64 byte_position, guint64 advance_amount, const gchar *advance_name)
{
	GstDSFParse *self = GST_DSFPARSE_CAST(parse);

	// Because ID3v2 metadata is located outside of the DSF header
	// chunk, the default implementation of this vmethod would report
	// an error when trying to parse metadata there. Override it to
	// avoid that error. Do check against the upstream size though
	// in case there is data corruption that'd lead to attempts
	// to read metadata beyond the the available media data.

	if (self->reading_id3v2_metadata) {
		guint64 advanced_position = byte_position + advance_amount;

		if (advanced_position > self->upstream_size) {
			GST_ERROR_OBJECT(
				self,
				"attempt to advance byte position %" G_GUINT64_FORMAT " by %" G_GUINT64_FORMAT
				" byte(s) for the \"%s\" operation goes %" G_GUINT64_FORMAT " byte(s) past "
				"the upstream size",
				byte_position,
				advance_amount,
				advance_name,
				advanced_position - self->upstream_size
			);
			return false;
		} else {
			return true;
		}
	} else {
		return GST_DSD_MEDIA_PARSE_CLASS(gst_dsfparse_parent_class)->verify_advance(parse, byte_position, advance_amount, advance_name);
	}
}


static void gst_dsfparse_reset_all_fields(GstDSFParse *self)
{
	self->fmt_data = std::nullopt;
	self->current_index = 0;
	self->total_num_indices = 0;
	self->alignment = 0;
	self->upstream_size = 0;
	gst_tag_list_replace(&(self->id3v2_tags), nullptr);
	self->reading_id3v2_metadata = false;
	self->id3v2_pre_seek_byte_position = 0;
	gst_buffer_replace(&(self->id3v2_header_buffer), nullptr);
	self->id3v2_size = 0;
}


static GstFlowReturn gst_dsfparse_parse_chunk_dsf_header(gpointer user_data, const Chunk &)
{
	GstDsdMediaParse *parse = GST_DSD_MEDIA_PARSE_CAST(user_data);
	GstDSFParse *self = GST_DSFPARSE(parse);

	// Due to the way ID3v2 metadata parsing works, this
	// parser function can be entered multiple times.
	// Sometimes for parsing the actual DSF header chunk,
	// sometimes to continue ID3v2 parsing. Distinguish
	// by looking at the reading_id3v2_metadata flag.

	if (!self->reading_id3v2_metadata) {
		// Get the 16 DSF header chunk bytes.
		GstBuffer *buffer;
		GstFlowReturn flow_ret = gst_dsd_media_parse_read_data_during_scan(parse, 16, &buffer);
		if (G_UNLIKELY(flow_ret != GST_FLOW_OK))
			return flow_ret;

		MappedBuffer mapped_dsf_header_data{buffer, GST_MAP_READ};
		RETURN_IF_GSTBUFFER_MAPPING_FAILED(
			self,
			mapped_dsf_header_data,
			GST_FLOW_ERROR,
			"Could not map the DSF header data"
		);

		guint64 total_file_size = GST_READ_UINT64_LE(mapped_dsf_header_data.data());
		GST_DEBUG_OBJECT(
			self,
			"DSD header chunk specifies a file size of %" G_GUINT64_FORMAT " byte(s)",
			total_file_size
		);

		// Query the upstream size in bytes as a safeguard in case
		// the total file size from the header is incorrect. Also,
		// other chunk parsers will need this size for their processing.
		if (!gst_dsd_media_parse_get_upstream_size(parse, &(self->upstream_size))) {
			GST_ELEMENT_ERROR(
				self,
				STREAM,
				FAILED,
				("Could not get size of the DSF medium."),
				("upstream bytes duration query failed; cannot get size of DSF media")
			);
			return GST_FLOW_ERROR;
		}

		// DSF media with less than (28+52+12) bytes cannot exist,
		// since that is the total size of the DSD and fmt chunks
		// and the fourcc+size of the data chunk.
		if (self->upstream_size < (28+52+12)) {
			GST_ELEMENT_ERROR(
				self,
				STREAM,
				DEMUX,
					("Invalid size of DSF medium."),
					(
						"upstream duration query reports a size of %" G_GUINT64_FORMAT
						" byte(s), which is not valid DSF",
						self->upstream_size
					)
			);
			return GST_FLOW_ERROR;
		}

		GST_DEBUG_OBJECT(self, "upstream specifies a size of %" G_GUINT64_FORMAT " byte(s)", self->upstream_size);

		// In truncated files, total_file_size will be larger than upstream_size.
		// Such files may still be playable though, so we cannot use this comparison
		// as a basis for GST_ELEMENT_ERROR() calls and for returning GST_FLOW_ERROR.
		// Instead, merely log this as a warning.

		if (total_file_size > self->upstream_size) {
			GST_WARNING_OBJECT(
				self,
				"upstream duration query reports a size of %" G_GUINT64_FORMAT
				" byte(s), but DSF header chunk reports a total file size of %"
				G_GUINT64_FORMAT " byte(s)",
				self->upstream_size,
				total_file_size
			);
		} else if (total_file_size < self->upstream_size) {
			GST_WARNING_OBJECT(
				self,
				"DSF header chunk's total file size %" G_GUINT64_FORMAT " is smaller "
				"than the upstream size %" G_GUINT64_FORMAT,
				total_file_size,
				self->upstream_size
			);
		}

		// Get the byte position of the ID3v2 metadata, and perform
		// sanity checks on that byte position to catch invalid ones.

		guint64 metadata_position = GST_READ_UINT64_LE(mapped_dsf_header_data.data() + 8);
		if (metadata_position == 0) {
			// Per the DSF spec, a metadata pointer of 0 means that the file has no
			// metadata. This is not an error, and not something to warn about.
			GST_DEBUG_OBJECT(self, "DSF header chunk indicates that this medium has no metadata");
			gst_dsd_chunk_parse_finish_current_chunk(GST_DSD_CHUNK_PARSE(self));
		} else if (metadata_position > (self->upstream_size - GST_TAG_ID3V2_HEADER_SIZE)) {
			// The metadata position does not allow for an ID3v2 header to be present.
			// Don't bother parsing ID3v2 - just finish the DSF header chunk.

			GST_WARNING_OBJECT(
				self,
				"metadata position is invalid since it exceeds the file size; not attempting to read ID3v2 metadata"
			);

			gst_dsd_chunk_parse_finish_current_chunk(GST_DSD_CHUNK_PARSE(self));
		} else {
			GST_DEBUG_OBJECT(
				self,
				"ID3v2 metadata is present at position %" G_GUINT64_FORMAT " in the medium; attempting to seek to it",
				metadata_position
			);

			// Save the current byte position to ensure chunk
			// parsing can continue later correctly.
			self->id3v2_pre_seek_byte_position = gst_dsd_media_parse_get_current_byte_position(parse);
			GST_DEBUG_OBJECT(self, "pre-seek byte position is %" G_GUINT64_FORMAT, self->id3v2_pre_seek_byte_position);

			// Now try to seek to the metadata position.
			if (gst_dsd_media_parse_seek_during_scan(parse, metadata_position)) {
				GST_DEBUG_OBJECT(self, "seek to metadata successful");
				self->reading_id3v2_metadata = true;
			} else {
				// If seeking failed, just finish the current chunk and continue.
				gst_dsd_chunk_parse_finish_current_chunk(GST_DSD_CHUNK_PARSE(self));
			}
		}
	}

	// If we are parsing ID3v2 metadata, or the code above just seeked
	// to the metadata, continue or commence the metadata parsing. It
	// takes place in two iterations. First, the fixed-size ID3v2 header
	// is parsed. This is necessary for determining the variable length
	// of the rest of the ID3v2 metadata. Next, read that remaining
	// metadata, and merge the gstbuffer that contains the ID3v2 header
	// with the gstbuffer that contains the rest. Then seek back to
	// where the byte position was prior to seeking to the metadata,
	// and finish the chunk, since it has been fully parsed by that point.

	if (self->reading_id3v2_metadata) {
		if (self->id3v2_header_buffer == nullptr) {
			// In the first iterations, the header has not been read yet.

			GstFlowReturn flow_ret = gst_dsd_media_parse_read_data_during_scan(parse, GST_TAG_ID3V2_HEADER_SIZE, &(self->id3v2_header_buffer));
			if (G_UNLIKELY(flow_ret != GST_FLOW_OK))
				return flow_ret;

			self->id3v2_size = gst_tag_get_id3v2_tag_size(self->id3v2_header_buffer);
			if (self->id3v2_size == 0) {
				GST_WARNING_OBJECT(
					self,
					"could not parse ID3v2 metadata header; no usable ID3v2 tag at the metadata position"
				);
			} else {
				GST_DEBUG_OBJECT(self, "ID3v2 metadata size is %u byte(s)", self->id3v2_size);
			}
		} else {
			// We get here in the second iteration, after having read the ID3v2 header.

			// NOTE: id3v2_size covers the _entire_ ID3v2 tag, including its
			// header. That header was already read in the first iteration, so
			// the byte position is at (metadata position + header size) by now,
			// and only the remaining bytes are left to read. Consequently, the
			// bounds checks below must use those remaining bytes and not
			// id3v2_size, otherwise a tag that ends exactly at the end of the
			// medium - which is where the DSF specification places it - would
			// be misdetected as exceeding the medium's bounds.
			guint64 num_leftover_id3v2_bytes = (self->id3v2_size > GST_TAG_ID3V2_HEADER_SIZE)
				? (guint64(self->id3v2_size) - GST_TAG_ID3V2_HEADER_SIZE)
				: 0;
			guint64 current_byte_position = gst_dsd_media_parse_get_current_byte_position(parse);
			guint64 advanced_byte_position = current_byte_position + num_leftover_id3v2_bytes;
			if (advanced_byte_position < current_byte_position) {
				// This occurs when (current_byte_position + num_leftover_id3v2_bytes)
				// causes an unsigned 64-bit integer overflow.
				GST_WARNING_OBJECT(
					self,
					"could not parse ID3v2 metadata header; reading "
					"%" G_GUINT64_FORMAT " byte(s) at position %" G_GUINT64_FORMAT " "
					"exceeds the 64-bit unsigned integer range",
					num_leftover_id3v2_bytes,
					current_byte_position
				);
			} else if (advanced_byte_position > self->upstream_size) {
				GST_WARNING_OBJECT(
					self,
					"could not parse ID3v2 metadata header; reading "
					"%" G_GUINT64_FORMAT " byte(s) at position %" G_GUINT64_FORMAT " "
					"exceeds the upstream size by %" G_GUINT64_FORMAT " byte(s)",
					num_leftover_id3v2_bytes,
					current_byte_position,
					advanced_byte_position - self->upstream_size
				);
			} else if (self->id3v2_size > GST_TAG_ID3V2_HEADER_SIZE) {
				// Read the rest of the ID3v2 metadata.
				GstBuffer *id3v2_buffer = nullptr;
				GstFlowReturn flow_ret = gst_dsd_media_parse_read_data_during_scan(parse, num_leftover_id3v2_bytes, &id3v2_buffer);
				if (G_UNLIKELY(flow_ret != GST_FLOW_OK))
					return flow_ret;

				// Now merge the header with the rest by prepending the former to the latter.
				id3v2_buffer = gst_buffer_append(self->id3v2_header_buffer, id3v2_buffer);
				self->id3v2_header_buffer = nullptr;
				ScopeGuard id3v2_buffer_guard([&]() { gst_buffer_unref(id3v2_buffer); });

				// Parse the ID3v2 metadata, converting it to a GStreamer tag list.
				GstTagList *id3v2_tags = gst_tag_list_from_id3v2_tag(id3v2_buffer);

				if (id3v2_tags == nullptr) {
					GST_WARNING_OBJECT(
						self,
						"could not parse ID3v2 metadata; no usable ID3v2 tag at the metadata position"
					);
				} else {
					gst_tag_list_take(&(self->id3v2_tags), id3v2_tags);
				}
			}

			self->reading_id3v2_metadata = false;

			// Now seek back to where the byte position was prior to the
			// seek to the metadata to allow the chunk parsing to continue.

			if (!gst_dsd_media_parse_seek_during_scan(parse, self->id3v2_pre_seek_byte_position)) {
				GST_ELEMENT_ERROR(
					self,
					STREAM,
					FAILED,
					("Error while parsing DSF metadata."),
					("failed to seek back to the pre-seek parse position %" G_GUINT64_FORMAT, self->id3v2_pre_seek_byte_position)
				);
				return GST_FLOW_ERROR;
			}

			GST_DEBUG_OBJECT(self, "seek back to the pre-seek parse position %" G_GUINT64_FORMAT " successful", self->id3v2_pre_seek_byte_position);

			gst_dsd_chunk_parse_finish_current_chunk(GST_DSD_CHUNK_PARSE(self));
		}
	}

	return GST_FLOW_OK;
}


static GstFlowReturn gst_dsfparse_parse_chunk_fmt(gpointer user_data, const Chunk &)
{
	GstDsdMediaParse *parse = GST_DSD_MEDIA_PARSE_CAST(user_data);
	GstDSFParse *self = GST_DSFPARSE(parse);

	GstBuffer *buffer = nullptr;
	GstFlowReturn flow_ret = gst_dsd_media_parse_read_data_during_scan(parse, 40, &buffer);
	if (G_UNLIKELY(flow_ret != GST_FLOW_OK))
		return flow_ret;

	MappedBuffer mapped_fmt_data{buffer, GST_MAP_READ};
	RETURN_IF_GSTBUFFER_MAPPING_FAILED(
		self,
		mapped_fmt_data,
		GST_FLOW_ERROR,
		"Could not map the FMT chunk payload"
	);

	FmtData new_fmt_data;

	guint32 format_version = GST_READ_UINT32_LE(mapped_fmt_data.data());
	if (G_UNLIKELY(format_version != 1)) {
		GST_ELEMENT_ERROR(
			self,
			STREAM,
			FORMAT,
			("Unsupported DSF format version."),
			(
				"format version %" G_GUINT32_FORMAT " is not supported (only version 1 is supported)",
				format_version
			)
		);
		return GST_FLOW_NOT_SUPPORTED;
	}

	guint32 format_id = GST_READ_UINT32_LE(mapped_fmt_data.data() + 4);
	if (G_UNLIKELY(format_id != 0)) {
		GST_ELEMENT_ERROR(
			self,
			STREAM,
			FORMAT,
			("Unsupported DSF format ID."),
			(
				"format ID %" G_GUINT32_FORMAT " is not supported (only ID 0 is supported, which is raw DSD)",
				format_id
			)
		);
		return GST_FLOW_NOT_SUPPORTED;
	}

	// DSF specifies a fixed set of channel orders. All of these
	// are compatible with GStreamer's canonical order. This implies
	// that there is no need to worry about channel reordering.

	guint32 channel_type = GST_READ_UINT32_LE(mapped_fmt_data.data() + 8);
	switch (channel_type) {
		case 1: {
			new_fmt_data.channel_positions = {
				GST_AUDIO_CHANNEL_POSITION_MONO,
			};
			break;
		}

		case 2: {
			new_fmt_data.channel_positions = {
				GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
				GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
			};
			break;
		}

		case 3: {
			new_fmt_data.channel_positions = {
				GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
				GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
				GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
			};
			break;
		}

		case 4: {
			new_fmt_data.channel_positions = {
				GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
				GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
				GST_AUDIO_CHANNEL_POSITION_REAR_LEFT,
				GST_AUDIO_CHANNEL_POSITION_REAR_RIGHT,
			};
			break;
		}

		case 5: {
			new_fmt_data.channel_positions = {
				GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
				GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
				GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
				GST_AUDIO_CHANNEL_POSITION_LFE1,
			};
			break;
		}

		case 6: {
			new_fmt_data.channel_positions = {
				GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
				GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
				GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
				GST_AUDIO_CHANNEL_POSITION_REAR_LEFT,
				GST_AUDIO_CHANNEL_POSITION_REAR_RIGHT,
			};
			break;
		}

		case 7: {
			new_fmt_data.channel_positions = {
				GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
				GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
				GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
				GST_AUDIO_CHANNEL_POSITION_LFE1,
				GST_AUDIO_CHANNEL_POSITION_REAR_LEFT,
				GST_AUDIO_CHANNEL_POSITION_REAR_RIGHT,
			};
			break;
		}

		default: {
			GST_ELEMENT_ERROR(
				self,
				STREAM,
				FORMAT,
				("Unsupported DSF channel type."),
				("channel type %" G_GUINT32_FORMAT " is not supported", channel_type)
			);
			return GST_FLOW_NOT_SUPPORTED;
		}
	}

	new_fmt_data.num_channels = GST_READ_UINT32_LE(mapped_fmt_data.data() + 12);
	if (G_UNLIKELY(new_fmt_data.num_channels != new_fmt_data.channel_positions.size())) {
		GST_ELEMENT_ERROR(
			self,
			STREAM,
			DEMUX,
			("DSF channel type does not match the number of channels."),
			(
				"channel type %" G_GUINT32_FORMAT " expects %" G_GSIZE_FORMAT " channel(s), but media specifies %" G_GUINT32_FORMAT " channel(s)",
				channel_type,
				gsize(new_fmt_data.channel_positions.size()),
				new_fmt_data.num_channels
			)
		);
		return GST_FLOW_ERROR;
	}

	const guint32 min_valid_sample_rate = GST_DSD_MAKE_DSD_RATE_44x(64) * 8;
	guint32 sample_rate = GST_READ_UINT32_LE(mapped_fmt_data.data() + 16);
	if (G_UNLIKELY(sample_rate < min_valid_sample_rate)) {
		GST_ELEMENT_ERROR(
			self,
			STREAM,
			DEMUX,
			("Invalid DSF sample rate."),
			(
				"got invalid sample rate %" G_GUINT32_FORMAT "; must be at least %" G_GUINT32_FORMAT,
				sample_rate,
				min_valid_sample_rate
			)
		);
		return GST_FLOW_ERROR;
	}

	GST_INFO_OBJECT(self, "sample rate is %" G_GUINT32_FORMAT " Hz (= bits per second)", sample_rate);

	// DSD in GStreamer uses bytes per second as the sample rate, not bits per second.
	// See https://gstreamer.freedesktop.org/documentation//audio/gstdsd.html?gi-language=c#GstDsdInfo for more.
	sample_rate /= 8;
	GST_INFO_OBJECT(self, "converted sample rate to bytes per second: %" G_GUINT32_FORMAT, sample_rate);

	new_fmt_data.sample_rate = sample_rate;

	guint32 bits_per_sample = GST_READ_UINT32_LE(mapped_fmt_data.data() + 20);
	switch (bits_per_sample) {
		case 1:
		case 8:
			new_fmt_data.reversed_bytes = (bits_per_sample == 1);
			GST_DEBUG_OBJECT(
				self,
				"bits per sample value is %" G_GUINT32_FORMAT " -> reversed bytes: %d",
				bits_per_sample,
				gint(new_fmt_data.reversed_bytes)
			);
			break;

		default:
			GST_ELEMENT_ERROR(
				self,
				STREAM,
				DEMUX,
				("Invalid DSF bits per sample value."),
				("got invalid bits per sample value %" G_GUINT32_FORMAT, bits_per_sample)
			);
			return GST_FLOW_ERROR;
	}

	guint64 num_dsd_bits = GST_READ_UINT64_LE(mapped_fmt_data.data() + 24);
	if (G_UNLIKELY(num_dsd_bits > (G_MAXUINT64 - 7))) {
		// The conversion to bytes below rounds up by adding 7, which would
		// wrap around in very rare cases where the bit count is already close
		// to the maximum of the 64-bit unsigned integer range. The result
		// would be a tiny (or zero) byte count due to wrap around. This
		// would silently discard the entire payload.
		GST_ELEMENT_ERROR(
			self,
			STREAM,
			DEMUX,
			("Invalid DSF sample count."),
			(
				"number of DSD bits %" G_GUINT64_FORMAT " is out of range",
				num_dsd_bits
			)
		);
		return GST_FLOW_ERROR;
	}
	if (G_UNLIKELY(num_dsd_bits == 0)) {
		GST_ELEMENT_ERROR(
			self,
			STREAM,
			DEMUX,
			("Invalid DSF sample count."),
			("number of DSD bits is 0")
		);
		return GST_FLOW_ERROR;
	}

	GST_DEBUG_OBJECT(self, "media contains %" G_GUINT64_FORMAT " dsd bits", num_dsd_bits);
	new_fmt_data.expected_num_dsd_bytes_per_channel = (num_dsd_bits + 7) / 8;
	new_fmt_data.actual_num_dsd_bytes_per_channel = new_fmt_data.expected_num_dsd_bytes_per_channel;

	new_fmt_data.block_size = GST_READ_UINT32_LE(mapped_fmt_data.data() + 32);
	if (G_UNLIKELY(new_fmt_data.block_size == 0)) {
		GST_ELEMENT_ERROR(
			self,
			STREAM,
			DEMUX,
			("Invalid DSF block size."),
			("DSF block size is 0")
		);
		return GST_FLOW_ERROR;
	}

	GST_DEBUG_OBJECT(self, "block size is %" G_GUINT32_FORMAT " byte(s)", new_fmt_data.block_size);

	self->alignment = guint64(new_fmt_data.block_size) * guint64(new_fmt_data.num_channels);
	GST_DEBUG_OBJECT(self, "alignment is %" G_GUINT64_FORMAT " byte(s)", self->alignment);

	// The last 4 bytes in the 40 byte chunk payload are reserved.

	self->fmt_data = new_fmt_data;

	gst_dsd_chunk_parse_finish_current_chunk(GST_DSD_CHUNK_PARSE(self));

	return GST_FLOW_OK;
}


static GstFlowReturn gst_dsfparse_parse_chunk_data(gpointer user_data, const Chunk &chunk)
{
	GstDsdMediaParse *parse = GST_DSD_MEDIA_PARSE_CAST(user_data);
	GstDSFParse *self = GST_DSFPARSE(parse);

	// The fmt chunk must come before the data chunk. Otherwise,
	// gst_dsd_media_parse_report_payload_found() cannot reliably
	// be called, since the base class must be configured prior
	// to that call - and gst_dsd_media_parse_configure() is
	// called right here, further below.
	if (G_UNLIKELY(!self->fmt_data.has_value())) {
		GST_ELEMENT_ERROR(
			self,
			STREAM,
			DEMUX,
			("Incomplete DSF media."),
			("encountered the data chunk before the fmt chunk was parsed")
		);
		return GST_FLOW_ERROR;
	}

	// Verify the chunk size. In truncated files, the number of available
	// bytes will be smaller than that size. This is therefore an indicator
	// that the DSD data is truncated.
	guint64 payload_size = chunk.size;
	guint64 num_available_bytes = self->upstream_size - gst_dsd_media_parse_get_current_byte_position(parse);
	bool data_truncated = false;

	if (G_UNLIKELY(num_available_bytes < self->alignment)) {
		GST_ELEMENT_ERROR(
			self,
			STREAM,
			DEMUX,
			("Insufficient DSF payload."),
			(
				"only %" G_GUINT64_FORMAT " payload byte(s) are available; alignment is %"
				G_GUINT64_FORMAT "; cannot play this residual data",
				num_available_bytes,
				self->alignment
			)
		);
		return GST_FLOW_ERROR;
	}

	if (G_UNLIKELY(payload_size > num_available_bytes)) {
		GST_WARNING_OBJECT(
			self,
			"data chunk declares %" G_GUINT64_FORMAT " payload byte(s), but only %"
			G_GUINT64_FORMAT " byte(s) are available; the medium seems to be "
			"truncated; clamping the payload size",
			payload_size,
			num_available_bytes
		);
		payload_size = num_available_bytes;
		data_truncated = true;

		// We also need to recalculate actual_num_dsd_bytes_per_channel,
		// since the original value does not match the actual truncated data size.

		self->fmt_data->actual_num_dsd_bytes_per_channel = payload_size / self->fmt_data->num_channels;
		GST_DEBUG_OBJECT(self, "recalculated number of DSD bytes per channel to %" G_GUINT64_FORMAT, self->fmt_data->actual_num_dsd_bytes_per_channel);
	}

	// Check that the data chunk's size is an integer multiple
	// of the alignment value. If not, there's partial data,
	// and a warning about that is logged.
	{
		guint64 remainder = payload_size % self->alignment;
		if (remainder != 0) {
			GST_WARNING_OBJECT(
				self,
				"there are %" G_GUINT64_FORMAT  " byte(s) at the end "
				"of the data chunk that do not fit the block alignment "
				"of %" G_GUINT64_FORMAT " byte(s)",
				remainder,
				self->alignment
			);
		}
	}

	// Round up when calculating the total number of indices
	// to account for partial data at the very last index.
	self->total_num_indices = (self->fmt_data->actual_num_dsd_bytes_per_channel + (self->fmt_data->block_size - 1)) / self->fmt_data->block_size;
	GST_DEBUG_OBJECT(self, "total number of indices is %" G_GUINT64_FORMAT, self->total_num_indices);

	GstClockTime duration = gst_util_uint64_scale_int_ceil(
		self->fmt_data->actual_num_dsd_bytes_per_channel,
		GST_SECOND,
		self->fmt_data->sample_rate
	);

	GstDsdInfo output_dsd_info;

	gst_dsd_info_set_format(
		&output_dsd_info,
		GST_DSD_FORMAT_U8,
		self->fmt_data->sample_rate,
		self->fmt_data->num_channels,
		self->fmt_data->channel_positions.data()
	);
	GST_DSD_INFO_LAYOUT(&output_dsd_info) = GST_AUDIO_LAYOUT_NON_INTERLEAVED;
	GST_DSD_INFO_REVERSED_BYTES(&output_dsd_info) = self->fmt_data->reversed_bytes;

	GstCaps *output_caps = gst_dsd_info_to_caps(&output_dsd_info);

	gst_dsd_media_parse_configure(
		parse,
		output_caps,
		duration
	);

	GST_DEBUG_OBJECT(self, "located DSD data with %" G_GUINT64_FORMAT " byte(s)", payload_size);

	GstFlowReturn flow_ret = gst_dsd_media_parse_report_payload_found(parse, payload_size, data_truncated);
	if (flow_ret != GST_FLOW_OK)
		return flow_ret;

	gst_dsd_media_parse_scanning_finished(parse);

	gst_dsd_chunk_parse_finish_current_chunk(GST_DSD_CHUNK_PARSE(self));

	return GST_FLOW_NOTHING_TO_READ;
}


void gst_dsfparse_type_find(GstTypeFind *tf, G_GNUC_UNUSED gpointer user_data)
{
	const guint8 *data;

	if ((data = gst_type_find_peek(tf, 0, 32)) != nullptr) {
		if ((std::memcmp(data, "DSD ", 4) == 0) && (std::memcmp(data + 28, "fmt ", 4) == 0)) {
			gst_type_find_suggest_simple(tf, GST_TYPE_FIND_NEARLY_CERTAIN, GST_DSF_MEDIA_TYPE, nullptr, nullptr);
		}
	}
}
