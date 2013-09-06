/* GStreamer MPEG TS Time Shifting
 * Copyright (C) 2011 Fluendo S.A. <support@fluendo.com>
 *               2013 YouView TV Ltd. <william.manley@youview.com>
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
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#define _GNU_SOURCE

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gsttsshifter.h"

/* For sync_file_range and posix_fadvise */
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include <glib/gstdio.h>

GST_DEBUG_CATEGORY_EXTERN (ts_shifter);
GST_DEBUG_CATEGORY_EXTERN (ts_flow);
#define GST_CAT_DEFAULT ts_shifter

enum
{
  SIGNAL_OVERRUN,
  LAST_SIGNAL
};

/* default property values */

enum
{
  PROP_0,
  PROP_BACKING_STORE_FD,
  PROP_LAST
};

#define STATUS(ts, pad, msg)                                              \
  GST_CAT_LOG_OBJECT (ts_flow, ts,                                        \
      "(%s:%s) " msg ":%" G_GUINT64_FORMAT " bytes",                      \
      GST_DEBUG_PAD_NAME (pad),                                           \
      gst_ts_cache_fullness (ts->cache))

#define FLOW_MUTEX_LOCK(ts) G_STMT_START {                                \
  g_mutex_lock (&ts->flow_lock);                                          \
} G_STMT_END

#define FLOW_MUTEX_LOCK_CHECK(ts,res,label) G_STMT_START {                \
  FLOW_MUTEX_LOCK (ts);                                                   \
  if (res != GST_FLOW_OK)                                                 \
    goto label;                                                           \
} G_STMT_END

#define FLOW_MUTEX_UNLOCK(ts) G_STMT_START {                              \
  g_mutex_unlock (&ts->flow_lock);                                        \
} G_STMT_END

#define FLOW_WAIT_ADD_CHECK(ts, res, label) G_STMT_START {                \
  STATUS (ts, ts->srcpad, "wait for ADD");                                \
  g_cond_wait (&ts->buffer_add, &ts->flow_lock);                          \
  if (res != GST_FLOW_OK) {                                               \
    STATUS (ts, ts->srcpad, "received ADD wakeup");                       \
    goto label;                                                           \
  }                                                                       \
  STATUS (ts, ts->srcpad, "received ADD");                                \
} G_STMT_END

#define FLOW_SIGNAL_ADD(ts) G_STMT_START {                                \
  STATUS (ts, ts->sinkpad, "signal ADD");                                 \
  g_cond_signal (&ts->buffer_add);                                        \
} G_STMT_END


G_DEFINE_TYPE (GstTSShifter, gst_ts_shifter, GST_TYPE_ELEMENT);
#define parent_class gst_ts_shifter_parent_class

#define MByte (1024 * 1024)

static GstStaticPadTemplate src_factory = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC, GST_PAD_ALWAYS, GST_STATIC_CAPS ("video/mpegts"));

static GstStaticPadTemplate sink_factory = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK, GST_PAD_ALWAYS, GST_STATIC_CAPS ("video/mpegts"));

static guint gst_ts_shifter_signals[LAST_SIGNAL] = { 0 };

static int
create_backing_store (GstTSShifter * ts, off64_t size)
{
  int fd = -1;
  char tmpfilename[] = "/var/tmp/gst_fluendo_timeshift.XXXXXX";
  fd = mkstemp (tmpfilename);
  if (fd == -1) {
    GST_ELEMENT_ERROR (ts, RESOURCE, OPEN_READ_WRITE,
        ("Could not create temporary file for timeshift backing store"),
        GST_ERROR_SYSTEM);
    goto err_create;
  }
  g_warn_if_fail (unlink (tmpfilename) == 0);
  if (fallocate (fd, 0, 0, size) != 0) {
    GST_ELEMENT_ERROR (ts, RESOURCE, OPEN_READ_WRITE,
        ("Allocating space for timeshift backing store failed"),
        GST_ERROR_SYSTEM);
    goto err_alloc;
  }
  return fd;
err_alloc:
  close (fd);
err_create:
  return -1;
}

