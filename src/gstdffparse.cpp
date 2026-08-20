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
#include <algorithm>
#include "mapped_buffer.hpp"
#include "gstdffparse.hpp"


GST_DEBUG_CATEGORY_STATIC(dffparse_debug);
#define GST_CAT_DEFAULT dffparse_debug


// TODO:
// - DST support
// - resync_deviated_upstream_seek() implementation
// - COMT chunk support


// DFF files are scanned chunk by chunk. The Scanning Info stage
// is finished once the top level FRM8 chunk is finished.


// About indices and position boundaries in GstDFFParse
//
// (See the GstDsdMediaParse documentation about these concepts
// if they aren't known already.)
//
// In DFF, channels are interleaved at a byte level. The grouping
// format is U8. Given N channels, the first N bytes contain the first
// 8 DSD bits for each channel. The next N bytes contain the next 8
// DSD bits for each channel etc. DFF calls these groups of N bytes
// a "frame". This implies that the DFF frame size in bytes equals
// the number of channels.
//
// Naturally, this maps to the position boundary concept. The alignment
// simply follows the frames. Thus, in GstDFFParse, an index is just
// the index of a DFF frame. The index -> bytes conversion then is:
//
//   bytes = index * num_channels
//
// Because num_channels equals the number of bytes per frame.
//
// As for index -> time conversion: In DFF, the sample rate defines
// how many DSD bytes per second per channel are output. This maps
// well to how GstDsd interprets sample rates (it also uses a byte
// based interpretation). Given N channels, and a sample rate S,
// this means there are S*N DSD bytes per second. This corresponds
// exactly to how DFF frames are structured (groups of N bytes).
// This means that the DFF sample rate specifies how many DFF frames
// per second there are. And since the index is the DFF frame index
// in GstDFFParse, the index -> time conversion goes as follows:
//
//   time = gst_util_uint64_scale_int(index, GST_SECOND, sample_rate)
//
// (time is in nanoseconds as it is commonly done in GStreamer.)
//
// gst_dffparse_from_index() performs these calculations.
//
// The other way round is:
//
//   index = bytes / num_channels
//   index = gst_util_uint64_scale_int(time, sample_rate, GST_SECOND)
//
// gst_dffparse_to_index() performs these calculations. However,
// that function has the specialty that it can be requested by
// the caller to round _up_, while the formulas above round _down_.
// the rounding-up variants are:
//
//   index = (bytes + (num_channels - 1)) / num_channels
//   index = gst_util_uint64_scale_int_ceil(time, sample_rate, GST_SECOND)


#define GST_DFFPARSE_FOURCC_CHUNK_FRM8             GST_MAKE_FOURCC('F', 'R', 'M', '8')
#define GST_DFFPARSE_FOURCC_CHUNK_FVER             GST_MAKE_FOURCC('F', 'V', 'E', 'R')
#define GST_DFFPARSE_FOURCC_CHUNK_PROP             GST_MAKE_FOURCC('P', 'R', 'O', 'P')
#define GST_DFFPARSE_FOURCC_CHUNK_FS               GST_MAKE_FOURCC('F', 'S', ' ', ' ')
#define GST_DFFPARSE_FOURCC_CHUNK_CHNL             GST_MAKE_FOURCC('C', 'H', 'N', 'L')
#define GST_DFFPARSE_FOURCC_CHUNK_CMPR             GST_MAKE_FOURCC('C', 'M', 'P', 'R')
#define GST_DFFPARSE_FOURCC_CHUNK_LSCO             GST_MAKE_FOURCC('L', 'S', 'C', 'O')
#define GST_DFFPARSE_FOURCC_CHUNK_DSD              GST_MAKE_FOURCC('D', 'S', 'D', ' ')
#define GST_DFFPARSE_FOURCC_CHUNK_DIIN             GST_MAKE_FOURCC('D', 'I', 'I', 'N')
#define GST_DFFPARSE_FOURCC_CHUNK_DIAR             GST_MAKE_FOURCC('D', 'I', 'A', 'R')
#define GST_DFFPARSE_FOURCC_CHUNK_DITI             GST_MAKE_FOURCC('D', 'I', 'T', 'I')

#define GST_DFFPARSE_FOURCC_FORM_TYPE_DSD          GST_MAKE_FOURCC('D', 'S', 'D', ' ')

#define GST_DFFPARSE_FOURCC_COMPRESSION_TYPE_DSD   GST_MAKE_FOURCC('D', 'S', 'D', ' ')
#define GST_DFFPARSE_FOURCC_COMPRESSION_TYPE_DST   GST_MAKE_FOURCC('D', 'S', 'T', ' ')

#define GST_DFFPARSE_FOURCC_PROP_TYPE_SND          GST_MAKE_FOURCC('S', 'N', 'D', ' ')

#define GST_DFFPARSE_FOURCC_CHANNEL_ID_SLFT        GST_MAKE_FOURCC('S', 'L', 'F', 'T')
#define GST_DFFPARSE_FOURCC_CHANNEL_ID_SRGT        GST_MAKE_FOURCC('S', 'R', 'G', 'T')
#define GST_DFFPARSE_FOURCC_CHANNEL_ID_MLFT        GST_MAKE_FOURCC('M', 'L', 'F', 'T')
#define GST_DFFPARSE_FOURCC_CHANNEL_ID_MRGT        GST_MAKE_FOURCC('M', 'R', 'G', 'T')
#define GST_DFFPARSE_FOURCC_CHANNEL_ID_LS          GST_MAKE_FOURCC('L', 'S', ' ', ' ')
#define GST_DFFPARSE_FOURCC_CHANNEL_ID_RS          GST_MAKE_FOURCC('R', 'S', ' ', ' ')
#define GST_DFFPARSE_FOURCC_CHANNEL_ID_C           GST_MAKE_FOURCC('C', ' ', ' ', ' ')
#define GST_DFFPARSE_FOURCC_CHANNEL_ID_LFE         GST_MAKE_FOURCC('L', 'F', 'E', ' ')


enum class DFFLoudspeakerConfig {
	Undefined,
	Reserved,
	Stereo,
	FiveChannels,
	FiveChannelsPlusLFE  // = 5.1 audio
};


static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE(
	"sink",
	GST_PAD_SINK,
	GST_PAD_ALWAYS,
	GST_STATIC_CAPS(GST_DFF_MEDIA_TYPE)
);


static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE(
	"src",
	GST_PAD_SRC,
	GST_PAD_ALWAYS,
	GST_STATIC_CAPS(
		GST_DSD_MEDIA_TYPE ", "
		"format = (string) DSDU8, "
		"rate = " GST_AUDIO_RATE_RANGE ", "
		"layout = (string) interleaved, "
		"reversed-bytes = (gboolean) false, "
		"channels = " GST_AUDIO_CHANNELS_RANGE
	)
);


// This struct contains all the data from the PROP chunk and its
// subchunks. The values in this must be valid by the time the
// PROP chunk is fully parsed, that is, its validate() method
// must return true by then.
struct PropData
{
	// These fields are required. If they are std::nullopt
	// (or, in the channel positions case, empty), it indicates
	// that required media information is missing, and the DFF
	// media is invalid.

	std::optional<guint32> sample_rate;
	std::optional<guint16> num_channels;
	std::vector<GstAudioChannelPosition> channel_positions;

	std::optional<guint32> compression_type;

	// These fields are optional. If they are missing, the
	// DFF media remains valid - the associated information
	// just is not there.
	// (Using std::optional even for std::string to distinguish
	// between "no value set" and an empty string.)

	DFFLoudspeakerConfig loudspeaker_config = DFFLoudspeakerConfig::Undefined;
	// This is only defined if loudspeaker_config is set to a valid configuration.
	guint64 num_lsconfig_channels = 0;
	std::optional<std::string> artist;
	std::optional<std::string> title;

	// Helper values used by parsers.

	std::optional<guint32> num_artist_chars;
	std::optional<guint32> num_title_chars;
	gsize num_compression_name_chars = 0;

	// Validates the parsed contents of the PROP chunk. If
	// this returns false, the dataflow cannot commence.

	bool validate(GstDFFParse *self) const
	{
		bool valid = true;

		if (!sample_rate) {
			GST_ERROR_OBJECT(self, "sample rate is missing");
			valid = false;
		}

		if (!num_channels) {
			GST_ERROR_OBJECT(self, "number of channels is missing");
			valid = false;
		}

		if (channel_positions.empty()) {
			GST_ERROR_OBJECT(self, "channel positions are missing");
			valid = false;
		}

		if (!compression_type) {
			GST_ERROR_OBJECT(self, "compression type is missing");
			valid = false;
		}

		return valid;
	}
};


struct _GstDFFParse
{
	GstDsdMediaParse parent;

	PropData prop_data;

