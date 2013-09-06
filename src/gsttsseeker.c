/* GStreamer MPEG TS Time Shifting
 * Copyright (C) 2011 Fluendo S.A. <support@fluendo.com>
 * Copyright (C) 2013 Youview TV Ltd. <william.manley@youview.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Suite 500,
 * Boston, MA 02110-1335, USA.
 */

/**
 * SECTION:element-gsttsseeker
 *
 * The tsseeker element transforms TIME to BYTES in segment/seek events
 * based upon a time/byte index.  This element is not useful by itself and
 * should be used in conjunction with an indexer with a shared index.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include <gst/gstelement.h>
#include <gst/base/gstbasetransform.h>
#include "gsttsseeker.h"

GST_DEBUG_CATEGORY_STATIC (gst_ts_seeker_debug_category);
#define GST_CAT_DEFAULT gst_ts_seeker_debug_category

/* prototypes */


static void gst_ts_seeker_set_property (GObject * object,
    guint property_id, const GValue * value, GParamSpec * pspec);
static void gst_ts_seeker_get_property (GObject * object,
    guint property_id, GValue * value, GParamSpec * pspec);
static void gst_ts_seeker_dispose (GObject * object);
static void gst_ts_seeker_finalize (GObject * object);

static gboolean gst_ts_seeker_start (GstBaseTransform * trans);
static gboolean gst_ts_seeker_stop (GstBaseTransform * trans);
static gboolean
gst_ts_seeker_sink_event (GstBaseTransform * trans, GstEvent * event);
static gboolean
gst_ts_seeker_src_event (GstBaseTransform * trans, GstEvent * event);
static void gst_ts_seeker_replace_index (GstTSSeeker * seeker,
    GstIndex * new_index);
static gboolean
gst_ts_seeker_query (GstBaseTransform * trans,
    GstPadDirection direction, GstQuery * query);
static GstFlowReturn gst_ts_seeker_transform_ip (GstBaseTransform *
    trans, GstBuffer * buf);

enum
{
  PROP_0,
  PROP_INDEX
};

/* pad templates */

static GstStaticPadTemplate gst_ts_seeker_sink_template =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/mpegts")
    );

static GstStaticPadTemplate gst_ts_seeker_src_template =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/mpegts")
    );


/* class initialization */

G_DEFINE_TYPE (GstTSSeeker, gst_ts_seeker, GST_TYPE_BASE_TRANSFORM);
#define parent_class gst_ts_seeker_parent_class

static void
gst_ts_seeker_class_init (GstTSSeekerClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstBaseTransformClass *base_transform_class =
      GST_BASE_TRANSFORM_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);

  gobject_class->set_property = gst_ts_seeker_set_property;
  gobject_class->get_property = gst_ts_seeker_get_property;
  gobject_class->dispose = gst_ts_seeker_dispose;
  gobject_class->finalize = gst_ts_seeker_finalize;
  base_transform_class->start = GST_DEBUG_FUNCPTR (gst_ts_seeker_start);
  base_transform_class->stop = GST_DEBUG_FUNCPTR (gst_ts_seeker_stop);
  base_transform_class->sink_event =
      GST_DEBUG_FUNCPTR (gst_ts_seeker_sink_event);
  base_transform_class->src_event = GST_DEBUG_FUNCPTR (gst_ts_seeker_src_event);
  base_transform_class->query = GST_DEBUG_FUNCPTR (gst_ts_seeker_query);
  base_transform_class->transform_ip =
      GST_DEBUG_FUNCPTR (gst_ts_seeker_transform_ip);

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&gst_ts_seeker_sink_template));
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&gst_ts_seeker_src_template));

  g_object_class_install_property (gobject_class, PROP_INDEX,
      g_param_spec_object ("index", "Index",
          "The index from which to read indexing information",
          GST_TYPE_INDEX, (G_PARAM_READABLE | G_PARAM_WRITABLE)));

  gst_element_class_set_static_metadata (element_class, "Time-shift seeker",
      "Generic", "Transforms time to bytes as required by seek/segment events",
      "William Manley <william.manley@youview.com>");

  GST_DEBUG_CATEGORY_INIT (gst_ts_seeker_debug_category,
      "gst_ts_seeker", 0, "Time shift seeker");
}

static void
gst_ts_seeker_init (GstTSSeeker * seeker)
{
}

/**
 * gst_ts_seeker_replace_index: 
 * @new_index: (allow-none) (transfer full): Replacement index
 */
static void
gst_ts_seeker_replace_index (GstTSSeeker * seeker, GstIndex * new_index)
{
  if (seeker->index) {
    gst_object_unref (seeker->index);
    seeker->index = NULL;
  }
  if (new_index) {
    seeker->index = new_index;
  }
}


