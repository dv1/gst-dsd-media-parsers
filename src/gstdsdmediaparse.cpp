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
#include <mutex>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <gst/gst.h>
#include <gst/base/base.h>
#include <gst/base/gstadapter.h>
#include <gst/audio/gstdsd.h>
#include "gstdsdmediaparse.hpp"
#include "scope_guard.hpp"


GST_DEBUG_CATEGORY_STATIC(dsdmediaparse_debug);
#define GST_CAT_DEFAULT dsdmediaparse_debug


// NOTE: This includes more stages than the ones documented
// in the header. These others are purely internal.
enum class ParseStage {
	ScanningInfo,
	AwaitingSegment,
	Streaming,
	StreamFailure
};


struct GstDsdMediaParsePrivate
{
	// IMPORTANT: These fields are protected by synchronization primitives
	// against race conditions. The two primitives are the field mutex
	// (present here as field_mutex) and the stream lock (the sink pad's
	// stream lock, which is a recursive GLib mutex).
	//
	// Fields that are only ever accessed from within the streaming thread
	// are marked as "protected by the stream lock". Since they are
	// accessed within that thread, the stream lock is already taken
	// by the time these fields are accessed. Thus, they do not require
	// any explicit synchronization.
	//
	// Fields that are marked as "protected by the field mutex" require
	// that mutex to be explicitly locked when these fields are written to.
	// These write operations happen from within the streaming thread, which
	// implies that read access, if it happens within the same thread, do
	// not require the mutex to be locked. The main locations that are driven
	// by a different thread are gst_dsd_media_parse_src_query(), and in
	// case of flush-start events, the sinkpad and srcpad event handlers.
	// Read access do require the field mutex to be locked then.
	//
	// A very important exception are the flushing and internal_seek_seqnum
	// fields. The flushing field can be set when the flush-start event
	// arrives, and that is an out-of-band event. Thus, the event handler is
	// then called by a different thread. The same applies to
	// internal_seek_seqnum, which that handler reads.
	// For this reason, the field_mutex must always be locked before these two
	// fields are accessed, in both read and write cases.
	//
	// Also, no mutex lock is required when gst_dsd_media_parse_reset_all_fields()
	// is called, because that function is anyway not called while the streaming
	// thread is active.
	//
	// One field is special, and that is parse_stage. It relies on neither mutex.
	// Instead, it is an atomic value. Its value changes during stage switches,
	// which happen at startup (Scanning Info -> Awaiting Segment -> Streaming),
	// and during seek (Streaming -> Awaiting Segment -> Streaming), the latter
	// only occurring in push mode. All of these can be implemented without
	// involving the field mutex or stream lock. However, since the seek case
	// switches the stage in a different thread, this means that the field mutex
	// rule (lock mutex on write always, but only lock it on read when reading
	// in a different thread) can't hold. One alternative would be to lock the
	// field mutex every time parse_stage is accessed, but this complicates the
	// code without yielding significant advantages.


	// Input adapter for aggregating data. Only used in push
	// mode. Parsing requires a minimum amount of data, so by
	// aggregating, eventually, there is enough data to parse.
	// Similarly, for producing valid output data, aggregation
	// is sometimes necessary. Protected by the stream lock.
	GstAdapter *input_adapter;

	// The position the parser is currently at, in bytes.
	// Protected by the stream lock.
	guint64 byte_position;

	// Sink and source pads of the parser element. The sink pad
	// drives the dataflow in pull mode. Not protected by any lock,
	// since they are created in _init().
	GstPad *sinkpad, *srcpad;

	// Set during pad activation. This is important to use the correct
	// code paths (push vs pull mode code paths). Not protected by any
	// lock, since this is set during pad activation.
	GstPadMode sinkpad_mode;

	// Current parse stage. Atomic value. See above for details.
	// Prefer using the get_parse_stage() helper for read access.
	std::atomic<ParseStage> parse_stage;

	// Initially, it is unknown whether upstream is seekable,
	// which is why this is wrapped in std::optional. Protected
	// by the field mutex.
	std::optional<bool> upstream_is_seekable;

	// The segment that is pushed downstream. This is initialized
	// just before the stage switches to Streaming, and is updated
	// when a seek operation occurs. Protected by the field mutex.
	GstSegment downstream_segment;

	// Seqnum for downstream segment events and other associated events
	// that get pushed downstream. Protected by the stream lock.
	guint32 current_seqnum;

	// These are filled when an external seek event arrives and when
	// the parser state transitions from ScanningInfo to AwaitingSegment.
	// Protected by the stream lock.
	guint64 pending_seek_position;
	guint64 pending_seek_index;
	guint32 pending_segment_seqnum;
	// Records whether the seek that is currently pending had the
	// GST_SEEK_FLAG_ACCURATE flag set. This is only consulted when the
	// upstream seek deviates, that is, when upstream lands somewhere other
	// than pending_seek_position. It selects between two readings of the
	// pending segment: with an accurate seek, the segment start is a promise
	// that was made to the application and is left alone; without one, the
	// segment is merely a report of where playback resumed, and is rewritten
	// to the position that was actually reached. See
	// gst_dsd_media_check_for_deviated_upstream_seek() for the full rationale.
	bool pending_seek_is_accurate;
	GstSegment pending_segment;
	// This is set to true prior to the transition from ScanningInfo to
	// AwaitingSegment. It marks the moment when the info scanning phase
	// is definitely done and the element needs to push the initial caps,
	// tags, and segment events downstream. See the GST_EVENT_SEGMENT
	// handling in gst_dsd_media_parse_sink_event() for more. Only used
	// in the push mode. Protected by the stream lock.
	bool pending_bootstrapping;

	// If set, an internal seek is currently in progress, and flush events
	// arriving at the sinkpad that carry this seqnum are dropped - those
	// flushes are a side effect of that internal seek and must remain
	// internal. This replaces a separate "drop flush events" boolean, which
	// was redundant: having a value here _is_ that flag.
	//
	// This is set at the internal seek sites, just before the seek event is
	// pushed upstream, and is cleared when the segment event that concludes
	// the seek arrives (see gst_dsd_media_parse_handle_sink_segment_event()).
	// It deliberately is _not_ cleared right after the seek event push
	// returns: upstream is not required to perform the seek synchronously in
	// the calling thread, and a source that defers it to its own thread would
	// then deliver the flush events after the registration had already been
	// withdrawn, causing them to be misclassified as external ones.
	//
	// Should an internal seek fail without ever producing a segment, this
	// simply stays set. That is harmless: seqnums are unique per event, so a
	// stale registration can only ever match events belonging to that one
	// failed seek, never a later, unrelated flush. Hence no cleanup is needed
	// on the internal seek failure paths.
	//
	// Protected by the field mutex, in both read and write cases.
	std::optional<guint32> internal_seek_seqnum;

	// Set to true during external flushes (that is, flushes coming from
	// the application, or any other source that is not the parser itself).
	// Protected by the field mutex, in both read and write cases.
	bool flushing;

	// Set to true by the flush-stop handlers when the flush carried reset_time.
	// Downstream restarts its running time at zero after such a flush, so the
	// next segment that goes downstream must have base 0 rather than a base
	// derived from the previous segment's position. Cleared when consumed.
	// Protected by the field mutex.
	bool reset_running_time_on_next_segment;

	// This is set to true right after the parser was informed that the scanning
	// is finished (via gst_dsd_media_parse_scanning_finished()) and it seeked back
	// to where the payload is (at the payload_position). After this final internal
	// seek is performed, it is expected that upstream pushes a segment event.
	// Should this not happen, it indicates that the upstream source is not working
	// correctly. This field is only used in push mode, and even then, only when
	// upstream_is_seekable is set to true. Protected by the field mutex, in both
	// read and write cases.
	bool expecting_upstream_segment;

	// In the StreamFailure stage, this field is used to communicate what
	// error occurred. Not used in any other stage. Protected by the stream lock.
	GstFlowReturn stream_flow_error;

	// The offset, in bytes, where the payload is found. This is an
	// std::optional to catch corrupted media that has no valid payload.
	// Protected by the field mutex.
	std::optional<guint64> payload_position;

	// The size of the payload, in bytes. If payload_position does
	// not hold a value, the size is undefined. Protected by the field mutex.
	guint64 payload_size;

	// The sum of payload_position and payload_size. In other words, this
	// is the location of the first byte past the DSD payload. If payload_position
	// does not hold a value, this is undefined. Protected by the field mutex.
	guint64 end_payload_position;

	// Set by gst_dsd_media_parse_configure().
	// Protected by the stream lock.
	GstCaps *output_caps;

	// Duration of the DSD content. Until the duration is known (discovered
	// during the ScanningInfo stage), this is set to GST_CLOCK_TIME_NONE.
	// Set by gst_dsd_media_parse_configure(). Protected by the field mutex,
	// since the duration query handler also accesses this.
	GstClockTime duration;

	// If true, the next output buffer must have its DISCONT flag set.
	// Set right after the GStreamer stage change to READY, and after
	// a seek operation. Protected by the stream lock.
	bool next_buffer_is_discont;

	// This is filled just before the associated events are pushed downstream.
	// The fill_tags() vmethod processes this.
	GstTagList *output_tag_list;

	// Tag list from an upstream tag event. This is merged with output_tag_list
	// just before the parser pushes its own tag event downstream.
	GstTagList *upstream_tag_list;

	std::mutex field_mutex;
};


// Cannot use G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE() here, since otherwise,
// the pad templates from the subclass will be inaccessible.

static GstElementClass *gst_dsd_media_parse_parent_class = nullptr;
static gint dsd_media_parse_private_offset = 0;

static void gst_dsd_media_parse_class_init(gpointer g_class, gpointer class_data);
static void gst_dsd_media_parse_init(GTypeInstance *instance, gpointer g_class);

GType gst_dsd_media_parse_get_type(void)
{
	static gsize dsd_media_parse_type = 0;

	if (g_once_init_enter(&dsd_media_parse_type)) {
		static const GTypeInfo dsd_media_parse_info = {
			sizeof(GstDsdMediaParseClass),
			GBaseInitFunc(nullptr),
			GBaseFinalizeFunc(nullptr),
			GClassInitFunc(gst_dsd_media_parse_class_init),
			nullptr,
			nullptr,
			sizeof(GstDsdMediaParse),
			0,
			GInstanceInitFunc(gst_dsd_media_parse_init),
			nullptr
		};
		GType _type;

		_type = g_type_register_static(
			GST_TYPE_ELEMENT,
			"GstDsdMediaParse",
			&dsd_media_parse_info,
			G_TYPE_FLAG_ABSTRACT
		);

		dsd_media_parse_private_offset = g_type_add_instance_private(
			_type,
			sizeof(GstDsdMediaParsePrivate)
		);

		g_once_init_leave(&dsd_media_parse_type, _type);
	}

	return GType(dsd_media_parse_type);
}


static void gst_dsd_media_parse_finalize(GObject *object);

static gboolean gst_dsd_media_parse_sink_event(GstPad *pad, GstObject *parent, GstEvent *event);
static GstFlowReturn gst_dsd_media_parse_sink_chain(GstPad *pad, GstObject *parent, GstBuffer *buffer);
static gboolean gst_dsd_media_parse_src_event(GstPad *pad, GstObject *parent, GstEvent *event);
static gboolean gst_dsd_media_parse_src_query(GstPad *pad, GstObject *parent, GstQuery *query);
static gboolean gst_dsd_media_parse_sink_activate(GstPad *pad, GstObject *parent);
static gboolean gst_dsd_media_parse_sink_activatemode(GstPad *pad, GstObject *parent, GstPadMode mode, gboolean active);

static GstStateChangeReturn gst_dsd_media_parse_change_state(GstElement *element, GstStateChange transition);

static void gst_dsd_media_parse_pull_mode_loop(gpointer user_data);
static gboolean gst_dsd_media_parse_start_pull_mode_loop(GstDsdMediaParse *self);
static void gst_dsd_media_parse_stop_pull_mode_loop(GstDsdMediaParse *self);

static void gst_dsd_media_parse_reset_all_fields(GstDsdMediaParse *self);

static bool gst_dsd_media_parse_check_if_pull_mode_possible(GstDsdMediaParse *self, GstPad *pad);
static bool gst_dsd_media_parse_check_if_upstream_seekable(GstDsdMediaParse *self);

static void gst_dsd_media_parse_handle_sink_segment_event(GstDsdMediaParse *self, GstEvent *event);
static gboolean gst_dsd_media_parse_handle_src_seek_event(GstDsdMediaParse *self, GstEvent *event);

static bool gst_dsd_media_parse_push_initial_events(GstDsdMediaParse *self);
static bool gst_dsd_media_parse_start_streaming(GstDsdMediaParse *self);
static void gst_dsd_media_parse_report_missing_buffer_timing(GstDsdMediaParse *self, GstBuffer *output_buffer);

static bool gst_dsd_media_parse_verify_advance(GstDsdMediaParse *self, guint64 advance_amount, const gchar *advance_name);

static bool gst_dsd_media_check_for_deviated_upstream_seek(GstDsdMediaParse *self, const GstSegment *event_segment);

static void gst_dsd_media_parse_init_downstream_segment(GstDsdMediaParsePrivate *priv);
static void gst_dsd_media_parse_stream_failed(GstDsdMediaParsePrivate *priv, GstFlowReturn flow_error);

static GstFlowReturn gst_dsd_media_parse_skip_data_during_scan_full(GstDsdMediaParse *self, guint64 num_bytes_to_skip, guint64 *num_actually_skipped_bytes, bool force_seek);



static inline GstDsdMediaParsePrivate* get_private(GstDsdMediaParse *self)
{
	return reinterpret_cast<GstDsdMediaParsePrivate *>(G_STRUCT_MEMBER_P(self, dsd_media_parse_private_offset));
}


static inline ParseStage get_parse_stage(GstDsdMediaParsePrivate *priv)
{
	return priv->parse_stage.load(std::memory_order_acquire);
}

static inline void set_parse_stage(GstDsdMediaParsePrivate *priv, ParseStage parse_stage)
{
	return priv->parse_stage.store(parse_stage, std::memory_order_release);
}

// Convenience accessor for the flushing flag. That field always requires the
// field mutex to be locked, in both read and write cases, since the flush-start
// event is out-of-band and thus may be handled by an arbitrary thread.
static inline bool gst_dsd_media_parse_is_flushing(GstDsdMediaParsePrivate *priv)
{
	std::lock_guard<std::mutex> lock(priv->field_mutex);
	return priv->flushing;
}


static void gst_dsd_media_parse_class_init(gpointer g_class, gpointer /*class_data*/)
{
	GstDsdMediaParseClass *klass = GST_DSD_MEDIA_PARSE_CLASS(g_class);
	GObjectClass *object_class;
	GstElementClass *element_class;

	if (dsd_media_parse_private_offset != 0)
		g_type_class_adjust_private_offset(klass, &dsd_media_parse_private_offset);

	gst_dsd_media_parse_parent_class = reinterpret_cast<GstElementClass*>(g_type_class_peek_parent(klass));

	GST_DEBUG_CATEGORY_INIT(dsdmediaparse_debug, "dsdmediaparse", 0, "DSD media parser base class");

	object_class = G_OBJECT_CLASS(klass);
	element_class = GST_ELEMENT_CLASS(klass);

	object_class->finalize = GST_DEBUG_FUNCPTR(gst_dsd_media_parse_finalize);

	element_class->change_state = GST_DEBUG_FUNCPTR(gst_dsd_media_parse_change_state);
}


static void gst_dsd_media_parse_init(GTypeInstance *instance, gpointer g_class)
{
	GstDsdMediaParse *self = GST_DSD_MEDIA_PARSE_CAST(instance);
	GstDsdMediaParseClass *klass = GST_DSD_MEDIA_PARSE_CLASS(g_class);
	GstDsdMediaParsePrivate *priv = get_private(self);
	GstPadTemplate *pad_template;

	// placement-new for the C++ fields like field_mutex
	// that must have their constructors executed.
	new (priv) GstDsdMediaParsePrivate();

	pad_template = gst_element_class_get_pad_template(GST_ELEMENT_CLASS(klass), "sink");
	g_return_if_fail(pad_template != NULL);
	priv->sinkpad = gst_pad_new_from_template(pad_template, "sink");
	gst_pad_set_event_function(priv->sinkpad, GST_DEBUG_FUNCPTR(gst_dsd_media_parse_sink_event));
	gst_pad_set_chain_function(priv->sinkpad, GST_DEBUG_FUNCPTR(gst_dsd_media_parse_sink_chain));
	gst_pad_set_activate_function(priv->sinkpad, GST_DEBUG_FUNCPTR(gst_dsd_media_parse_sink_activate));
	gst_pad_set_activatemode_function(priv->sinkpad, GST_DEBUG_FUNCPTR(gst_dsd_media_parse_sink_activatemode));
	gst_element_add_pad(GST_ELEMENT(self), priv->sinkpad);

	pad_template = gst_element_class_get_pad_template(GST_ELEMENT_CLASS(klass), "src");
	g_return_if_fail(pad_template != NULL);
	priv->srcpad = gst_pad_new_from_template(pad_template, "src");
	gst_pad_set_event_function(priv->srcpad, GST_DEBUG_FUNCPTR(gst_dsd_media_parse_src_event));
	gst_pad_set_query_function(priv->srcpad, GST_DEBUG_FUNCPTR(gst_dsd_media_parse_src_query));
	gst_element_add_pad(GST_ELEMENT(self), priv->srcpad);

	priv->input_adapter = gst_adapter_new();

	gst_dsd_media_parse_reset_all_fields(self);
}


