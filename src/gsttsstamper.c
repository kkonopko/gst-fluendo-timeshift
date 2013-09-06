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
 * Free Software Foundation, Inc., 51 Franklin Street, Suite 500,
 * Boston, MA 02110-1335, USA.
 */

/**
 * SECTION:element-gsttsstamper
 *
 * Restamps PCRs in the input stream so that output stream will contain
 * PCRs in a required range. This is only useful for tests because it
 * doesn't modify PTSes and DTSes so the produced stream is not valid
 * from the decoder point of view. But since timeshifter relies on the PCRs
 * this is good enough for our tests.
 *
 * This element is used by tests for tsshifterbin.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include <gst/gstelement.h>
#include <gst/base/gstbasetransform.h>
#include "gsttsstamper.h"

GST_DEBUG_CATEGORY_STATIC (gst_ts_stamper_debug_category);
#define GST_CAT_DEFAULT gst_ts_stamper_debug_category

#define DEFAULT_DELTA           500

#define TS_PACKET_SYNC_CODE     0x47
#define TS_MIN_PACKET_SIZE      188
#define TS_MAX_PACKET_SIZE      208
#define INVALID_PID             -1

#define CLOCK_BASE 9LL
/* 90Khz base clock*/
#define PCR_CLOCK_FREQ (CLOCK_BASE * 10000)
/* 27Mhz extension clock */
#define PCR_CLOCK_FREQ_EXT (27000000ULL)
#define PCR_CLOCK_RATIO (PCR_CLOCK_FREQ_EXT/PCR_CLOCK_FREQ)

/* multiply by PCR_CLOCK_RATIO to get it into 27Mhz timer unit*/
#define MAX_PCR (PCR_CLOCK_RATIO*0x1ffffffffULL)


#define GSTTIME_TO_MPEGTIME(time) (gst_util_uint64_scale ((time), \
            CLOCK_BASE, GST_MSECOND/10))

/* prototypes */


static void gst_ts_stamper_set_property (GObject * object,
    guint property_id, const GValue * value, GParamSpec * pspec);
static void gst_ts_stamper_get_property (GObject * object,
    guint property_id, GValue * value, GParamSpec * pspec);
static void gst_ts_stamper_finalize (GObject * object);
static GstFlowReturn
gst_ts_stamper_transform_ip (GstBaseTransform * trans, GstBuffer * buf);
static void gst_ts_stamper_modify_time (GstTSStamper *
    base, guint8 * data, gsize size);
static inline GstPidTracker *gst_ts_stamper_add_pid (GstTSStamper * ts,
    guint16 pid);
static inline GstPidTracker *gst_ts_stamper_find_pid (GstTSStamper * ts,
    guint16 pid);

enum
{
  PROP_0,
  PROP_PCR_PID,
  PROP_PCR_MIN,
  PROP_PCR_MAX,
};

/* pad templates */

static GstStaticPadTemplate gst_ts_stamper_sink_template =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/mpegts")
    );

static GstStaticPadTemplate gst_ts_stamper_src_template =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/mpegts")
    );


/* class initialization */

G_DEFINE_TYPE (GstTSStamper, gst_ts_stamper, GST_TYPE_BASE_TRANSFORM);
#define parent_class gst_ts_stamper_parent_class

