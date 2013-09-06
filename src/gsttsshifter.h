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

#ifndef __GST_TS_SHIFTER_H__
#define __GST_TS_SHIFTER_H__

#include "tscache.h"

G_BEGIN_DECLS
#define GST_TS_SHIFTER_TYPE \
  (gst_ts_shifter_get_type())
#define GST_TS_SHIFTER(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TS_SHIFTER_TYPE,GstTSShifter))
#define GST_TS_SHIFTER_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TS_SHIFTER_TYPE,GstTSShifterClass))
#define GST_IS_TS_SHIFTER(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TS_SHIFTER_TYPE))
#define GST_IS_TS_SHIFTER_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TS_SHIFTER_TYPE))
#define GST_TS_SHIFTER_CAST(obj) \
  ((GstTSShifter *)(obj))
typedef struct _GstTSShifter GstTSShifter;
typedef struct _GstTSShifterClass GstTSShifterClass;

struct _GstTSShifter
{
  GstElement element;

  /*< private > */
  GstPad *sinkpad;
  GstPad *srcpad;

  /* Segment */
  GstSegment segment;

  /* flowreturn when srcpad is paused */
  GstFlowReturn srcresult;
  GstFlowReturn sinkresult;
  gboolean is_eos;
  gboolean unexpected;
  gboolean need_newsegment;
  gboolean is_leaking;

  /* the cache of data we're keeping our hands on */
  GstTSCache *cache;

  guint cur_bytes;              /* current position in bytes  */

  GMutex flow_lock;             /* lock for flow control */
  GCond buffer_add;             /* signals buffers added to the cache */

  gchar *allocator_name;

  GstEvent *stream_start_event;

  int backing_store_fd;
};

struct _GstTSShifterClass
{
  GstElementClass parent_class;

  /* signals */
  void (*overrun) (GstTSShifter *tsshifter);
};

GType gst_ts_shifter_get_type (void);

G_END_DECLS
#endif /* __GST_TS_SHIFTER_H__ */