static void gst_dsd_media_parse_finalize(GObject *object)
{
	GstDsdMediaParse *self = GST_DSD_MEDIA_PARSE(object);
	GstDsdMediaParsePrivate *priv = get_private(self);

	if (priv->input_adapter != nullptr)
		g_object_unref(G_OBJECT(priv->input_adapter));

	gst_caps_replace(&(priv->output_caps), nullptr);
	gst_tag_list_replace(&(priv->output_tag_list), nullptr);
	gst_tag_list_replace(&(priv->upstream_tag_list), nullptr);

	// Manual destructor invocation to call the destructors
	// of C++ fields like field_mutex.
	priv->~GstDsdMediaParsePrivate();

	G_OBJECT_CLASS(gst_dsd_media_parse_parent_class)->finalize(object);
}


static gboolean gst_dsd_media_parse_sink_event(GstPad *pad, GstObject *parent, GstEvent *event)
{
	GstDsdMediaParse *self = GST_DSD_MEDIA_PARSE_CAST(parent);
	GstDsdMediaParsePrivate *priv = get_private(self);

	switch (GST_EVENT_TYPE(event)) {
		case GST_EVENT_STREAM_START:
			// This occurs in push scheduling mode, since then, upstream
			// runs the pad task that produces the stream-start event.
			// In pull scheduling mode, we generate it ourselves. See
			// gst_dffparse_pull_mode_loop().
			GST_DEBUG_OBJECT(self, "forwarding upstream stream-start event: %" GST_PTR_FORMAT, gpointer(event));
			priv->current_seqnum = gst_event_get_seqnum(event);
			return gst_pad_push_event(priv->srcpad, event);

		case GST_EVENT_CAPS: {
			GST_DEBUG_OBJECT(self, "dropping upstream caps event: %" GST_PTR_FORMAT, gpointer(event));
			gst_event_unref(event);
			return TRUE;
		}

		case GST_EVENT_TAG: {
			if (get_parse_stage(priv) != ParseStage::Streaming) {
				GstTagList *upstream_tag_list;
				gst_event_parse_tag(event, &upstream_tag_list);

				GST_DEBUG_OBJECT(
					self,
					"got upstream tag event in non Streaming stage; storing the "
					"upstream tags; tag list: %" GST_PTR_FORMAT,
					gpointer(upstream_tag_list)
				);

				// Upstream might send several tag lists. Merge these into
				// priv->upstream_tag_list to take that into account.
				// Replace existing tags instead of appending. Otherwise,
				// should for whatever reason upstream spam this element
				// with tag events, the tag list would grow unbounded.
				GstTagList *merged_upstream_tag_list = gst_tag_list_merge(
					priv->upstream_tag_list,
					upstream_tag_list,
					GST_TAG_MERGE_REPLACE
				);
				gst_tag_list_take(&(priv->upstream_tag_list), merged_upstream_tag_list);

				gst_event_unref(event);

				return TRUE;
			} else {
				GST_DEBUG_OBJECT(
					self,
					"passing through upstream tag event in Streaming stage"
				);
				break;
			}
		}

		case GST_EVENT_FLUSH_START: {
			bool is_internal_flush;

			{
				std::lock_guard<std::mutex> lock(priv->field_mutex);

				is_internal_flush = priv->internal_seek_seqnum.has_value() &&
				                    (*(priv->internal_seek_seqnum) == gst_event_get_seqnum(event));

				// Set the flushing flag only if the flush is coming from an external
				// source. That is a "real" flush, that is, not one associated with an
				// internal seek as part of the parsing process.
				if (!is_internal_flush)
					priv->flushing = true;
			}

			GST_DEBUG_OBJECT(
				self,
				"got flush-start event with seqnum %" G_GUINT32_FORMAT "; treating it as %s",
				gst_event_get_seqnum(event),
				is_internal_flush ? "internal" : "external"
			);

			if (is_internal_flush) {
				gst_event_unref(event);
				return TRUE;
			}

			return gst_pad_push_event(priv->srcpad, event);
		}

		case GST_EVENT_FLUSH_STOP: {
			bool is_internal_flush;

			gboolean reset_time;
			gst_event_parse_flush_stop(event, &reset_time);

			{
				// Access to the flushing, internal_seek_seqnum and
				// expecting_upstream_segment fields requires the field mutex
				// to be locked. See their documentation for why.
				std::lock_guard<std::mutex> lock(priv->field_mutex);

				is_internal_flush = priv->internal_seek_seqnum.has_value() &&
				                    (*(priv->internal_seek_seqnum) == gst_event_get_seqnum(event));

				// NOTE: internal_seek_seqnum is deliberately not cleared here.
				// It is withdrawn once the segment event that concludes the
				// internal seek arrives. See its documentation for why.

				if (!is_internal_flush) {
					priv->flushing = false;

					if (reset_time) {
						priv->downstream_segment.base = 0;
						priv->reset_running_time_on_next_segment = true;
					}

					if (get_parse_stage(priv) != ParseStage::ScanningInfo)
						priv->expecting_upstream_segment = true;
				}
			}

			GST_DEBUG_OBJECT(
				self,
				"got flush-stop event with seqnum %" G_GUINT32_FORMAT "; treating it as %s",
				gst_event_get_seqnum(event),
				is_internal_flush ? "internal" : "external"
			);

			gst_adapter_clear(priv->input_adapter);
			priv->next_buffer_is_discont = true;

			if (is_internal_flush) {
				gst_event_unref(event);
				return TRUE;
			} else {
				gboolean ret = gst_pad_push_event(priv->srcpad, event);

				return ret;
			}
		}

		case GST_EVENT_SEGMENT: {
			gst_dsd_media_parse_handle_sink_segment_event(self, event);
			return TRUE;
		}

		case GST_EVENT_EOS: {
			gst_event_unref(event);

			// An EOS event makes no sense in pull scheduling mode. It is
			// generated by a pad task loop, which in pull scheduling mode
			// is run in this element's sinkpad. Upstream is passive.
			// Upstream _can_ report an EOS, but in pull mode, it does so
			// via gst_pad_pull_range().
			if (priv->sinkpad_mode == GST_PAD_MODE_PULL) {
				GST_ELEMENT_ERROR(
					self, STREAM, FAILED,
					("Internal data stream error."),
					("got an EOS event while operating in the pull scheduling mode")
				);
				std::lock_guard<std::mutex> lock(priv->field_mutex);
				gst_dsd_media_parse_stream_failed(priv, GST_FLOW_ERROR);
				return TRUE;
			}

			if (get_parse_stage(priv) != ParseStage::Streaming) {
				// Reaching EOS during the Scanning Info stage is not fatal if the
				// payload was already located. The metadata past the payload is
				// lost, but it is not essential.
				if ((get_parse_stage(priv) == ParseStage::ScanningInfo) &&
				    gst_dsd_media_parse_was_payload_reported(self)) {
					GST_WARNING_OBJECT(
						self,
						"got EOS while scanning, but the payload was already found; "
						"concluding the scan - media might be corrupted, metadata "
						"past the payload is unreadable, but payload can be used"
					);

					// NOTE: This seeks back to the payload, which also undoes the EOS
					// state upstream, so the EOS event is not forwarded downstream.
					gst_dsd_media_parse_scanning_finished(self);

					return TRUE;
				}

				// Upstream reached the end of the medium before this parser located
				// the payload. The medium is therefore unusable. (This can also be
				// caused by a subclass that never finishes the Scanning Info stage.)
				GST_ELEMENT_ERROR(
					self, STREAM, DEMUX,
					("Media ended unexpectedly. It may be truncated and invalid."),
					("got EOS before the Streaming stage was reached")
				);

				std::lock_guard<std::mutex> lock(priv->field_mutex);
				gst_dsd_media_parse_stream_failed(priv, GST_FLOW_ERROR);

				return TRUE;
			}

			// If EOS is pushed in the Streaming stage, forward it. It may come from
			// the application itself (for example, gst-launch with the -e switch).
			break;
		}

		default:
			break;
	}

	return gst_pad_event_default(pad, parent, event);
}


static GstFlowReturn gst_dsd_media_parse_sink_chain(GstPad *, GstObject *parent, GstBuffer *buffer)
{
	GstDsdMediaParse *self = GST_DSD_MEDIA_PARSE_CAST(parent);
	GstDsdMediaParseClass *klass = GST_DSD_MEDIA_PARSE_GET_CLASS(self);
	GstDsdMediaParsePrivate *priv = get_private(self);

	bool keep_parsing = true;
	bool expecting_upstream_segment;

	{
		std::lock_guard<std::mutex> lock(priv->field_mutex);

		expecting_upstream_segment = priv->expecting_upstream_segment;

		if (G_UNLIKELY(priv->flushing)) {
			GST_LOG_OBJECT(
				self,
				"got input buffer, but currently flushing: %" GST_PTR_FORMAT,
				gpointer(buffer)
			);
			gst_buffer_unref(buffer);
			return GST_FLOW_FLUSHING;
		}
	}

	if (G_UNLIKELY(expecting_upstream_segment)) {
		GST_ELEMENT_ERROR(
			self, STREAM, DEMUX,
			("Internal data stream error."),
			("upstream pushed a buffer without having sent a segment event after the last flush")
		);
		gst_buffer_unref(buffer);
		return GST_FLOW_ERROR;
	}

	GST_LOG_OBJECT(self, "got input buffer: %" GST_PTR_FORMAT, gpointer(buffer));
	gst_adapter_push(priv->input_adapter, buffer);

	while (keep_parsing) {
		GstFlowReturn flow_ret = GST_FLOW_OK;

		// The flushing flag is re-checked on every iteration, not just once
		// on entry. A flush can begin at any point while this loop runs, since
		// flush-start is an out-of-band event that is handled by a different
		// thread and does not wait for the stream lock.
		//
		// In the Streaming stage, carrying on regardless would be survivable,
		// since gst_pad_push() reports GST_FLOW_FLUSHING and the loop unwinds.
		// In the ScanningInfo stage there is no such signal - nothing is pushed
		// downstream yet - so the loop would keep consuming adapter data,
		// advancing the byte position, and potentially issuing internal
		// upstream seeks at an upstream element that is already flushing.
		if (G_UNLIKELY(gst_dsd_media_parse_is_flushing(priv))) {
			GST_DEBUG_OBJECT(self, "aborting the parse loop - a flush started while parsing");
			return GST_FLOW_FLUSHING;
		}

		// If there is no data to read, there's no reason to continue the loop.
		// Later, when upstream delivers more data, this chain function is called
		// again, with that additional data.
		if (gst_adapter_available(priv->input_adapter) == 0)
			break;

		switch (get_parse_stage(priv)) {
			case ParseStage::ScanningInfo: {
				if (G_UNLIKELY(!priv->upstream_is_seekable.has_value()))
				{
					// Do the query with field_mutex unlocked, since it issues
					// upstream queries. These could in theory re-enter code
					// here, which takes the field_mutex. Locking it here
					// would therefore potentially lead to a deadlock.
					bool upstream_is_seekable = gst_dsd_media_parse_check_if_upstream_seekable(self);

					std::lock_guard<std::mutex> lock(priv->field_mutex);
					priv->upstream_is_seekable = upstream_is_seekable;
				}

				guint64 previous_byte_position = priv->byte_position;

				flow_ret = klass->scan_info(self);

				switch (flow_ret) {
					case GST_FLOW_OK:
						if (previous_byte_position == priv->byte_position) {
							GST_ELEMENT_ERROR(
								self, STREAM, FAILED,
								("Internal data stream error."),
								(
									"subclass %s returned GST_FLOW_OK out of scan_info() "
									"without having read or skipped anything",
									G_OBJECT_TYPE_NAME(self)
								)
							);
							flow_ret = GST_FLOW_ERROR;
							keep_parsing = false;
						}
						break;

					case GST_FLOW_NOTHING_TO_READ:
						GST_DEBUG_OBJECT(self, "scan_info() intentionally did not read anything");
						flow_ret = GST_FLOW_OK;
						// _Not_ setting keep_parsing to false here. This is because
						// scan_info() might have set internal states that will lead
						// to data being produced in the next loop iteration.
						break;

					case GST_FLOW_ADVANCE_OUT_OF_BOUNDS:
						// The subclass did not handle this, so treat it as the error
						// it is. This custom code must never leave this function.
						GST_ELEMENT_ERROR(
							self,
							STREAM,
							DEMUX,
							("Invalid or corrupted media."),
							(
								"a read or skip during scanning exceeded the bounds, "
								"and %s did not handle the associated flow error code",
								G_OBJECT_TYPE_NAME(self)
							)
						);
						flow_ret = GST_FLOW_ERROR;
						keep_parsing = false;
						break;

					default:
						// In any other case, we want to stop the loop.
						keep_parsing = false;
						break;
				}

				break;
			}

			case ParseStage::Streaming: {
				GstBuffer *output_buffer = nullptr;

				flow_ret = klass->produce_output(self, priv->byte_position, priv->end_payload_position, &output_buffer);
				if (G_UNLIKELY((flow_ret == GST_FLOW_OK) && (output_buffer == nullptr))) {
					GST_DEBUG_OBJECT(self, "subclass cannot currently produce output; will try again");
					// NOTE: _not_ setting keep_parsing to false, since the subclass might
					// currently be skipping corrupted data, or might be resynchronizing
					// after a seek landed at an unexpected byte position.
					break;
				}

				ScopeGuard guard([&]() { if (output_buffer != nullptr) gst_buffer_unref(output_buffer); });

				if (G_LIKELY(flow_ret == GST_FLOW_OK)) {
					g_assert(output_buffer != nullptr);
					gsize output_size = gst_buffer_get_size(output_buffer);
					GST_LOG_OBJECT(self, "produced %" G_GSIZE_FORMAT " byte(s) of output", output_size);
				} else {
					switch (flow_ret) {
						case GST_FLOW_EOS:
							break;
						case GST_FLOW_FLUSHING:
							// In push mode, this cannot be a valid flow return code. It is only
							// returned by gst_dsd_media_parse_read_data_during_streaming() in
							// the pull mode path. This means that the only possible source is
							// the subclass, which should not return this.
							GST_ERROR_OBJECT(self, "got unexpected flushing flow return code; subclass might have an error");
							break;
						case GST_FLOW_NOT_ENOUGH_DATA:
							keep_parsing = false;
							break;
						default:
							GST_ERROR_OBJECT(self, "could not produce output: %s", gst_flow_get_name(flow_ret));
							break;
					}
					break;
				}

				if (G_UNLIKELY(!GST_BUFFER_PTS_IS_VALID(output_buffer) ||
				               !GST_BUFFER_DURATION_IS_VALID(output_buffer))) {
					gst_dsd_media_parse_report_missing_buffer_timing(self, output_buffer);
					flow_ret = GST_FLOW_ERROR;
					keep_parsing = false;
					break;
				}

				if (priv->next_buffer_is_discont) {
					GST_BUFFER_FLAG_SET(output_buffer, GST_BUFFER_FLAG_DISCONT);
					priv->next_buffer_is_discont = false;
				}

				{
					std::lock_guard<std::mutex> lock(priv->field_mutex);
					priv->downstream_segment.position = GST_BUFFER_PTS(output_buffer) + GST_BUFFER_DURATION(output_buffer);
				}

				GST_LOG_OBJECT(
					self,
					"pushing %" G_GUINT64_FORMAT " byte(s) from byte position %" G_GUINT64_FORMAT,
					gst_buffer_get_size(output_buffer),
					priv->byte_position
				);

				flow_ret = gst_pad_push(priv->srcpad, output_buffer);

				guard.dismiss();

				break;
			}

			case ParseStage::StreamFailure:
				// No GST_ELEMENT_ERROR() call here - whatever put the parser into this
				// stage already reported the error, and has more context for doing so.
				flow_ret = priv->stream_flow_error;
				keep_parsing = false;
				break;

			case ParseStage::AwaitingSegment:
				// This can happen when a seek event concurrently arrives
				// at the srcpad. Buffers may be in flight while the stage
				// was already switched to AwaitingSegment as part of the
				// seek operation. Do nothing in that case; the seek
				// operation will eventually wipe the contents of the
				// input adapter already.
				GST_DEBUG_OBJECT(self, "ignoring incoming buffer while in AwaitingSegment stage");
				flow_ret = GST_FLOW_OK;
				gst_adapter_clear(priv->input_adapter);
				keep_parsing = false;
				break;

			default:
				GST_ERROR_OBJECT(self, "invalid parse stage %d reached", gint(get_parse_stage(priv)));
				g_assert_not_reached();
				flow_ret = GST_FLOW_ERROR;
				break;
		}

		switch (flow_ret) {
			case GST_FLOW_OK:
				break;

			case GST_FLOW_EOS:
				GST_LOG_OBJECT(self, "EOS reached in push mode");
				return GST_FLOW_EOS;

			case GST_FLOW_FLUSHING:
				GST_LOG_OBJECT(self, "got FLUSHING flow return code in push mode");
				return GST_FLOW_FLUSHING;

			case GST_FLOW_NOT_ENOUGH_DATA: {
				// If there was insufficient data for a read operation to
				// complete, GST_FLOW_NOT_ENOUGH_DATA (a custom flow error
				// code) is returned. It is not an error; the code just
				// has to try again next time this chain function is called,
				// since when it is called, more data is pushed into it.
				GST_LOG_OBJECT(self, "a read operation had insufficent data in the input adapter");
				return GST_FLOW_OK;
			}

			default: {
				GST_ERROR_OBJECT(self, "aborting data processing due to flow error: %s", gst_flow_get_name(flow_ret));
				return flow_ret;
			}
		}
	}

	GST_LOG_OBJECT(self, "all available data processed");

	return GST_FLOW_OK;
}


