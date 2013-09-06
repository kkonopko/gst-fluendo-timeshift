/* GStreamer MPEG TS Time Shifting
 * Copyright (C) 2013 YouView TV Ltd. <krzysztof.konopko@youview.com>
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

#include "gsttsshifterbin.h"
#include "tscache.h"
#include "tsindex.h"

GST_DEBUG_CATEGORY_EXTERN (ts_shifterbin);
#define GST_CAT_DEFAULT ts_shifterbin

G_DEFINE_TYPE (GstTSShifterBin, gst_ts_shifter_bin, GST_TYPE_BIN);

#define DEFAULT_MIN_CACHE_SIZE  (4 * CACHE_SLOT_SIZE)   /* 4 cache slots */
#define DEFAULT_CACHE_SIZE      (32 * 1024 * 1024)      /* 32 MB */

enum
{
  PROP_0,
  PROP_BACKING_STORE_FD,
  PROP_LAST
};

static void gst_ts_shifter_bin_handle_message (GstBin * bin, GstMessage * msg);

static GstStaticPadTemplate srctemplate = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/mpegts"));

static GstStaticPadTemplate sinktemplate = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/mpegts"));

static void
gst_ts_shifter_bin_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec)
{
  GstTSShifterBin *ts_bin = GST_TS_SHIFTER_BIN (object);

  switch (prop_id) {
    case PROP_BACKING_STORE_FD:
      /* Forward directly onto the cache */
      g_object_set_property (G_OBJECT (ts_bin->timeshifter),
          pspec->name, value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_ts_shifter_bin_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec)
{
  GstTSShifterBin *ts_bin = GST_TS_SHIFTER_BIN (object);

  switch (prop_id) {
    case PROP_BACKING_STORE_FD:
      /* Forward directly onto the cache */
      g_object_get_property (G_OBJECT (ts_bin->timeshifter),
          pspec->name, value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_ts_shifter_bin_class_init (GstTSShifterBinClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;
  GstBinClass *gstbin_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gstelement_class = GST_ELEMENT_CLASS (klass);
  gstbin_class = GST_BIN_CLASS (klass);

  gobject_class->set_property = gst_ts_shifter_bin_set_property;
  gobject_class->get_property = gst_ts_shifter_bin_get_property;

  g_object_class_install_property (gobject_class, PROP_BACKING_STORE_FD,
      g_param_spec_int ("backing-store-fd",
          "Backing store FD",
          "File descriptor of a file in which to store video stream",
          -1, G_MAXINT, -1, G_PARAM_READWRITE | G_PARAM_CONSTRUCT));

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&srctemplate));

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&sinktemplate));

  gstbin_class->handle_message =
      GST_DEBUG_FUNCPTR (gst_ts_shifter_bin_handle_message);

  gst_element_class_set_static_metadata (gstelement_class,
      "Time Shift + TS parser for MPEG TS streams", "Generic/Bin",
      "Provide time shift operations on MPEG TS streams",
      "Krzysztof Konopko <krzysztof.konopko@youview.com>");
}

static void
mirror_pad (GstElement * element, const gchar * static_pad_name, GstBin * bin)
{
  GstPad *orig_pad, *ghost_pad;

  orig_pad = gst_element_get_static_pad (element, static_pad_name);
  g_return_if_fail (orig_pad);

  ghost_pad = gst_ghost_pad_new (static_pad_name, orig_pad);
  gst_object_unref (orig_pad);
  g_return_if_fail (ghost_pad);

  g_return_if_fail (gst_element_add_pad (GST_ELEMENT (bin), ghost_pad));
}

static void
gst_element_clear (GstElement ** elem)
{
  g_return_if_fail (elem);
  if (*elem) {
    g_object_unref (G_OBJECT (*elem));
    *elem = NULL;
  }
}

static void
gst_ts_shifter_bin_init (GstTSShifterBin * ts_bin)
{
  GstIndex *index = NULL;
  GstBin *bin = GST_BIN (ts_bin);

  ts_bin->parser = gst_element_factory_make ("tsparse", "parser");
  ts_bin->indexer = gst_element_factory_make ("tsindexer", "indexer");
  ts_bin->timeshifter = gst_element_factory_make ("tsshifter", "timeshifter");
  ts_bin->seeker = gst_element_factory_make ("tsseeker", "seeker");
  if (!ts_bin->parser || !ts_bin->indexer || !ts_bin->timeshifter
      || !ts_bin->seeker)
    goto error;

  gst_bin_add_many (bin, ts_bin->parser, ts_bin->indexer, ts_bin->timeshifter,
      ts_bin->seeker, NULL);
  if (!gst_element_link_many (ts_bin->parser, ts_bin->indexer,
          ts_bin->timeshifter, ts_bin->seeker, NULL)) {
    g_warning ("Failed to link pipeline in timeshifter bin");
    goto error;
  }

  index = gst_index_factory_make ("memindex");
  g_object_set (G_OBJECT (ts_bin->indexer), "index", index, NULL);
  g_object_set (G_OBJECT (ts_bin->seeker), "index", index, NULL);
  g_object_unref (index);

  mirror_pad (ts_bin->parser, "sink", bin);
  mirror_pad (ts_bin->seeker, "src", bin);

  return;
error:
  gst_element_clear (&ts_bin->parser);
  gst_element_clear (&ts_bin->indexer);
  gst_element_clear (&ts_bin->timeshifter);
  gst_element_clear (&ts_bin->seeker);
}

/* gets a real pad peer, ie. the that is not a proxy */

static GstPad *
gst_ts_shifter_get_real_peer_pad (GstPad * pad, GstElement * common_parent)
{
  GstPad *peer = gst_pad_get_peer (pad);

  if (GST_IS_PROXY_PAD (peer)) {
    /* it's a proxypad so find real pad (which will be a ghostpad) */
    GstIterator *pI = gst_pad_iterate_internal_links (peer);
    GValue realpeer = { 0, };
    GstIteratorResult res;
    res = gst_iterator_next (pI, &realpeer);
    gst_iterator_free (pI);
    gst_object_unref (peer);
    if (res != GST_ITERATOR_OK) {
      return NULL;
    }

    peer = GST_PAD (g_value_get_object (&realpeer));
    gst_object_ref (peer);
    g_value_unset (&realpeer);
  }

  return peer;
}

static GstPadProbeReturn
gst_ts_shifter_pad_event_cb (GstPad * pad, GstPadProbeInfo * info,
    gpointer user_data)
{
  GstElement *toremove = GST_ELEMENT (user_data);
  GstElement *parentbin = NULL;
  GstPad *upstreampeer = NULL;
  GstPad *downstreampeer = NULL;
  GstPad *srcpad, *sinkpad;

  if (GST_EVENT_TYPE (GST_PAD_PROBE_INFO_DATA (info)) != GST_EVENT_EOS)
    return GST_PAD_PROBE_OK;

  parentbin = GST_ELEMENT (gst_element_get_parent (toremove));

  srcpad = gst_element_get_static_pad (toremove, "src");
  downstreampeer = gst_ts_shifter_get_real_peer_pad (srcpad, parentbin);
  gst_object_unref (srcpad);

  sinkpad = gst_element_get_static_pad (toremove, "sink");
  upstreampeer = gst_ts_shifter_get_real_peer_pad (sinkpad, parentbin);
  gst_object_unref (sinkpad);

  gst_element_set_state (toremove, GST_STATE_NULL);

  /* remove unlinks automatically */
  GST_DEBUG_OBJECT (parentbin, "removing %" GST_PTR_FORMAT, toremove);
  gst_bin_remove (GST_BIN (parentbin), toremove);

  GST_DEBUG_OBJECT (toremove, "linking..");

  if (!GST_IS_GHOST_PAD (upstreampeer)
      && !GST_IS_GHOST_PAD (downstreampeer)) {
    gst_pad_link (upstreampeer, downstreampeer);
  } else if (GST_IS_GHOST_PAD (upstreampeer)
      && !GST_IS_GHOST_PAD (downstreampeer)) {
    /* handle the case when we removed an element at the begining of a bin */
    if (!gst_ghost_pad_set_target (GST_GHOST_PAD (upstreampeer),
            downstreampeer)) {
      GST_ERROR ("Couldn't set target on ghost pad.");
    }
  } else if (!GST_IS_GHOST_PAD (upstreampeer)
      && GST_IS_GHOST_PAD (downstreampeer)) {
    /* handle the case when we removed an element at the end of a bin */
    if (!gst_ghost_pad_set_target (GST_GHOST_PAD (downstreampeer),
            upstreampeer)) {
      GST_ERROR ("Couldn't set target on ghost pad.");
    }
  } else {
    GST_ERROR ("Couldn't connect ghost pads. Perhaps you're trying to remove"
        " the last element from the bin?");
  }
  gst_object_unref (downstreampeer);
  gst_object_unref (upstreampeer);

  GST_DEBUG_OBJECT (parentbin, "done");
  gst_object_unref (parentbin);

  return GST_PAD_PROBE_DROP;
}

static GstPadProbeReturn
gst_ts_shifter_padblocked_cb (GstPad * pad, GstPadProbeInfo * info,
    gpointer user_data)
{
  GstElement *toremove = GST_ELEMENT (user_data);
  GstPad *srcpad, *sinkpad;

  GST_DEBUG_OBJECT (pad, "pad is blocked now");

  /* install new probe for EOS */
  srcpad = gst_element_get_static_pad (toremove, "src");
  gst_pad_add_probe (srcpad, GST_PAD_PROBE_TYPE_BLOCK |
      GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, gst_ts_shifter_pad_event_cb,
      user_data, NULL);
  gst_object_unref (srcpad);

  sinkpad = gst_element_get_static_pad (toremove, "sink");
  gst_pad_send_event (sinkpad, gst_event_new_eos ());
  gst_object_unref (sinkpad);
  /* Remove ref added in gst_ts_shifter_remove_element */
  gst_object_unref (toremove);

  return GST_PAD_PROBE_REMOVE;
}

/*
 * A helper function that removes an element from a live pipeline/bin.
 * It can be used while data is flowing through the pipeline.
 */
static void
gst_ts_shifter_remove_element (GstElement * toremove)
{
  GstPad *sinkpad = gst_element_get_static_pad (toremove, "sink");
  GstPad *blockpad = gst_pad_get_peer (sinkpad);
  gst_object_unref (sinkpad);

  /* 
   * Add ref to make sure the element will be alive throught the process.
   * Reference will be dropped in gst_ts_shifter_padblocked_cb when element
   * will be already removed.
   */
  gst_object_ref (toremove);

  gst_pad_add_probe (blockpad, GST_PAD_PROBE_TYPE_BLOCK_DOWNSTREAM,
      gst_ts_shifter_padblocked_cb, (gpointer) toremove, NULL);

  gst_object_unref (blockpad);
}

static void
gst_ts_shifter_bin_handle_message (GstBin * bin, GstMessage * msg)
{
  GstTSShifterBin *ts_bin = GST_TS_SHIFTER_BIN (bin);

  if (gst_message_has_name (msg, "pmt")) {
    guint pcr_pid;

    const GstStructure *gs = gst_message_get_structure (msg);

    if (!gst_structure_get_uint (gs, "pcr-pid", &pcr_pid)) {
      GST_ERROR ("Cannot extract PCR PID");
    }

    GST_DEBUG ("Setting PCR PID: %u", pcr_pid);
    g_object_set (ts_bin->indexer, "pcr-pid", pcr_pid, NULL);

    gst_ts_shifter_remove_element (ts_bin->parser);
    ts_bin->parser = NULL;
  }

  GST_BIN_CLASS (gst_ts_shifter_bin_parent_class)
      ->handle_message (bin, msg);
}