static void
gst_ts_shifter_start (GstTSShifter * ts)
{
  FLOW_MUTEX_LOCK (ts);
  if (ts->cache) {
    gst_ts_cache_unref (ts->cache);
    ts->cache = NULL;
  }

  gst_segment_init (&ts->segment, GST_FORMAT_BYTES);

  if (ts->backing_store_fd == -1)
    ts->backing_store_fd = create_backing_store (ts, 512 * MByte);
  if (ts->backing_store_fd == -1) {
    GST_ERROR ("Failed to create backing store");
  } else {
    ts->cache = gst_ts_cache_new (ts->backing_store_fd);
  }
  FLOW_MUTEX_UNLOCK (ts);
}

/* Pop a buffer from the cache and push it downstream.
 * This functions returns the result of the push. */
static GstFlowReturn
gst_ts_shifter_pop (GstTSShifter * ts)
{
  GstFlowReturn ret = GST_FLOW_OK;
  GstBuffer *buffer;

  if (!(buffer = gst_ts_cache_pop (ts->cache, ts->is_eos)))
    goto no_item;

  if (ts->srcresult == GST_FLOW_FLUSHING) {
    gst_buffer_unref (buffer);
    goto out_flushing;
  }

  if (ts->stream_start_event) {
    if (!gst_pad_push_event (ts->srcpad, ts->stream_start_event)) {
      goto stream_start_failed;
    }
    ts->stream_start_event = NULL;
  }

  if (G_UNLIKELY (ts->need_newsegment)) {
    GstEvent *newsegment;

    ts->segment.start = GST_BUFFER_OFFSET (buffer);
    ts->segment.time = 0;       /* <- Not relevant for FORMAT_BYTES */
    ts->segment.flags |= GST_SEGMENT_FLAG_RESET;

    GST_DEBUG_OBJECT (ts, "pushing segment %" GST_SEGMENT_FORMAT, &ts->segment);

    newsegment = gst_event_new_segment (&ts->segment);
    if (newsegment) {
      if (!gst_pad_push_event (ts->srcpad, newsegment)) {
        goto segment_failed;
      }
    }
    ts->need_newsegment = FALSE;
  }
  ts->cur_bytes = GST_BUFFER_OFFSET_END (buffer);
  FLOW_MUTEX_UNLOCK (ts);

  GST_CAT_LOG_OBJECT (ts_flow, ts,
      "pushing buffer %p of size %d, offset %" G_GUINT64_FORMAT,
      buffer, gst_buffer_get_size (buffer), GST_BUFFER_OFFSET (buffer));

  ret = gst_pad_push (ts->srcpad, buffer);

  /* need to check for srcresult here as well */
  FLOW_MUTEX_LOCK_CHECK (ts, ts->srcresult, out_flushing);
  if (ret == GST_FLOW_EOS) {
    GST_CAT_LOG_OBJECT (ts_flow, ts, "got GST_FLOW_EOS from downstream");
    /* stop pushing buffers, we pop all buffers until we see an item that we
     * can push again, which is EOS or NEWSEGMENT. If there is nothing in the
     * cache we can push, we set a flag to make the sinkpad refuse more
     * buffers with an EOS return value until we receive something
     * pushable again or we get flushed. */
    while ((buffer = gst_ts_cache_pop (ts->cache, ts->is_eos))) {
      GST_CAT_LOG_OBJECT (ts_flow, ts, "dropping UNEXPECTED buffer %p", buffer);
      gst_buffer_unref (buffer);
    }
    /* no more items in the cache. Set the unexpected flag so that upstream
     * make us refuse any more buffers on the sinkpad. Since we will still
     * accept EOS and NEWSEGMENT we return GST_FLOW_OK to the caller
     * so that the task function does not shut down. */
    ts->unexpected = TRUE;
    ret = GST_FLOW_OK;
  }

  return ret;

  /* ERRORS */
no_item:
  {
    if (ts->is_eos) {
      GST_CAT_LOG_OBJECT (ts_flow, ts, "pushing EOS");
      gst_pad_push_event (ts->srcpad, gst_event_new_eos ());
      gst_pad_pause_task (ts->srcpad);
      GST_CAT_LOG_OBJECT (ts_flow, ts, "pause task, reason: EOS");
      return GST_FLOW_OK;
    } else {
      GST_CAT_LOG_OBJECT (ts_flow, ts, "exit because we have no item");
      return GST_FLOW_ERROR;
    }
  }
out_flushing:
  {
    GST_CAT_LOG_OBJECT (ts_flow, ts, "exit because we are flushing");
    return GST_FLOW_FLUSHING;
  }
segment_failed:
  {
    GST_CAT_LOG_OBJECT (ts_flow, ts, "push of SEGMENT event failed");
    return GST_FLOW_FLUSHING;
  }
stream_start_failed:
  {
    ts->stream_start_event = NULL;
    GST_CAT_LOG_OBJECT (ts_flow, ts, "push of STREAM_START event failed");
    return GST_FLOW_FLUSHING;
  }
}

