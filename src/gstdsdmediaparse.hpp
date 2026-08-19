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

#include <utility>
#include <gst/gst.h>
#include <gst/audio/gstdsd.h>


// NOTE: Not using G_BEGIN_DECLS and G_END_DECLS since the API is C++ only


#define GST_TYPE_DSD_MEDIA_PARSE (gst_dsd_media_parse_get_type())
G_DECLARE_DERIVABLE_TYPE(GstDsdMediaParse, gst_dsd_media_parse, GST, DSD_MEDIA_PARSE, GstElement)
#define GST_DSD_MEDIA_PARSE_CAST(obj) ((GstDsdMediaParse *)(obj))


// IMPORTANT IMPLEMENTATION NOTE: GST_FLOW_NOT_ENOUGH_DATA is a purely
// internal custom return code, and must not go beyond the chain function
// in push mode. Otherwise, it will be incorrectly interpreted as a
// dataflow error.

/** Custom #GstFlowReturn used in push mode when the input adapter currently does not have enough data. */
#define GST_FLOW_NOT_ENOUGH_DATA  (GstFlowReturn(GST_FLOW_CUSTOM_ERROR))

/**
 * A custom #GstFlowReturn used solely in #GstDsdMediaParseClass::scan_info implementations.
 *
 * It informs the base class that it deliberately did not read anything. Normally, not reading
 * anything is interpreted as a bug, and leads to an error report.
 */
#define GST_FLOW_NOTHING_TO_READ  (GstFlowReturn(GST_FLOW_CUSTOM_SUCCESS))


enum class ToIndexRoundingMode {
	RoundingDown,
	RoundingUp
};


