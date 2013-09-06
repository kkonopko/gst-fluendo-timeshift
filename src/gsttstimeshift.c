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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gsttsshifter.h"
#include "gsttsshifterbin.h"
#include "gsttsseeker.h"
#include "gsttsindexer.h"
#include "gsttsstamper.h"

GST_DEBUG_CATEGORY (ts_flow);
GST_DEBUG_CATEGORY (ts_shifter);
GST_DEBUG_CATEGORY (ts_shifterbin);

static gboolean
plugin_init (GstPlugin * plugin)
{

  GST_DEBUG_CATEGORY_INIT (ts_flow, "ts_shifter_flow", 0,
      "dataflow in the Time Shift element");

  GST_DEBUG_CATEGORY_INIT (ts_shifter, "ts_shifter", 0,
      "MPEG TS Time Shifting");

  GST_DEBUG_CATEGORY_INIT (ts_shifterbin, "ts_shifterbin", 0,
      "MPEG Time Shifting bin");

  if (!gst_element_register (plugin, "tsshifter", GST_RANK_NONE,
          gst_ts_shifter_get_type ()))
    return FALSE;

  if (!gst_element_register (plugin, "tsshifterbin", GST_RANK_NONE,
          gst_ts_shifter_bin_get_type ()))
    return FALSE;

  if (!gst_element_register (plugin, "tsseeker", GST_RANK_NONE,
          gst_ts_seeker_get_type ()))
    return FALSE;

  if (!gst_element_register (plugin, "tsindexer", GST_RANK_NONE,
          gst_ts_indexer_get_type ()))
    return FALSE;

  if (!gst_element_register (plugin, "tsstamper", GST_RANK_NONE,
          gst_ts_stamper_get_type ()))
    return FALSE;

  return TRUE;
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR, GST_VERSION_MINOR,
    tstimeshift, "MPEG TS Time Shift element",
    plugin_init, VERSION, "LGPL", PACKAGE_NAME,
    "http://www.fluendo.com, http://www.youview.com");