/* called repeadedly with @pad as the source pad. This function should push out
 * data to the peer element. */
static void
gst_ts_shifter_loop (GstPad * pad)
{
  GstTSShifter *ts;
  GstFlowReturn ret;

  ts = GST_TS_SHIFTER (GST_PAD_PARENT (pad));

  /* have to lock for thread-safety */
  FLOW_MUTEX_LOCK_CHECK (ts, ts->srcresult, out_flushing);

  if (gst_ts_cache_is_empty (ts->cache) && !ts->is_eos) {
    GST_CAT_LOG_OBJECT (ts_flow, ts, "empty, waiting for new data");
    do {
      /* Wait for data to be available, we could be unlocked because of a flush. */
      FLOW_WAIT_ADD_CHECK (ts, ts->srcresult, out_flushing);
    }
    while (gst_ts_cache_is_empty (ts->cache) && !ts->is_eos);
  }
  ret = gst_ts_shifter_pop (ts);
  ts->srcresult = ret;
  if (ret != GST_FLOW_OK)
    goto out_flushing;

  FLOW_MUTEX_UNLOCK (ts);

  return;

  /* ERRORS */
out_flushing:
  {
    gboolean eos = ts->is_eos;
    GstFlowReturn ret = ts->srcresult;

    gst_pad_pause_task (ts->srcpad);
    FLOW_MUTEX_UNLOCK (ts);
    GST_CAT_LOG_OBJECT (ts_flow, ts,
        "pause task, reason:  %s", gst_flow_get_name (ts->srcresult));
    /* let app know about us giving up if upstream is not expected to do so */
    /* UNEXPECTED is already taken care of elsewhere */
    if (eos && (ret == GST_FLOW_NOT_LINKED || ret < GST_FLOW_EOS)) {
      GST_ELEMENT_ERROR (ts, STREAM, FAILED,
          ("Internal data flow error."),
          ("streaming task paused, reason %s (%d)",
              gst_flow_get_name (ret), ret));
      GST_CAT_LOG_OBJECT (ts_flow, ts, "pushing EOS");
      gst_pad_push_event (ts->srcpad, gst_event_new_eos ());
    }
    return;
  }
}

static GstFlowReturn
gst_ts_shifter_push (GstTSShifter * ts, guint8 * data, gsize size)
{
  /* we have to lock since we span threads */
  FLOW_MUTEX_LOCK_CHECK (ts, ts->sinkresult, out_flushing);
  /* when we received EOS, we refuse more data */
  if (ts->is_eos)
    goto out_eos;
  /* when we received unexpected from downstream, refuse more buffers */
  if (ts->unexpected)
    goto out_unexpected;

  /* add data to the cache */
  if (!gst_ts_cache_push (ts->cache, data, size)) {
    goto out_leaking;
  }
  ts->is_leaking = FALSE;

  FLOW_SIGNAL_ADD (ts);

  FLOW_MUTEX_UNLOCK (ts);

  return GST_FLOW_OK;

  /* special conditions */
out_leaking:
  {
    gboolean emit_overrun = FALSE;

    GST_CAT_LOG_OBJECT (ts_flow, ts,
        "leaking %" G_GSIZE_FORMAT " bytes of data", size);
    if (!ts->is_leaking) {
      ts->is_leaking = TRUE;
      emit_overrun = TRUE;
    }
    FLOW_MUTEX_UNLOCK (ts);

    if (emit_overrun) {
      g_signal_emit (ts, gst_ts_shifter_signals[SIGNAL_OVERRUN], 0);
    }
    return GST_FLOW_OK;
  }
out_flushing:
  {
    GstFlowReturn ret = ts->sinkresult;

    GST_CAT_LOG_OBJECT (ts_flow, ts,
        "exit because task paused, reason: %s", gst_flow_get_name (ret));
    FLOW_MUTEX_UNLOCK (ts);
    return ret;
  }
out_eos:
  {
    GST_CAT_LOG_OBJECT (ts_flow, ts, "exit because we received EOS");
    FLOW_MUTEX_UNLOCK (ts);
    return GST_FLOW_EOS;
  }
out_unexpected:
  {
    GST_CAT_LOG_OBJECT (ts_flow, ts, "exit because we received UNEXPECTED");
    FLOW_MUTEX_UNLOCK (ts);
    return GST_FLOW_EOS;
  }
}