/**
 * Base class for DSD media parser implementation that cannot use GstBaseParse.
 *
 * This parser base class is meant for DSD media that is of finite length and has a
 * structure that makes it unsuitable for GstBaseParse. For DSD media that is not
 * structured in such a manner, and is compatible with GstBaseParse, please use that
 * base class instead.
 *
 * The two prominent examples are DSF and DFF. Both are unsuitable for GstBaseParse,
 * since they require seek operations to be able to read metadata that is located
 * after the actual DSD payload.
 *
 * Prerequisites:
 *
 * In the explanations below, the notion of a "byte position" is mentioned. This
 * is the current read position in the DSD media, in bytes. If the DSD media is a
 * file, this is the current read position in the file handle.
 *
 * Also, the notion of "indices" is mentioned as part of seeking operations. This
 * is a measure to abstract away specifics about the format structure that are
 * relevant during seeking. For example, a format like DSF organizes DSD data
 * into fixed-size blocks and in a planar manner, meaning that in case of, say,
 * 3-channel sound, block 1 contains data for the first channel, block 2 data for
 * the second channel, block 3 data for the third channel, block 4 data for the
 * first channel again etc. and blocks 1-3 are all meant to be played at the same
 * time (through their respective channels). In such a scenario, seeking can only
 * happen at (block_size * num_channels) granularity. Other formats have different
 * structures.
 *
 * The abstract "index" allows for addressing this, since the meaning of "index"
 * is defined by the subclass. The base class only knows that a higher index means
 * seeking further ahead, and that index 0 is the beginning. In the example above,
 * the subclass could convert the index to a byte position by calculating
 * (index * block_size * num_channels). And, if the subclass knows how much playtime
 * is covered by those (block_size * num_channels) bytes, it could convert an index
 * to a timestamp by calculating (index * nanoseconds_per_block_group).
 *
 * In conversions from and to indices, when the converted value's format is
 * GST_FORMAT_BYTES, it refers to the bytes of the DSD payload. This is true
 * regardless of whether the payload is compressed or uncompressed. Should the
 * payload be split up in structures like chunks inside the payload, the bytes
 * value must include the bytes that make up that structure. For example, if
 * the payload is partitioned into chunks, the bytes values include the chunk
 * header sizes.
 *
 * The conversions from and to indices are handled by to_index() and from_index().
 *
 * Parsing process:
 *
 * Parsing takes place in two main stages. (Additional internal intermediate stages
 * exist, but these are an implementation detail, and omitted here for clarity.)
 *
 * 1. Scanning Info stage
 *
 * This is the initial stage. The media is scanned. No DSD payload is output in this
 * stage. Should DSD data be encountered, its position and size are recorded, and then,
 * they are skipped to continue scanning. Should upstream not support seeking (to be
 * specific, bytes-level seeking), the base class will instead finish this stage when
 * DSD data is encountered, and switch immediately to the Streaming stage (which is
 * documented further below).
 *
 * In this stage, the scan_info() vmethod is called. That vmethod decides on its own
 * how much data to read, and whether to skip data. gst_dsd_media_parse_read_data_during_scan()
 * and gst_dsd_media_parse_skip_data_during_scan() are used for that.
 *
 * During this stage, the following information must be discovered by scan_info():
 * - Output caps
 * - Media duration
 *
 * When all of these are known, gst_dsd_media_parse_configure() must be called.
 * This must happen before the switch to the Streaming stage takes place. This means
 * that formats where this information is present _after_ the DSD payload are tricky
 * to suppport - they absolutely require upstream seeking support, since otherwise,
 * the DSD payload can't be skipped in this stage (more on that below).
 *
 * When the subclass encounters DSD data in this stage, it must call
 * gst_dsd_media_parse_report_payload_found(). This is the function that records
 * the position and size of the DSD payload. The parse position at time of that call
 * is recorded as the "payload position".
 *
 * From here on, it depends on whether upstream elements support bytes-level seeking
 * or not. If they do, the base class will skip (= seek past) the DSD payload to
 * see if there is more data to scan there. This is commonly the case with DFF and
 * DSF formats. But, if upstream does _not_ support bytes-level seeking, the base
 * class cannot skip the DSD payload. It then instead immediately switches to the
 * Streaming stage. The data behind the DSD payload is inaccessible in this case;
 * there's no way to scan it, since skipping the DSD payload isn't possible without
 * seeking. This is also why formats that store number of channels etc. behind
 * the DSD payload are trickier to support.
 *
 * If upstream can handle bytes-seeking, and the DSD payload was thus skipped by
 * the base class, it continues to call scan_info() to allow the subclass to scan
 * the data behind the DSD payload. Should the subclass encounter there what it
 * considers the end of the media (for example, the end of a top level chunk),
 * it calls gst_dsd_media_parse_scanning_finished(). This will trigger the switch
 * to the Streaming stage.
 *
 * Such seek operations as part of the parsing process are referred to as "internal
 * seeking". By contrast, "external seeking" are seek events that originate from
 * an external source, typically an application, and intend to seek within the
 * actual DSD payload. Internal seeking operations are not communicated to the
 * outside.
 *
 * If the subclass needs to perform additional internal seeking in this stage
 * (for example, because the header indicates that the parser must jump to
 * a specific position in the file to read metadata or seeking tables etc.),
 * it can use gst_dsd_media_parse_seek_during_scan() to do so. Note that this
 * will not work if upstream cannot do bytes seeking. Also, the subclass must
 * return to the original byte position once the data from the location it
 * manually seeked to has been read. Otherwise, parsing breaks. Subclasses
 * should therefore call gst_dsd_media_parse_get_current_byte_position()
 * prior to using gst_dsd_media_parse_seek_during_scan() to know where to
 * go back to.
 *
 * Seeking and duration queries are not answered during the Scanning Info stage.
 * External seeking events are rejected in this stage.
 *
 * For added robustness, the verify_advance() vmethod is called in this stage
 * every time the byte position advances. This allows the subclass to perform
 * its own checks to ensure that no out of bounds IO happens. For example, in
 * a chunk based format, this can verify that the byte position stays within
 * the boundaries of the current chunk.
 *
 * 2. Streaming DSD stage
 *
 * No matter how the stage switched from Scanning Info to Streaming (either
 * immediately when DSD payload was reported, or later, when the subclass called
 * gst_dsd_media_parse_scanning_finished(), from now on, the behavior is the same.
 *
 * Initially, the current index (see above) is 0. The base class will call the
 * produce_output() vmethod in a loop. It is the subclass' job to produce output
 * buffers. It must set the PTS and duration of output buffers. Should the subclass
 * currently not be able to produce buffers, it can return null instead.
 *
 * \warning The subclass _must_ be certain that it eventually will produce buffers.
 * In other words, this null output must be temporary. Otherwise, the whole process
 * is caught in an infinite loop that is only interrupted by setting the element
 * state to READY or NULL.
 *
 * Should seeking occur, the base class will call the to_index() and from_index()
 * vmethods to compute the current byte position and handle seeking details.
 * Once seeking is done, it will call current_index_after_seek() to inform the
 * subclass what the current index is now. It will not be called if the seek
 * event did not set the start seek value, however (that is, the start seek_type
 * is GST_SEEK_TYPE_NONE). For more on that, see:
 *
 * https://gstreamer.freedesktop.org/documentation/additional/design/seeking.html?gi-language=c
 * https://gstreamer.freedesktop.org/documentation/gstreamer/gstsegment.html?gi-language=c#GstSeekType
 *
 * Only flushing seeks are supported. Non-1.0 rates are not supported.
 * Neither are segment seeks. Only bytes and time formats are supported in seeks.
 *
 * Convert queries snap to the position boundary. See to_index() for more about
 * that boundary. This is to ensure that positions do not land in the middle
 * of units (as defined by subclasses), like in the middle of compressed blocks.
 * 
 * In the Streaming stage, inside the produce_output() vmethod, subclasses call
 * gst_dsd_media_parse_read_data_during_streaming() to read data. They do _not_
 * call gst_dsd_media_parse_read_data_during_scan() or its skipping counterpart.
 * The return value of gst_dsd_media_parse_read_data_during_streaming() is to be
 * analyzed - if it is anything other than GST_FLOW_OK, the vmethod must return
 * it immediately. This is important for proper EOS handling and for dealing with
 * situations in push scheduling mode when there is insufficient data.
 *
 */