static gboolean gst_dsd_media_parse_src_event(GstPad *pad, GstObject *parent, GstEvent *event)
{
	GstDsdMediaParse *self = GST_DSD_MEDIA_PARSE_CAST(parent);
	GstDsdMediaParsePrivate *priv = get_private(self);

	switch (GST_EVENT_TYPE(event)) {
		case GST_EVENT_FLUSH_START: {
			GST_DEBUG_OBJECT(
				self,
				"got flush-start event from downstream; setting flushing flag and forwarding event upstream"
			);

			{
				std::lock_guard<std::mutex> lock(priv->field_mutex);
				priv->flushing = true;
			}

			return gst_pad_push_event(priv->sinkpad, event);
		}

		case GST_EVENT_FLUSH_STOP: {
			GST_DEBUG_OBJECT(
				self,
				"got flush-stop event from downstream; clearing flushing flag and forwarding event upstream"
			);

			gboolean reset_time;
			gst_event_parse_flush_stop(event, &reset_time);

			GST_PAD_STREAM_LOCK(priv->sinkpad);

			GstEvent *segment_event;

			{
				std::lock_guard<std::mutex> lock(priv->field_mutex);

				priv->flushing = false;

				if (get_parse_stage(priv) != ParseStage::ScanningInfo)
					priv->expecting_upstream_segment = true;

				if (reset_time) {
					priv->downstream_segment.base = 0;
					priv->reset_running_time_on_next_segment = true;
				}

				// After a flush in pull mode, a segment event must be pushed
				// downstream, because the sticky segment events downstream
				// have been cleared by the flush. In push mode, upstream
				// pushes a new segment. The segment event handler processes
				// that, and pushes a new segment downstream. Nothing like
				// this exists in pull mode, since upstream is completely
				// passive then.

				if ((priv->sinkpad_mode == GST_PAD_MODE_PULL) && (priv->downstream_segment.format != GST_FORMAT_UNDEFINED)) {
					segment_event = gst_event_new_segment(&(priv->downstream_segment));
					gst_event_set_seqnum(segment_event, priv->current_seqnum);
				} else {
					segment_event = nullptr;
				}
			}

			gst_adapter_clear(priv->input_adapter);

			gboolean ret = TRUE;

			if (segment_event != nullptr)
				ret = gst_pad_push_event(priv->srcpad, segment_event);

			if (ret) {
				priv->next_buffer_is_discont = true;

				if (priv->sinkpad_mode == GST_PAD_MODE_PULL)
					gst_dsd_media_parse_start_pull_mode_loop(self);

				ret = gst_pad_push_event(priv->sinkpad, event);
			} else {
				GST_ELEMENT_ERROR(
					self, CORE, EVENT,
					("Internal data stream error."),
					("failed to push segment event downstream")
				);

				{
					std::lock_guard<std::mutex> lock(priv->field_mutex);
					gst_dsd_media_parse_stream_failed(priv, GST_FLOW_ERROR);
				}

				// Forward the flush-stop event also in the error case. Otherwise,
				// upstream stays flushing, and the pipeline hangs, rather than
				// reporting the error above.
				gst_pad_push_event(priv->sinkpad, event);
				ret = FALSE;
			}

			GST_PAD_STREAM_UNLOCK(priv->sinkpad);

			return ret;
		}

		case GST_EVENT_SEEK:
			return gst_dsd_media_parse_handle_src_seek_event(self, event);

		default:
			break;
	}

	return gst_pad_event_default(pad, parent, event);
}


static gboolean gst_dsd_media_parse_src_query(GstPad *pad, GstObject *parent, GstQuery *query)
{
	// NOTE: Since queries can be issued from different threads,
	// all field_mutex protected fields must be accessed with
	// that mutex locked. (Stream lock protected fields are
	// not accessed here.)

	GstDsdMediaParse *self = GST_DSD_MEDIA_PARSE(parent);
	GstDsdMediaParseClass *klass = GST_DSD_MEDIA_PARSE_GET_CLASS(self);
	GstDsdMediaParsePrivate *priv = get_private(self);

	switch (GST_QUERY_TYPE(query)) {
		case GST_QUERY_DURATION: {
			GstFormat format;

			gst_query_parse_duration(query, &format, nullptr);

			{
				std::lock_guard<std::mutex> lock(priv->field_mutex);

				if (get_parse_stage(priv) == ParseStage::ScanningInfo) {
					GST_DEBUG_OBJECT(
						self,
						"cannot handle duration query yet - still scanning info"
					);
					return FALSE;
				}
			}

			GST_LOG_OBJECT(self, "got duration query with format %s", gst_format_get_name(format));

			std::lock_guard<std::mutex> lock(priv->field_mutex);

			switch (format) {
				case GST_FORMAT_BYTES:
					// Check for a valid payload_position. If it is
					// not defined, then neither is payload_size.
					if (!priv->payload_position.has_value()) {
						GST_DEBUG_OBJECT(self, "cannot respond to bytes duration query - payload size is not currently known");
						return FALSE;
					}
					gst_query_set_duration(query, format, priv->payload_size);
					break;

				case GST_FORMAT_TIME:
					if (!GST_CLOCK_TIME_IS_VALID(priv->duration)) {
						GST_DEBUG_OBJECT(self, "cannot respond to time duration query - duration is not currently known");
						return FALSE;
					}
					gst_query_set_duration(query, format, priv->duration);
					break;

				default:
					GST_DEBUG_OBJECT(self, "%s duration queries are not supported", gst_format_get_name(format));
					return FALSE;
			}


			return TRUE;
		}

		case GST_QUERY_POSITION: {
			GstFormat format;
			gint64 position;

			gst_query_parse_position(query, &format, nullptr);

			{
				std::lock_guard<std::mutex> lock(priv->field_mutex);

				if (get_parse_stage(priv) == ParseStage::ScanningInfo) {
					GST_DEBUG_OBJECT(
						self,
						"cannot handle position query yet - still scanning info"
					);
					return FALSE;
				}
			}

			switch (format) {
				case GST_FORMAT_TIME: {
					std::lock_guard<std::mutex> lock(priv->field_mutex);
					position = priv->downstream_segment.position;
					break;
				}

				case GST_FORMAT_BYTES: {
					guint64 segment_position;

					{
						std::lock_guard<std::mutex> lock(priv->field_mutex);
						segment_position = priv->downstream_segment.position;
					}

					// Do a round trip via an index. This is the most accurate
					// result we can produce, since the subclass will de facto
					// produce output with index granularity anyway.
					//
					// Note that the round-trip snaps the value to the position
					// boundary, which is intentional, since positions that
					// aren't aligned can't be handled by the subclass.
					guint64 index = klass->to_index(
						self,
						GST_FORMAT_TIME,
						segment_position,
						ToIndexRoundingMode::RoundingDown
					);
					position = klass->from_index(self, GST_FORMAT_BYTES, index);

					break;
				}

				default:
					GST_DEBUG_OBJECT(self, "%s position query is not supported", gst_format_get_name(format));
					return FALSE;
			}

			GST_DEBUG_OBJECT(
				self,
				"responding to %s position query with position %" G_GINT64_FORMAT,
				gst_format_get_name(format),
				position
			);

			gst_query_set_position(query, format, position);

			return TRUE;
		}

		case GST_QUERY_CONVERT: {
			GstFormat source_format, dest_format;
			gint64 source_value, dest_value;
			gint64 source_max_value;

			gst_query_parse_convert(
				query,
				&source_format, &source_value,
				&dest_format, nullptr
			);

			{
				std::lock_guard<std::mutex> lock(priv->field_mutex);

				if (get_parse_stage(priv) == ParseStage::ScanningInfo) {
					GST_DEBUG_OBJECT(
						self,
						"cannot handle convert query yet - still scanning info"
					);
					return FALSE;
				}

				if (source_format == GST_FORMAT_TIME)
					source_max_value = priv->downstream_segment.duration;
				else
					source_max_value = priv->payload_size;
			}

			if (source_value < 0) {
				GST_DEBUG_OBJECT(
					self,
					"cannot handle negative convert query source value %" G_GINT64_FORMAT,
					source_value
				);
				return FALSE;
			}

			if (
				!((source_format == GST_FORMAT_BYTES) && (dest_format == GST_FORMAT_TIME)) &&
				!((source_format == GST_FORMAT_TIME) && (dest_format == GST_FORMAT_BYTES)) &&
				!((source_format == GST_FORMAT_BYTES) && (dest_format == GST_FORMAT_BYTES)) &&
				!((source_format == GST_FORMAT_TIME) && (dest_format == GST_FORMAT_TIME))
			) {
				GST_DEBUG_OBJECT(
					self,
					"%s -> %s conversion queries are not supported",
					gst_format_get_name(source_format),
					gst_format_get_name(dest_format)
				);
				return FALSE;
			}

			if (source_format == dest_format) {
				dest_value = source_value;
				GST_DEBUG_OBJECT(
					self,
					"got convert query with the same source and dest formats (%s); passing source value %" G_GINT64_FORMAT " through",
					gst_format_get_name(source_format),
					source_value
				);
			} else {
				// Do a round trip via an index. This is the most accurate
				// result we can produce, since the subclass will de facto
				// produce output with index granularity anyway.
				// Clamp the source value, since it can be out of bounds.
				// Note that the round-trip snaps the value to the position
				// boundary, which is intentional, since positions that
				// aren't aligned can't be handled by the subclass.
				guint64 index = klass->to_index(
					self,
					source_format,
					std::min(std::max(source_value, gint64(0)), source_max_value),
					ToIndexRoundingMode::RoundingDown
				);
				dest_value = klass->from_index(self, dest_format, index);
			}

			gst_query_set_convert(
				query,
				source_format, source_value,
				dest_format, dest_value
			);

			return TRUE;
		}

		case GST_QUERY_SEEKING: {
			guint64 payload_size;
			GstClockTime duration;
			gboolean upstream_is_seekable;

			{
				std::lock_guard<std::mutex> lock(priv->field_mutex);

				if (get_parse_stage(priv) == ParseStage::ScanningInfo) {
					GST_DEBUG_OBJECT(
						self,
						"cannot handle seeking query yet - still scanning info"
					);
					return FALSE;
				}

				if (!(priv->upstream_is_seekable.has_value())) {
					GST_DEBUG_OBJECT(
						self,
						"cannot respond to seeking query - not yet known whether upstream can handle seeking"
					);
					return FALSE;
				}

				upstream_is_seekable = *(priv->upstream_is_seekable);

				duration = priv->duration;

				payload_size = priv->payload_size;
			}

			GstFormat format;
			gst_query_parse_seeking(query, &format, nullptr, nullptr, nullptr);

			gint64 seek_query_end;

			switch (format) {
				case GST_FORMAT_BYTES:
					seek_query_end = payload_size;
					break;

				case GST_FORMAT_TIME:
					seek_query_end = duration;
					break;

				default:
					GST_DEBUG_OBJECT(self, "%s seeking query is not supported", gst_format_get_name(format));
					return FALSE;
			}

			GST_DEBUG_OBJECT(
				self,
				"responding to %s seeking query: seekable: %d; range 0 - %" G_GINT64_FORMAT,
				gst_format_get_name(format),
				upstream_is_seekable,
				seek_query_end
			);

			gst_query_set_seeking(query, format, upstream_is_seekable, 0, seek_query_end);

			return TRUE;
		}

		default:
			break;
	}

	return gst_pad_query_default(pad, parent, query);
}


static gboolean gst_dsd_media_parse_sink_activate(GstPad *pad, GstObject *parent)
{
	GstDsdMediaParse *self = GST_DSD_MEDIA_PARSE(parent);

	// Try pull mode first. Pull mode is more convenient and efficient for
	// parsing since it allows for upstream data retrievals at arbitrary
	// offsets and sizes. No aggregation the input_adapter is needed, and no
	// upstream seek events are required for upstream random access. However,
	// pull mode is not supported by all upstream elements, so push mode
	// is used as fallback.

	if (gst_dsd_media_parse_check_if_pull_mode_possible(self, pad)) {
		if (gst_pad_activate_mode(pad, GST_PAD_MODE_PULL, TRUE)) {
			GST_DEBUG_OBJECT(self, "activating pull mode succeeded");
			return TRUE;
		} else {
			GST_DEBUG_OBJECT(self, "activating pull mode failed; falling back to push mode");
		}
	}

	GST_DEBUG_OBJECT(self, "activating push mode");
	return gst_pad_activate_mode(pad, GST_PAD_MODE_PUSH, TRUE);
}


static gboolean gst_dsd_media_parse_sink_activatemode(GstPad *pad, GstObject *parent, GstPadMode mode, gboolean active)
{
	GstDsdMediaParse *self = GST_DSD_MEDIA_PARSE(parent);
	GstDsdMediaParsePrivate *priv = get_private(self);

	if (active) {
		priv->sinkpad_mode = mode;

		// In pull mode, upstream random access is always possible,
		// so set upstream_is_seekable to true here. Should pull
		// mode activation fail later, it can still be unset.
		if (mode == GST_PAD_MODE_PULL)
			priv->upstream_is_seekable = true;
	}

	switch (mode) {
		case GST_PAD_MODE_PUSH:
			// Nothing special needs to be done here when push mode is activated.
			return TRUE;

		case GST_PAD_MODE_PULL: {
			// In pull mode, we need to start/stop here the pad task that drives the upstream dataflow.
			gboolean ret;

			if (active) {
				GST_DEBUG_OBJECT(self, "starting upstream pull mode dataflow pad task");
				ret = gst_dsd_media_parse_start_pull_mode_loop(self);
			} else {
				GST_DEBUG_OBJECT(self, "stopping upstream pull mode dataflow pad task");
				ret = gst_pad_stop_task(pad);
			}

			if (!ret) {
				// If we could not start the pull mode loop or stop the task,
				// and FALSE is returned here, it will cause gst_pad_activate_mode()
				// to return FALSE as well, which initiates a fallback to the
				// PUSH mode. This means that the upstream_is_seekable = true
				// assignment from above must be undone before this branch is
				// finished. Otherwise, the push mode will run without actually
				// checking whether upstrema can seek or not.
				priv->upstream_is_seekable = std::nullopt;
			}

			return ret;
		}

		default:
			GST_ERROR_OBJECT(self, "unknown mode %d", int(mode));
			return FALSE;
	}
}