static GstFlowReturn
gst_ts_shifter_chain (GstPad * pad, GstObject * parent, GstBuffer * buffer)
{
  GstTSShifter *ts = GST_TS_SHIFTER (parent);
  GstFlowReturn res;
  GstMapInfo map;

  GST_CAT_LOG_OBJECT (ts_flow, ts,
      "received buffer %p of size %d, time %" GST_TIME_FORMAT ", duration %"
      GST_TIME_FORMAT, buffer, gst_buffer_get_size (buffer),
      GST_TIME_ARGS (GST_BUFFER_TIMESTAMP (buffer)),
      GST_TIME_ARGS (GST_BUFFER_DURATION (buffer)));

  gst_buffer_map (buffer, &map, GST_MAP_READ);
  res = gst_ts_shifter_push (ts, map.data, map.size);
  gst_buffer_unmap (buffer, &map);

  gst_buffer_unref (buffer);

  return res;
}

static inline gboolean
gst_ts_shifter_sink_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  GstTSShifter *ts = GST_TS_SHIFTER (parent);
  gboolean ret = TRUE;

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_EOS:
    {
      GST_CAT_LOG_OBJECT (ts_flow, ts, "received eos event");
      FLOW_MUTEX_LOCK (ts);
      ts->is_eos = TRUE;
      /* Ensure to unlock the pushing loop */
      FLOW_SIGNAL_ADD (ts);
      FLOW_MUTEX_UNLOCK (ts);
      gst_event_unref (event);
      break;
    }
    case GST_EVENT_SEGMENT:
    {
      GST_CAT_LOG_OBJECT (ts_flow, ts, "received newsegment event");
      FLOW_MUTEX_LOCK (ts);
      ts->unexpected = FALSE;
      FLOW_MUTEX_UNLOCK (ts);
      gst_event_unref (event);
      break;
    }
    case GST_EVENT_FLUSH_START:
    {
      GST_CAT_LOG_OBJECT (ts_flow, ts, "received flush start event");
      /* forward event */
      gst_pad_push_event (ts->srcpad, event);

      /* now unblock the chain function */
      FLOW_MUTEX_LOCK (ts);
      ts->srcresult = GST_FLOW_FLUSHING;
      ts->sinkresult = GST_FLOW_FLUSHING;
      /* unblock the loop and chain functions */
      FLOW_SIGNAL_ADD (ts);
      FLOW_MUTEX_UNLOCK (ts);

      /* make sure it pauses, this should happen since we sent
       * flush_start downstream. */
      gst_pad_pause_task (ts->srcpad);
      GST_CAT_LOG_OBJECT (ts_flow, ts, "loop stopped");
      break;
    }
    case GST_EVENT_FLUSH_STOP:
    {
      GST_CAT_LOG_OBJECT (ts_flow, ts, "received flush stop event");

      /* forward event */
      gst_pad_push_event (ts->srcpad, event);

      gst_ts_shifter_start (ts);

      FLOW_MUTEX_LOCK (ts);
      gst_event_replace (&ts->stream_start_event, NULL);
      ts->cur_bytes = 0;
      ts->srcresult = GST_FLOW_OK;
      ts->sinkresult = GST_FLOW_OK;
      ts->is_eos = FALSE;
      ts->unexpected = FALSE;
      gst_pad_start_task (ts->srcpad, (GstTaskFunction) gst_ts_shifter_loop,
          ts->srcpad, NULL);
      FLOW_MUTEX_UNLOCK (ts);
      break;
    }
    case GST_EVENT_STREAM_START:
    {
      gst_event_replace (&ts->stream_start_event, event);
      gst_event_unref (event);
      break;
    }
    case GST_EVENT_CAPS:
    {
      /* we swallow this event to make some upstream elements happy */
      gst_event_unref (event);
      break;
    }
    default:
    {
      GST_CAT_LOG_OBJECT (ts_flow, ts, "dropped event %s",
          GST_EVENT_TYPE_NAME (event));
      gst_event_unref (event);
      ret = FALSE;
      break;
    }
  }

  return ret;
}