struct _GstDsdMediaParseClass {
	GstElementClass parent_class;

	/**
	 * Vmethod for custom subclass setup procedures.
	 *
     * Called during the NULL->READY state change. Custom subclass allocations
	 * and field initializations take place here. The base class will have set
	 * its stage to the Scanning Info stage prior to this vmethod call. If this
	 * returns false, the stage change fails.
	 *
	 * Optional.
	 */
	bool (*setup)(GstDsdMediaParse *parse);

	/**
	 * Vmethod for custom subclass teardown procedures.
	 *
     * Called during the READY->NULL state change. Custom subclass deallocations
	 * take place here. The base class will have reset its stage back to the
	 * Scanning Info stage prior to this vmethod call.
	 *
	 * Optional.
	 */
	void (*teardown)(GstDsdMediaParse *parse);

	/**
	 * Scans the media for structural information during the Scanning Info stage.
	 *
	 * Subclasses use gst_dsd_media_parse_read_data_during_scan() and
	 * gst_dsd_media_parse_skip_data_during_scan() in this vmethod to scan
	 * for information like the media duration, metadata, caps, and so on.
	 * Should these functions return anything other than GST_FLOW_OK, this
	 * vmethod must immediately return that flow return value.
	 *
	 * If this vmethod intentionally does not read any data, it must return
	 * GST_FLOW_NOTHING_TO_READ. Otherwise, the base class will interpret the
	 * lack of byte position advance as a bug in the vmethod and will report
	 * an error.
	 *
	 * \warning If this vmethod keeps returning GST_FLOW_NOTHING_TO_READ all
	 * the time, the base class is caught in an infinite loop. That return
	 * code is meant for when an internal subclass state dictates that at
	 * the moment, no data can be returned - but in the next call, or, at
	 * most, after a few calls, it will produce data again.
	 *
	 * Only used in the Scanning Info stage.
	 *
	 * Required.
	 */
	GstFlowReturn (*scan_info)(GstDsdMediaParse *parse);