static GstStateChangeReturn gst_dsd_media_parse_change_state(GstElement *element, GstStateChange transition)
{
	GstDsdMediaParse *self = GST_DSD_MEDIA_PARSE(element);
	GstDsdMediaParseClass *klass = GST_DSD_MEDIA_PARSE_GET_CLASS(self);
	GstDsdMediaParsePrivate *priv = get_private(self);
	GstStateChangeReturn result;

	switch (transition) {
		case GST_STATE_CHANGE_NULL_TO_READY: {
			bool all_required_vmethods_set = true;

			// Perform sanity check on the vmethods. Should the subclass
			// not have set the required ones, the state change fails.
			// Without these methods, further processing is not possible.
#define CHECK_IF_REQUIRED_VMETHOD_IS_SET(VMETHOD) \
			G_STMT_START { \
				if (G_UNLIKELY(klass->VMETHOD == nullptr)) { \
					GST_ELEMENT_ERROR( \
						self, \
						STREAM, \
						FAILED, \
						("Error in parser implementation."), \
						( \
							"subclass %s did not set required vmethod %s", \
							G_OBJECT_TYPE_NAME(self), \
							#VMETHOD \
						) \
					); \
					all_required_vmethods_set = false; \
				} \
			} G_STMT_END

			CHECK_IF_REQUIRED_VMETHOD_IS_SET(scan_info);
			CHECK_IF_REQUIRED_VMETHOD_IS_SET(to_index);
			CHECK_IF_REQUIRED_VMETHOD_IS_SET(from_index);
			CHECK_IF_REQUIRED_VMETHOD_IS_SET(produce_output);
			CHECK_IF_REQUIRED_VMETHOD_IS_SET(verify_advance);

#undef CHECK_IF_REQUIRED_VMETHOD_IS_SET

			if (G_UNLIKELY(!all_required_vmethods_set))
				return GST_STATE_CHANGE_FAILURE;

			gst_dsd_media_parse_reset_all_fields(self);

			if (klass->setup != nullptr) {
				if (G_UNLIKELY(!klass->setup(self)))
					return GST_STATE_CHANGE_FAILURE;
			}

			break;
		}

		default:
			break;
	}

	if ((result = GST_ELEMENT_CLASS(gst_dsd_media_parse_parent_class)->change_state(element, transition)) == GST_STATE_CHANGE_FAILURE)
		return result;

	switch (transition) {
		case GST_STATE_CHANGE_PAUSED_TO_READY:
			if (priv->sinkpad_mode == GST_PAD_MODE_PULL) {
				// Set the flushing flag so that the pull mode loop pauses itself
				// instead of starting another round of processing. The flag is
				// deliberately not cleared afterwards: doing so would clobber the
				// state of an external flush that happens to be in progress, and
				// gst_dsd_media_parse_reset_all_fields() below clears it anyway.
				{
					std::lock_guard<std::mutex> lock(priv->field_mutex);
					priv->flushing = true;
				}

				gst_pad_stop_task(priv->sinkpad);
			}

			gst_dsd_media_parse_reset_all_fields(self);

			break;

		case GST_STATE_CHANGE_READY_TO_NULL:
			if (klass->teardown != nullptr)
				klass->teardown(self);

			break;

		default:
			break;
	}

	return result;
}


static void gst_dsd_media_parse_pull_mode_loop(gpointer user_data)
{
	GstDsdMediaParse *self = GST_DSD_MEDIA_PARSE_CAST(user_data);
	GstDsdMediaParseClass *klass = GST_DSD_MEDIA_PARSE_GET_CLASS(self);
	GstDsdMediaParsePrivate *priv = get_private(self);

	GstFlowReturn flow_ret = GST_FLOW_OK;

	bool flushing;

	{
		std::lock_guard<std::mutex> lock(priv->field_mutex);
		flushing = priv->flushing;
	}

	if (G_UNLIKELY(flushing)) {
		GST_DEBUG_OBJECT(
			self,
			"cannot run normal pull mode processing - currently flushing"
		);

		gst_dsd_media_parse_stop_pull_mode_loop(self);

		return;
	}

	switch (get_parse_stage(priv)) {
		case ParseStage::ScanningInfo: {
			guint64 previous_byte_position = priv->byte_position;

			flow_ret = klass->scan_info(self);

			switch (flow_ret) {
				case GST_FLOW_OK:
					if (previous_byte_position == priv->byte_position) {
						GST_ELEMENT_ERROR(
							self,
							STREAM,
							FAILED,
							("Internal data stream error."),
							(
								"subclass %s returned GST_FLOW_OK out of scan_info() without "
								"having advanced the parse position",
								G_OBJECT_TYPE_NAME(self)
							)
						);
						flow_ret = GST_FLOW_ERROR;
					}
					break;

				case GST_FLOW_NOTHING_TO_READ:
					GST_DEBUG_OBJECT(self, "scan_info() intentionally did not read anything");
					flow_ret = GST_FLOW_OK;
					// _Not_ setting keep_parsing to false here. This is because
					// scan_info() might have set internal states that will lead
					// to data being produced in the next loop iteration.
					break;

				case GST_FLOW_ADVANCE_OUT_OF_BOUNDS:
					// The subclass did not handle this, so treat it as the error
					// it is. This custom code must never leave this function.
					GST_ELEMENT_ERROR(
						self,
						STREAM,
						DEMUX,
						("Invalid or corrupted media."),
						(
							"a read or skip during scanning exceeded the bounds, "
							"and %s did not handle the associated flow error code",
							G_OBJECT_TYPE_NAME(self)
						)
					);
					flow_ret = GST_FLOW_ERROR;
					break;

				default:
					break;
			}

			break;
		}

		case ParseStage::Streaming: {
			GstBuffer *output_buffer = nullptr;

			flow_ret = klass->produce_output(self, priv->byte_position, priv->end_payload_position, &output_buffer);
			if (G_UNLIKELY((flow_ret == GST_FLOW_OK) && (output_buffer == nullptr))) {
				GST_DEBUG_OBJECT(self, "subclass cannot currently produce output; will try again");
				break;
			}

			ScopeGuard guard([&]() { if (output_buffer != nullptr) gst_buffer_unref(output_buffer); });

			if (G_LIKELY(flow_ret == GST_FLOW_OK)) {
				g_assert(output_buffer != nullptr);
				gsize output_size = gst_buffer_get_size(output_buffer);
				GST_LOG_OBJECT(self, "produced %" G_GSIZE_FORMAT " byte(s) of output", output_size);
			} else {
				switch (flow_ret) {
					case GST_FLOW_EOS:
						break;
					case GST_FLOW_FLUSHING:
						break;
					default:
						GST_ERROR_OBJECT(self, "could not produce output: %s", gst_flow_get_name(flow_ret));
						break;
				}
				break;
			}

			if (G_UNLIKELY(!GST_BUFFER_PTS_IS_VALID(output_buffer) ||
			               !GST_BUFFER_DURATION_IS_VALID(output_buffer))) {
				gst_dsd_media_parse_report_missing_buffer_timing(self, output_buffer);
				flow_ret = GST_FLOW_ERROR;
				break;
			}

			if (priv->next_buffer_is_discont) {
				GST_BUFFER_FLAG_SET(output_buffer, GST_BUFFER_FLAG_DISCONT);
				priv->next_buffer_is_discont = false;
			}

			{
				std::lock_guard<std::mutex> lock(priv->field_mutex);
				priv->downstream_segment.position = GST_BUFFER_PTS(output_buffer) + GST_BUFFER_DURATION(output_buffer);
			}

			GST_LOG_OBJECT(
				self,
				"pushing %" G_GUINT64_FORMAT " byte(s) from byte position %" G_GUINT64_FORMAT,
				gst_buffer_get_size(output_buffer),
				priv->byte_position
			);

			flow_ret = gst_pad_push(priv->srcpad, output_buffer);

			guard.dismiss();

			break;
		}

		case ParseStage::StreamFailure:
			flow_ret = priv->stream_flow_error;
			break;

		default:
			GST_ERROR_OBJECT(self, "invalid parse stage %d reached", gint(get_parse_stage(priv)));
			g_assert_not_reached();
			flow_ret = GST_FLOW_ERROR;
			break;
	}

	switch (flow_ret) {
		case GST_FLOW_OK:
			break;

		case GST_FLOW_FLUSHING:
			gst_dsd_media_parse_stop_pull_mode_loop(self);
			break;

		case GST_FLOW_EOS: {
			GST_DEBUG_OBJECT(self, "reached EOS");

			if (get_parse_stage(priv) == ParseStage::Streaming) {
				GstEvent *event = gst_event_new_eos();
				gst_event_set_seqnum(event, priv->current_seqnum);
				if (!gst_pad_push_event(priv->srcpad, event))
					GST_ERROR_OBJECT(self, "could not push EOS event downstream");
			} else {
				GST_ELEMENT_ERROR(
					self,
					STREAM,
					DEMUX,
					("Media ended unexpectedly - it may be truncated and invalid"),
					(nullptr)
				);
			}

			gst_dsd_media_parse_stop_pull_mode_loop(self);

			break;
		}

		case GST_FLOW_NOT_ENOUGH_DATA:
			// This should not happen in pull mode, since in pull mode,
			// the pull range calls do not rely on the fill level of the
			// input adapter (and in fact do not use that adapter at all).
			GST_ERROR_OBJECT(
				self,
				"got unexpected not-enough-data flow return value while in pull mode"
			);
			gst_dsd_media_parse_stop_pull_mode_loop(self);
			break;

		default:
			GST_ERROR_OBJECT(
				self,
				"aborting data processing due to flow error %s",
				gst_flow_get_name(flow_ret)
			);
			gst_dsd_media_parse_stop_pull_mode_loop(self);
			break;
	}
}


static gboolean gst_dsd_media_parse_start_pull_mode_loop(GstDsdMediaParse *self)
{
	GstDsdMediaParsePrivate *priv = get_private(self);
	GST_DEBUG_OBJECT(self, "(re)starting pull mode loop by starting pull mode dataflow pad task");
	return gst_pad_start_task(priv->sinkpad, gst_dsd_media_parse_pull_mode_loop, self, nullptr);
}


static void gst_dsd_media_parse_stop_pull_mode_loop(GstDsdMediaParse *self)
{
	GstDsdMediaParsePrivate *priv = get_private(self);
	GST_DEBUG_OBJECT(self, "stopping pull mode loop by pausing pull mode dataflow pad task");
	gst_pad_pause_task(priv->sinkpad);
}


static void gst_dsd_media_parse_reset_all_fields(GstDsdMediaParse *self)
{
	// Write operations do not require mutex locks here,
	// because this function is never called during streaming.

	GstDsdMediaParsePrivate *priv = get_private(self);

	gst_adapter_clear(priv->input_adapter);
	priv->byte_position = 0;
	priv->sinkpad_mode = GST_PAD_MODE_PUSH;
	set_parse_stage(priv, ParseStage::ScanningInfo);
	priv->upstream_is_seekable = std::nullopt;
	gst_segment_init(&(priv->downstream_segment), GST_FORMAT_UNDEFINED);
	priv->current_seqnum = GST_SEQNUM_INVALID;
	priv->pending_seek_position = 0;
	priv->pending_seek_index = 0;
	priv->pending_segment_seqnum = GST_SEQNUM_INVALID;
	priv->pending_seek_is_accurate = false;
	gst_segment_init(&(priv->pending_segment), GST_FORMAT_UNDEFINED);
	priv->pending_bootstrapping = false;
	priv->internal_seek_seqnum = std::nullopt;
	priv->flushing = false;
	priv->reset_running_time_on_next_segment = false;
	priv->expecting_upstream_segment = false;
	priv->stream_flow_error = GST_FLOW_OK;
	priv->payload_position = std::nullopt;
	priv->payload_size = 0;
	priv->end_payload_position = 0;
	gst_caps_replace(&(priv->output_caps), nullptr);
	priv->duration = GST_CLOCK_TIME_NONE;
	priv->next_buffer_is_discont = true;
	gst_tag_list_replace(&(priv->output_tag_list), nullptr);
	gst_tag_list_replace(&(priv->upstream_tag_list), nullptr);
}


static bool gst_dsd_media_parse_check_if_pull_mode_possible(GstDsdMediaParse *self, GstPad *pad)
{
	GstQuery *query = gst_query_new_scheduling();

	if (!gst_pad_peer_query(pad, query)) {
		GST_DEBUG_OBJECT(self, "scheduling query failed; pull mode is not possible");
		gst_query_unref(query);
		return false;
	}

	bool pull_mode_possible = gst_query_has_scheduling_mode_with_flags(query, GST_PAD_MODE_PULL, GST_SCHEDULING_FLAG_SEEKABLE);
	GST_DEBUG_OBJECT(
		self,
		"scheduling query reports that pull mode is %spossible",
		pull_mode_possible ? "" : "not "
	);

	gst_query_unref (query);

	return pull_mode_possible;
}


static bool gst_dsd_media_parse_check_if_upstream_seekable(GstDsdMediaParse *self)
{
	GstDsdMediaParsePrivate *priv = get_private(self);

	GST_DEBUG_OBJECT(self, "querying whether or not upstream can seek");

	GstQuery *seeking_query = gst_query_new_seeking(GST_FORMAT_BYTES);
	auto query_guard = ScopeGuard([&]() { gst_query_unref(seeking_query); });

	if (!gst_pad_peer_query(priv->sinkpad, seeking_query)) {
		GST_DEBUG_OBJECT(self, "seeking query failed; assuming that seeking is not possible");
		return false;
	}

	gboolean seekable;
	gint64 start, stop;
	gst_query_parse_seeking(seeking_query, nullptr, &seekable, &start, &stop);

	if (!seekable) {
		GST_DEBUG_OBJECT(self, "upstream reports that seeking is not possible");
		return false;
	}

	if (stop == -1) {
		GST_DEBUG_OBJECT(self, "seeking query returned no valid stop value; querying upstream duration");
		if (!gst_pad_peer_query_duration(priv->sinkpad, GST_FORMAT_BYTES, &stop)) {
			GST_DEBUG_OBJECT(self, "duration query failed; assuming that seeking is not possible");
			return false;
		}
	}

	if (start >= stop) {
		GST_WARNING_OBJECT(
			self,
			"start value %" G_GINT64_FORMAT " exceeds stop value %" G_GINT64_FORMAT "; assuming that seeking is not possible",
			start,
			stop
		);
		return false;
	}

	GST_DEBUG_OBJECT(self, "upstream reports that seeking is possible");

	return true;
}