void
gst_ts_seeker_set_property (GObject * object, guint property_id,
    const GValue * value, GParamSpec * pspec)
{
  GstTSSeeker *seeker = GST_TS_SEEKER (object);

  switch (property_id) {
    case PROP_INDEX:
      gst_ts_seeker_replace_index (seeker, g_value_dup_object (value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

void
gst_ts_seeker_get_property (GObject * object, guint property_id,
    GValue * value, GParamSpec * pspec)
{
  GstTSSeeker *seeker = GST_TS_SEEKER (object);

  switch (property_id) {
    case PROP_INDEX:
      g_value_set_object (value, seeker->index);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

void
gst_ts_seeker_dispose (GObject * object)
{
  GstTSSeeker *seeker = GST_TS_SEEKER (object);

  /* clean up as possible.  may be called multiple times */
  gst_ts_seeker_replace_index (seeker, NULL);

  G_OBJECT_CLASS (parent_class)->dispose (object);
}

void
gst_ts_seeker_finalize (GObject * object)
{
  /* GstTSSeeker *timeshiftseeker = GST_TS_SEEKER (object); */

  /* clean up object here */

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static gboolean
gst_ts_seeker_start (GstBaseTransform * trans)
{

  return TRUE;
}

static gboolean
gst_ts_seeker_stop (GstBaseTransform * trans)
{

  return TRUE;
}

static gint64
gst_ts_seeker_first_offset (GstTSSeeker * ts)
{
  GstIndexEntry *entry = NULL;
  gint64 offset = -1;

  entry = gst_index_get_assoc_entry (ts->index,
      GST_INDEX_LOOKUP_BEFORE, GST_ASSOCIATION_FLAG_NONE, GST_FORMAT_TIME, 0);

  if (entry) {
    gst_index_entry_assoc_map (entry, GST_FORMAT_BYTES, &offset);
  }

  return offset;
}

static GstClockTime
gst_ts_seeker_bytes_to_stream_time (GstTSSeeker * ts, guint64 buffer_offset,
    gboolean accurate)
{
  GstIndexEntry *entry = NULL;
  GstClockTime ret;

  if (!ts->index) {
    GST_DEBUG_OBJECT (ts, "no index");
    return GST_CLOCK_TIME_NONE;
  }

  /* Let's check if we have an index entry for that seek bytes */
  entry = gst_index_get_assoc_entry (ts->index,
      GST_INDEX_LOOKUP_BEFORE, GST_ASSOCIATION_FLAG_NONE, GST_FORMAT_BYTES,
      buffer_offset);

  if (entry) {
    gint64 offset;
    gint64 time = GST_CLOCK_TIME_NONE;

    gst_index_entry_assoc_map (entry, GST_FORMAT_BYTES, &offset);
    gst_index_entry_assoc_map (entry, GST_FORMAT_TIME, &time);

    GST_DEBUG_OBJECT (ts, "found index entry at %" GST_TIME_FORMAT " pos %"
        G_GINT64_FORMAT, GST_TIME_ARGS (time), offset);
    if (accurate && buffer_offset != offset) {
      GST_ELEMENT_WARNING (ts, RESOURCE, FAILED,
          ("Bytes->time conversion inaccurate"),
          ("Lookup of byte offset not accurate: "
              "Returned byte offset %" G_GINT64_FORMAT " doesn't match "
              "requested offset %" G_GUINT64_FORMAT ".  Time: %"
              GST_TIME_FORMAT, offset, buffer_offset, GST_TIME_ARGS (time)));
    } else {
      GST_DEBUG_OBJECT (ts, "found index entry at %" GST_TIME_FORMAT
          " pos %" G_GUINT64_FORMAT, GST_TIME_ARGS (time), offset);
    }
    ret = (GstClockTime) time;
  } else if (buffer_offset < gst_ts_seeker_first_offset (ts)) {
    /* This means that the upstream returned a segment starting before the first
     * indexed buffer (the first one that has PCR).
     */
    ret = 0;
  } else {
    if (accurate) {
      GST_ELEMENT_WARNING (ts, RESOURCE, FAILED,
          ("Bytes->time conversion failed"),
          ("Lookup of byte offset %" G_GUINT64_FORMAT " failed: "
              "No index entry for that byte offset", buffer_offset));
    } else {
      GST_DEBUG_OBJECT (ts, "no entry for position %" G_GUINT64_FORMAT " in %p",
          buffer_offset, ts->index);
    }

    ret = GST_CLOCK_TIME_NONE;
  }
  return ret;
}

static void
gst_ts_seeker_transform_segment_event (GstTSSeeker * seeker, GstEvent ** event)
{
  GstEvent *newevent;
  GstSegment segment;

  gst_event_copy_segment (*event, &segment);
  if (segment.format != GST_FORMAT_BYTES) {
    GST_DEBUG_OBJECT (seeker, "time shift seeker received non-bytes segment");
    goto beach;
  }
  if (!(segment.flags & GST_SEGMENT_FLAG_RESET)) {
    /* We can only handle flushing segments ATM as otherwise running time is >0
       so filling in segment.base becomes trickier TODO: Fix this. */
    GST_DEBUG_OBJECT (seeker, "time shift seeker can only deal with flushing "
        "seeks");
    goto beach;
  }
  if (!seeker->index) {
    GST_DEBUG_OBJECT (seeker, "no index");
    goto beach;
  }

  segment.format = GST_FORMAT_TIME;
  segment.base = 0;
  segment.start =
      gst_ts_seeker_bytes_to_stream_time (seeker, segment.start, TRUE);
  if (segment.stop != -1) {
    segment.stop =
        gst_ts_seeker_bytes_to_stream_time (seeker, segment.stop, TRUE);
  }
  segment.time = segment.start;

  newevent = gst_event_new_segment (&segment);
  gst_event_set_seqnum (newevent, gst_event_get_seqnum (*event));
  gst_event_replace (event, newevent);

  seeker->timestamp_next_buffer = TRUE;

  GST_DEBUG_OBJECT (seeker, "forwarding segment %" GST_SEGMENT_FORMAT,
      &segment);
beach:
  return;
}

static gboolean
gst_ts_seeker_sink_event (GstBaseTransform * trans, GstEvent * event)
{
  GstTSSeeker *seeker = GST_TS_SEEKER (trans);

  if (GST_EVENT_TYPE (event) == GST_EVENT_SEGMENT) {
    gst_ts_seeker_transform_segment_event (seeker, &event);
  }
  return GST_BASE_TRANSFORM_CLASS (parent_class)->sink_event (trans, event);
}

static guint64
gst_ts_seeker_get_duration_bytes (GstTSSeeker * seeker)
{
  gint64 duration = 0;

  GstBaseTransform *base = GST_BASE_TRANSFORM (seeker);
  GstQuery *query = gst_query_new_duration (GST_FORMAT_BYTES);

  if (gst_pad_peer_query (base->sinkpad, query)) {
    gst_query_parse_duration (query, NULL, &duration);
  }

  gst_query_unref (query);
  return duration;
}

static GstClockTime
gst_ts_seeker_get_last_time (GstTSSeeker * base)
{
  guint64 len = gst_ts_seeker_get_duration_bytes (base);

  return gst_ts_seeker_bytes_to_stream_time (base, len - 1000000, FALSE);
}

static guint64
gst_ts_seeker_seek (GstTSSeeker * base, GstSeekType type, gint64 start)
{
  GstIndexEntry *entry = NULL;
  gint64 offset = -1;
  gint64 time;
  GstClockTime pos = 0;

  if (type == GST_SEEK_TYPE_NONE) {
    /* Base class checks: Should never happen */
    goto beach;
  }

  GST_DEBUG_OBJECT (base, "seeking at time %" GST_TIME_FORMAT " type %d",
      GST_TIME_ARGS (start), type);

  if (!base->index) {
    GST_DEBUG_OBJECT (base, "no index");
    goto beach;
  }

  if (type == GST_SEEK_TYPE_SET) {
    pos = start;
  } else if (type == GST_SEEK_TYPE_END) {
    pos = gst_ts_seeker_get_last_time (base) + start;
  }

  GST_DEBUG_OBJECT (base, "seek in index for %" GST_TIME_FORMAT,
      GST_TIME_ARGS (pos));

  /* Let's check if we have an index entry for that seek time */
  entry = gst_index_get_assoc_entry (base->index, GST_INDEX_LOOKUP_BEFORE,
      GST_ASSOCIATION_FLAG_NONE, GST_FORMAT_TIME, pos);

  if (entry) {
    gst_index_entry_assoc_map (entry, GST_FORMAT_BYTES, &offset);
    gst_index_entry_assoc_map (entry, GST_FORMAT_TIME, &time);

    GST_DEBUG_OBJECT (base, "found index entry at %" GST_TIME_FORMAT " pos %"
        G_GUINT64_FORMAT, GST_TIME_ARGS (time), offset);
  }

beach:
  return offset;
}

static void
gst_ts_seeker_transform_offset (GstTSSeeker * ts,
    GstSeekType * type, gint64 * offset)
{
  if (*type == GST_SEEK_TYPE_NONE) {
    /* pass: no transformation required */
  } else {
    *offset = gst_ts_seeker_seek (ts, *type, *offset);
    *type = GST_SEEK_TYPE_SET;
  }
}

/* Converts any seek events with format TIME to one with format BYTES */
static void
gst_ts_seeker_transform_seek_event (GstTSSeeker * seeker, GstEvent ** event)
{
  GstEvent *new_event = NULL;
  GstFormat format;
  GstSeekFlags flags;
  GstSeekType start_type, stop_type;
  gint64 start, stop;
  gdouble rate;

  gst_event_parse_seek (*event, &rate, &format, &flags,
      &start_type, &start, &stop_type, &stop);

  if (format != GST_FORMAT_TIME) {
    goto beach;
  }

  if (!seeker->index) {
    GST_ELEMENT_WARNING (seeker, CORE, SEEK, NULL, ("Seeker has no index set"));
    goto beach;
  }

  if (rate < 0.0) {
    GST_WARNING_OBJECT (seeker, "we only support forward playback");
    goto beach;
  }

  gst_ts_seeker_transform_offset (seeker, &start_type, &start);
  gst_ts_seeker_transform_offset (seeker, &stop_type, &stop);
  new_event = gst_event_new_seek (rate, GST_FORMAT_BYTES, flags, start_type,
      start, stop_type, stop);
  gst_event_set_seqnum (new_event, gst_event_get_seqnum (*event));
  gst_event_replace (event, new_event);

beach:
  return;
}

static gboolean
gst_ts_seeker_src_event (GstBaseTransform * trans, GstEvent * event)
{
  GstTSSeeker *seeker = GST_TS_SEEKER (trans);

  if (GST_EVENT_TYPE (event) == GST_EVENT_SEEK) {
    gst_ts_seeker_transform_seek_event (seeker, &event);
  }
  return GST_BASE_TRANSFORM_CLASS (parent_class)->src_event (trans, event);
}

static gboolean
gst_ts_seeker_query (GstBaseTransform * base, GstPadDirection direction,
    GstQuery * query)
{
  GstTSSeeker *ts = GST_TS_SEEKER (base);

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_DURATION:
    {
      GstFormat format;

      gst_query_parse_duration (query, &format, NULL);
      if (format == GST_FORMAT_TIME && direction == GST_PAD_SRC) {
        GST_DEBUG_OBJECT (base,
            "Responding to duration query with time  %" GST_TIME_FORMAT,
            GST_TIME_ARGS (gst_ts_seeker_get_last_time (ts)));

        gst_query_set_duration (query, format,
            gst_ts_seeker_get_last_time (ts));
        return TRUE;
      }
      break;
    }

    case GST_QUERY_SEEKING:
    {
      GstFormat fmt;

      gst_query_parse_seeking (query, &fmt, NULL, NULL, NULL);
      if (fmt == GST_FORMAT_TIME) {
        gst_query_set_seeking (query, fmt, TRUE, 0, -1);
        return TRUE;
      }
      break;
    }

    case GST_QUERY_BUFFERING:
    {
      GstFormat format;
      GstQuery *buf_query;
      gboolean ret = FALSE;

      if (!GST_QUERY_IS_UPSTREAM (query)) {
        /* Can't see any use cases for non-upstream queries here */
        break;
      }

      gst_query_parse_buffering_range (query, &format, NULL, NULL, NULL);
      if (format != GST_FORMAT_TIME) {
        /* Not interested in handling other formats.
           Let the upstream elements deal with it. */
        break;
      }

      buf_query = gst_query_new_buffering (GST_FORMAT_BYTES);
      if (GST_BASE_TRANSFORM_CLASS (parent_class)->query (base, direction,
              buf_query)) {
        gint64 start, stop;
        GstClockTime start_time, stop_time;

        gst_query_parse_buffering_range (buf_query, NULL, &start, &stop, NULL);

        start_time = gst_ts_seeker_bytes_to_stream_time (ts, start, FALSE);
        stop_time = gst_ts_seeker_bytes_to_stream_time (ts, stop, FALSE);
        gst_query_set_buffering_range (query, format, start_time, stop_time,
            -1);

        ret = TRUE;
      }

      gst_query_unref (buf_query);
      return ret;
    }

    default:
      break;
  }

  return GST_BASE_TRANSFORM_CLASS (parent_class)->query (base, direction,
      query);
}

static GstFlowReturn
gst_ts_seeker_transform_ip (GstBaseTransform * base, GstBuffer * buf)
{
  GstTSSeeker *seeker = GST_TS_SEEKER (base);
  g_assert (gst_buffer_is_writable (buf));
  if (seeker->timestamp_next_buffer) {
    GST_BUFFER_TIMESTAMP (buf) =
        gst_ts_seeker_bytes_to_stream_time (seeker, GST_BUFFER_OFFSET (buf),
        TRUE);
    seeker->timestamp_next_buffer = FALSE;
  }
  return GST_FLOW_OK;
}