	/**
	 * Informs the subclass that the base class just switched to the Streaming stage.
	 *
	 * If this return false, the base class reports an error, and dataflow stops.
	 *
	 * Optional.
	 */
	bool (*streaming_started)(GstDsdMediaParse *parse);

	/**
	 * Transforms a source value in bytes or time format into an index.
	 *
	 * See the overview above about what indices are and how to interpret bytes values.
	 *
	 * source_format is GST_FORMAT_BYTES or GST_FORMAT_TIME.
	 *
	 * rounding_mode controls how subclasses shall proceed if the source_value
	 * does not align perfectly with the position boundary. For example, if the
	 * subclass operates block-based, and the indices are effectively the block
	 * numbers, then in the ToIndexRoundingMode::RoundingDown case, this vmethod
	 * returns the index of the nearest block that comes before the source value.
	 * In the ToIndexRoundingMode::RoundingUp case, it would pick the nearest
	 * block that comes after.
	 *
	 * NOTE: In the ToIndexRoundingMode::RoundingUp case, this function can return
	 * an index that lies just outside of the bounds of the payload. This is used
	 * for defining the stop values of segments in some cases. However, it does
	 * not return indices further than that. If for example the last index that
	 * points to a valid location in the payload is 399, then this function can
	 * return 400 in the ToIndexRoundingMode::RoundingUp case if the source_value
	 * is at the very end of the payload - but it can never return 401 or higher.
	 *
	 * Do not call any API functions here. Otherwise, a deadlock may occur.
	 * Also, once the streaming stage is reached, any factors that involve
	 * the transformation must have been determined and be read-only from
	 * then on to avoid race conditions.
	 *
	 * Only used when the Streaming stage started.
	 *
	 * Required.
	 */
	guint64 (*to_index)(GstDsdMediaParse *parse, GstFormat source_format, guint64 source_value, ToIndexRoundingMode rounding_mode);

	/**
	 * Transforms an index into a value of the given format.
	 *
	 * See the overview above about what indices are and how to interpret bytes values.
	 *
	 * NOTE: It is possible that the index exceeds the bounds of the valid indices.
	 * This is done by the subclass when it needs a value that denotes a position
	 * right after very end of the payload (either in bytes or time, depending on
	 * dest_format). This means that such an index is not an error, and instead,
	 * the subclass must return that very end of the payload. This position must
	 * be exclusive, that is, the very first position that is outside of the payload.
	 *
	 * dest_format is GST_FORMAT_BYTES or GST_FORMAT_TIME.
	 *
	 * Do not call any API functions here. Otherwise, a deadlock may occur.
	 * Also, once the streaming stage is reached, any factors that involve
	 * the transformation must have been determined and be read-only from
	 * then on to avoid race conditions.
	 *
	 * Only used when the Streaming stage started.
	 *
	 * Required.
	 */
	guint64 (*from_index)(GstDsdMediaParse *parse, GstFormat dest_format, guint64 index);