	// Computed once the PROP chunk has been fully parsed
	// (including all of its subchunks). This is calculated
	// to cover approximately 5 ms worth of playtime.
	// See gst_dffparse_finish_chunk_prop() for more.
	guint64 output_buffer_size;

	// Incremented in gst_dffparse_produce_output()
	// and set in gst_dffparse_current_index_after_seek().
	guint64 current_index;

	// Set when handling the DSD chunk.
	guint64 total_num_indices;

	// Set when the FRM8 chunk's start is parsed.
	guint64 upstream_size;
};


G_DEFINE_TYPE(GstDFFParse, gst_dffparse, GST_TYPE_DSD_CHUNK_PARSE)

GST_ELEMENT_REGISTER_DEFINE(dffparse, "dffparse", GST_RANK_PRIMARY + 1, GST_TYPE_DFFPARSE);


static void gst_dffparse_finalize(GObject *object);

static bool gst_dffparse_setup(GstDsdMediaParse *parse);
static void gst_dffparse_teardown(GstDsdMediaParse *parse);

static guint64 gst_dffparse_to_index(GstDsdMediaParse *parse, GstFormat source_format, guint64 source_value, ToIndexRoundingMode rounding_mode);
static guint64 gst_dffparse_from_index(GstDsdMediaParse *parse, GstFormat dest_format, guint64 index);

static GstFlowReturn gst_dffparse_produce_output(GstDsdMediaParse *parse, guint64 byte_position, guint64 end_payload_position, GstBuffer **output);

static GstTagList* gst_dffparse_fill_tags(GstDsdMediaParse *parse, GstTagList *tag_list);

static void gst_dffparse_current_index_after_seek(GstDsdMediaParse *parse, guint64 new_current_index);

static void gst_dffparse_reset_all_fields(GstDFFParse *self);

static std::string gst_dffparse_convert_text_bytes_to_utf8(GstDFFParse *self, const gchar *text_bytes, gsize length, const gchar *text_bytes_name);

static GstFlowReturn gst_dffparse_parse_chunk_frm8(gpointer user_data, const Chunk &chunk);
static bool gst_dffparse_finish_chunk_frm8(gpointer user_data, const Chunk &chunk);
static GstFlowReturn gst_dffparse_parse_chunk_fver(gpointer user_data, const Chunk &chunk);
static GstFlowReturn gst_dffparse_parse_chunk_prop(gpointer user_data, const Chunk &chunk);
static bool gst_dffparse_finish_chunk_prop(gpointer user_data, const Chunk &chunk);
static GstFlowReturn gst_dffparse_parse_chunk_fs(gpointer user_data, const Chunk &chunk);
static GstFlowReturn gst_dffparse_parse_chunk_chnl(gpointer user_data, const Chunk &chunk);
static GstFlowReturn gst_dffparse_parse_chunk_cmpr(gpointer user_data, const Chunk &chunk);
static GstFlowReturn gst_dffparse_parse_chunk_lsco(gpointer user_data, const Chunk &chunk);
static GstFlowReturn gst_dffparse_parse_chunk_dsd(gpointer user_data, const Chunk &chunk);
static GstFlowReturn gst_dffparse_parse_chunk_diin(gpointer user_data, const Chunk &chunk);
static GstFlowReturn gst_dffparse_parse_chunk_diar(gpointer user_data, const Chunk &chunk);
static GstFlowReturn gst_dffparse_parse_chunk_diti(gpointer user_data, const Chunk &chunk);


static void gst_dffparse_class_init(GstDFFParseClass *klass)
{
	GObjectClass *object_class;
	GstElementClass *element_class;
	GstDsdMediaParseClass *dsd_media_parse_class;

	GST_DEBUG_CATEGORY_INIT(dffparse_debug, "dffparse", 0, "DSDIFF (= DFF) parser");

	object_class = G_OBJECT_CLASS(klass);
	element_class = GST_ELEMENT_CLASS(klass);
	dsd_media_parse_class = GST_DSD_MEDIA_PARSE_CLASS(klass);

	object_class->finalize = GST_DEBUG_FUNCPTR(gst_dffparse_finalize);

	dsd_media_parse_class->setup = GST_DEBUG_FUNCPTR(gst_dffparse_setup);
	dsd_media_parse_class->teardown = GST_DEBUG_FUNCPTR(gst_dffparse_teardown);
	dsd_media_parse_class->to_index = GST_DEBUG_FUNCPTR(gst_dffparse_to_index);
	dsd_media_parse_class->from_index = GST_DEBUG_FUNCPTR(gst_dffparse_from_index);
	dsd_media_parse_class->produce_output = GST_DEBUG_FUNCPTR(gst_dffparse_produce_output);
	dsd_media_parse_class->fill_tags = GST_DEBUG_FUNCPTR(gst_dffparse_fill_tags);
	dsd_media_parse_class->current_index_after_seek = GST_DEBUG_FUNCPTR(gst_dffparse_current_index_after_seek);

	gst_element_class_add_pad_template(element_class, gst_static_pad_template_get(&sink_template));
	gst_element_class_add_pad_template(element_class, gst_static_pad_template_get(&src_template));

	gst_element_class_set_static_metadata(
		element_class,
		"DFF parser",
		"Codec/Parser/Audio",
		"Parses DFF data and outputs DSD content",
		"Carlos Rafael Giani <crg7475@mailbox.org>"
	);
}


static void gst_dffparse_init(GstDFFParse *self)
{
	new (&(self->prop_data)) PropData();

	gst_dsd_chunk_parse_configure(
		GST_DSD_CHUNK_PARSE(self),
		ChunkSizeEndianness::BigEndian,
		false,
		{
			{
				GST_DFFPARSE_FOURCC_CHUNK_FRM8,
				{
					true,
					// NOTE: Either the DSD or the DST chunk must
					// be present. These are not included here however,
					// since only one of the two can be present in the
					// same DFF media, and this exclusive-or behavior
					// cannot be modeled as a set. The base class will
					// anyway recognize that no payload was reported
					// if both of these chunks are missing.
					{
						GST_DFFPARSE_FOURCC_CHUNK_FVER,
						GST_DFFPARSE_FOURCC_CHUNK_PROP,
					},
					ChunkSizeType::FirstNumBytes,
					4,
					gst_dffparse_parse_chunk_frm8,
					gst_dffparse_finish_chunk_frm8
				}
			},

			{
				GST_DFFPARSE_FOURCC_CHUNK_FVER,
				{
					true,
					{},
					ChunkSizeType::ExactSize,
					4,
					gst_dffparse_parse_chunk_fver
				}
			},

			{
				GST_DFFPARSE_FOURCC_CHUNK_PROP,
				{
					true,
					{
						GST_DFFPARSE_FOURCC_CHUNK_FS,
						GST_DFFPARSE_FOURCC_CHUNK_CHNL,
						GST_DFFPARSE_FOURCC_CHUNK_CMPR,
					},
					ChunkSizeType::FirstNumBytes,
					4,
					gst_dffparse_parse_chunk_prop,
					gst_dffparse_finish_chunk_prop
				}
			},

			{
				GST_DFFPARSE_FOURCC_CHUNK_FS,
				{
					true,
					{},
					ChunkSizeType::ExactSize,
					4,
					gst_dffparse_parse_chunk_fs
				}
			},

			{
				GST_DFFPARSE_FOURCC_CHUNK_CHNL,
				{
					true,
					{},
					ChunkSizeType::FirstNumBytes,
					2,
					gst_dffparse_parse_chunk_chnl
				}
			},

			{
				GST_DFFPARSE_FOURCC_CHUNK_CMPR,
				{
					true,
					{},
					ChunkSizeType::FirstNumBytes,
					5,
					gst_dffparse_parse_chunk_cmpr
				}
			},

			{
				GST_DFFPARSE_FOURCC_CHUNK_LSCO,
				{
					true,
					{},
					ChunkSizeType::ExactSize,
				    2,
				    gst_dffparse_parse_chunk_lsco
				}
			},

			{
				GST_DFFPARSE_FOURCC_CHUNK_DSD,
				{
					true,
					{},
					ChunkSizeType::FirstNumBytes,
					0,
					gst_dffparse_parse_chunk_dsd
				}
			},

			{
				GST_DFFPARSE_FOURCC_CHUNK_DIIN,
				{
					true,
					{},
					ChunkSizeType::FirstNumBytes,
					0,
					gst_dffparse_parse_chunk_diin
				}
			},

			{
				GST_DFFPARSE_FOURCC_CHUNK_DIAR,
				{
					true,
					{},
					ChunkSizeType::FirstNumBytes,
					0,
					gst_dffparse_parse_chunk_diar
				}
			},

			{
				GST_DFFPARSE_FOURCC_CHUNK_DITI,
				{
					true,
					{},
					ChunkSizeType::FirstNumBytes,
					0,
					gst_dffparse_parse_chunk_diti
				}
			},
		}
	);

	gst_dffparse_reset_all_fields(self);
}