static guint64
gst_ts_shifter_get_bytes_offset (GstTSShifter * ts, GstFormat format,
    GstSeekType type, gint64 start)
{
  guint64 offset;

  if (format != GST_FORMAT_BYTES) {
    GST_WARNING_OBJECT (ts, "can only seek in bytes");
    offset = -1;
  } else {
    GST_DEBUG_OBJECT (ts, "seeking at bytes %" G_GINT64_FORMAT " type %d",
        start, type);

    if (type == GST_SEEK_TYPE_SET) {
      offset = start;
    } else if (type == GST_SEEK_TYPE_END) {
      offset = gst_ts_cache_get_total_bytes_received (ts->cache) + start;
    } else {
      offset = -1;
    }
  }
  return offset;
}

static gboolean
gst_ts_shifter_handle_seek (GstTSShifter * ts, GstEvent * event)
{
  GstFormat format;
  GstSeekFlags flags;
  GstSeekType start_type, stop_type;
  gint64 start, stop;
  gdouble rate;
  guint64 offset = 0;
  gboolean ret = FALSE;

  gst_event_parse_seek (event, &rate, &format, &flags,
      &start_type, &start, &stop_type, &stop);

  if (!(flags & GST_SEEK_FLAG_FLUSH)) {
    GST_WARNING_OBJECT (ts, "we only support flushing seeks");
    goto beach;
  }

  offset = gst_ts_shifter_get_bytes_offset (ts, format, start_type, start);
  if (G_UNLIKELY (offset == (guint64) - 1
          || !gst_ts_cache_has_offset (ts->cache, offset))) {
    GST_WARNING_OBJECT (ts, "seek failed");
    goto beach;
  }

  /* remember the rate */
  ts->segment.rate = rate;
  ts->segment.flags |= GST_SEGMENT_FLAG_RESET;

  GST_DEBUG_OBJECT (ts, "seeking at offset %" G_GUINT64_FORMAT, offset);

  /* now unblock the loop function */
  FLOW_MUTEX_LOCK (ts);
  /* Flush start downstream to make sure loop is idle */
  gst_pad_push_event (ts->srcpad, gst_event_new_flush_start ());
  ts->srcresult = GST_FLOW_FLUSHING;
  /* unblock the loop function */
  FLOW_SIGNAL_ADD (ts);
  FLOW_MUTEX_UNLOCK (ts);

  /* make sure it pauses, this should happen since we sent
   * flush_start downstream. */
  gst_pad_pause_task (ts->srcpad);
  GST_DEBUG_OBJECT (ts, "loop stopped");
  /* Flush stop downstream to ensure that all pushed cache slots come back
   * to our control */
  gst_pad_push_event (ts->srcpad, gst_event_new_flush_stop (TRUE));

  /* Reconfigure the cache to handle the new offset */
  FLOW_MUTEX_LOCK (ts);
  gst_ts_cache_seek (ts->cache, offset);

  /* Restart the pushing loop */
  ts->srcresult = GST_FLOW_OK;
  ts->is_eos = FALSE;
  ts->unexpected = FALSE;
  ts->need_newsegment = TRUE;
  gst_pad_start_task (ts->srcpad, (GstTaskFunction) gst_ts_shifter_loop,
      ts->srcpad, NULL);
  FLOW_MUTEX_UNLOCK (ts);
  GST_DEBUG_OBJECT (ts, "loop started");
  ret = TRUE;
beach:
  return ret;
}

static inline gboolean
gst_ts_shifter_src_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  gboolean ret = TRUE;
  GstTSShifter *ts = GST_TS_SHIFTER (parent);

  if (G_UNLIKELY (ts == NULL)) {
    gst_event_unref (event);
    return FALSE;
  }

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_SEEK:
    {
      GST_CAT_LOG_OBJECT (ts_flow, ts, "received seek event");
      /* Do the seek ourself now */
      ret = gst_ts_shifter_handle_seek (ts, event);
      break;
    }
    case GST_EVENT_QOS:
    {
      break;
    }
    default:
      GST_CAT_LOG_OBJECT (ts_flow, ts, "dropped event %s",
          GST_EVENT_TYPE_NAME (event));
      ret = FALSE;
      break;
  }

  gst_event_unref (event);

  return ret;
}