	/**
	 * Produces output in the Streaming stage that the base class pushes downstream.
	 *
	 * See the overview about the basic function of this vmethod. It must allocate
	 * an output buffer, use gst_dsd_media_parse_read_data_during_streaming() to
	 * read DSD data, fill the allocated buffer with DSD data, set the buffer's PTS
	 * and duration (according to the amount of DSD data that was actually read),
	 * and set *output to that buffer.
	 *
	 * At the end of the DSD payload, when less than normal (but still some) data is
	 * available, this vmethod still must return GST_FLOW_OK (the output buffer is
	 * just smaller than usual, and its duration reflects the smaller amount of data
	 * it contains). But, the next time this is called, it must return GST_FLOW_EOS
	 * to inform the base class that the end of the payload was reached.
	 *
	 * This follows conventions also found in POSIX IO for example; when there is
	 * still data that can be read, a read() call will return the number of bytes
	 * read, just as usual. The _next_ time read() is called, it returns 0, indicating
	 * that the end of file was reached.
	 *
	 * Should the vmethod currently not be able to produce an output buffer, it must
	 * set *output to null. See the overview for more about this.
	 *
	 * Should the gst_dsd_media_parse_read_data_during_streaming() call mentioned
	 * earlier return anything other than GST_FLOW_OK, this vmethod must immediately
	 * return that flow error value.
	 *
	 * Should an error occur, this vmethod must call GST_ELEMENT_ERROR() and then
	 * return GST_FLOW_ERROR.
	 *
	 * Only used when the Streaming stage started.
	 *
	 * Required.
	 */
	GstFlowReturn (*produce_output)(GstDsdMediaParse *parse, guint64 byte_position, guint64 end_payload_position, GstBuffer **output);

	/**
	 * Allows the subclass to add tags to the given tag list.
	 *
	 * This is useful for inserting metadata that was encountered in the Scanning
	 * Info stage. The vmethod takes the given tag list and either modifies it in
	 * place and returns it, or creates its own tag list based on this one and
	 * returns the new tag list, or merges its own internal tag list with this one
	 * using gst_tag_list_merge() and returns the merged tag list. In the latter
	 * two cases, it must unref the given tag list to avoid a resource leak.
	 *
	 * The return value must not be null.
	 *
	 * Optional.
	 */
	GstTagList* (*fill_tags)(GstDsdMediaParse *parse, GstTagList *tag_list);

	/**
	 * Informs the subclass about what the new current index is after seeking.
	 *
	 * In case of a deviated seek (see resync_deviated_upstream_seek()), this
	 * reports the _actual_ index the seek landed on, that is, the index that
	 * corresponds to where the deviated seek actually went to.
	 *
	 * Only used when the Streaming stage started.
	 *
	 * Optional.
	 */
	void (*current_index_after_seek)(GstDsdMediaParse *parse, guint64 new_current_index);

	/**
	 * Verifies whether advancing the current byte position by the given amount is okay.
	 *
	 * The advance_name is meant for logging, to give additional context what
	 * the advance is for (reading or skipping).
	 *
	 * byte_position is the byte position before it is avdanced. advance_amount
	 * is the advance amount in bytes.
	 *
	 * If the advance exceeds bounds, this must call GST_ELEMENT_ERROR() with
	 * details about this error.
	 *
	 * Only used in the Scanning Info stage.
	 *
	 * Required.
	 */
	bool (*verify_advance)(GstDsdMediaParse *parse, guint64 byte_position,
	                       guint64 advance_amount, const gchar *advance_name);

	/**
	 * Allows the subclass to try to resynchronize in case upstream seeked to an incorrect position.
	 *
	 * In rare cases, it can happen that the base class requests upstream to seek
	 * to byte position X, but upstream seeks to X+N instead. If X+N still lies
	 * within the payload, this vmethod is called to try to resynchronize itself.
	 * For example, position X might land at the start of chunk A of the format
	 * that is being parsed, while X+N lands squarely in the middle of chunk B.
	 * The subclass (depending on the format it is parsing) might then have to
	 * throw away that partial data of B until it reaches the next chunk.
	 *
	 * This is one example where the produce_output() vmethod returns null;
	 * it would do so in this case to skip the partial data.
	 *
	 * The position X in the example above is specified as expected_byte_position.
	 * X+N is specified as actual_byte_position.
	 *
	 * Should the seek have landed outside of the payload, this is not called.
	 * Instead, the base class will report an error. Such a case is not recoverable.
	 *
	 * If this returns false, or if this vmethod is not defined, the base class will
	 * report an error in such a deviated seek case.
	 *
	 * Optional.
	 */
	bool (*resync_deviated_upstream_seek)(GstDsdMediaParse *parse, guint64 expected_byte_position, guint64 actual_byte_position);
};