static void gst_dffparse_finalize(GObject *object)
{
	GstDFFParse *self = GST_DFFPARSE(object);

	self->prop_data.~PropData();

	G_OBJECT_CLASS(gst_dffparse_parent_class)->finalize(object);
}


static bool gst_dffparse_setup(GstDsdMediaParse *parse)
{
	GstDFFParse *self = GST_DFFPARSE(parse);

	gst_dffparse_reset_all_fields(self);

	return GST_DSD_MEDIA_PARSE_CLASS(gst_dffparse_parent_class)->setup(parse);
}


static void gst_dffparse_teardown(GstDsdMediaParse *parse)
{
	GstDFFParse *self = GST_DFFPARSE(parse);

	gst_dffparse_reset_all_fields(self);

	GST_DSD_MEDIA_PARSE_CLASS(gst_dffparse_parent_class)->teardown(parse);
}


static guint64 gst_dffparse_to_index(GstDsdMediaParse *parse, GstFormat source_format, guint64 source_value, ToIndexRoundingMode rounding_mode)
{
	GstDFFParse *self = GST_DFFPARSE_CAST(parse);
	guint64 index;

	switch (source_format) {
		case GST_FORMAT_BYTES: {
			g_assert(self->prop_data.num_channels.has_value());
			guint16 num_channels = *(self->prop_data.num_channels);

			if (rounding_mode == ToIndexRoundingMode::RoundingDown)
				index = source_value / num_channels;
			else
				index = (source_value + (num_channels - 1)) / num_channels;

			break;
		}

		case GST_FORMAT_TIME: {
			g_assert(self->prop_data.sample_rate.has_value());
			guint32 sample_rate = *(self->prop_data.sample_rate);

			if (rounding_mode == ToIndexRoundingMode::RoundingDown)
				index = gst_util_uint64_scale_int(
					source_value,
					sample_rate,
					GST_SECOND
				);
			else
				index = gst_util_uint64_scale_int_ceil(
					source_value,
					sample_rate,
					GST_SECOND
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


static guint64 gst_dffparse_from_index(GstDsdMediaParse *parse, GstFormat dest_format, guint64 index)
{
	GstDFFParse *self = GST_DFFPARSE_CAST(parse);
	guint64 dest_value;

	switch (dest_format) {
		case GST_FORMAT_BYTES: {
			g_assert(self->prop_data.num_channels.has_value());
			guint16 num_channels = *(self->prop_data.num_channels);

			dest_value = index * num_channels;
			dest_value = std::min(dest_value, gst_dsd_media_parse_get_payload_size(parse));

			break;
		}

		case GST_FORMAT_TIME: {
			g_assert(self->prop_data.sample_rate.has_value());
			guint32 sample_rate = *(self->prop_data.sample_rate);

			dest_value = gst_util_uint64_scale_int(
				index,
				GST_SECOND,
				sample_rate
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


static GstFlowReturn gst_dffparse_produce_output(GstDsdMediaParse *parse, guint64 byte_position, guint64 end_payload_position, GstBuffer **output)
{
	GstDFFParse *self = GST_DFFPARSE(parse);
	guint64 num_indices_in_buffer;
	guint16 num_channels = *(self->prop_data.num_channels);
	guint64 output_buffer_size = self->output_buffer_size;

	guint64 num_available_payload_bytes = end_payload_position - byte_position;

	// This assumes that the DFF DSD payload has only complete frames.
	// In other words, it assumes the DSD payload size is an integer
	// multiple of num_channels. If this is not the case, the residual
	// partial frame is not output; instead, EOS is returned. See
	// gst_dffparse_parse_chunk_dsd() for more about this.

	if (G_LIKELY(num_available_payload_bytes >= self->output_buffer_size)) {
		num_indices_in_buffer = output_buffer_size / num_channels;
	} else {
		num_indices_in_buffer = num_available_payload_bytes / num_channels;
		output_buffer_size = num_indices_in_buffer * num_channels;
	}

	if (G_UNLIKELY(num_indices_in_buffer == 0))
		return GST_FLOW_EOS;

	GST_LOG_OBJECT(
		self,
		"got %" G_GUINT64_FORMAT " indices (= %" G_GUINT64_FORMAT " bytes) to output",
		num_indices_in_buffer,
		output_buffer_size
	);

	GstBuffer *output_buffer;
	GstFlowReturn flow_ret = gst_dsd_media_parse_read_data_during_streaming(parse, output_buffer_size, &output_buffer);
	if (flow_ret != GST_FLOW_OK)
		return flow_ret;

	// Compute the PTS and duration for this buffer out of the current index.

	GstClockTime pts = gst_dffparse_from_index(parse, GST_FORMAT_TIME, self->current_index);
	GstClockTime next_pts = gst_dffparse_from_index(parse, GST_FORMAT_TIME, self->current_index + num_indices_in_buffer);

	GST_BUFFER_PTS(output_buffer) = pts;
	GST_BUFFER_DURATION(output_buffer) = GST_CLOCK_DIFF(pts, next_pts);

	*output = output_buffer;

	self->current_index += num_indices_in_buffer;

	return GST_FLOW_OK;
}


static GstTagList* gst_dffparse_fill_tags(GstDsdMediaParse *parse, GstTagList *tag_list)
{
	GstDFFParse *self = GST_DFFPARSE(parse);

	if (self->prop_data.title.has_value() && !(self->prop_data.title->empty()))
		gst_tag_list_add(tag_list, GST_TAG_MERGE_APPEND, GST_TAG_TITLE, self->prop_data.title->c_str(), nullptr);
	if (self->prop_data.artist.has_value() && !(self->prop_data.artist->empty()))
		gst_tag_list_add(tag_list, GST_TAG_MERGE_APPEND, GST_TAG_ARTIST, self->prop_data.artist->c_str(), nullptr);

	return tag_list;
}


static void gst_dffparse_current_index_after_seek(GstDsdMediaParse *parse, guint64 new_current_index)
{
	GstDFFParse *self = GST_DFFPARSE(parse);

	self->current_index = new_current_index;
}


static void gst_dffparse_reset_all_fields(GstDFFParse *self)
{
	self->prop_data = PropData();
	self->output_buffer_size = 0;
	self->current_index = 0;
	self->total_num_indices = 0;
	self->upstream_size = 0;
}


static std::string gst_dffparse_convert_text_bytes_to_utf8(GstDFFParse *self, const gchar *text_bytes, gsize length, const gchar *text_bytes_name)
{
	// The DFF specification does not mention any encoding for the text bytes,
	// so we assume strict UTF-8 and fall back to alternatives if that doesn't work.

	// If the text bytes already are valid UTF-8, it can be used directly.
	if (g_utf8_validate_len(text_bytes, length, nullptr)) {
		GST_DEBUG_OBJECT(self, "DFF text bytes of %s are valid UTF-8 content", text_bytes_name);
		return std::string(
			reinterpret_cast<const char *>(text_bytes),
			length
		);
	}

	// It is not valid UTF-8. Try to interpret the text bytes as WINDOWS-1252 and
	// convert that to UTF-8. WINDOWS-1252 is often used when mastering records.
	gchar *converted = g_convert(
		text_bytes,
		length,
		"UTF-8",
		"WINDOWS-1252",
		nullptr,
		nullptr,
		nullptr
	);

	// Last resort: force conversion to UTF-8 by replacing invalid characters
	// with Unicode replacement characters.
	if (converted == nullptr) {
		GST_DEBUG_OBJECT(
			self,
			"DFF text bytes of %s could not be interpreted with any encoding; using UTF-8 with Unicode replacement characters instead",
			text_bytes_name
		);
		converted = g_utf8_make_valid(text_bytes, length);
	} else {
		GST_DEBUG_OBJECT(self, "DFF text bytes of %s are valid WINDOWS-1252 content; converting to UTF-8", text_bytes_name);
	}

	std::string converted_str(converted);

	g_free(converted);

	return converted_str;
}


static GstFlowReturn gst_dffparse_parse_chunk_frm8(gpointer user_data, const Chunk &chunk)
{
	GstDsdMediaParse *parse = GST_DSD_MEDIA_PARSE(user_data);
	GstDFFParse *self = GST_DFFPARSE(user_data);

	GstBuffer *buffer;
	GstFlowReturn flow_ret = gst_dsd_media_parse_read_data_during_scan(parse, 4, &buffer);
	if (G_UNLIKELY(flow_ret != GST_FLOW_OK))
		return flow_ret;

	MappedBuffer mapped_form_data{buffer, GST_MAP_READ};
	RETURN_IF_GSTBUFFER_MAPPING_FAILED(
		self,
		mapped_form_data,
		GST_FLOW_ERROR,
		"Could not map the FRM8 chunk payload"
	);

	guint32 form_fourcc = GST_MAKE_FOURCC(
		mapped_form_data.data()[0],
		mapped_form_data.data()[1],
		mapped_form_data.data()[2],
		mapped_form_data.data()[3]
	);

	if (G_UNLIKELY(form_fourcc != GST_DFFPARSE_FOURCC_FORM_TYPE_DSD)) {
		GST_ELEMENT_ERROR(
			self,
			STREAM,
			FORMAT,
			("Unsupported DFF type."),
			(
				"expected FRM8 FORM type \"%" GST_FOURCC_FORMAT "\", got \"%" GST_FOURCC_FORMAT "\"",
				GST_FOURCC_ARGS(GST_DFFPARSE_FOURCC_FORM_TYPE_DSD),
				GST_FOURCC_ARGS(form_fourcc)
			)
		);
		return GST_FLOW_NOT_SUPPORTED;
	}

	// Query the upstream size in bytes as a safeguard in case
	// the FRM8 chunk's specified length is incorrect. Also,
	// other chunk parsers will need this size for their processing.
	if (!gst_dsd_media_parse_get_upstream_size(parse, &(self->upstream_size))) {
		GST_ELEMENT_ERROR(
			self,
			STREAM,
			FAILED,
			("Could not get size of the DFF medium."),
			("upstream bytes duration query failed; cannot get size of DFF media")
		);
		return GST_FLOW_ERROR;
	}

	// DFF media with less than 12 bytes cannot exist,
	// since that is the size of a chunk header.
	if (self->upstream_size < 12) {
		GST_ELEMENT_ERROR(
			self,
			STREAM,
			DEMUX,
			("Invalid size of DFF medium."),
			(
				"upstream duration query reports a size of %" G_GINT64_FORMAT
				" byte(s), which is not valid DFF",
				self->upstream_size
			)
		);
		return GST_FLOW_ERROR;
	}

	GST_DEBUG_OBJECT(self, "upstream specifies a size of %" G_GINT64_FORMAT " byte(s)", self->upstream_size);

	// The expected chunk size is the size upstream
	// reported, minus the bytes for the FRM8 header.
	// This value is not reliable enough though - reporting
	// an error based on a size mismatch won't work correctly
	// with truncated but still playable files. For this
	// reason, size mismatches are merely logged as warnings.
	guint64 expected_chunk_size = self->upstream_size - 12;

	if (chunk.size > expected_chunk_size) {
		GST_WARNING_OBJECT(
			self,
			"FRM8 chunk size %" G_GUINT64_FORMAT " is larger than the expected "
			"chunk size %" G_GUINT64_FORMAT " according to reported upstream size",
			chunk.size,
			expected_chunk_size
		);
	} else if (chunk.size < expected_chunk_size) {
		GST_WARNING_OBJECT(
			self,
			"FRM8 chunk size %" G_GUINT64_FORMAT " is smaller than the expected "
			"chunk size %" G_GUINT64_FORMAT " according to reported upstream size",
			chunk.size,
			expected_chunk_size
		);
	}

	gst_dsd_chunk_parse_begin_parsing_subchunks(GST_DSD_CHUNK_PARSE(self));

	return GST_FLOW_OK;
}


static bool gst_dffparse_finish_chunk_frm8(gpointer user_data, const Chunk &)
{
	GstDsdMediaParse *parse = GST_DSD_MEDIA_PARSE(user_data);

	GST_DEBUG_OBJECT(parse, "FRM8 chunk finished; there is no more data to parse");

	gst_dsd_media_parse_scanning_finished(parse);

	return true;
}


static GstFlowReturn gst_dffparse_parse_chunk_fver(gpointer user_data, const Chunk &)
{
	GstDsdMediaParse *parse = GST_DSD_MEDIA_PARSE(user_data);
	GstDFFParse *self = GST_DFFPARSE(user_data);

	GstBuffer *buffer = nullptr;
	GstFlowReturn flow_ret = gst_dsd_media_parse_read_data_during_scan(parse, 4, &buffer);
	if (G_UNLIKELY(flow_ret != GST_FLOW_OK))
		return flow_ret;

	MappedBuffer mapped_fver_data{buffer, GST_MAP_READ};
	RETURN_IF_GSTBUFFER_MAPPING_FAILED(
		self,
		mapped_fver_data,
		GST_FLOW_ERROR,
		"Could not map the FVER chunk payload"
	);

	guint32 fver_version = GST_READ_UINT32_BE(mapped_fver_data.data());

	int major_version_nr            = int((fver_version & 0xFF000000) >> 24);
	int addition_version_nr         = int((fver_version & 0x00FF0000) >> 16);
	int reserved_minor_version_nr_1 = int((fver_version & 0x0000FF00) >> 8);
	int reserved_minor_version_nr_2 = int((fver_version & 0x000000FF) >> 0);

	GST_INFO_OBJECT(
		self,
		"format is DFF version %d.%d.%d.%d",
		major_version_nr,
		addition_version_nr,
		reserved_minor_version_nr_1,
		reserved_minor_version_nr_2
	);

	if (G_UNLIKELY(major_version_nr != 1)) {
		GST_ELEMENT_ERROR(
			self,
			STREAM,
			DEMUX,
			("Unsupported DFF version."),
			("expected major DFF version 1, got %d; this DFF content is not supported", major_version_nr)
		);
		return GST_FLOW_NOT_SUPPORTED;
	}

	gst_dsd_chunk_parse_finish_current_chunk(GST_DSD_CHUNK_PARSE(self));

	return GST_FLOW_OK;
}


static GstFlowReturn gst_dffparse_parse_chunk_prop(gpointer user_data, const Chunk &)
{
	GstDsdMediaParse *parse = GST_DSD_MEDIA_PARSE(user_data);
	GstDFFParse *self = GST_DFFPARSE(user_data);

	// The PROP chunk contains other chunks, so start parsing
	// its subchunks after reading its additional header data.

	GstBuffer *buffer = nullptr;
	GstFlowReturn flow_ret = gst_dsd_media_parse_read_data_during_scan(parse, 4, &buffer);
	if (G_UNLIKELY(flow_ret != GST_FLOW_OK))
		return flow_ret;

	MappedBuffer mapped_prop_data{buffer, GST_MAP_READ};
	RETURN_IF_GSTBUFFER_MAPPING_FAILED(
		self,
		mapped_prop_data,
		GST_FLOW_ERROR,
		"Could not map the PROP chunk payload"
	);

	guint32 prop_type_fourcc = GST_MAKE_FOURCC(
		mapped_prop_data.data()[0],
		mapped_prop_data.data()[1],
		mapped_prop_data.data()[2],
		mapped_prop_data.data()[3]
	);

	if (G_UNLIKELY(prop_type_fourcc != GST_DFFPARSE_FOURCC_PROP_TYPE_SND)) {
		GST_ELEMENT_ERROR(
			self,
			STREAM,
			DEMUX,
			("Unsupported DFF content."),
			(
				"expected PROP type \"%" GST_FOURCC_FORMAT "\", got \"%" GST_FOURCC_FORMAT "\"",
				GST_FOURCC_ARGS(GST_DFFPARSE_FOURCC_PROP_TYPE_SND),
				GST_FOURCC_ARGS(prop_type_fourcc)
			)
		);
		return GST_FLOW_NOT_SUPPORTED;
	}

	gst_dsd_chunk_parse_begin_parsing_subchunks(GST_DSD_CHUNK_PARSE(self));

	return GST_FLOW_OK;
}


static bool gst_dffparse_finish_chunk_prop(gpointer user_data, const Chunk &)
{
	GstDFFParse *self = GST_DFFPARSE(user_data);

	// Validate PROP chunk content and compute various quantities out of the content.

	if (G_UNLIKELY(!self->prop_data.validate(self))) {
		GST_ERROR_OBJECT(
			self,
			"at least some of the required PROP chunk subchunk data is missing; cannot continue"
		);
		return false;
	}

	guint64 num_channels = *(self->prop_data.num_channels);

	// In the unlikely case that the CHNL chunk's number of channels and actual
	// number of channel positions do not match, either discard excess positions
	// or fill in missing ones with the INVALID position to ensure these two match.
	if (G_UNLIKELY(num_channels != self->prop_data.channel_positions.size())) {
		guint64 old_num_channel_positions = self->prop_data.channel_positions.size();

		self->prop_data.channel_positions.resize(num_channels);

		if (old_num_channel_positions < num_channels) {
			GST_WARNING_OBJECT(
				self,
				"CHNL chunk contained fewer channel positions (%" G_GUINT64_FORMAT
				") than the specified number of channels (%" G_GUINT64_FORMAT "); "
				"discarded excess channel positions",
				old_num_channel_positions,
				num_channels
			);
		} else {
			GST_WARNING_OBJECT(
				self,
				"CHNL chunk contained more channel positions (%" G_GUINT64_FORMAT
				") than the specified number of channels (%" G_GUINT64_FORMAT "); "
				"filling excess channel positions with position INVALID",
				old_num_channel_positions,
				num_channels
			);

			for (guint64 idx = old_num_channel_positions; idx < num_channels; ++idx)
				self->prop_data.channel_positions[idx] = GST_AUDIO_CHANNEL_POSITION_INVALID;
		}
	}

	// Go through the channel positions and check for invalid ones. These can come
	// either from filling in a truncated channel position map, or from custom
	// C000-C999 channel ID fourCCs in the CHNL chunk. If there are invalid ones,
	// the channel position array as a whole cannot be relied upon. Instead,
	// several fallback methods for determining a channel position array are used.

	bool channel_position_array_is_valid = (std::find(
		self->prop_data.channel_positions.begin(),
		self->prop_data.channel_positions.end(),
		GST_AUDIO_CHANNEL_POSITION_INVALID
	) == self->prop_data.channel_positions.end());

	if (!channel_position_array_is_valid) {
		GST_DEBUG_OBJECT(self, "channel position array contains invalid values");

		// If there is a valid LSCO configuration, try that first.
		if (
			(self->prop_data.loudspeaker_config != DFFLoudspeakerConfig::Undefined) &&
			(self->prop_data.loudspeaker_config != DFFLoudspeakerConfig::Reserved)
		) {
			if (self->prop_data.num_lsconfig_channels == num_channels) {
				// The channel position orders here match what is specified
				// in the DFF spec version 1.5, sections 3.2.2, 3.7.2, and 4.3.

				switch (self->prop_data.loudspeaker_config) {
					case DFFLoudspeakerConfig::Stereo: {
						GST_DEBUG_OBJECT(self, "using LSCO stereo configuration as fallback");
						self->prop_data.channel_positions = {
							GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
							GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
						};
						break;
					}

					case DFFLoudspeakerConfig::FiveChannels: {
						GST_DEBUG_OBJECT(self, "using LSCO 5 channel configuration as fallback");
						self->prop_data.channel_positions = {
							GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
							GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
							GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
							GST_AUDIO_CHANNEL_POSITION_REAR_LEFT,
							GST_AUDIO_CHANNEL_POSITION_REAR_RIGHT,
						};
						break;
					}

					case DFFLoudspeakerConfig::FiveChannelsPlusLFE: {
						GST_DEBUG_OBJECT(self, "using LSCO 5.1 channel configuration as fallback");
						self->prop_data.channel_positions = {
							GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
							GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
							GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
							GST_AUDIO_CHANNEL_POSITION_LFE1,
							GST_AUDIO_CHANNEL_POSITION_REAR_LEFT,
							GST_AUDIO_CHANNEL_POSITION_REAR_RIGHT,
						};
						break;
					}

					default:
						g_assert_not_reached();
						break;
				}

				channel_position_array_is_valid = true;
			} else {
				GST_DEBUG_OBJECT(
					self,
					"could not use LSCO configuration due to mismatching number of channels "
					"(CHNL chunk reports %" G_GUINT64_FORMAT ", LSCO chunk reports %" G_GUINT64_FORMAT " channel(s))",
					num_channels, self->prop_data.num_lsconfig_channels
				);
			}
		}

		// Last resort: Use hardcoded channel position arrays
		// according to the number of channels.
		if (!channel_position_array_is_valid) {
			if (num_channels == 1) {
				GST_DEBUG_OBJECT(self, "using hardcoded mono channel position array");
				// Special case: mono does not use any channel mask.
				self->prop_data.channel_positions = {
					GST_AUDIO_CHANNEL_POSITION_MONO
				};
			} else {
				GST_DEBUG_OBJECT(self, "using fallback channel masks to compute channel position array");

				self->prop_data.channel_positions.resize(num_channels);

				guint64 channel_mask = gst_audio_channel_get_fallback_mask(num_channels);
				if (!gst_audio_channel_positions_from_mask(
					num_channels,
					channel_mask,
					self->prop_data.channel_positions.data()
				)) {
					GST_DEBUG_OBJECT(
						self,
						"could not get audio positions out of mask for %" G_GUINT64_FORMAT
						" channels; using unpositioned channel position array instead",
						num_channels
					);
					std::fill(
						self->prop_data.channel_positions.begin(),
						self->prop_data.channel_positions.end(),
						GST_AUDIO_CHANNEL_POSITION_NONE
					);
				}
			}

			channel_position_array_is_valid = true;
		}
	}

	// DFF groups DSD bits as DSDU8, that is, in individual bytes. The sample rate
	// equals the number of DFF frames per second. (See the "About indices and
	// position boundaries in GstDFFParse" section at the top for more.) Thus, the
	// number of frames per 5ms, multiplied by the bytes per frame (which equals
	// the number of channels - also explained in the aforementioned section),
	// yields an output buffer size that corresponds to 5 ms of playtime. (The
	// frames per channel in 5ms are rounded up in case the actual playtime
	// would be a non-integer amount of milliseconds, meaning that the actual
	// output buffer size can be 6 ms in some cases.)
	guint64 frames_per_channel_in_5ms = (guint64(*(self->prop_data.sample_rate)) * 5 + 999) / 1000;
	guint64 output_bytes_per_frame = *(self->prop_data.num_channels);
	self->output_buffer_size = frames_per_channel_in_5ms * output_bytes_per_frame;
	GstClockTime buffer_playtime = gst_util_uint64_scale_int(
		frames_per_channel_in_5ms,
		GST_SECOND,
		*(self->prop_data.sample_rate)
	);
	GST_DEBUG_OBJECT(
		self,
		"computed output buffer size of %" G_GUINT64_FORMAT " byte(s), "
		"which corresponds to playtime %" GST_TIME_FORMAT,
		self->output_buffer_size,
		GST_TIME_ARGS(buffer_playtime)
	);

	return true;
}


static GstFlowReturn gst_dffparse_parse_chunk_fs(gpointer user_data, const Chunk &)
{
	GstDsdMediaParse *parse = GST_DSD_MEDIA_PARSE(user_data);
	GstDFFParse *self = GST_DFFPARSE(user_data);

	GstBuffer *buffer = nullptr;
	GstFlowReturn flow_ret = gst_dsd_media_parse_read_data_during_scan(parse, 4, &buffer);
	if (G_UNLIKELY(flow_ret != GST_FLOW_OK))
		return flow_ret;

	MappedBuffer mapped_fs_data{buffer, GST_MAP_READ};
	RETURN_IF_GSTBUFFER_MAPPING_FAILED(
		self,
		mapped_fs_data,
		GST_FLOW_ERROR,
		"Could not map the FS chunk payload"
	);

	// DSD with a sample rate below what DSD64-44x requires is not valid DSD.
	// Here, we compare bits per second, since that is what DFF directly stores
	// as unit for its sample rate value.
	const guint32 min_valid_sample_rate = GST_DSD_MAKE_DSD_RATE_44x(64) * 8;
	guint32 sample_rate = GST_READ_UINT32_BE(mapped_fs_data.data());
	if (G_UNLIKELY(sample_rate < min_valid_sample_rate)) {
		GST_ELEMENT_ERROR(
			self,
			STREAM,
			DEMUX,
			("Invalid DFF sample rate."),
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

	self->prop_data.sample_rate = sample_rate;

	gst_dsd_chunk_parse_finish_current_chunk(GST_DSD_CHUNK_PARSE(self));

	return GST_FLOW_OK;
}


static GstFlowReturn gst_dffparse_parse_chunk_chnl(gpointer user_data, const Chunk &chunk)
{
	GstDsdMediaParse *parse = GST_DSD_MEDIA_PARSE(user_data);
	GstDFFParse *self = GST_DFFPARSE(user_data);
	GstFlowReturn flow_ret;
	GstBuffer *buffer;

	guint16 num_channels;

	// This chunk has a fixed-size and a variable-size part. The fixed-size
	// part encodes the number of channel. The variable-size part contains
	// the channel ID array, whose length depends on the number of channels.
	// Since gst_dsd_media_parse_read_data_during_scan() can return
	// GST_FLOW_NOT_ENOUGH_DATA when it has insufficient data, use the
	// presence or absence of a value in num_channels as a state to
	// distinguish between having to still read the number of channels
	// and being in the process of parsing the channel ID array.

	if (!self->prop_data.num_channels) {
		buffer = nullptr;
		flow_ret = gst_dsd_media_parse_read_data_during_scan(parse, 2, &buffer);
		if (G_UNLIKELY(flow_ret != GST_FLOW_OK))
			return flow_ret;

		MappedBuffer mapped_num_channels_data{buffer, GST_MAP_READ};
		RETURN_IF_GSTBUFFER_MAPPING_FAILED(
			self,
			mapped_num_channels_data,
			GST_FLOW_ERROR,
			"Could not map the number of channels data from the CHNL chunk payload"
		);

		num_channels = GST_READ_UINT16_BE(mapped_num_channels_data.data());
		if (G_UNLIKELY(num_channels < 1)) {
			GST_ELEMENT_ERROR(
				self,
				STREAM,
				DEMUX,
				("Invalid DFF channel count."),
				("got invalid number of channels %" G_GUINT16_FORMAT, num_channels)
			);
			return GST_FLOW_ERROR;
		}

		self->prop_data.num_channels = num_channels;

		GST_INFO_OBJECT(self, "number of channels is %" G_GUINT16_FORMAT, num_channels);

		// A channel ID is a fourCC. This implies that, past the 2 bytes for
		// the number of channels, the remaining chunk payload consists of fourCCs.
		guint64 num_channel_ids_in_chunk = (chunk.size - 2) / 4;

		// Sanity check to verify that the specified number of channels and the
		// actual number of channel IDs are the same. A difference between those
		// makes no sense, since otherwise, the channel ID array is useless.
		if (num_channel_ids_in_chunk != guint64(num_channels)) {
			GST_ELEMENT_ERROR(
				self,
				STREAM,
				DEMUX,
				("Invalid number of DFF channel IDs."),
				(
					"expected %" G_GUINT16_FORMAT " channel IDs, got %" G_GUINT64_FORMAT,
					num_channels,
					num_channel_ids_in_chunk
				)
			);
			return GST_FLOW_ERROR;
		}
	} else {
		num_channels = *(self->prop_data.num_channels);
	}

	// Read the channel ID fourCCs.

	buffer = nullptr;
	flow_ret = gst_dsd_media_parse_read_data_during_scan(parse, num_channels * 4, &buffer);
	switch (flow_ret) {
		case GST_FLOW_NOT_ENOUGH_DATA:
			GST_LOG_OBJECT(self, "insufficient bytes available to read channel IDs");
			return GST_FLOW_NOT_ENOUGH_DATA;
		case GST_FLOW_OK:
			break;
		default:
			return flow_ret;
	}

	MappedBuffer mapped_channel_id_data{buffer, GST_MAP_READ};
	RETURN_IF_GSTBUFFER_MAPPING_FAILED(
		self,
		mapped_channel_id_data,
		GST_FLOW_ERROR,
		"Could not map the channel ID data from the CHNL chunk payload"
	);

	// Convert the channel IDs to GStreamer channel positions.

	for (guint64 channel_id_index = 0; channel_id_index < num_channels; ++channel_id_index) {
		guint32 channel_id = GST_MAKE_FOURCC(
			mapped_channel_id_data.data()[channel_id_index * 4 + 0],
			mapped_channel_id_data.data()[channel_id_index * 4 + 1],
			mapped_channel_id_data.data()[channel_id_index * 4 + 2],
			mapped_channel_id_data.data()[channel_id_index * 4 + 3]
		);

		GstAudioChannelPosition channel_position;

		switch (channel_id) {
			case GST_DFFPARSE_FOURCC_CHANNEL_ID_SLFT: channel_position = GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT; break;
			case GST_DFFPARSE_FOURCC_CHANNEL_ID_SRGT: channel_position = GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT; break;
			case GST_DFFPARSE_FOURCC_CHANNEL_ID_MLFT: channel_position = GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT; break;
			case GST_DFFPARSE_FOURCC_CHANNEL_ID_MRGT: channel_position = GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT; break;
			case GST_DFFPARSE_FOURCC_CHANNEL_ID_LS:   channel_position = GST_AUDIO_CHANNEL_POSITION_SURROUND_LEFT; break;
			case GST_DFFPARSE_FOURCC_CHANNEL_ID_RS:   channel_position = GST_AUDIO_CHANNEL_POSITION_SURROUND_RIGHT; break;
			case GST_DFFPARSE_FOURCC_CHANNEL_ID_C:    channel_position = GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER; break;
			case GST_DFFPARSE_FOURCC_CHANNEL_ID_LFE:  channel_position = GST_AUDIO_CHANNEL_POSITION_LFE1; break;
			default: channel_position = GST_AUDIO_CHANNEL_POSITION_INVALID; break;
		}

		gchar *position_str = gst_audio_channel_positions_to_string(&channel_position, 1);
		GST_INFO_OBJECT(
			self,
			"channel %" G_GUINT64_FORMAT " ID is %" GST_FOURCC_FORMAT " -> position is %s",
			channel_id_index,
			GST_FOURCC_ARGS(channel_id),
			position_str
		);
		g_free(position_str);

		self->prop_data.channel_positions.push_back(channel_position);
	}

	// TODO: The DFF channel orders are compatible with GStreamer's - but that
	// guarantee holds only for DFF data with 2, 5, or 6 (= 5.1) channels
	// (see the DFF specification version 1.5, section 3.2.2). For any other
	// channel count, the channel order could be anything, including ones that
	// are not GStreamer compatible. Add channel reodering support for those.

	gst_dsd_chunk_parse_finish_current_chunk(GST_DSD_CHUNK_PARSE(self));

	return GST_FLOW_OK;
}


static GstFlowReturn gst_dffparse_parse_chunk_cmpr(gpointer user_data, const Chunk &chunk)
{
	GstDsdMediaParse *parse = GST_DSD_MEDIA_PARSE(user_data);
	GstDFFParse *self = GST_DFFPARSE(user_data);
	GstFlowReturn flow_ret;
	GstBuffer *buffer;

	// The CMPR chunk is a variable size chunk, since the compression
	// name is stored as a variable sized string. For this reason, read
	// the CMPR chunk in steps. First, the leading fixed size portion
	// that contains the compression type fourCC must be read. If that was
	// done already, the variable size portion - the compression name -
	// is read.

	if (!self->prop_data.compression_type) {
		buffer = nullptr;
		flow_ret = gst_dsd_media_parse_read_data_during_scan(parse, 5, &buffer);
		if (G_UNLIKELY(flow_ret != GST_FLOW_OK))
			return flow_ret;

		MappedBuffer mapped_compression_info_data{buffer, GST_MAP_READ};
		RETURN_IF_GSTBUFFER_MAPPING_FAILED(
			self,
			mapped_compression_info_data,
			GST_FLOW_ERROR,
			"Could not map the compression type and compression name length from the CMPR chunk payload"
		);

		self->prop_data.compression_type = GST_MAKE_FOURCC(
			mapped_compression_info_data.data()[0],
			mapped_compression_info_data.data()[1],
			mapped_compression_info_data.data()[2],
			mapped_compression_info_data.data()[3]
		);
		self->prop_data.num_compression_name_chars = mapped_compression_info_data.data()[4];
	}

	guint64 num_compression_name_chars = self->prop_data.num_compression_name_chars;

	if (num_compression_name_chars == 0) {
		// Corner case where the CMPR chunk's compression name is an empty string.
		gst_dsd_chunk_parse_finish_current_chunk(GST_DSD_CHUNK_PARSE(self));
		// Not returning GST_FLOW_NOTHING_TO_READ, since we _did_
		// read something (the 4 fourCC bytes plus the 1-byte compression name length).
		return GST_FLOW_OK;
	}

	// Check for an invalid compression name length (= exceeds the remaining number of payload bytes).
	guint64 num_remaining = chunk.payload_end_pos - gst_dsd_media_parse_get_current_byte_position(parse);
	if (G_UNLIKELY(num_compression_name_chars > num_remaining)) {
		GST_ELEMENT_ERROR(
			self,
			STREAM,
			DEMUX,
			("Invalid DFF compression name length."),
			(
				"compression name length in CMPR chunk is specified as %" G_GUINT64_FORMAT " byte(s), "
				"but only %" G_GUINT64_FORMAT " byte(s) of payload are remaining in the chunk",
				num_compression_name_chars,
				num_remaining
			)
		);
		return GST_FLOW_ERROR;
	}

	buffer = nullptr;
	flow_ret = gst_dsd_media_parse_read_data_during_scan(parse, num_compression_name_chars, &buffer);
	switch (flow_ret) {
		case GST_FLOW_NOT_ENOUGH_DATA:
			GST_LOG_OBJECT(self, "insufficient bytes available to read compression name");
			return GST_FLOW_NOT_ENOUGH_DATA;
		case GST_FLOW_OK:
			break;
		default:
			return flow_ret;
	}

	MappedBuffer mapped_compression_name_data{buffer, GST_MAP_READ};
	RETURN_IF_GSTBUFFER_MAPPING_FAILED(
		self,
		mapped_compression_name_data,
		GST_FLOW_ERROR,
		"Could not map the compression name data from the CMPR chunk payload"
	);

	std::string compression_name = gst_dffparse_convert_text_bytes_to_utf8(
		self,
		reinterpret_cast<const char *>(mapped_compression_name_data.data()),
		num_compression_name_chars,
		"compression name"
	);

	GST_INFO_OBJECT(self, "compression name is \"%s\"", compression_name.c_str());

	switch (*(self->prop_data.compression_type)) {
		case GST_DFFPARSE_FOURCC_COMPRESSION_TYPE_DSD:
			GST_INFO_OBJECT(self, "DSD data is uncompressed");
			break;
		case GST_DFFPARSE_FOURCC_COMPRESSION_TYPE_DST:
			GST_INFO_OBJECT(self, "DSD data is DST encoded");
			// TODO: Change this to "break;" once DST support is in place
			return GST_FLOW_NOT_SUPPORTED;
		default:
			GST_ELEMENT_ERROR(
				self,
				STREAM,
				DEMUX,
				("Unknown DFF DSD compression type."),
				(
					"unknown DSD data compression type \"%" GST_FOURCC_FORMAT "\"",
					GST_FOURCC_ARGS(*(self->prop_data.compression_type))
				)
			);
			return GST_FLOW_NOT_SUPPORTED;
	}

	gst_dsd_chunk_parse_finish_current_chunk(GST_DSD_CHUNK_PARSE(self));

	return GST_FLOW_OK;
}


static GstFlowReturn gst_dffparse_parse_chunk_lsco(gpointer user_data, const Chunk &)
{
	GstDsdMediaParse *parse = GST_DSD_MEDIA_PARSE(user_data);
	GstDFFParse *self = GST_DFFPARSE(user_data);

	GstBuffer *buffer = nullptr;
	GstFlowReturn flow_ret = gst_dsd_media_parse_read_data_during_scan(parse, 2, &buffer);
	if (G_UNLIKELY(flow_ret != GST_FLOW_OK))
		return flow_ret;

	MappedBuffer mapped_lsco_data{buffer, GST_MAP_READ};
	RETURN_IF_GSTBUFFER_MAPPING_FAILED(
		self,
		mapped_lsco_data,
		GST_FLOW_ERROR,
		"Could not map the LSCO chunk payload"
	);

	guint16 raw_loudspeaker_config = GST_READ_UINT16_BE(mapped_lsco_data.data());
	switch (raw_loudspeaker_config) {
		case 0:
			self->prop_data.loudspeaker_config = DFFLoudspeakerConfig::Stereo;
			self->prop_data.num_lsconfig_channels = 2;
			GST_DEBUG_OBJECT(self, "LSCO chunk defines a stereo channel setup");
			break;

		case 3:
			self->prop_data.loudspeaker_config = DFFLoudspeakerConfig::FiveChannels;
			self->prop_data.num_lsconfig_channels = 5;
			GST_DEBUG_OBJECT(self, "LSCO chunk defines a 5 channel setup");
			break;

		case 4:
			self->prop_data.loudspeaker_config = DFFLoudspeakerConfig::FiveChannelsPlusLFE;
			self->prop_data.num_lsconfig_channels = 6;
			GST_DEBUG_OBJECT(self, "LSCO chunk defines a 5.1 channel setup");
			break;

		case 65535:
			self->prop_data.loudspeaker_config = DFFLoudspeakerConfig::Undefined;
			GST_DEBUG_OBJECT(self, "LSCO chunk does not define a channel setup");
			break;

		default:
			self->prop_data.loudspeaker_config = DFFLoudspeakerConfig::Reserved;
			GST_DEBUG_OBJECT(
				self,
				"LSCO chunk contains reserved loudspeaker config %" G_GUINT16_FORMAT,
				raw_loudspeaker_config
			);
			break;
	}

	gst_dsd_chunk_parse_finish_current_chunk(GST_DSD_CHUNK_PARSE(self));

	return GST_FLOW_OK;
}


static GstFlowReturn gst_dffparse_parse_chunk_dsd(gpointer user_data, const Chunk &chunk)
{
	// In DFF, the DSD chunk contains the uncompressed payload.
	// This is also where the media duration is computed - it
	// has to be calculated out of the DSD chunk size, since
	// DFF has no dedicated media length field.

	GstDsdMediaParse *parse = GST_DSD_MEDIA_PARSE(user_data);
	GstDFFParse *self = GST_DFFPARSE(user_data);

	// The PROP chunk must come before the DSD chunk. Otherwise,
	// gst_dsd_media_parse_report_payload_found() cannot reliably
	// be called, since the base class must be configured prior
	// to that call - and gst_dsd_media_parse_configure() requires
	// information from the PROP chunk.
	if (G_UNLIKELY(!self->prop_data.validate(self))) {
		GST_ELEMENT_ERROR(
			self,
			STREAM,
			DEMUX,
			("Incomplete DFF media."),
			("encountered the DSD chunk before a complete PROP chunk was parsed")
		);
		return GST_FLOW_ERROR;
	}

	// Verify the chunk size. In truncated files, the number of available
	// bytes will be smaller than that size. This is therefore an indicator
	// that the DSD data is truncated.
	guint64 payload_size = chunk.size;
	guint64 num_available_bytes = self->upstream_size - gst_dsd_media_parse_get_current_byte_position(parse);
	bool data_truncated = false;

	if (G_UNLIKELY(num_available_bytes < *(self->prop_data.num_channels))) {
		GST_ELEMENT_ERROR(
			self,
			STREAM,
			DEMUX,
			("Insufficient DFF payload."),
			(
				"only %" G_GUINT64_FORMAT " payload byte(s) are available; frame size is %"
				G_GUINT16_FORMAT "; cannot play this residual data",
				num_available_bytes,
				*(self->prop_data.num_channels)
			)
		);
		return GST_FLOW_ERROR;
	}

	if (G_UNLIKELY(payload_size > num_available_bytes)) {
		GST_WARNING_OBJECT(
			self,
			"DSD chunk declares %" G_GUINT64_FORMAT " payload byte(s), but only %"
			G_GUINT64_FORMAT " byte(s) are available; the medium seems to be "
			"truncated; clamping the payload size",
			payload_size,
			num_available_bytes
		);
		payload_size = num_available_bytes;
		data_truncated = true;
	}

	// Deliberately truncating: a residual partial frame at the end of the payload
	// cannot be output. Note that report_payload_found() below gets the unrounded
	// payload_size rather than the index-aligned byte count, so end_payload_position
	// may lie a few bytes past from_index(BYTES, total_num_indices).
	// gst_dffparse_produce_output() handles that by reporting EOS once fewer than
	// num_channels bytes remain.
	self->total_num_indices = payload_size / *(self->prop_data.num_channels);
	GST_DEBUG_OBJECT(self, "total number of indices is %" G_GUINT64_FORMAT, self->total_num_indices);

	// Use the _ceil variant of this function, since this is a duration calculation.
	GstClockTime duration = gst_util_uint64_scale_int_ceil(
		self->total_num_indices,
		GST_SECOND,
		*(self->prop_data.sample_rate)
	);

	GST_INFO_OBJECT(self, "got DSD media duration %" GST_TIME_FORMAT, GST_TIME_ARGS(duration));

	// Configuring the base class here, and not when the FRM8 chunk is finished.
	// This is in case upstream is not seekable - the base class will then immediately
	// switch to the Streaming stage. This will fail unless the base class is configured.

	GstDsdInfo output_dsd_info;

	gst_dsd_info_set_format(
		&output_dsd_info,
		GST_DSD_FORMAT_U8,
		*(self->prop_data.sample_rate),
		*(self->prop_data.num_channels),
		self->prop_data.channel_positions.data()
	);

	GstCaps *output_caps = gst_dsd_info_to_caps(&output_dsd_info);

	gst_dsd_media_parse_configure(
		parse,
		output_caps,
		duration
	);

	GstFlowReturn flow_ret = gst_dsd_media_parse_report_payload_found(parse, payload_size, data_truncated);
	if (flow_ret != GST_FLOW_OK)
		return flow_ret;

	gst_dsd_chunk_parse_finish_current_chunk(GST_DSD_CHUNK_PARSE(self));

	// Return this error code. We don't yet read anything from the DSD chunk
	// here - we'll do that in the Streaming stage, in produce_output(). And,
	// should upstream be non-seekable, gst_dsd_media_parse_report_payload_found()
	// won't be able to skip the payload for further parsing. The base class
	// will then see that nothing was read or skipped. If GST_FLOW_OK were
	// returned, the base class would interpret this lack of read or skip
	// activity as a parser bug. Return GST_FLOW_NOTHING_TO_READ to tell
	// the base class that this is intentional.
	return GST_FLOW_NOTHING_TO_READ;
}


static GstFlowReturn gst_dffparse_parse_chunk_diin(gpointer user_data, const Chunk &)
{
	GstDFFParse *self = GST_DFFPARSE(user_data);

	// The DIIN chunk contains other chunks, and it has no data of its own,
	// so just switch back to the ChunkParseState::ParsingChunkStart state.

	gst_dsd_chunk_parse_begin_parsing_subchunks(GST_DSD_CHUNK_PARSE(self));

	// We read nothing here. Inform the base class so it does not interpret
	// this lack of read or skip activity as a parser bug.
	return GST_FLOW_NOTHING_TO_READ;
}


static GstFlowReturn gst_dffparse_parse_chunk_diar(gpointer user_data, const Chunk &chunk)
{
	GstDsdMediaParse *parse = GST_DSD_MEDIA_PARSE(user_data);
	GstDFFParse *self = GST_DFFPARSE(user_data);
	GstFlowReturn flow_ret;
	GstBuffer *buffer;

	// The DIAR chunk is a variable size chunk, since the artist is stored
	// as a variable sized string. For this reason, read the DIAR chunk in steps.
	// First, the leading fixed size portion that contains the number of characters
	// in the artist string. If that was done already, the variable size portion -
	// the artist - is read.

	if (!(self->prop_data.num_artist_chars.has_value())) {
		buffer = nullptr;
		flow_ret = gst_dsd_media_parse_read_data_during_scan(parse, 4, &buffer);
		if (G_UNLIKELY(flow_ret != GST_FLOW_OK))
			return flow_ret;

		MappedBuffer mapped_num_artist_chars_data{buffer, GST_MAP_READ};
		RETURN_IF_GSTBUFFER_MAPPING_FAILED(
			self,
			mapped_num_artist_chars_data,
			GST_FLOW_ERROR,
			"Could not map the artist length from the DIAR chunk payload"
		);

		self->prop_data.num_artist_chars = GST_READ_UINT32_BE(mapped_num_artist_chars_data.data());
	}

	guint32 num_artist_chars = *(self->prop_data.num_artist_chars);

	if (num_artist_chars == 0) {
		// Corner case where the DIAR chunk is present but the artist is an empty string.
		self->prop_data.artist = "";
		gst_dsd_chunk_parse_finish_current_chunk(GST_DSD_CHUNK_PARSE(self));
		// Not returning GST_FLOW_NOTHING_TO_READ, since we _did_
		// read something (the 4 bytes containing the artist length).
		return GST_FLOW_OK;
	}

	// Check for an invalid artist length (= exceeds the remaining number of payload bytes).
	guint64 num_remaining = chunk.payload_end_pos - gst_dsd_media_parse_get_current_byte_position(parse);
	if (G_UNLIKELY(num_artist_chars > num_remaining)) {
		GST_ELEMENT_ERROR(
			self,
			STREAM,
			DEMUX,
			("Invalid DFF artist length."),
			(
				"artist length in DIAR chunk is specified as %" G_GUINT32_FORMAT " byte(s), "
				"but only %" G_GUINT64_FORMAT " byte(s) of payload are remaining in the chunk",
				num_artist_chars,
				num_remaining
			)
		);
		return GST_FLOW_ERROR;
	}

	buffer = nullptr;
	flow_ret = gst_dsd_media_parse_read_data_during_scan(parse, num_artist_chars, &buffer);
	switch (flow_ret) {
		case GST_FLOW_NOT_ENOUGH_DATA:
			GST_LOG_OBJECT(self, "insufficient bytes available to read artist");
			return GST_FLOW_NOT_ENOUGH_DATA;
		case GST_FLOW_OK:
			break;
		default:
			return flow_ret;
	}

	MappedBuffer mapped_artist_data{buffer, GST_MAP_READ};
	RETURN_IF_GSTBUFFER_MAPPING_FAILED(
		self,
		mapped_artist_data,
		GST_FLOW_ERROR,
		"Could not map the artist data from the DIAR chunk payload"
	);

	self->prop_data.artist = gst_dffparse_convert_text_bytes_to_utf8(
		self,
		reinterpret_cast<const char *>(mapped_artist_data.data()),
		num_artist_chars,
		"artist"
	);

	GST_INFO_OBJECT(self, "artist is \"%s\"", self->prop_data.artist->c_str());

	gst_dsd_chunk_parse_finish_current_chunk(GST_DSD_CHUNK_PARSE(self));

	return GST_FLOW_OK;
}


static GstFlowReturn gst_dffparse_parse_chunk_diti(gpointer user_data, const Chunk &chunk)
{
	GstDsdMediaParse *parse = GST_DSD_MEDIA_PARSE(user_data);
	GstDFFParse *self = GST_DFFPARSE(user_data);
	GstFlowReturn flow_ret;
	GstBuffer *buffer;

	// The DITI chunk is a variable size chunk, since the title is stored
	// as a variable sized string. For this reason, read the DITI chunk in steps.
	// First, the leading fixed size portion that contains the number of characters
	// in the title string. If that was done already, the variable size portion -
	// the title - is read.

	if (!(self->prop_data.num_title_chars.has_value())) {
		buffer = nullptr;
		flow_ret = gst_dsd_media_parse_read_data_during_scan(parse, 4, &buffer);
		if (G_UNLIKELY(flow_ret != GST_FLOW_OK))
			return flow_ret;

		MappedBuffer mapped_num_title_chars_data{buffer, GST_MAP_READ};
		RETURN_IF_GSTBUFFER_MAPPING_FAILED(
			self,
			mapped_num_title_chars_data,
			GST_FLOW_ERROR,
			"Could not map the title length from the DITI chunk payload"
		);

		self->prop_data.num_title_chars = GST_READ_UINT32_BE(mapped_num_title_chars_data.data());
	}

	guint32 num_title_chars = *(self->prop_data.num_title_chars);

	if (num_title_chars == 0) {
		// Corner case where the DITI chunk is present but the artist is an empty string.
		self->prop_data.title = "";
		gst_dsd_chunk_parse_finish_current_chunk(GST_DSD_CHUNK_PARSE(self));
		// Not returning GST_FLOW_NOTHING_TO_READ, since we _did_
		// read something (the 4 bytes containing the artist length).
		return GST_FLOW_OK;
	}

	// Check for an invalid title length (= exceeds the remaining number of payload bytes).
	guint64 num_remaining = chunk.payload_end_pos - gst_dsd_media_parse_get_current_byte_position(parse);
	if (G_UNLIKELY(num_title_chars > num_remaining)) {
		GST_ELEMENT_ERROR(
			self,
			STREAM,
			DEMUX,
			("Invalid DFF title length."),
			(
				"title length in DITI chunk is specified as %" G_GUINT32_FORMAT " byte(s), "
				"but only %" G_GUINT64_FORMAT " byte(s) of payload are remaining in the chunk",
				num_title_chars,
				num_remaining
			)
		);
		return GST_FLOW_ERROR;
	}

	buffer = nullptr;
	flow_ret = gst_dsd_media_parse_read_data_during_scan(parse, num_title_chars, &buffer);
	switch (flow_ret) {
		case GST_FLOW_NOT_ENOUGH_DATA:
			GST_LOG_OBJECT(self, "insufficient bytes available to read title");
			return GST_FLOW_NOT_ENOUGH_DATA;
		case GST_FLOW_OK:
			break;
		default:
			return flow_ret;
	}

	MappedBuffer mapped_title_data{buffer, GST_MAP_READ};
	RETURN_IF_GSTBUFFER_MAPPING_FAILED(
		self,
		mapped_title_data,
		GST_FLOW_ERROR,
		"Could not map the title data from the DITI chunk payload"
	);

	self->prop_data.title = gst_dffparse_convert_text_bytes_to_utf8(
		self,
		reinterpret_cast<const char *>(mapped_title_data.data()),
		num_title_chars,
		"title"
	);

	GST_INFO_OBJECT(self, "title is \"%s\"", self->prop_data.title->c_str());

	gst_dsd_chunk_parse_finish_current_chunk(GST_DSD_CHUNK_PARSE(self));

	return GST_FLOW_OK;
}


void gst_dffparse_type_find(GstTypeFind *tf, gpointer)
{
	const guint8 *data;

	// TODO: There is a flaw in GStreamer's IFF typefinder. For inexplicable reasons,
	// the iff_type_find() function (found in GStreamer's gsttypefindfunctions.c)
	// also does the same check as seen here, and reports "application/x-iff" as
	// caps. But this lumps DFF data together with 8SVX, ILBM etc. data. DFF is
	// _not_ valid IFF - for one, it uses 64-bit chunk sizes, not 32-bit ones. The
	// result is that if that typefinder is run, GstDFFParse will not be autoplugged.
	// Submit a patch for iff_type_find() that removes the check there. Perhaps also
	// move this typefinder in there.
	// As a workaround, use GST_TYPE_FIND_MAXIMUM as probability. This overrides the
	// IFF typefinder. Once that typefinder is fixed, drop back to GST_TYPE_FIND_LIKELY
	// or GST_TYPE_FIND_NEARLY_CERTAIN.
	if ((data = gst_type_find_peek(tf, 0, 16)) != nullptr) {
		if ((std::memcmp(data, "FRM8", 4) == 0) && (std::memcmp(data + 12, "DSD ", 4) == 0)) {
			gst_type_find_suggest_simple(tf, GST_TYPE_FIND_MAXIMUM, GST_DFF_MEDIA_TYPE, nullptr, nullptr);
		}
	}
}