static void gst_dsd_media_parse_handle_sink_segment_event(GstDsdMediaParse *self, GstEvent *event)
{
	GstDsdMediaParseClass *klass = GST_DSD_MEDIA_PARSE_GET_CLASS(self);
	GstDsdMediaParsePrivate *priv = get_private(self);

	bool bootstrapping = false;

	const GstSegment *event_segment;
	gst_event_parse_segment(event, &event_segment);

	// The segment event is never forwarded or otherwise stored.
	// It is always unref'd. Use RAII to ensure this is enforced.
	// Using = instead of & capture in case the event pointer is
	// reused for a new segment event.
	ScopeGuard segment_event_guard([=]() { gst_event_unref(event); });

	// We cannot handle non-bytes segments coming from upstream.
	if (event_segment->format != GST_FORMAT_BYTES) {
		GST_ELEMENT_ERROR(
			self, STREAM, FAILED,
			("Internal data stream error."),
			(
				"upstream sent a segment that is not in the bytes format: %" GST_SEGMENT_FORMAT,
				gpointer(event_segment)
			)
		);

		{
			std::lock_guard<std::mutex> lock(priv->field_mutex);
			gst_dsd_media_parse_stream_failed(priv, GST_FLOW_ERROR);
		}

		return;
	}

	switch (get_parse_stage(priv)) {
		case ParseStage::ScanningInfo: {
			GST_DEBUG_OBJECT(
				self,
				"got segment in ScanningInfo stage; discarding; %" GST_SEGMENT_FORMAT,
				gpointer(event_segment)
			);

			if (G_UNLIKELY(priv->byte_position != event_segment->start)) {
				GST_WARNING_OBJECT(
					self,
					"upstream pushed bytes segment whose start value %" G_GUINT64_FORMAT " "
					"differs from the current parse position %" G_GUINT64_FORMAT "; "
					"using the start value as new parse position",
					event_segment->start,
					priv->byte_position
				);

				priv->byte_position = event_segment->start;
			}

			{
				std::lock_guard<std::mutex> lock(priv->field_mutex);
				priv->expecting_upstream_segment = false;

				// A segment arriving in this stage concludes any internal seek that
				// was in progress - either the seek that skips over the payload, or
				// one the subclass requested through
				// gst_dsd_media_parse_seek_during_scan(). Withdraw the registration
				// so that any subsequent flush is treated as an external one again.
				//
				// This is done unconditionally rather than by matching the segment's
				// seqnum against internal_seek_seqnum. Upstream is not required to
				// stamp the segment with the seqnum of the seek that caused it, so
				// matching here would make correctness depend on upstream behavior
				// that is not otherwise assumed. It is the arrival of a segment in
				// this stage that ends the internal seek, whatever it is stamped with.
				priv->internal_seek_seqnum = std::nullopt;
			}

			return;
		}

		case ParseStage::StreamFailure: {
			// This should never happen - dataflow is ceased once the
			// StreamFailure is reached. But, to be safe, handle
			// this case here by discarding the event.
			GST_ERROR_OBJECT(
				self,
				"got segment in StreamFailure stage; discarding; %" GST_SEGMENT_FORMAT,
				gpointer(event_segment)
			);

			return;
		}

		case ParseStage::AwaitingSegment: {
			if (!gst_dsd_media_check_for_deviated_upstream_seek(self, event_segment))
				return;

			priv->byte_position = event_segment->start;

			gst_adapter_clear(priv->input_adapter);

			guint64 pending_seek_index;

			{
				std::lock_guard<std::mutex> lock(priv->field_mutex);

				bootstrapping = priv->pending_bootstrapping;

				if (bootstrapping) {
					gst_dsd_media_parse_init_downstream_segment(priv);
					priv->pending_bootstrapping = false;
				} else {
					gst_segment_copy_into(&priv->pending_segment, &priv->downstream_segment);
				}

				priv->next_buffer_is_discont = true;
				priv->current_seqnum = priv->pending_segment_seqnum;
				priv->expecting_upstream_segment = false;

				// This segment concludes the internal seek back to the payload that
				// gst_dsd_media_parse_scanning_finished() started (in the
				// bootstrapping case). Withdraw the registration; from here on,
				// flushes arriving at the sinkpad are external ones. See the
				// ScanningInfo branch above for why this is unconditional.
				priv->internal_seek_seqnum = std::nullopt;

				pending_seek_index = priv->pending_seek_index;
			}

			if (klass->current_index_after_seek != nullptr)
				klass->current_index_after_seek(self, pending_seek_index);

			set_parse_stage(priv, ParseStage::Streaming);

			if (bootstrapping) {
				// NOTE: gst_dsd_media_parse_start_streaming() reports errors on its own.
				if (!gst_dsd_media_parse_start_streaming(self)) {
					std::lock_guard<std::mutex> lock(priv->field_mutex);
					gst_dsd_media_parse_stream_failed(priv, GST_FLOW_ERROR);
				}
			} else {
				event = gst_event_new_segment(&(priv->downstream_segment));
				gst_event_set_seqnum(event, priv->current_seqnum);
				if (!gst_pad_push_event(priv->srcpad, event)) {
					GST_ELEMENT_ERROR(
						self,
						STREAM,
						FAILED,
						("Internal data stream error."),
						("failed to push segment event downstream")
					);
					std::lock_guard<std::mutex> lock(priv->field_mutex);
					gst_dsd_media_parse_stream_failed(priv, GST_FLOW_ERROR);
				}
			}

			return;
		}

		case ParseStage::Streaming: {
			// This handles the rare case when upstream pushes a segment on its own.

			GstSegment event_segment_copy;
			gst_event_copy_segment(event, &event_segment_copy);
			priv->current_seqnum = gst_event_get_seqnum(event);

			// Transform the start/stop values from the upstream bytes segment
			// to a downstream times segment. The payload_position must be taken
			// into account; it marks the start of the payload, and thus must
			// correspond to timestamp 0 in the downstream segment. Do this
			// by subtracting payload_position from upstream's start and stop.

			// NOTE: In the sanity checks here, the priv->end_payload_position
			// against start uses <, but against stop, uses <=. That's because
			// the segment start value is inclusive (= the first valid value
			// in the segment), while the stop value is exclusive (= the first
			// value beyond the valid range of the segment).

			// The four out-of-range cases are not equivalent, and split into two
			// groups. Two of them can be substituted with a value that describes
			// reality: a start before the payload still overlaps it - upstream
			// just began earlier - so it clamps to 0, and a stop beyond the
			// payload end is what formats storing metadata behind the payload
			// normally produce, meaning the segment is unbounded as far as the
			// payload is concerned. The remaining two - a start at or past the
			// payload end, and a stop before the payload begins - describe a
			// segment that covers no payload at all. There is no value that could
			// stand in for those, so they are reported as errors instead.
			//
			// (A segment that both starts and stops before the payload lands in
			// the first group on the start check and the second on the stop
			// check; the error wins, which is the desired outcome.)

			if (event_segment_copy.start != guint64(-1)) {
				guint64 bytes_start = event_segment_copy.start;
				if (G_LIKELY(
					(bytes_start >= *(priv->payload_position)) &&
					(bytes_start < priv->end_payload_position)
				)) {
					bytes_start -= *(priv->payload_position);
				} else if (bytes_start < *(priv->payload_position)) {
					GST_WARNING_OBJECT(
						self,
						"event segment start %" G_GUINT64_FORMAT " comes "
						"before payload position %" G_GUINT64_FORMAT
						" - using 0 as start for conversion",
						event_segment_copy.start,
						*(priv->payload_position)
					);
					bytes_start = 0;
				} else {
					// The segment begins at or past the end of the payload, so it
					// contains no payload at all. There is no value that could be
					// substituted here: mapping to 0 would announce the whole
					// payload from the beginning, which is the opposite of what
					// upstream just said, and mapping to the payload end would
					// produce an empty segment. Treat it as an error and stop.

					GST_ELEMENT_ERROR(
						self,
						STREAM,
						FAILED,
						("Internal data stream error."),
						(
							"event segment start %" G_GUINT64_FORMAT " is at or past "
							"the end of the payload position %" G_GUINT64_FORMAT
							" - the segment contains no payload",
							event_segment_copy.start,
							priv->end_payload_position
						)
					);

					{
						std::lock_guard<std::mutex> lock(priv->field_mutex);
						gst_dsd_media_parse_stream_failed(priv, GST_FLOW_ERROR);
					}

					return;
				}

				guint64 start_index = klass->to_index(self, GST_FORMAT_BYTES, bytes_start, ToIndexRoundingMode::RoundingDown);
				event_segment_copy.start = klass->from_index(self, GST_FORMAT_TIME, start_index);
			}

			if (event_segment_copy.stop != guint64(-1)) {
				if (G_LIKELY(
					(event_segment_copy.stop >= *(priv->payload_position)) &&
					(event_segment_copy.stop <= priv->end_payload_position)
				)) {
					// Convert by setting the stop value to one past the stop_index.
					// Using the RoundingUp mode in to_index() since we want the
					// segment stop value, not the start value.
					guint64 stop_index = klass->to_index(
						self,
						GST_FORMAT_BYTES,
						event_segment_copy.stop - *(priv->payload_position),
						ToIndexRoundingMode::RoundingUp
					);
					event_segment_copy.stop = klass->from_index(self, GST_FORMAT_TIME, stop_index);
				} else if (event_segment_copy.stop < *(priv->payload_position)) {
					// Mirror image of the start case above: the segment ends before
					// the payload begins, so it contains no payload. -1 would turn
					// "empty" into "unbounded", which is the opposite of what
					// upstream reported.

					GST_ELEMENT_ERROR(
						self,
						STREAM,
						FAILED,
						("Internal data stream error."),
						(
							"event segment stop %" G_GUINT64_FORMAT " comes "
							"before payload position %" G_GUINT64_FORMAT
							" - the segment contains no payload",
							event_segment_copy.stop,
							*(priv->payload_position)
						)
					);

					{
						std::lock_guard<std::mutex> lock(priv->field_mutex);
						gst_dsd_media_parse_stream_failed(priv, GST_FLOW_ERROR);
					}

					return;
				} else {
					// The segment extends past the end of the payload. This is not
					// an anomaly - it is exactly what upstream reports for formats
					// that store metadata behind the payload. As far as the payload
					// is concerned, the  segment is unbounded. Logged at debug level,
					// since this would otherwise warn on every upstream segment
					// during normal playback.
					GST_DEBUG_OBJECT(
						self,
						"event segment stop %" G_GUINT64_FORMAT " lies past "
						"the end of the payload position %" G_GUINT64_FORMAT
						" - using -1 as segment stop, not using conversion",
						event_segment_copy.stop,
						priv->end_payload_position
					);
					event_segment_copy.stop = -1;
				}
			}

			event_segment_copy.format = GST_FORMAT_TIME;
			event_segment_copy.time = event_segment_copy.position = event_segment_copy.start;
			event_segment_copy.duration = priv->duration;

			if (priv->reset_running_time_on_next_segment) {
				std::lock_guard<std::mutex> lock(priv->field_mutex);

				// A reset_time flush happened; running time restarts at zero.
				event_segment_copy.base = 0;
				priv->reset_running_time_on_next_segment = false;
			} else if (GST_CLOCK_TIME_IS_VALID(priv->downstream_segment.position)) {
				// The old segment position becomes the new segment base. This
				// ensures that running time continues to increase monotonically,
				// even if we seek backwards.
				event_segment_copy.base = gst_segment_to_running_time(
					&(priv->downstream_segment),
					GST_FORMAT_TIME,
					priv->downstream_segment.position
				);

				// TODO: This is perhaps too cautious. But, it is currently
				// unclear how to proceed in such a case. If the position
				// is outside of the segment bounds, it means that value
				// can be anything. Clamping it might hide a deeper problem.
				if (event_segment_copy.base == guint64(-1)) {
					GST_ELEMENT_ERROR(
						self, STREAM, FAILED,
						("Internal data stream error."),
						(
							"computed segment base indicates position %" G_GUINT64_FORMAT
							" is outside of the segment",
							priv->downstream_segment.position
						)
					);

					{
						std::lock_guard<std::mutex> lock(priv->field_mutex);
						gst_dsd_media_parse_stream_failed(priv, GST_FLOW_ERROR);
					}

					return;
				}
			}

			{
				std::lock_guard<std::mutex> lock(priv->field_mutex);
				gst_segment_copy_into(&event_segment_copy, &(priv->downstream_segment));
				priv->expecting_upstream_segment = false;
			}

			event = gst_event_new_segment(&(priv->downstream_segment));
			gst_event_set_seqnum(event, priv->current_seqnum);
			if (!gst_pad_push_event(priv->srcpad, event)) {
				GST_ELEMENT_ERROR(
					self, STREAM, FAILED,
					("Internal data stream error."),
					("could not push downstream segment")
				);

				{
					std::lock_guard<std::mutex> lock(priv->field_mutex);
					gst_dsd_media_parse_stream_failed(priv, GST_FLOW_ERROR);
				}

				return;
			}

			return;
		}
	}
}


