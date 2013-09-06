/* GStreamer MPEG TS Time Shifting
 * Copyright (C) 2011 Fluendo S.A. <support@fluendo.com>
 *               2013 YouView TV Ltd. <krzysztof.konopko@youview.com>
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

#include <gst/gst.h>

#ifndef __TS_CACHE_H__
#define __TS_CACHE_H__

G_BEGIN_DECLS
#define CACHE_SLOT_SIZE (32 * 1024)     /* Ring Buffer data unit size */
/**
 * GstTSCache:
 *
 * Opaque data cache.
 *
 * Use the accessor functions to get the stored values.
 *
 */
typedef struct _GstTSCache GstTSCache;

GstTSCache *gst_ts_cache_new (int fd);

GstTSCache *gst_ts_cache_ref (GstTSCache * cache);
void gst_ts_cache_unref (GstTSCache * cache);

gboolean gst_ts_cache_push (GstTSCache * cache, guint8 * data, gsize size);
GstBuffer *gst_ts_cache_pop (GstTSCache * cache, gboolean drain);

gboolean gst_ts_cache_has_offset (GstTSCache * cache, guint64 offset);
guint64 gst_ts_cache_get_total_bytes_received (GstTSCache * cache);
gboolean gst_ts_cache_seek (GstTSCache * cache, guint64 offset);

gboolean gst_ts_cache_is_empty (GstTSCache * cache);
guint64 gst_ts_cache_fullness (GstTSCache * cache);

void gst_ts_cache_buffered_range (GstTSCache * cache, guint64 * begin, guint64 * end);
G_END_DECLS
#endif /* __TS_CACHE_H__ */