/**
 * Returns the payload size that the subclass reported earlier.
 *
 * The return value of this function will be meaningless until
 * gst_dsd_media_parse_report_payload_found() was called.
 *
 * This can be called in both the Scanning Info and Streaming stages.
 * But, in the former, it must be called from within scan_info(),
 * and in the latter, from within produce_output().
 */
guint64 gst_dsd_media_parse_get_payload_size(GstDsdMediaParse *parse);

/**
 * Returns the duration that the base class was configured with.
 *
 * The return value of this function will be meaningless until
 * gst_dsd_media_parse_configure() was called.
 *
 * This can be called in both the Scanning Info and Streaming stages.
 * But, in the former, it must be called from within scan_info(),
 * and in the latter, from within produce_output().
 */
GstClockTime gst_dsd_media_parse_get_duration(GstDsdMediaParse *parse);

/**
 * Returns the size of the media as reported by upstream, in bytes.
 *
 * Should upstream not be able to report the size, the base class
 * reports an error, and this returns false. Otherwise, this function
 * writes the size to where upstream_size points to, and returns true.
 *
 * This can be called in both the Scanning Info and Streaming stages.
 * But, in the former, it must be called from within scan_info(),
 * and in the latter, from within produce_output().
 *
 * upstream_size must be a valid pointer.
 */
bool gst_dsd_media_parse_get_upstream_size(GstDsdMediaParse *parse, guint64 *upstream_size);

/**
 * Reads data during the Scanning Info stage.
 *
 * This is only used by the scan_info() vmethod. See the overview
 * above for details.
 *
 * This will attempt to read as many bytes as indicated by
 * num_bytes_to_read. If it currently can't because of insufficient
 * data, it returns GST_FLOW_NOT_ENOUGH_DATA. This only happens
 * when the element uses the push scheduling mode.
 *
 * num_bytes_to_read must be nonzero. data must point to a valid
 * GstBuffer pointer. Upon a successful read, a buffer will be
 * allocated with the data that was read, and that pointer will
 * be set to point to that buffer. The caller takes ownership
 * over that buffer.
 *
 * This will call the verify_advance() vmethod to give the subclass
 * a chance to check that this read operation stays within the
 * bounds of the format that is being parsed.
 *
 * In case of a return code that is not GST_FLOW_OK, scan_info()
 * must immediately return that return code and not continue
 * its processing. The data pointer is not set in such a case.
 */
GstFlowReturn gst_dsd_media_parse_read_data_during_scan(GstDsdMediaParse *parse,
	                                                    guint64 num_bytes_to_read,
	                                                    GstBuffer **data);

/**
 * Skips data during the Scanning Info stage.
 *
 * This is only used by the scan_info() vmethod. See the overview
 * above for details.
 *
 * This will attempt to skip as many bytes as indicated by
 * num_bytes_to_skip. Unlike gst_dsd_media_parse_read_data_during_scan()
 * though, this will not return GST_FLOW_NOT_ENOUGH_DATA if
 * it cannot skip all of those bytes at once. Instead, it
 * writes the number of bytes it could actually skip into the
 * value num_actually_skipped_bytes points to.
 *
 * This will call the verify_advance() vmethod to give the subclass
 * a chance to check that this skip operation stays within the
 * bounds of the format that is being parsed.
 *
 * In case of a return code that is not GST_FLOW_OK, scan_info()
 * must immediately return that return code and not continue
 * its processing. The value num_actually_skipped_bytes points
 * is not written to in that case.
 */