static gboolean
gst_ts_shifter_query (GstElement * element, GstQuery * query)
{
  gboolean ret = TRUE;
  GstTSShifter *ts = GST_TS_SHIFTER (element);

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_POSITION:
    {
      gint64 pos = -1;
      GstFormat format;

      /* get format */
      gst_query_parse_position (query, &format, NULL);

      switch (format) {
        case GST_FORMAT_BYTES:
          pos = ts->cur_bytes;
          break;
        default:
          GST_WARNING_OBJECT (ts, "dropping query in %s format, don't "
              "know how to handle", gst_format_get_name (format));
          ret = FALSE;
          break;
      }
      /* set updated position */
      if (ret)
        gst_query_set_position (query, format, pos);
      break;
    }
    case GST_QUERY_DURATION:
    {
      GstFormat format;

      gst_query_parse_duration (query, &format, NULL);

      if (format == GST_FORMAT_BYTES) {
        GST_LOG_OBJECT (ts, "replying duration query with %" G_GUINT64_FORMAT,
            gst_ts_cache_get_total_bytes_received (ts->cache));
        gst_query_set_duration (query, GST_FORMAT_BYTES,
            gst_ts_cache_get_total_bytes_received (ts->cache));
        ret = TRUE;
      } else {
        ret = FALSE;
      }
      break;
    }
    case GST_QUERY_SEEKING:
    {
      GstFormat fmt;

      gst_query_parse_seeking (query, &fmt, NULL, NULL, NULL);
      if (fmt == GST_FORMAT_BYTES) {
        gst_query_set_seeking (query, fmt, TRUE, 0, -1);
        ret = TRUE;
      } else {
        ret = FALSE;
      }
      break;
    }
    case GST_QUERY_LATENCY:
    {
      gst_query_set_latency (query, FALSE, 0, -1);
      break;
    }

    case GST_QUERY_BUFFERING:
    {
      GstFormat format;
      guint64 bytes_begin, bytes_end;

      gst_query_parse_buffering_range (query, &format, NULL, NULL, NULL);
      if (format != GST_FORMAT_BYTES) {
        ret = FALSE;
        break;
      }

      gst_ts_cache_buffered_range (ts->cache, &bytes_begin, &bytes_end);
      gst_query_set_buffering_range (query, format, bytes_begin, bytes_end, -1);

      ret = TRUE;
      break;
    }
    default:
      ret = FALSE;
      break;
  }

  return ret;
}

/* sink currently only operates in push mode */
static inline gboolean
gst_ts_shifter_sink_activate (GstPad * pad, GstObject * parent, gboolean active)
{
  GstTSShifter *ts = GST_TS_SHIFTER (parent);

  if (active) {
    FLOW_MUTEX_LOCK (ts);
    GST_DEBUG_OBJECT (ts, "activating push mode");
    ts->srcresult = GST_FLOW_OK;
    ts->sinkresult = GST_FLOW_OK;
    ts->is_eos = FALSE;
    ts->unexpected = FALSE;
    FLOW_MUTEX_UNLOCK (ts);
  } else {
    /* unblock chain function */
    FLOW_MUTEX_LOCK (ts);
    GST_DEBUG_OBJECT (ts, "deactivating push mode");
    ts->srcresult = GST_FLOW_FLUSHING;
    ts->sinkresult = GST_FLOW_FLUSHING;
    gst_event_replace (&ts->stream_start_event, NULL);
    FLOW_MUTEX_UNLOCK (ts);
  }

  return TRUE;
}

/* src operating in push mode, we start a task on the source pad that pushes out
 * buffers from the cache */
