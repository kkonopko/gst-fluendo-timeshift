/* GStreamer MPEG TS Time Shifting
 * Copyright (C) 2011 Fluendo S.A. <support@fluendo.com>
 * Copyright (C) 2013 YouView TV Ltd. <william.manley@youview.com>
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

#ifndef _GST_TS_STAMPER_H_
#define _GST_TS_STAMPER_H_

#include <gst/base/gstbasetransform.h>

G_BEGIN_DECLS
#define GST_TYPE_TS_STAMPER   (gst_ts_stamper_get_type())
#define GST_TS_STAMPER(obj)   (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_TS_STAMPER,GstTSStamper))
#define GST_TS_STAMPER_CLASS(klass)   (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_TS_STAMPER,GstTSStamperClass))
#define GST_IS_TS_STAMPER(obj)   (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_TS_STAMPER))
#define GST_IS_TS_STAMPER_CLASS(obj)   (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_TS_STAMPER))
typedef struct _GstTSStamper GstTSStamper;
typedef struct _GstTSStamperClass GstTSStamperClass;
typedef struct _GstPidTracker GstPidTracker;

struct _GstPidTracker
{
  gint64 first_pcr;
  gint64 last_pcr;
  gint64 wrap_pcr;
  gint32 pcr_delta;
  gint16 pcr_pid;
};

struct _GstTSStamper
{
  GstBaseTransform base_ts_stamper;
 
  /* PCR tracking */

  guint64 pcr_min;
  guint64 pcr_max;
  
  guint32 num_tracked_pids;
  GstPidTracker* tracked_pids;
};

struct _GstTSStamperClass
{
  GstBaseTransformClass base_ts_stamper_class;
};

GType gst_ts_stamper_get_type (void);

G_END_DECLS
#endif