GstFlowReturn gst_dsd_media_parse_skip_data_during_scan(GstDsdMediaParse *parse,
	                                                    guint64 num_bytes_to_skip,
	                                                    guint64 *num_actually_skipped_bytes);

/**
 * Reads data during the Streaming stage.
 *
 * This is the gst_dsd_media_parse_read_data_during_scan() counterpart
 * for the Streaming stage. It only reads data within the DSD payload
 * area in the media. It does not call verify_advance(). Otherwise,
 * usage is like in gst_dsd_media_parse_read_data_during_scan().
 */
GstFlowReturn gst_dsd_media_parse_read_data_during_streaming(GstDsdMediaParse *parse,
	                                                         guint64 num_bytes_to_read,
	                                                         GstBuffer **data);

/**
 * Returns the current byte position of the base class.
 */
guint64 gst_dsd_media_parse_get_current_byte_position(GstDsdMediaParse *parse);

/**
 * Returns true if the base class is currently in the scanning info stage.
 */
bool gst_dsd_media_parse_is_currently_scanning(GstDsdMediaParse *parse);

/**
 * Allows the subclass to perform manual internal seeking during the Scanning Info phase.
 *
 * See the overview for more details.
 *
 * This must not be called outside of scan_info().
 *
 * Returns false if seeking could not be performed, either because
 * upstream does not support bytes seeking, or because pushing the
 * internal seek event failed. Subclasses should then resort to
 * a fallback; for example, if it intended to seek to a metadata
 * block, it must continue without said metadata.
 */
bool gst_dsd_media_parse_seek_during_scan(GstDsdMediaParse *parse, guint64 new_byte_position);

/**
 * Configures the base class for the Streaming stage.
 *
 * This must be called before the base class switches to the Streaming
 * Stage. That is, it is called by scan_info(), and is called before
 * invoking gst_dsd_media_parse_report_payload_found() and
 * gst_dsd_media_parse_scanning_finished().
 *
 * See the overview for more details.
 *
 * output_caps must contain valid caps, which have GST_DSD_MEDIA_TYPE
 * as media type in case of plain, uncompressed DSD, and something
 * else in case of compressed DSD. For example, if the payload is
 * DST-compressed, subclasses should use audio/x-dst as media type
 * for example.
 *
 * This function takes ownership over output_caps. If the calling
 * function needs the caps after this call, it must ref the caps
 * before this call.
 *
 * This is only used during the Scanning Info stage.
 */
void gst_dsd_media_parse_configure(GstDsdMediaParse *parse, GstCaps *output_caps,
                                   GstClockTime duration);

/**
 * Called during the Scanning Info stage when the subclass locates the DSD payload.
 *
 * The base class will record the current byte position as the position
 * the payload is located at. That position, combined with payload_size,
 * defines the subregion within the media where the DSD payload is present.
 *
 * See the overview for more about this function.
 *
 * This must not be called outside of scan_info(), and even then, must not
 * be called more than once in there.
 *
 * payload_size must be nonzero.
 *
 * This returns GST_FLOW_OK upon success. If this returns anything other
 * than GST_FLOW_OK, the scan_info() vmethod must return that flow return
 * code.
 */
GstFlowReturn gst_dsd_media_parse_report_payload_found(GstDsdMediaParse *parse, guint64 payload_size);

/**
 * Informs the base class that during the Scanning Info stage, the subclass found the end of the media.
 *
 * See the overview for more details. In particular, see the note about
 * how the base class might switch to the Streaming stage on its own
 * if upstream cannot handle seeking. This implies that this function
 * does not always end up being called during the media processing.
 *
 * This switches the base class stage from Scanning Info to Streaming.
 *
 * This must not be called outside of scan_info(), and even then, must not
 * be called more than once in there.
 */
void gst_dsd_media_parse_scanning_finished(GstDsdMediaParse *parse);