static inline gboolean
gst_ts_shifter_src_activate (GstPad * pad, GstObject * parent, gboolean active)
{
  GstTSShifter *ts = GST_TS_SHIFTER (parent);
  gboolean ret = FALSE;

  if (active) {
    FLOW_MUTEX_LOCK (ts);
    GST_DEBUG_OBJECT (ts, "activating push mode");
    ts->srcresult = GST_FLOW_OK;
    ts->sinkresult = GST_FLOW_OK;
    ts->is_eos = FALSE;
    ts->unexpected = FALSE;
    ret =
        gst_pad_start_task (pad, (GstTaskFunction) gst_ts_shifter_loop, pad,
        NULL);
    FLOW_MUTEX_UNLOCK (ts);
  } else {
    /* unblock loop function */
    FLOW_MUTEX_LOCK (ts);
    GST_DEBUG_OBJECT (ts, "deactivating push mode");
    ts->srcresult = GST_FLOW_FLUSHING;
    ts->sinkresult = GST_FLOW_FLUSHING;
    /* the item add signal will unblock */
    FLOW_SIGNAL_ADD (ts);
    FLOW_MUTEX_UNLOCK (ts);

    /* step 2, make sure streaming finishes */
    ret = gst_pad_stop_task (pad);
  }

  return ret;
}

static gboolean
gst_ts_shifter_handle_sink_event (GstPad * pad, GstObject * parent,
    GstEvent * event)
{
  return gst_ts_shifter_sink_event (pad, parent, event);
}

static gboolean
gst_ts_shifter_handle_src_event (GstPad * pad, GstObject * parent,
    GstEvent * event)
{
  return gst_ts_shifter_src_event (pad, parent, event);
}

static gboolean
gst_ts_shifter_handle_src_query (GstPad * pad, GstObject * parent,
    GstQuery * query)
{
  return gst_ts_shifter_query (GST_ELEMENT (parent), query);
}

static gboolean
gst_ts_shifter_sink_activate_mode (GstPad * pad, GstObject * parent,
    GstPadMode mode, gboolean active)
{
  gboolean ret = FALSE;

  if (mode == GST_PAD_MODE_PUSH) {
    ret = gst_ts_shifter_sink_activate (pad, parent, active);
  }
  return ret;
}

static gboolean
gst_ts_shifter_src_activate_mode (GstPad * pad, GstObject * parent,
    GstPadMode mode, gboolean active)
{
  gboolean ret = FALSE;

  if (mode == GST_PAD_MODE_PUSH) {
    ret = gst_ts_shifter_src_activate (pad, parent, active);
  }
  return ret;
}

static GstStateChangeReturn
gst_ts_shifter_change_state (GstElement * element, GstStateChange transition)
{
  GstTSShifter *ts;
  GstStateChangeReturn ret = GST_STATE_CHANGE_SUCCESS;

  ts = GST_TS_SHIFTER (element);

  switch (transition) {
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      gst_ts_shifter_start (ts);
      gst_event_replace (&ts->stream_start_event, NULL);
      break;
    case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
      break;
    default:
      break;
  }

  if (ret == GST_STATE_CHANGE_FAILURE)
    return ret;

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);

  if (ret == GST_STATE_CHANGE_FAILURE)
    return ret;

  switch (transition) {
    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
    case GST_STATE_CHANGE_PAUSED_TO_READY:
    case GST_STATE_CHANGE_READY_TO_NULL:
      break;
    default:
      break;
  }

  return ret;
}

static void
gst_ts_shifter_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec)
{
  GstTSShifter *ts = GST_TS_SHIFTER (object);

  /* someone could change size here, and since this
   * affects the get/put funcs, we need to lock for safety. */
  FLOW_MUTEX_LOCK (ts);

  switch (prop_id) {
    case PROP_BACKING_STORE_FD:
      /* Has no effect until the timeshifer is restarted.  We take ownership. */
      close (ts->backing_store_fd);
      ts->backing_store_fd = g_value_get_int (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }

  FLOW_MUTEX_UNLOCK (ts);
}

static void
gst_ts_shifter_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec)
{
  GstTSShifter *ts = GST_TS_SHIFTER (object);

  FLOW_MUTEX_LOCK (ts);

  switch (prop_id) {
    case PROP_BACKING_STORE_FD:
      g_value_set_int (value, ts->backing_store_fd);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }

  FLOW_MUTEX_UNLOCK (ts);
}