static void
gst_ts_stamper_class_init (GstTSStamperClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstBaseTransformClass *base_transform_class =
      GST_BASE_TRANSFORM_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&gst_ts_stamper_sink_template));
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&gst_ts_stamper_src_template));

  gst_element_class_set_static_metadata (element_class,
      "Stamper for MPEG-TS streams", "Generic",
      "Restamps in-place mpegts packets with desired PCR value.",
      "Fluendo S.A. <support@fluendo.com>, "
      "William Manley <will@williammanley.net>,"
      "Mariusz Buras <mariusz.buras@gmail.com>");

  GST_DEBUG_CATEGORY_INIT (gst_ts_stamper_debug_category,
      "gst_ts_stamper", 0, "Stamper for MPEG-TS streams");


  gobject_class->set_property = gst_ts_stamper_set_property;
  gobject_class->get_property = gst_ts_stamper_get_property;
  gobject_class->finalize = gst_ts_stamper_finalize;
  base_transform_class->transform_ip =
      GST_DEBUG_FUNCPTR (gst_ts_stamper_transform_ip);

  g_object_class_install_property (gobject_class, PROP_PCR_PID,
      g_param_spec_int ("pcr-pid", "PCR pid",
          "Defines the PCR pid to restamp the time (-1 = restamp everything)",
          INVALID_PID, 0x1fff, INVALID_PID,
          G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_PCR_MIN,
      g_param_spec_uint64 ("pcr-min", "PCR min",
          "Defines the lower PCR value to which PCR will wrap",
          0, MAX_PCR, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_PCR_MAX,
      g_param_spec_uint64 ("pcr-max", "PCR min",
          "Defines the upper PCR value to which PCR will wrap",
          0, MAX_PCR, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
}

static void
gst_ts_stamper_init (GstTSStamper * stamper)
{
  GstBaseTransform *base = GST_BASE_TRANSFORM (stamper);
  gst_base_transform_set_passthrough (base, TRUE);

  stamper->num_tracked_pids = 1;
  stamper->tracked_pids = g_malloc (sizeof (GstPidTracker));
  stamper->tracked_pids->pcr_pid = INVALID_PID;

  stamper->pcr_min = 0;
  stamper->pcr_max = MAX_PCR;
}

static void
gst_ts_stamper_set_property (GObject * object, guint property_id,
    const GValue * value, GParamSpec * pspec)
{
  GstTSStamper *stamper = GST_TS_STAMPER (object);

  switch (property_id) {
    case PROP_PCR_PID:
      gst_ts_stamper_add_pid (stamper, g_value_get_int (value));
      break;
    case PROP_PCR_MIN:
      stamper->pcr_min = g_value_get_uint64 (value) * PCR_CLOCK_RATIO;
      break;
    case PROP_PCR_MAX:
      stamper->pcr_max = g_value_get_uint64 (value) * PCR_CLOCK_RATIO;
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

static void
gst_ts_stamper_get_property (GObject * object, guint property_id,
    GValue * value, GParamSpec * pspec)
{
  GstTSStamper *stamper = GST_TS_STAMPER (object);

  switch (property_id) {
    case PROP_PCR_MIN:
      g_value_set_uint64 (value, stamper->pcr_min / PCR_CLOCK_RATIO);
      break;
    case PROP_PCR_MAX:
      g_value_set_uint64 (value, stamper->pcr_max / PCR_CLOCK_RATIO);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

void
gst_ts_stamper_finalize (GObject * object)
{
  GstTSStamper *stamper = GST_TS_STAMPER (object);

  /* clean up object here */

  g_free (stamper->tracked_pids);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static GstFlowReturn
gst_ts_stamper_transform_ip (GstBaseTransform * trans, GstBuffer * buf)
{
  GstTSStamper *stamper = GST_TS_STAMPER (trans);

  GstFlowReturn ret = GST_FLOW_OK;
  GstMapInfo map;

  /* collect time info from that buffer */
  if (!gst_buffer_map (buf, &map, GST_MAP_READWRITE)) {
    ret = GST_FLOW_NOT_SUPPORTED;
    goto out;
  }

  gst_ts_stamper_modify_time (stamper, map.data, map.size);

  gst_buffer_unmap (buf, &map);

out:
  return ret;
}

static inline GstPidTracker *
gst_ts_stamper_add_pid (GstTSStamper * ts, guint16 pid)
{
  GstPidTracker *pid_slot = NULL;
  /* we've got a new pid */
  guint32 pid_index = ts->num_tracked_pids++;
  ts->tracked_pids =
      g_realloc (ts->tracked_pids,
      sizeof (GstPidTracker) * ts->num_tracked_pids);
  ts->tracked_pids[pid_index].pcr_pid = INVALID_PID;
  pid_slot = &ts->tracked_pids[pid_index - 1];
  pid_slot->pcr_pid = pid;
  pid_slot->first_pcr = -1;
  pid_slot->last_pcr = -1;
  pid_slot->wrap_pcr = 0;
  pid_slot->pcr_delta = 0;
  return pid_slot;
}

static inline GstPidTracker *
gst_ts_stamper_find_pid (GstTSStamper * ts, guint16 pid)
{
  size_t i = 0;
  for (i = 0; ts->tracked_pids[i].pcr_pid != INVALID_PID; ++i) {
    if (pid == ts->tracked_pids[i].pcr_pid) {
      return &ts->tracked_pids[i];
    }
  }
  return NULL;
}

static inline gint64
gst_ts_stamper_extract_pcr (guint8 * data)
{
  guint64 pcr = 0;
  guint64 pcr1 = GST_READ_UINT32_BE (data);
  guint64 pcr2 = GST_READ_UINT16_BE (data + 4);
  guint64 pcr_ext = (pcr2 & 0x01ff) % PCR_CLOCK_RATIO;
  pcr = pcr1 << 1;
  pcr |= (pcr2 & 0x8000) >> 15;

  pcr = (pcr * PCR_CLOCK_RATIO + pcr_ext);

  return pcr;
}

static inline void
gst_ts_stamper_replace_pcr (guint8 * data, guint64 pcr)
{
  guint64 new_pcr = pcr / PCR_CLOCK_RATIO;
  guint32 pcr1 = new_pcr >> 1;
  guint32 pcr2 = ((new_pcr & 0x1) << 15) | (pcr % PCR_CLOCK_RATIO);

  GST_WRITE_UINT32_BE (data, pcr1);
  GST_WRITE_UINT16_BE (data + 4, pcr2);
}

static inline void
gst_ts_stamper_parse_pcr (GstTSStamper * ts, guint8 * data)
{
  guint16 pid;
  guint64 pcr = (guint64) - 1, new_pcr = (guint64) - 1;
  GstPidTracker *pid_slot = NULL;

  if (TS_PACKET_SYNC_CODE == data[0]) {
    /* Check Adaptation field, if it == b10 or b11 */
    if (data[3] & 0x20) {

      /* Check PID Match */
      pid = GST_READ_UINT16_BE (data + 1);
      pid &= 0x1fff;

      if (!(pid_slot = gst_ts_stamper_find_pid (ts, pid))) {
        pid_slot = gst_ts_stamper_add_pid (ts, pid);
      }

      /* Check Adaptation field size */
      /* and if PCR is present */
      if (data[5] & 0x10) {

        pcr = gst_ts_stamper_extract_pcr (data + 6);

        if (pcr == -1) {
          return;
        }

        if (!(pid_slot = gst_ts_stamper_find_pid (ts, pid))) {
          pid_slot = gst_ts_stamper_add_pid (ts, pid);
        }

        /* fix potential discontinuities in the orginal PCR */

        if (pid_slot->last_pcr != -1 && pcr < pid_slot->last_pcr) {
          if (pid_slot->last_pcr >
              (MAX_PCR - GSTTIME_TO_MPEGTIME (2 * GST_SECOND))) {
            pid_slot->wrap_pcr += MAX_PCR;
          } else {
            gint64 aproximate_gap = (gint64) pid_slot->pcr_delta - (gint64) pcr;
            if (aproximate_gap > 0) {
              pid_slot->wrap_pcr += pid_slot->last_pcr + aproximate_gap;
            } else {
              pid_slot->wrap_pcr += pid_slot->last_pcr + pcr;
            }
          }
        } else {
          pid_slot->pcr_delta = pcr - pid_slot->last_pcr;
          pid_slot->last_pcr = pcr;
        }

        /* caltulate restamped PCR value */
        if (pid_slot->first_pcr == -1) {
          pid_slot->first_pcr = pcr;
        }

        new_pcr = ts->pcr_min +
            (pcr - pid_slot->first_pcr + pid_slot->wrap_pcr) % (ts->pcr_max -
            ts->pcr_min);

        gst_ts_stamper_replace_pcr (data + 6, new_pcr);

      }
    }
  }
}

static inline void
gst_ts_stamper_modify_time (GstTSStamper * ts, guint8 * data, gsize size)
{
  gint i = 0;

  /* 12 bytes is the minimum mpegts header that will do for PCR replacement */
  while ((i + 12) <= size) {
    if (TS_PACKET_SYNC_CODE == data[i]) {
      /* Check the next SYNC byte for all packets except the last packet
       * in a buffer... */
      if (size - i >= TS_MIN_PACKET_SIZE) {
        gst_ts_stamper_parse_pcr (ts, data + i);
        i += TS_MIN_PACKET_SIZE;
      }
    } else {
      i++;                      /* next byte in buffer until we find sync */
    }
  }
}