static gboolean gst_dsd_media_parse_handle_src_seek_event(GstDsdMediaParse *self, GstEvent *event)
{
	GstDsdMediaParsePrivate *priv = get_private(self);
	GstDsdMediaParseClass *klass = GST_DSD_MEDIA_PARSE_GET_CLASS(self);

	gdouble rate;
	GstFormat format, start_format;
	GstSeekFlags flags;
	GstSeekType start_type, stop_type;
	gint64 start, stop;
	guint32 seqnum = gst_event_get_seqnum(event);
	guint64 payload_position;
	std::optional<guint64> seek_position;
	guint64 seek_index;
	gboolean ret = TRUE;

	gst_event_parse_seek(event, &rate, &format, &flags, &start_type, &start, &stop_type, &stop);
	gst_event_unref(event);

	start_format = format;

	if (!(flags & GST_SEEK_FLAG_FLUSH)) {
		GST_DEBUG_OBJECT(self, "cannot handle non-flushing seeks");
		return FALSE;
	}

	if (flags & GST_SEEK_FLAG_SEGMENT) {
		GST_DEBUG_OBJECT(self, "cannot handle segment seeks");
		return FALSE;
	}

	if (rate != 1.0) {
		GST_DEBUG_OBJECT(self, "cannot handle non-1.0 rates");
		return FALSE;
	}

	switch (format) {
		case GST_FORMAT_BYTES:
		case GST_FORMAT_TIME:
			break;

		default:
			GST_DEBUG_OBJECT(self, "cannot handle seek with format %s", gst_format_get_name(format));
			return FALSE;
	}

	GstSegment seek_segment;

	{
		std::lock_guard<std::mutex> lock(priv->field_mutex);

		if (get_parse_stage(priv) != ParseStage::Streaming) {
			GST_DEBUG_OBJECT(
				self,
				"cannot handle seek events in a non-Streaming stage"
			);
			return FALSE;
		}

		if (!(*priv->upstream_is_seekable)) {
			GST_DEBUG_OBJECT(
				self,
				"cannot respond to seeking query - upstream does not support seeking"
			);
			return FALSE;
		}

		g_assert(GST_CLOCK_TIME_IS_VALID(priv->duration));
	}

	bool accurate_seek = (flags & GST_SEEK_FLAG_ACCURATE);

	// These are procedures that are exactly the same for the push and
	// pull modes. Do these in the lambda here for code deduplication.
	// The field_mutex must be locked before this is called!
	auto common_seek_procedures = [&]() -> bool {
		gst_segment_copy_into(&priv->downstream_segment, &seek_segment);

		g_assert(priv->payload_position.has_value());
		payload_position = *(priv->payload_position);

		// Seeking is done by adjusting the current downstream segment according
		// to the parameters from the seeek event. The start and stop parameters
		// adjust the start and stop segment fields, respectively, based on their
		// seek types.
		//
		// In addition, in push mode, the parser must issue its own upstream seek
		// event, in bytes. Upstream must be directed to seek such that the next
		// gstbuffer's PTS comes before or is right at the start of the segment.
		//
		// However, there is a discrepancy, because the subclass might have
		// a certain position boundary in place that effectively restricts
		// what values the seek position can have. For example, a subclass which
		// operates with payloads that are organized in blocks has a position
		// boundary that requires the seek position to snap to integer multiples
		// of that block size, because otherwise, seeking would produce partial
		// blocks that the subclass cannot process - it expects whole blocks.
		//
		// The presence or absence of the ACCURATE seek flag influences how this
		// is reconciled with the segment fields:
		//
		// * Flag is present: The start parameter of the seek even is snapped to the
		//   position boundary. The segment's start, position, time values are set
		//   to the original, un-snapped start parameter of the seek event. (Additional
		//   offsets are applied, but these are omitted here for clarity, and
		//   gst_segment_do_seek() takes care of that internally.)
		// * Flag is absent: Just like when the flag is present, except that the
		//   segment's start, position, time values are set to the _snapped_ start.
		//
		// There is additional normalization of the start and stop values going on
		// in case their seek types are GST_SEEK_TYPE_END. After the normalization,
		// the values are such that only GST_SEEK_TYPE_SET and GST_SEEK_TYPE_NONE
		// are possible.
		//
		// Furthermore, bytes seek requests are translated to time seek requests.
		//
		// The way this is all implemented is by means of abstract "indices" that
		// denote seek positions that are exactly aligned to the position boundary.
		// The subclass defines what the indices exactly mean. In the earlier
		// block example, the subclass could associate indices with block offsets
		// that are an integer multiple of the block size.

		gint64 max_value;
		if (format == GST_FORMAT_TIME)
			max_value = seek_segment.duration;
		else
			max_value = priv->payload_size;

		std::optional<guint64> normalized_start;
		std::optional<guint64> normalized_stop;

		// Produce a normalized start parameter. If it can't be produced, no upstream
		// seek takes place - the current downstream segment is adjusted, but that's
		// about it. This can happen when the event only aims to modify the stop value
		// of the downstream segment for example.

		// Normalize the start parameters in case it is of seek type END.
		switch (start_type) {
			case GST_SEEK_TYPE_NONE:
				// NONE start type is rare, and handling it in a fully optimal way
				// adds even more complexity. Instead, turn it into a time SET type
				// by using the current segment position as the "start". The price
				// is one redundant seek, which is acceptable.
				normalized_start = seek_segment.position;
				start_format = GST_FORMAT_TIME;
				start_type = GST_SEEK_TYPE_SET;
				break;

			case GST_SEEK_TYPE_SET:
				// A start value of -1 means "the beginning", which here, equals
				// start value 0, so map it accordingly.
				normalized_start = (start == -1) ? 0 : std::min(std::max(start, gint64(0)), max_value);
				break;

			case GST_SEEK_TYPE_END: {
				// GST_SEEK_TYPE_END parameters are relative to the end of the
				// media and are typically (but not necessarily) negative.

				gint64 resolved_offset = max_value + start;

				// Clamp, since the result may lie beyond either end of the payload.
				normalized_start = std::min(std::max(resolved_offset, gint64(0)), max_value);

				start_type = GST_SEEK_TYPE_SET;

				GST_DEBUG_OBJECT(
					self,
					"normalized seek start parameter %" G_GINT64_FORMAT " of type END "
					"to seek start parameter %" G_GUINT64_FORMAT " of type SET",
					start,
					*normalized_start
				);

				break;
			}

			default:
				GST_ERROR_OBJECT(self, "invalid start seek type %d reached", gint(start_type));
				g_assert_not_reached();
				return false;
		}

		g_assert(normalized_start.has_value());

		// Find the index that corresponds to the normalized start parameter.
		// This index will be used for both the seek start parameter(if the
		// accurate flag isn't set) and for upstream seeking.
		seek_index = klass->to_index(self, start_format, *normalized_start, ToIndexRoundingMode::RoundingDown);
		GST_DEBUG_OBJECT(
			self,
			"got index %" G_GUINT64_FORMAT " for normalized seek start parameter %" G_GINT64_FORMAT,
			seek_index,
			*normalized_start
		);

		// In the TIME format case, accurate seek is trivially possible
		// by setting the segment start directly to the normalized start.
		// If however the format is BYTES, we have to resort to converting
		// the index back, since then, the segment boundary must correspond
		// to the position boundary. There is no way to be more exact then,
		// since the payload may be compressed, which does not permit
		// any simple linear bytes <-> time conversions.
		// And, in case of non-accurate seek, it is anyway sufficient
		// to snap to the position boundary
		if (accurate_seek && (start_format == GST_FORMAT_TIME)) {
			start = *normalized_start;
		} else {
			start = klass->from_index(self, GST_FORMAT_TIME, seek_index);
		}

		// Apply normalization etc. to the stop parameter, similar to
		// how the start parameter was just processed. Unlike the start
		// parameter though, this one is never snapped to the position
		// boundary. Unlike start, it is not a seek target - it is an
		// endpoint against which downstream clips samples that were
		// already produced. Snapping it would needlessly discard audio.
		// The accurate flag has no meaning for the stop parameter; it is
		// adjusted just like how the start one is when that flag is set.

		// Normalize the start parameters in case it is of seek type END.
		switch (stop_type) {
			case GST_SEEK_TYPE_SET:
				if (stop != -1)
					normalized_stop = std::min(std::max(stop, gint64(0)), max_value);
				break;

			case GST_SEEK_TYPE_END: {
				// GST_SEEK_TYPE_END parameters are relative to the end of the
				// media and are typically (but not necessarily) negative.

				gint64 resolved_offset = max_value + stop;

				// Clamp, since the result may lie beyond either end of the payload.
				normalized_stop = std::min(std::max(resolved_offset, gint64(0)), max_value);

				stop_type = GST_SEEK_TYPE_SET;

				GST_DEBUG_OBJECT(
					self,
					"normalized seek stop parameter %" G_GINT64_FORMAT " of type END "
					"to seek stop parameter %" G_GUINT64_FORMAT " of type SET",
					stop,
					*normalized_stop
				);

				break;
			}

			default:
				break;
		}

		if (normalized_stop.has_value()) {
			if (format == GST_FORMAT_TIME) {
				stop = *normalized_stop;
			} else {
				// Snap to position boundaries if the normalized stop is given
				// in bytes. There is no way to be more accurate, since the
				// payload may be compressed, which does not permit any simple
				// linear bytes <-> time conversions.
				//
				// Using the RoundingUp mode in to_index() since we want the
				// segment stop value, not the start value.
				guint64 index = klass->to_index(self, GST_FORMAT_BYTES, *normalized_stop, ToIndexRoundingMode::RoundingUp);
				stop = klass->from_index(self, GST_FORMAT_TIME, index);
			}
		}

		// At this stage, the start and stop parameters, if their original seek
		// types were GST_SEEK_TYPE_END, are normalized to be as if the types
		// were GST_SEEK_TYPE_SET. The start parameter is snapped to the position
		// boundary in case of an absent ACCURATE seek flag. And, both start and
		// stop are in the times format (converted in case they originally were
		// in the bytes format).

		// NOTE: start and stop are deliberately not clamped against the
		// duration here. gst_segment_do_seek() already clamps the
		// corresponding segment fields both against seek_segment.duration,
		// which is filled in by gst_dsd_media_parse_init_downstream_segment().

		gboolean update;
		if (!gst_segment_do_seek(
			&seek_segment,
			rate,
			GST_FORMAT_TIME,
			flags,
			start_type,
			start,
			stop_type,
			stop,
			&update
		)) {
			GST_DEBUG_OBJECT(self, "could not update segment for seek operation");
			return false;
		}

		// There is no need for this flag anymore. gst_segment_do_seek()
		// already sets base to zero if the flushing seek flag is set.
		priv->reset_running_time_on_next_segment = false;

		seek_position = klass->from_index(self, GST_FORMAT_BYTES, seek_index) + payload_position;
		GST_DEBUG_OBJECT(
			self,
			"produced seek position %" G_GUINT64_FORMAT " out of start index %" G_GUINT64_FORMAT " "
			"and payload position %" G_GUINT64_FORMAT,
			*seek_position,
			seek_index,
			payload_position
		);

		return true;
	};

	if (priv->sinkpad_mode == GST_PAD_MODE_PULL) {
		// In pull mode, the parser must flush both up- and downstream
		// to unlock them. Otherwise, a blocking pull_range() call (which
		// may currently be blocking the pull mode loop here) will never
		// unblock for example. Also, it must pause and unpause the sinkpad
		// task to avoid race conditions. Only then can seeking be done
		// safely.

		bool flushed_upstream = false, flushed_downstream = false;
		bool stream_is_locked = false;

		GstSegment segment_to_push;
		gst_segment_init(&segment_to_push, GST_FORMAT_UNDEFINED);

		// Begin the seek by by sending flush-start to up- and downstream
		// to flush their current data and unlock them. Then, the sinkpad
		// task must be paused, and the stream lock must be taken to wait
		// for the sinkpad task to fully pause. (It also helps to ensure
		// memory visibility when fields are modified in this thread that
		// are later accessed in the streaming thread.)
		//
		// Once flushing is done, the seek_guard scope guard uses RAII
		// to ensure that the flush is finished by sending flush-stop
		// to both up- and downstream, the sinkpad task is restartd,
		// and the stream lock is released (in that order).

		ScopeGuard seek_guard([&]() {
			GstEvent *flush_stop_event = gst_event_new_flush_stop(TRUE);

			gst_event_set_seqnum(flush_stop_event, seqnum);

			bool upstream_not_flushed = true;
			bool downstreamstream_not_flushed = true;

			if (flushed_upstream) {
				upstream_not_flushed = gst_pad_push_event(priv->sinkpad, gst_event_ref(flush_stop_event));
				if (!upstream_not_flushed)
					GST_ERROR_OBJECT(self, "could not push flush-stop event upstream");
			}

			if (flushed_downstream) {
				downstreamstream_not_flushed = gst_pad_push_event(priv->srcpad, gst_event_ref(flush_stop_event));
				if (!downstreamstream_not_flushed)
					GST_ERROR_OBJECT(self, "could not push flush-stop event downstream");
			}

			gst_event_unref(flush_stop_event);

			if (!upstream_not_flushed || !downstreamstream_not_flushed)
				ret = FALSE;

			if (segment_to_push.format != GST_FORMAT_UNDEFINED) {
				GstEvent *segment_event = gst_event_new_segment(&segment_to_push);
				gst_event_set_seqnum(segment_event, seqnum);
				if (!gst_pad_push_event(priv->srcpad, segment_event)) {
					GST_ERROR_OBJECT(self, "could not push segment event downstream");
					ret = FALSE;
				}
			}

			if (!gst_dsd_media_parse_start_pull_mode_loop(self))
				ret = FALSE;

			if (stream_is_locked) {
				stream_is_locked = false;
				GST_PAD_STREAM_UNLOCK(priv->sinkpad);
			}
		});

		GstEvent *flush_start_event = gst_event_new_flush_start();
		gst_event_set_seqnum(flush_start_event, seqnum);
		flushed_upstream = gst_pad_push_event(priv->sinkpad, gst_event_ref(flush_start_event));
		if (!flushed_upstream)
			GST_ERROR_OBJECT(self, "could not push flush-start event upstream");
		flushed_downstream = gst_pad_push_event(priv->srcpad, gst_event_ref(flush_start_event));
		if (!flushed_downstream)
			GST_ERROR_OBJECT(self, "could not push flush-start event downstream");
		gst_event_unref(flush_start_event);

		if (!flushed_upstream || !flushed_downstream)
			return FALSE;

		gst_dsd_media_parse_stop_pull_mode_loop(self);

		GST_PAD_STREAM_LOCK(priv->sinkpad);
		stream_is_locked = true;

		// Seeking can be done safely now.

		{
			std::lock_guard<std::mutex> lock(priv->field_mutex);

			if (!common_seek_procedures())
				return FALSE;

			priv->byte_position = *seek_position;
			if (klass->current_index_after_seek != nullptr)
				klass->current_index_after_seek(self, seek_index);

			gst_segment_copy_into(&seek_segment, &priv->downstream_segment);
			gst_segment_copy_into(&seek_segment, &segment_to_push);

			priv->next_buffer_is_discont = true;
			priv->current_seqnum = seqnum;
		}
	} else {
		// In push mode, the parser does not have to flush up- and downstream,
		// since some other element drives the dataflow (usually the topmost
		// source element) - that element takes care of these flush events.
		// However, the parser must send a seek event upstream, since unlike
		// in pull mode, random upstream access is not possible in push mode.
		//
		// Also, in push mode, the downstream segment is pushed later, when
		// upstream delivers its own segment. See the event_segment handler.

		{
			std::lock_guard<std::mutex> lock(priv->field_mutex);

			if (!common_seek_procedures())
				return FALSE;

			gst_segment_copy_into(&seek_segment, &(priv->pending_segment));

			priv->pending_seek_position  = *seek_position;
			priv->pending_seek_index     = seek_index;
			priv->pending_segment_seqnum = seqnum;
			priv->pending_bootstrapping  = false;
			priv->pending_seek_is_accurate = accurate_seek;

			// It is theoretically possible that between the parse_stage
			// check above and the code here, the stage was changed
			// to StreamFailure. This would silently be overwritten
			// if set_parse_stage() were used here. Check to be sure.
			ParseStage expected = ParseStage::Streaming;
			if (!priv->parse_stage.compare_exchange_strong(
				expected,
				ParseStage::AwaitingSegment,
				std::memory_order_acq_rel,
				std::memory_order_acquire
			)) {
				GST_ERROR_OBJECT(self, "expected the Streaming state to still be active");
				return FALSE;
			}
		}

		GstEvent *upstream_seek_event = gst_event_new_seek(
			1.0,
			GST_FORMAT_BYTES,
			GstSeekFlags(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE),
			GST_SEEK_TYPE_SET, *seek_position,
			GST_SEEK_TYPE_NONE, -1
		);
		gst_event_set_seqnum(upstream_seek_event, seqnum);
		if (!gst_pad_push_event(priv->sinkpad, upstream_seek_event)) {
			// There is no graceful exit out of this situation. Especially
			// if we are bootstrapping, the seek must succeed. Report an
			// error and consider the stream start to be failed.

			GST_ELEMENT_ERROR(
				self,
				STREAM,
				FAILED,
				("Internal data stream error."),
				("failed to push seek event upstream")
			);

			std::lock_guard<std::mutex> lock(priv->field_mutex);
			gst_dsd_media_parse_stream_failed(priv, GST_FLOW_ERROR);

			return FALSE;
		}
	}

	return ret;
}


static bool gst_dsd_media_parse_push_initial_events(GstDsdMediaParse *self)
{
	GstDsdMediaParseClass *klass = GST_DSD_MEDIA_PARSE_GET_CLASS(self);
	GstDsdMediaParsePrivate *priv = get_private(self);

	if G_UNLIKELY(!GST_CLOCK_TIME_IS_VALID(priv->duration)) {
		GST_ELEMENT_ERROR(
			self,
			STREAM,
			FAILED,
			("Parser did not set duration."),
			(nullptr)
		);
		return false;
	}

	if (G_UNLIKELY(priv->output_caps == nullptr)) {
		GST_ELEMENT_ERROR(
			self,
			STREAM,
			FAILED,
			("Parser did not set output caps."),
			(nullptr)
		);
		return false;
	}

	GstTagList *tag_list = gst_tag_list_new(
		GST_TAG_DURATION, priv->duration,
		nullptr
	);

	if (klass->fill_tags != nullptr) {
		tag_list = klass->fill_tags(self, tag_list);
		if (G_UNLIKELY(tag_list == nullptr)) {
			GST_ELEMENT_ERROR(
				self,
				STREAM,
				FAILED,
				("Parser returned no tags."),
				(nullptr)
			);
			return false;
		}
	}

	if (priv->upstream_tag_list != nullptr) {
		// If upstream provided tags, merge them with the tags
		// the subclass reported. Prefer the tags of the subclass
		// (hence the GST_TAG_MERGE_KEEP mode).

		GstTagList *merged_tag_list = gst_tag_list_merge(
			tag_list,
			priv->upstream_tag_list,
			GST_TAG_MERGE_KEEP
		);

		gst_tag_list_take(&(priv->output_tag_list), merged_tag_list);

		gst_tag_list_unref(tag_list);
		gst_tag_list_replace(&(priv->upstream_tag_list), nullptr);
	} else {
		gst_tag_list_take(&(priv->output_tag_list), tag_list);
	}

	GstEvent *event;

	if (priv->sinkpad_mode == GST_PAD_MODE_PULL) {
		gchar *stream_id = gst_pad_create_stream_id(
			priv->srcpad,
			GST_ELEMENT_CAST(self),
			nullptr
		);
		event = gst_event_new_stream_start(stream_id);
		g_free(stream_id);

		priv->current_seqnum = gst_event_get_seqnum(event);

		gst_event_set_group_id(event, gst_util_group_id_next());
		if (!gst_pad_push_event(priv->srcpad, event)) {
			GST_ELEMENT_ERROR(
				self, CORE, EVENT,
				("Internal data stream error."),
				("failed to push stream-start event downstream")
			);
			return false;
		}
	}

	event = gst_event_new_caps(priv->output_caps);
	gst_event_set_seqnum(event, priv->current_seqnum);
	if (!gst_pad_push_event(priv->srcpad, event)) {
		GST_ELEMENT_ERROR(
			self, CORE, NEGOTIATION,
			("Internal data stream error."),
			("downstream did not accept the caps %" GST_PTR_FORMAT, gpointer(priv->output_caps))
		);
		return false;
	}

	event = gst_event_new_segment(&(priv->downstream_segment));
	gst_event_set_seqnum(event, priv->current_seqnum);
	if (!gst_pad_push_event(priv->srcpad, event)) {
		GST_ELEMENT_ERROR(
			self, CORE, EVENT,
			("Internal data stream error."),
			("failed to push segment event downstream")
		);
		return false;
	}

	event = gst_event_new_tag(gst_tag_list_ref(priv->output_tag_list));
	gst_event_set_seqnum(event, priv->current_seqnum);
	if (!gst_pad_push_event(priv->srcpad, event)) {
		// Not treated as an error. Tags are informational, so failing to deliver
		// them does not prevent the medium from being played.
		GST_WARNING_OBJECT(self, "failed to push tag event downstream");
	}

	return true;
}


static bool gst_dsd_media_parse_start_streaming(GstDsdMediaParse *self)
{
	GstDsdMediaParseClass *klass = GST_DSD_MEDIA_PARSE_GET_CLASS(self);

	if (!gst_dsd_media_parse_push_initial_events(self))
		return false;

	if ((klass->streaming_started != nullptr) && !klass->streaming_started(self)) {
		GST_ELEMENT_ERROR(
			self, STREAM, FAILED,
			("Internal data stream error."),
			("subclass %s could not start streaming", G_OBJECT_TYPE_NAME(self))
		);
		return false;
	}

	return true;
}


static void gst_dsd_media_parse_report_missing_buffer_timing(GstDsdMediaParse *self, GstBuffer *output_buffer)
{
	GST_ELEMENT_ERROR(
		self, STREAM, FAILED,
		("Internal data stream error."),
		(
			"subclass %s did not set output buffer fields: PTS missing: %d; duration missing: %d",
			G_OBJECT_TYPE_NAME(self),
			!GST_BUFFER_PTS_IS_VALID(output_buffer),
			!GST_BUFFER_DURATION_IS_VALID(output_buffer)
		)
	);
}


static bool gst_dsd_media_parse_verify_advance(GstDsdMediaParse *self, guint64 advance_amount, const gchar *advance_name)
{
	GstDsdMediaParseClass *klass = GST_DSD_MEDIA_PARSE_GET_CLASS(self);
	GstDsdMediaParsePrivate *priv = get_private(self);
	return klass->verify_advance(self, priv->byte_position, advance_amount, advance_name);
}