static void
gst_ts_shifter_finalize (GObject * object)
{
  GstTSShifter *ts = GST_TS_SHIFTER (object);

  GST_DEBUG_OBJECT (ts, "finalizing ts_shifter");

  g_mutex_clear (&ts->flow_lock);
  g_cond_clear (&ts->buffer_add);

  g_free (ts->allocator_name);
  if (ts->cache) {
    gst_ts_cache_unref (ts->cache);
  }

  if (ts->backing_store_fd >= 0) {
    close (ts->backing_store_fd);
    ts->backing_store_fd = -1;
  }

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_ts_shifter_class_init (GstTSShifterClass * klass)
{
  GObjectClass *gclass = G_OBJECT_CLASS (klass);
  GstElementClass *eclass = GST_ELEMENT_CLASS (klass);

  /* GstElement related stuff */
  gst_element_class_add_pad_template (eclass,
      gst_static_pad_template_get (&src_factory));
  gst_element_class_add_pad_template (eclass,
      gst_static_pad_template_get (&sink_factory));

  gst_element_class_set_static_metadata (eclass,
      "Time Shift for MPEG TS streams",
      "Generic",
      "Provide time shift operations on MPEG TS streams",
      "Fluendo S.A. <support@fluendo.com>, "
      "YouView TV Ltd <william.manley@youview.com>");

  gclass->set_property = gst_ts_shifter_set_property;
  gclass->get_property = gst_ts_shifter_get_property;

  /* signals */
  /**
   * GstTSShifter::overrun:
   * @tsshifter: the shifter instance
   *
   * Reports that the ring buffer buffer became full (overrun).
   * A buffer is full if the total amount of data inside it (size) is higher
   * than the boundary value which can be set through the GObject properties.
   */
  gst_ts_shifter_signals[SIGNAL_OVERRUN] =
      g_signal_new ("overrun", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_FIRST,
      G_STRUCT_OFFSET (GstTSShifterClass, overrun), NULL, NULL,
      g_cclosure_marshal_VOID__VOID, G_TYPE_NONE, 0);

  /* properties */
  g_object_class_install_property (gclass, PROP_BACKING_STORE_FD,
      g_param_spec_int ("backing-store-fd",
          "Backing store FD",
          "File descriptor of a file in which to store video stream",
          -1, G_MAXINT, -1, G_PARAM_READWRITE | G_PARAM_CONSTRUCT));

  /* set several parent class virtual functions */
  gclass->finalize = gst_ts_shifter_finalize;

  eclass->change_state = GST_DEBUG_FUNCPTR (gst_ts_shifter_change_state);
  eclass->query = GST_DEBUG_FUNCPTR (gst_ts_shifter_query);
}

static void
gst_ts_shifter_init (GstTSShifter * ts)
{
  ts->sinkpad = gst_pad_new_from_static_template (&sink_factory, "sink");

  gst_pad_set_chain_function (ts->sinkpad,
      GST_DEBUG_FUNCPTR (gst_ts_shifter_chain));
  gst_pad_set_activatemode_function (ts->sinkpad,
      GST_DEBUG_FUNCPTR (gst_ts_shifter_sink_activate_mode));
  gst_pad_set_event_function (ts->sinkpad,
      GST_DEBUG_FUNCPTR (gst_ts_shifter_handle_sink_event));
  gst_element_add_pad (GST_ELEMENT (ts), ts->sinkpad);

  ts->srcpad = gst_pad_new_from_static_template (&src_factory, "src");

  gst_pad_set_activatemode_function (ts->srcpad,
      GST_DEBUG_FUNCPTR (gst_ts_shifter_src_activate_mode));
  gst_pad_set_event_function (ts->srcpad,
      GST_DEBUG_FUNCPTR (gst_ts_shifter_handle_src_event));
  gst_pad_set_query_function (ts->srcpad,
      GST_DEBUG_FUNCPTR (gst_ts_shifter_handle_src_query));
  gst_element_add_pad (GST_ELEMENT (ts), ts->srcpad);

  /* set default values */
  ts->cur_bytes = 0;

  ts->srcresult = GST_FLOW_FLUSHING;
  ts->sinkresult = GST_FLOW_FLUSHING;
  ts->is_eos = FALSE;
  ts->need_newsegment = TRUE;
  ts->is_leaking = FALSE;

  g_mutex_init (&ts->flow_lock);
  g_cond_init (&ts->buffer_add);

  ts->allocator_name = NULL;

  ts->cache = NULL;

  ts->backing_store_fd = -1;

  GST_DEBUG_OBJECT (ts, "initialized time shifter");
}