static bool gst_dsd_media_check_for_deviated_upstream_seek(GstDsdMediaParse *self, const GstSegment *event_segment)
{
	GstDsdMediaParseClass *klass = GST_DSD_MEDIA_PARSE_GET_CLASS(self);
	GstDsdMediaParsePrivate *priv = get_private(self);

	// Check for a deviated seek position. These are rare,
	// but if they happen, they can be a fatal error.

	if (G_LIKELY(event_segment->start == priv->pending_seek_position))
		return true;

	GST_WARNING_OBJECT(
		self,
		"expected seek to move to byte position %" G_GUINT64_FORMAT ", but it actually moved to %" G_GUINT64_FORMAT,
		priv->pending_seek_position,
		event_segment->start
	);

	// If the seek landed outside of the payload, it is non-recoverable error.
	if ((event_segment->start < *(priv->payload_position)) || (event_segment->start >= (*(priv->payload_position) + priv->payload_size))) {
		GST_ELEMENT_ERROR(
			self,
			STREAM,
			FAILED,
			("Internal data stream error."),
			(
				"upstream seek moved to unexpected position %" G_GUINT64_FORMAT " that is outside of the payload",
				event_segment->start
			)
		);

		std::lock_guard<std::mutex> lock(priv->field_mutex);
		gst_dsd_media_parse_stream_failed(priv, GST_FLOW_ERROR);

		return false;
	}

	// A deviated seek also means that the pending seek index is no
	// longer correct and needs to be updated to reflect the true index.
	priv->pending_seek_index = klass->to_index(
		self,
		GST_FORMAT_BYTES,
		event_segment->start - *(priv->payload_position),
		ToIndexRoundingMode::RoundingDown
	);
	GST_DEBUG_OBJECT(
		self,
		"updating pending seek index to %" G_GUINT64_FORMAT " due to deviated seek",
		priv->pending_seek_index
	);

	// Try if the subclass can rescue this. If not, report this as an error.
	if ((klass->resync_deviated_upstream_seek == nullptr) || !klass->resync_deviated_upstream_seek(self, priv->pending_seek_position, event_segment->start)) {
		GST_ELEMENT_ERROR(
			self,
			STREAM,
			FAILED,
			("Internal data stream error."),
			(
				"upstream seek moved to unexpected position %" G_GUINT64_FORMAT " and subclass could not resynchronize",
				event_segment->start
			)
		);

		std::lock_guard<std::mutex> lock(priv->field_mutex);
		gst_dsd_media_parse_stream_failed(priv, GST_FLOW_ERROR);

		return false;
	}

	// The subclass resynchronized, so playback can continue - but it continues
	// from a different position than the one the seek asked for. What that means
	// for the pending segment depends on whether the seek was an accurate one.
	//
	// * Non-accurate seek: the application only asked to get "somewhere near"
	//   the requested position, and is expected to consult the segment to find
	//   out where playback actually resumed. Landing at a different index is
	//   therefore within contract. Rewrite the segment to report the position
	//   that was really reached; the first buffer's PTS then matches the
	//   segment start exactly, so there is neither clipping nor a gap, and
	//   position queries report the truth.
	//
	// * Accurate seek: the segment start is a promise that was made to the
	//   application, so it is left untouched. The two deviation directions
	//   then resolve differently, and both correctly:
	//   - Deviated backwards: the subclass, having been told the real index
	//     through current_index_after_seek(), produces buffers whose PTS lie
	//     before the segment start. Downstream clips them against the segment,
	//     so the accurate seek is honored exactly despite upstream's miss.
	//     This only works because the segment was left alone.
	//   - Deviated forwards: the data before the requested position is gone,
	//     and nothing can recover it. The resulting gap between the segment
	//     start and the first PTS is the honest signal that the request could
	//     not be met. Rewriting the start would instead silently misreport an
	//     accurate seek as having succeeded somewhere else.
	//
	// The bootstrapping case is excluded because it does not use pending_segment
	// at all - gst_dsd_media_parse_handle_sink_segment_event() builds a fresh
	// segment with gst_dsd_media_parse_init_downstream_segment() instead.

	bool degenerate_segment = false;
	guint64 actual_start = 0;

	{
		std::lock_guard<std::mutex> lock(priv->field_mutex);

		if (!priv->pending_bootstrapping && !priv->pending_seek_is_accurate) {
			actual_start = klass->from_index(self, GST_FORMAT_TIME, priv->pending_seek_index);

			if (G_UNLIKELY((priv->pending_segment.stop != guint64(-1)) &&
			               (actual_start >= priv->pending_segment.stop))) {
				// The deviation pushed the start at or past the segment's stop
				// value. Rewriting would produce an empty segment, so treat this
				// like any other deviation that cannot be recovered from.
				degenerate_segment = true;
			} else {
				GST_DEBUG_OBJECT(
					self,
					"seek was not accurate; rewriting pending segment start from %" G_GUINT64_FORMAT
					" to %" G_GUINT64_FORMAT " to reflect the deviated seek",
					priv->pending_segment.start,
					actual_start
				);

				priv->pending_segment.start    = actual_start;
				priv->pending_segment.time     = actual_start;
				priv->pending_segment.position = actual_start;
			}
		}
	}

	if (G_UNLIKELY(degenerate_segment)) {
		GST_ELEMENT_ERROR(
			self,
			STREAM,
			FAILED,
			("Internal data stream error."),
			(
				"deviated seek moved the segment start to %" G_GUINT64_FORMAT
				", which is at or past the segment stop value; the segment would be empty",
				actual_start
			)
		);

		std::lock_guard<std::mutex> lock(priv->field_mutex);
		gst_dsd_media_parse_stream_failed(priv, GST_FLOW_ERROR);

		return false;
	}

	return true;
}


static void gst_dsd_media_parse_init_downstream_segment(GstDsdMediaParsePrivate *priv)
{
	g_assert(GST_CLOCK_TIME_IS_VALID(priv->duration));
	gst_segment_init(&priv->downstream_segment, GST_FORMAT_TIME);
	priv->downstream_segment.duration = priv->duration;
	priv->downstream_segment.stop = -1;
}


static void gst_dsd_media_parse_stream_failed(GstDsdMediaParsePrivate *priv, GstFlowReturn flow_error)
{
	// NOTE: This function deliberately does _not_ post an error. Callers must have
	// reported the error themselves before calling this - they know why the failure
	// occurred, this function does not.
	priv->stream_flow_error = flow_error;
	set_parse_stage(priv, ParseStage::StreamFailure);
}


guint64 gst_dsd_media_parse_get_payload_size(GstDsdMediaParse *parse)
{
	g_assert(GST_IS_DSD_MEDIA_PARSE(parse));
	GstDsdMediaParsePrivate *priv = get_private(parse);
	return priv->payload_size;
}


GstClockTime gst_dsd_media_parse_get_duration(GstDsdMediaParse *parse)
{
	g_assert(GST_IS_DSD_MEDIA_PARSE(parse));
	GstDsdMediaParsePrivate *priv = get_private(parse);
	return priv->duration;
}


bool gst_dsd_media_parse_get_upstream_size(GstDsdMediaParse *parse, guint64 *upstream_size)
{
	g_assert(GST_IS_DSD_MEDIA_PARSE(parse));
	g_return_val_if_fail(upstream_size != nullptr, false);

	GstDsdMediaParsePrivate *priv = get_private(parse);

	GstQuery *duration_query = gst_query_new_duration(GST_FORMAT_BYTES);
	ScopeGuard guard([&]() { gst_query_unref(duration_query); });

	if (!gst_pad_peer_query(priv->sinkpad, duration_query)) {
		GST_ELEMENT_ERROR(
			parse,
			STREAM,
			FAILED,
			("Internal dataflow error."),
			("upstream bytes duration query failed; cannot get size of media")
		);
		return false;
	}

	GstFormat format;
	gint64 duration;
	gst_query_parse_duration(duration_query, &format, &duration);

	if (G_UNLIKELY(format != GST_FORMAT_BYTES)) {
		GST_ELEMENT_ERROR(
			parse,
			STREAM,
			FAILED,
			("Internal dataflow error."),
			("upstream duration query should have bytes format, but instead has format %s",
			gst_format_get_name(format))
		);
		return false;
	}

	if (G_UNLIKELY(duration < 0)) {
		GST_ELEMENT_ERROR(
			parse,
			STREAM,
			FAILED,
			("Internal dataflow error."),
			("a negative upstream duration %" G_GINT64_FORMAT " is invalid", duration)
		);
		return false;
	}

	*upstream_size = duration;

	return true;
}


GstFlowReturn gst_dsd_media_parse_read_data_during_scan(GstDsdMediaParse *parse, guint64 num_bytes_to_read, GstBuffer **data)
{
	g_assert(GST_IS_DSD_MEDIA_PARSE(parse));
	g_return_val_if_fail(num_bytes_to_read > 0, GST_FLOW_ERROR);
	g_return_val_if_fail(data != nullptr, GST_FLOW_ERROR);

	GstDsdMediaParsePrivate *priv = get_private(parse);

	g_return_val_if_fail(get_parse_stage(priv) == ParseStage::ScanningInfo, GST_FLOW_ERROR);

	switch (priv->sinkpad_mode) {
		case GST_PAD_MODE_PUSH: {
			// In push mode, we have to rely on the contents of input_adapter,
			// since it is not possible to get an exact amount of data from
			// upstream (it decides on its own how much data to deliver).
			// If there is not enough data in it yet, we must exit, and report
			// this to the caller so it tries again later.

			guint64 num_available_bytes = gst_adapter_available(priv->input_adapter);

			if (num_available_bytes < num_bytes_to_read) {
				GST_LOG_OBJECT(
					parse,
					"cannot read %" G_GUINT64_FORMAT " byte(s) since only %" G_GUINT64_FORMAT " are currently available",
					num_bytes_to_read,
					num_available_bytes
				);
				return GST_FLOW_NOT_ENOUGH_DATA;
			} else {
				if (!gst_dsd_media_parse_verify_advance(parse, num_bytes_to_read, "read"))
					return GST_FLOW_ADVANCE_OUT_OF_BOUNDS;

				*data = gst_adapter_take_buffer(priv->input_adapter, num_bytes_to_read);
				GST_LOG_OBJECT(
					parse,
					"read %" G_GUINT64_FORMAT " byte(s) from the adapter",
					num_bytes_to_read
				);
			}

			break;
		}

		case GST_PAD_MODE_PULL: {
			// Since in pull mode, random access from upstream is
			// possible, the input_adapter is not used at all here.

			if (!gst_dsd_media_parse_verify_advance(parse, num_bytes_to_read, "read"))
				return GST_FLOW_ADVANCE_OUT_OF_BOUNDS;

			GstFlowReturn flow_ret;

			*data = nullptr;

			flow_ret = gst_pad_pull_range(
				priv->sinkpad,
				priv->byte_position,
				num_bytes_to_read,
				data
			);

			switch (flow_ret) {
				case GST_FLOW_OK: {
					guint64 actual_num_bytes_read = gst_buffer_get_size(*data);

					if (G_UNLIKELY(actual_num_bytes_read != num_bytes_to_read)) {
						GST_ERROR_OBJECT(
							parse,
							"requested %" G_GUINT64_FORMAT " byte(s) from upstream, got %" G_GUINT64_FORMAT "; "
							"this should not happen in pull mode - data may be truncated and thus invalid; "
							"discarding and reporting this as EOS",
							num_bytes_to_read,
							actual_num_bytes_read
						);
						gst_buffer_unref(*data);
						return GST_FLOW_EOS;
					}

					break;
				}
				case GST_FLOW_EOS:
					GST_DEBUG_OBJECT(parse, "reached EOS in pull mode");
					return GST_FLOW_EOS;
				default:
					GST_ERROR_OBJECT(parse, "could not pull from upstream: %s", gst_flow_get_name(flow_ret));
					return flow_ret;
			}

			break;
		}

		default:
			GST_ERROR_OBJECT(parse, "invalid sinkpad scheduling mode %d reached", gint(priv->sinkpad_mode));
			g_assert_not_reached();
			return GST_FLOW_ERROR;
	}

	gsize output_size = gst_buffer_get_size(*data);

	GST_LOG_OBJECT(
		parse,
		"got %" G_GSIZE_FORMAT " byte(s) from upstream position %" G_GUINT64_FORMAT,
		output_size,
		priv->byte_position
	);

	priv->byte_position += output_size;

	return GST_FLOW_OK;
}


GstFlowReturn gst_dsd_media_parse_skip_data_during_scan(GstDsdMediaParse *parse, guint64 num_bytes_to_skip, guint64 *num_actually_skipped_bytes)
{
	return gst_dsd_media_parse_skip_data_during_scan_full(parse, num_bytes_to_skip, num_actually_skipped_bytes, false);
}


static GstFlowReturn gst_dsd_media_parse_skip_data_during_scan_full(GstDsdMediaParse *self, guint64 num_bytes_to_skip, guint64 *num_actually_skipped_bytes, bool force_seek)
{
	g_assert(GST_IS_DSD_MEDIA_PARSE(self));
	g_return_val_if_fail(num_bytes_to_skip > 0, GST_FLOW_ERROR);
	g_return_val_if_fail(num_actually_skipped_bytes != nullptr, GST_FLOW_ERROR);

	GstDsdMediaParsePrivate *priv = get_private(self);	

	g_return_val_if_fail(get_parse_stage(priv) == ParseStage::ScanningInfo, GST_FLOW_ERROR);

	switch (priv->sinkpad_mode) {
		case GST_PAD_MODE_PUSH: {
			// In push mode, skip by flushing the adapter if the skip operation
			// only gets rid of the oldest N bytes from the adapter. No seeking
			// is required then.
			//
			// If the skip amount is larger than the current amount of bytes in
			// the adapter, but still rather small, just clear the adapter and
			// report that fewer bytes were skipped than requested (the caller
			// then has to try again later to skip the remaining bytes). This
			// is an optimization, since an upstream seek operation might be
			// expensive.
			//
			// If the skip amount is larger than a given threshold, do use
			// an upstream seek operation to not have to wait for upstream
			// to push tons of GstBuffers. This is only done if upstream seeking
			// is actually possible. If not, the adapter is just cleared, like
			// in the aforementioned case.

			guint64 num_available_bytes = gst_adapter_available(priv->input_adapter);

			if (num_bytes_to_skip <= num_available_bytes) {
				*num_actually_skipped_bytes = num_bytes_to_skip;

				if (!gst_dsd_media_parse_verify_advance(self, *num_actually_skipped_bytes, "skip"))
					return GST_FLOW_ADVANCE_OUT_OF_BOUNDS;

				GST_LOG_OBJECT(
					self,
					"skipping %" G_GUINT64_FORMAT " byte(s) by flushing those bytes from the input adapter",
					num_bytes_to_skip
				);

				gst_adapter_flush(priv->input_adapter, num_bytes_to_skip);

				priv->byte_position += *num_actually_skipped_bytes;
			} else {
				g_assert(priv->upstream_is_seekable.has_value());

				static const guint64 UPSTREAM_SEEK_THRESHOLD = 1024;

				if (
					(!force_seek && (num_bytes_to_skip < UPSTREAM_SEEK_THRESHOLD)) ||
					!(*(priv->upstream_is_seekable))
				) {
					*num_actually_skipped_bytes = num_available_bytes;

					if (num_available_bytes > 0) {
						if (!gst_dsd_media_parse_verify_advance(self, *num_actually_skipped_bytes, "skip"))
							return GST_FLOW_ADVANCE_OUT_OF_BOUNDS;

						GST_LOG_OBJECT(
							self,
							"partially skipping %" G_GUINT64_FORMAT " byte(s) by clearing all %" G_GUINT64_FORMAT " bytes from the input adapter",
							num_bytes_to_skip,
							num_available_bytes
						);

						gst_adapter_clear(priv->input_adapter);

						priv->byte_position += *num_actually_skipped_bytes;
					} else {
						GST_LOG_OBJECT(
							self,
							"can't currently skip by clearing the input adapter because it already is empty"
						);
					}
				} else {
					// An internal seek must not be issued while an external flush
					// is in progress. Upstream is being flushed by whoever started
					// that flush, so the seek would either be refused or, worse,
					// race with that flush and leave the parse position and the
					// actual upstream position disagreeing. Report FLUSHING and
					// let the caller unwind; the scan resumes after the flush.
					//
					// Note that the flush events caused by _this_ seek are dropped
					// by the sinkpad event handler and never set the flushing flag,
					// so they cannot trigger this check.
					if (G_UNLIKELY(gst_dsd_media_parse_is_flushing(priv))) {
						GST_DEBUG_OBJECT(
							self,
							"not performing the internal skip seek - a flush is in progress"
						);
						return GST_FLOW_FLUSHING;
					}

					*num_actually_skipped_bytes = num_bytes_to_skip;

					if (!gst_dsd_media_parse_verify_advance(self, *num_actually_skipped_bytes, "skip"))
						return GST_FLOW_ADVANCE_OUT_OF_BOUNDS;

					GST_LOG_OBJECT(
						self,
						"skipping %" G_GUINT64_FORMAT " byte(s) by upstream seeking",
						num_bytes_to_skip
					);

					// Clear all adapter data since it will be stale after seeking.
					gst_adapter_clear(priv->input_adapter);

					guint64 old_byte_position = priv->byte_position;
					guint64 new_byte_position = priv->byte_position + num_bytes_to_skip;

					GstEvent *event = gst_event_new_seek(
						1.0,
						GST_FORMAT_BYTES,
						GstSeekFlags(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE),
						GST_SEEK_TYPE_SET, new_byte_position,
						GST_SEEK_TYPE_NONE, -1
					);

					// Since we are about to perform an internal seek, register its
					// seqnum so that the sinkpad event handler drops the flush events
					// it causes, instead of processing them as if they were coming
					// from an external source. The registration is withdrawn when the
					// segment event concluding this seek arrives, not here - see the
					// internal_seek_seqnum documentation for why.

					{
						std::lock_guard<std::mutex> lock(priv->field_mutex);
						priv->internal_seek_seqnum = gst_event_get_seqnum(event);
					}

					// Do not simply increment the byte position here. In case the
					// gst_pad_push_event() call causes the segment event handler
					// to synchronously be invoked, it will set byte_position to
					// segment->start (whose value is new_byte_position). If we
					// incremented here, we'd move past new_byte_position. By
					// instead assigning, this is avoided.
					priv->byte_position = new_byte_position;

					if (!gst_pad_push_event(priv->sinkpad, event)) {
						GST_ELEMENT_ERROR(
							self,
							STREAM,
							FAILED,
							("Internal data stream error."),
							("failed to push seek event upstream")
						);
						// Restore old byte position if initiating the upstream seek failed.
						priv->byte_position = old_byte_position;
						return GST_FLOW_ERROR;
					}
				}
			}

			break;
		}

		case GST_PAD_MODE_PULL:
			// In pull mode, random access in upstream is always possible,
			// so no special skip operation needs to be performed.

			*num_actually_skipped_bytes = num_bytes_to_skip;

			if (!gst_dsd_media_parse_verify_advance(self, *num_actually_skipped_bytes, "skip"))
				return GST_FLOW_ADVANCE_OUT_OF_BOUNDS;

			priv->byte_position += *num_actually_skipped_bytes;

			break;

		default:
			GST_ERROR_OBJECT(self, "invalid sinkpad scheduling mode %d reached", gint(priv->sinkpad_mode));
			g_assert_not_reached();
			return GST_FLOW_ERROR;
	}

	return GST_FLOW_OK;
}


GstFlowReturn gst_dsd_media_parse_read_data_during_streaming(GstDsdMediaParse *parse,
	                                                         guint64 num_bytes_to_read,
	                                                         GstBuffer **data)
{
	g_assert(GST_IS_DSD_MEDIA_PARSE(parse));
	g_return_val_if_fail(num_bytes_to_read > 0, GST_FLOW_ERROR);
	g_return_val_if_fail(data != nullptr, GST_FLOW_ERROR);

	GstDsdMediaParsePrivate *priv = get_private(parse);

	g_return_val_if_fail(get_parse_stage(priv) == ParseStage::Streaming, GST_FLOW_ERROR);

	switch (priv->sinkpad_mode) {
		case GST_PAD_MODE_PUSH: {
			// In push mode, we have to rely on the contents of input_adapter,
			// since it is not possible to get an exact amount of data from
			// upstream (it decides on its own how much data to deliver).
			// If there is not enough data in it yet, we must exit, and report
			// this to the caller so it tries again later.

			guint64 num_available_bytes = gst_adapter_available(priv->input_adapter);

			if (num_available_bytes < num_bytes_to_read) {
				GST_LOG_OBJECT(
					parse,
					"cannot read %" G_GUINT64_FORMAT " byte(s) since only %" G_GUINT64_FORMAT " are currently available",
					num_bytes_to_read,
					num_available_bytes
				);
				return GST_FLOW_NOT_ENOUGH_DATA;
			} else {
				if (G_UNLIKELY((priv->byte_position + num_bytes_to_read) > priv->end_payload_position)) {
					GST_ELEMENT_ERROR(
						parse,
						STREAM,
						FAILED,
						("Attempting to read out of bounds."),
						(
							"Attempt to read %" G_GUINT64_FORMAT " payload bytes at byte position %" G_GUINT64_FORMAT
							" exceed bounds of DSD payload by %" G_GUINT64_FORMAT " byte(s)",
							num_bytes_to_read,
							priv->byte_position,
							(priv->byte_position + num_bytes_to_read - priv->end_payload_position)
						)
					);
					return GST_FLOW_ERROR;
				}

				*data = gst_adapter_take_buffer(priv->input_adapter, num_bytes_to_read);
				GST_LOG_OBJECT(
					parse,
					"read %" G_GUINT64_FORMAT " byte(s) from the adapter",
					num_bytes_to_read
				);
			}

			break;
		}

		case GST_PAD_MODE_PULL: {
			// Since in pull mode, random access from upstream is
			// possible, the input_adapter is not used at all here.

			if (G_UNLIKELY((priv->byte_position + num_bytes_to_read) > priv->end_payload_position)) {
				GST_ELEMENT_ERROR(
					parse,
					STREAM,
					FAILED,
					("Attempting to read out of bounds."),
					(
						"Attempt to read %" G_GUINT64_FORMAT " payload bytes at byte position %" G_GUINT64_FORMAT
						" exceed bounds of DSD payload by %" G_GUINT64_FORMAT " byte(s)",
						num_bytes_to_read,
						priv->byte_position,
						(priv->byte_position + num_bytes_to_read - priv->end_payload_position)
					)
				);
				return GST_FLOW_ERROR;
			}

			GstFlowReturn flow_ret;

			*data = nullptr;

			flow_ret = gst_pad_pull_range(
				priv->sinkpad,
				priv->byte_position,
				num_bytes_to_read,
				data
			);

			switch (flow_ret) {
				case GST_FLOW_OK: {
					guint64 actual_num_bytes_read = gst_buffer_get_size(*data);

					if (G_UNLIKELY(actual_num_bytes_read != num_bytes_to_read)) {
						GST_ERROR_OBJECT(
							parse,
							"requested %" G_GUINT64_FORMAT " byte(s) from upstream, got %" G_GUINT64_FORMAT "; "
							"this should not happen in pull mode - data may be truncated and thus invalid; "
							"discarding and reporting this as EOS",
							num_bytes_to_read,
							actual_num_bytes_read
						);
						gst_buffer_unref(*data);
						return GST_FLOW_EOS;
					}

					break;
				}
				case GST_FLOW_EOS:
					GST_DEBUG_OBJECT(parse, "reached EOS in pull mode");
					return GST_FLOW_EOS;
				default:
					GST_ERROR_OBJECT(parse, "could not pull from upstream: %s", gst_flow_get_name(flow_ret));
					return flow_ret;
			}

			break;
		}

		default:
			GST_ERROR_OBJECT(parse, "invalid sinkpad scheduling mode %d reached", gint(priv->sinkpad_mode));
			g_assert_not_reached();
			return GST_FLOW_ERROR;
	}

	gsize output_size = gst_buffer_get_size(*data);

	GST_LOG_OBJECT(
		parse,
		"got %" G_GSIZE_FORMAT " byte(s) from upstream position %" G_GUINT64_FORMAT,
		output_size,
		priv->byte_position
	);

	priv->byte_position += output_size;

	return GST_FLOW_OK;
}


guint64 gst_dsd_media_parse_get_current_byte_position(GstDsdMediaParse *parse)
{
	g_assert(GST_IS_DSD_MEDIA_PARSE(parse));
	GstDsdMediaParsePrivate *priv = get_private(parse);
	return priv->byte_position;
}


bool gst_dsd_media_parse_is_currently_scanning(GstDsdMediaParse *parse)
{
	g_assert(GST_IS_DSD_MEDIA_PARSE(parse));
	GstDsdMediaParsePrivate *priv = get_private(parse);
	return get_parse_stage(priv) == ParseStage::ScanningInfo;
}


bool gst_dsd_media_parse_was_payload_reported(GstDsdMediaParse *parse)
{
	g_assert(GST_IS_DSD_MEDIA_PARSE(parse));
	GstDsdMediaParsePrivate *priv = get_private(parse);
	std::lock_guard<std::mutex> lock(priv->field_mutex);
	return priv->payload_position.has_value();
}


bool gst_dsd_media_parse_seek_during_scan(GstDsdMediaParse *parse, guint64 new_byte_position)
{
	g_assert(GST_IS_DSD_MEDIA_PARSE(parse));
	GstDsdMediaParsePrivate *priv = get_private(parse);

	g_assert(priv->upstream_is_seekable.has_value());

	if (!(*(priv->upstream_is_seekable))) {
		GST_DEBUG_OBJECT(parse, "cannot seek - upstream does not support it");
		return false;
	}

	guint64 old_byte_position = priv->byte_position;

	// The byte position must be set here, even in push mode, before
	// the upstream seek event is pushed. Otherwise, the sinkpad segment
	// event handler will incorrectly see a mismatch between the segment
	// event start and the not-updated byte position, and log a warning.
	//
	// However, the current byte position is saved in old_byte_position
	// in case pushing the event upstream fails.
	priv->byte_position = new_byte_position;

	if (priv->sinkpad_mode == GST_PAD_MODE_PUSH) {
		gst_adapter_clear(priv->input_adapter);

		GstEvent *event = gst_event_new_seek(
			1.0,
			GST_FORMAT_BYTES,
			GstSeekFlags(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE),
			GST_SEEK_TYPE_SET, new_byte_position,
			GST_SEEK_TYPE_NONE, -1
		);

		// Register the internal seek's seqnum so that the sinkpad event handler
		// drops the flush events it causes. The registration is withdrawn when
		// the segment event concluding this seek arrives, not here - see the
		// internal_seek_seqnum documentation for why.
		{
			std::lock_guard<std::mutex> lock(priv->field_mutex);
			priv->internal_seek_seqnum = gst_event_get_seqnum(event);
		}

		gboolean ret = gst_pad_push_event(priv->sinkpad, event);

		if (!ret) {
			// Restore the byte position in case of a failed event
			// push, since then, no seeking actually took place.
			priv->byte_position = old_byte_position;
		}

		return ret;
	} else {
		return true;
	}
}


void gst_dsd_media_parse_configure(GstDsdMediaParse *parse, GstCaps *output_caps,
                                    GstClockTime duration)
{
	ScopeGuard guard([&]() { if (output_caps != nullptr) gst_caps_unref(output_caps); });

	g_assert(GST_IS_DSD_MEDIA_PARSE(parse));
	g_return_if_fail(output_caps != nullptr);
	g_return_if_fail(GST_CLOCK_TIME_IS_VALID(duration));

	GstDsdMediaParsePrivate *priv = get_private(parse);

	GST_DEBUG_OBJECT(parse, "configuring parser:");
	GST_DEBUG_OBJECT(parse, "output caps: %" GST_PTR_FORMAT, gpointer(output_caps));
	GST_DEBUG_OBJECT(parse, "duration: %" GST_TIME_FORMAT, GST_TIME_ARGS(duration));

	guard.dismiss();
	gst_caps_take(&(priv->output_caps), output_caps);

	{
		std::lock_guard<std::mutex> lock(priv->field_mutex);
		priv->duration = duration;
	}
}


GstFlowReturn gst_dsd_media_parse_report_payload_found(GstDsdMediaParse *parse, guint64 payload_size,
                                                       bool force_immediate_streaming)
{
	g_assert(GST_IS_DSD_MEDIA_PARSE(parse));
	g_return_val_if_fail(payload_size > 0, GST_FLOW_ERROR);

	GstDsdMediaParsePrivate *priv = get_private(parse);

	{
		std::lock_guard<std::mutex> lock(priv->field_mutex);

		g_return_val_if_fail(get_parse_stage(priv) == ParseStage::ScanningInfo, GST_FLOW_ERROR);

		// Check that this function was not called more than once
		g_assert(!(priv->payload_position.has_value()));

		priv->payload_position = priv->byte_position;
		priv->payload_size = payload_size;
		priv->end_payload_position = priv->byte_position + payload_size;
	}

	g_assert(priv->upstream_is_seekable.has_value());

	if (*(priv->upstream_is_seekable) && !force_immediate_streaming) {
		GST_DEBUG_OBJECT(
			parse,
			"found payload of size %" G_GUINT64_FORMAT " at position %" G_GUINT64_FORMAT "; "
			"will continue to parse and later seek back to it",
			priv->payload_size,
			*(priv->payload_position)
		);

		guint64 dummy;
		GstFlowReturn skip_flow_ret = gst_dsd_media_parse_skip_data_during_scan_full(parse, payload_size, &dummy, true);
		if (G_UNLIKELY(skip_flow_ret != GST_FLOW_OK)) {
			// No GST_ELEMENT_ERROR() call here. Unlike the other callsites of
			// gst_dsd_media_parse_stream_failed(), this one propagates a flow
			// return that originated elsewhere (and that can legitimately be
			// GST_FLOW_FLUSHING, which must not be reported as an error at all).

			// TODO: However, the handling of a flushing pipeline during the
			// Scanning Info stage is currently not well-defined. It is an odd
			// use case that is highly unlikely to occur in practice. Should
			// flushing be handled later, also revisit the part of the documentation
			// that states that it "must not be called more than once in there"
			// (that is, in scan_info()).

			return skip_flow_ret;
		}

		return GST_FLOW_OK;
	} else {
		if (force_immediate_streaming) {
			GST_DEBUG_OBJECT(
				parse,
				"found payload at position %" G_GUINT64_FORMAT ", and caller "
				"requested to start the Streaming stage immediately",
				*(priv->payload_position)
			);
		} else {
			GST_DEBUG_OBJECT(
				parse,
				"found payload at position %" G_GUINT64_FORMAT ", but seeking is not possible; "
				"cannot skip payload to parse data behind it; start streaming immediately instead",
				*(priv->payload_position)
			);
		}

		{
			std::lock_guard<std::mutex> lock(priv->field_mutex);

			// In this case, there won't be a new segment event from upstream,
			// at least not at first. (In theory, upstream can push segment
			// events downstream at any time.) Set up the downstream segment
			// right away instead.
			gst_dsd_media_parse_init_downstream_segment(priv);
			priv->current_seqnum = gst_util_seqnum_next();

			set_parse_stage(priv, ParseStage::Streaming);
		}

		// NOTE: gst_dsd_media_parse_start_streaming() reports errors on its own.
		if (!gst_dsd_media_parse_start_streaming(parse)) {
			return GST_FLOW_ERROR;
		}

		return GST_FLOW_OK;
	}
}


void gst_dsd_media_parse_scanning_finished(GstDsdMediaParse *parse)
{
	g_assert(GST_IS_DSD_MEDIA_PARSE(parse));
	GstDsdMediaParsePrivate *priv = get_private(parse);

	g_assert(priv->upstream_is_seekable.has_value());

	if (get_parse_stage(priv) == ParseStage::Streaming)
		return;

	if (G_UNLIKELY(!priv->payload_position.has_value())) {
		// STREAM/WRONG_TYPE rather than STREAM/DEMUX: the medium was scanned to its
		// end without any DSD payload turning up, so this element is most likely
		// not the right one for it. Autoplugging elements evaluate this code.
		GST_ELEMENT_ERROR(
			parse, STREAM, WRONG_TYPE,
			("This medium contains no payload."),
			("finished scanning the medium without ever encountering payload")
		);

		std::lock_guard<std::mutex> lock(priv->field_mutex);
		gst_dsd_media_parse_stream_failed(priv, GST_FLOW_ERROR);

		return;
	}

	if (priv->sinkpad_mode == GST_PAD_MODE_PUSH) {
		// upstream_is_seekable being false is impossible, since the code
		// in gst_dsd_media_parse_report_payload_found() immediately switches
		// to the Streaming stage if seeking is not supported. And, this
		// function here checks above if the Streaming stage was entered
		// and exits early then. Thus, upstream_is_seekable being false
		// here can only happen if gst_dsd_media_parse_report_payload_found()
		// has a bug.
		g_assert(*(priv->upstream_is_seekable));

		GST_DEBUG_OBJECT(
			parse,
			"going to seek to payload at position %" G_GUINT64_FORMAT,
			*(priv->payload_position)
		);

		GstEvent *event = gst_event_new_seek(
			1.0,
			GST_FORMAT_BYTES,
			GstSeekFlags(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE),
			GST_SEEK_TYPE_SET, *(priv->payload_position),
			GST_SEEK_TYPE_NONE, -1
		);

		// Register the internal seek's seqnum so that the sinkpad event handler
		// drops the flush events it causes. The registration is withdrawn when
		// the segment event concluding this seek arrives, not here - see the
		// internal_seek_seqnum documentation for why. That matters especially
		// for this seek, since the segment it produces is what drives the switch
		// to the Streaming stage.
		{
			std::lock_guard<std::mutex> lock(priv->field_mutex);
			priv->internal_seek_seqnum = gst_event_get_seqnum(event);
			priv->pending_seek_position = *(priv->payload_position);
			priv->pending_seek_index = 0;
			priv->pending_segment_seqnum = gst_event_get_seqnum(event);
			priv->pending_seek_is_accurate = true;
			priv->pending_bootstrapping = true;
			priv->expecting_upstream_segment = true;
			set_parse_stage(priv, ParseStage::AwaitingSegment);
		}

		if (!gst_pad_push_event(priv->sinkpad, event)) {
			GST_ELEMENT_ERROR(
				parse, CORE, EVENT,
				("Internal data stream error."),
				("upstream refused the seek back to the payload at position %" G_GUINT64_FORMAT, *(priv->payload_position))
			);
			std::lock_guard<std::mutex> lock(priv->field_mutex);
			gst_dsd_media_parse_stream_failed(priv, GST_FLOW_ERROR);
		}
	} else {
		GST_DEBUG_OBJECT(parse, "switching to Streaming state in pull scheduling mode");

		GST_DEBUG_OBJECT(
			parse,
			"resetting parse position to payload position %" G_GUINT64_FORMAT,
			*(priv->payload_position)
		);
		priv->byte_position = *(priv->payload_position);

		{
			std::lock_guard<std::mutex> lock(priv->field_mutex);

			gst_dsd_media_parse_init_downstream_segment(priv);
			priv->current_seqnum = gst_util_seqnum_next();

			set_parse_stage(priv, ParseStage::Streaming);
		}

		// NOTE: gst_dsd_media_parse_start_streaming() reports errors on its own.
		if (!gst_dsd_media_parse_start_streaming(parse)) {
			std::lock_guard<std::mutex> lock(priv->field_mutex);
			gst_dsd_media_parse_stream_failed(priv, GST_FLOW_ERROR);
		}
	}
}

