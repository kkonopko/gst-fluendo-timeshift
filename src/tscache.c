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

#define _GNU_SOURCE

#ifdef HAVE_CONFIG
#include "config.h"
#endif

#include "tscache.h"

#include <stdio.h>
#include <glib/gstdio.h>
#include <string.h>

/* For sync_file_range */
#include <sys/types.h>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>

GST_DEBUG_CATEGORY_EXTERN (ts_flow);
#define GST_CAT_DEFAULT (ts_flow)

#define DEBUG 0
#define DEBUG_RINGBUFFER 0

#define INVALID_OFFSET ((guint64) -1)

typedef struct _Slot Slot;
typedef struct _SlotMeta SlotMeta;

#define SLOT_META_INFO  (gst_slot_meta_get_info())
#define gst_buffer_get_slot_meta(b) ((SlotMeta*)gst_buffer_get_meta((b),SLOT_META_INFO))

/*
 * Page cache management
 *
 * The timeshift cache takes care to avoid taking up too much of the linux
 * page cache as it has fairly predictible read/write behaviour.  We can say:
 *
 *  * Once a page has been read it is not likely to be read again.
 *  * Once a page has been written it will not be written again until the
 *    timeshifter wraps
 *
 * We can use sync_file_range and posix_fadvise to tell the kernel to drop the
 * pages from cache.  We always do this for pages that have just been read.
 * We also wish to do this for pages that have been written but are not going
 * to be read for a while (e.g. while timeshifting) while still giving the
 * kernel some time to write the pages out to disk before we block waiting for
 * it to do so.  We don't want to ask the kernel to drop a page from cache if
 * it's just about to be read.
 *
 * So we define two values:
 *
 *  * PAGE_SYNC_TIME_SLOTS - how long do we want to give the kernel to write
 *    newly written pages to disk before we block until pages are successfully
 *    written.
 *  * READ_KEEP_PAGE_SLOTS - Don't throw pages away if the read head is going
 *    to be needing them.
 *
 * Diagram:
 *
 *         r−−−−>                         Legend
 *                  <−−−−−−−−−−−w
 * --------#--------#############······   r−−−−> - read head plus line showing
 *                                                 READ_KEEP_PAGE_SLOTS
 *              r−−−−>                    <−−−−w - write head plus line showing
 *                  <−−−−−−−−−−−w                  PAGE_SYNC_TIME_SLOTS
 * -------------#################······   ······ - Unwritten slots
 *                                        ###### - Slots in page cache
 *                    r−−−−>              ------ - Written slots not in page
 *                  <−−−−−−−−−−−w                  cache
 * -----------------#############······
 *
 *                           r−−−−>
 *                  <−−−−−−−−−−−w
 * -----------------#############······
 *
 * Note: there's still the possiblity of some data being left in page cache
 * in the specific case when you seek while in the state indicated in the
 * second diagram above.  It is not worth the additonal complexity to the
 * seeking code to "fix" this as it it unlikely and nothing too bad happens
 * even if it occurs.
 */

#define PAGE_SYNC_TIME_SLOTS (20)       /* 640kB */
#define READ_KEEP_PAGE_SLOTS (10)       /* 320kB */

/* GstTSCache */

struct _GstTSCache
{
  volatile gint refcount;

  GMutex lock;

  guint64 h_offset;             /* highest offset */
  guint64 l_rb_offset;          /* lowest offset in the ringbuffer */
  guint64 h_rb_offset;          /* highest offset in the ringbuffer (FULL slots) */

  gboolean need_discont;

  /* ring buffer */
  guint nslots;
  volatile gint fslots;         /* number of full slots */
  Slot *slots;

  guint head;
  guint tail;

  GstClockTime mtime;           /* timestamp when migration started */

  int write_fd;
  int read_fd;
};

#define GST_CACHE_LOCK(cache) G_STMT_START {                                \
  g_mutex_lock (&cache->lock);                                              \
} G_STMT_END

#define GST_CACHE_UNLOCK(cache) G_STMT_START {                              \
  g_mutex_unlock (&cache->lock);                                            \
} G_STMT_END

typedef enum
{
  STATE_EMPTY = 0,
  STATE_PART = 1,
  STATE_FULL = 2,
  STATE_POP = 3,
  STATE_RECYCLE = 4
} CacheState;

/* Slot manager */

struct _Slot
{
  volatile gint state;

  guint32 size;

  /* Offset into the timeshift backing store of this slot */
  off64_t offset;

  /* Offset in bytes from beginning of the stream (e.g. GstBuffer offset) */
  off64_t stream_offset;
};

static inline gboolean
slot_available (Slot * slot, gsize * size)
{
  CacheState state = g_atomic_int_get (&slot->state);

  if (state <= STATE_PART) {
    if (size) {
      *size = CACHE_SLOT_SIZE - slot->size;
    }
    return TRUE;
  }

  return FALSE;
}

#define TS_FLOW_REQUIRE_MORE_DATA GST_FLOW_CUSTOM_SUCCESS
#define TS_FLOW_SLOT_FILLED GST_FLOW_CUSTOM_SUCCESS_1

/* returns TRUE if slot is full */
static inline GstFlowReturn
slot_write (int fd, Slot * slot, guint8 * data, guint size,
    guint64 stream_offset)
{
  slot->stream_offset = stream_offset;

  if (lseek (fd, slot->offset + slot->size, SEEK_SET) == -1) {
    GST_WARNING ("Timeshift buffer seek failed: %s", strerror (errno));
    goto err;
  }
  while (size > 0) {
    /* TODO: poll first to make these writes interruptable */
    ssize_t bytes_written = write (fd, data, size);
    if (bytes_written < 0) {
      if (errno == EAGAIN)
        continue;
      else {
        GST_WARNING ("Timeshift buffer write failed: %s", strerror (errno));
        goto err;
      }
    } else {
      size -= bytes_written;
      data += bytes_written;
      slot->size += bytes_written;
    }
  }

  if (slot->size == CACHE_SLOT_SIZE) {
    g_atomic_int_set (&slot->state, STATE_FULL);
    return TS_FLOW_SLOT_FILLED;
  } else {
    g_atomic_int_set (&slot->state, STATE_PART);
    return TS_FLOW_REQUIRE_MORE_DATA;
  }
err:
  return GST_FLOW_ERROR;
}

/* Slot Buffer */

/**
 * SlotMeta:
 * @cache: a reference to the our #cache
 * @slot: the slot of this buffer
 *
 * Subclass containing additional information about our cache.
 */
struct _SlotMeta
{
  GstMeta meta;

  /* Reference to the cache we belong to */
  GstTSCache *cache;
  Slot *slot;
};

GType gst_slot_meta_api_get_type (void);
#define SLOT_META_API_TYPE  (gst_slot_meta_api_get_type())

static gboolean
gst_slot_meta_init (SlotMeta * meta, gpointer params, GstBuffer * buffer)
{
  meta->cache = NULL;
  meta->slot = NULL;
  return TRUE;
}

static void
gst_slot_meta_free (SlotMeta * meta, GstBuffer * buffer)
{
  if (meta->slot)
    g_atomic_int_set (&meta->slot->state, STATE_RECYCLE);

  if (meta->cache)
    gst_ts_cache_unref (meta->cache);
}

GType
gst_slot_meta_api_get_type (void)
{
  static volatile GType type;
  static const gchar *tags[] = { NULL };

  if (g_once_init_enter (&type)) {
    GType _type = gst_meta_api_type_register ("GstSlotMetaAPI", tags);
    g_once_init_leave (&type, _type);
  }
  return type;
}

static const GstMetaInfo *
gst_slot_meta_get_info (void)
{
  static const GstMetaInfo *info = NULL;

  if (g_once_init_enter (&info)) {
    const GstMetaInfo *_info = gst_meta_register (SLOT_META_API_TYPE,
        "GstSlotMeta", sizeof (SlotMeta),
        (GstMetaInitFunction) gst_slot_meta_init,
        (GstMetaFreeFunction) gst_slot_meta_free,
        (GstMetaTransformFunction) NULL);
    g_once_init_leave (&info, _info);
  }
  return info;
}

typedef struct MmappedSlotContext_
{
  Slot *slot;
  GstTSCache *cache;
  gpointer data;
} MmappedSlotContext;

static void
munmap_slot (gpointer * data)
{
  MmappedSlotContext *ctx = (MmappedSlotContext *) data;
  g_warn_if_fail (munmap (ctx->data, CACHE_SLOT_SIZE) == 0);
  /* Avoid trashing caches.  This will fail if the pages haven't hit disk yet
   * but that's OK as in that case the write head will take care of it. */
  g_warn_if_fail (posix_fadvise64 (ctx->cache->read_fd, ctx->slot->offset,
          CACHE_SLOT_SIZE, POSIX_FADV_DONTNEED) == 0);

  g_slice_free (MmappedSlotContext, ctx);
}

static GstBuffer *
gst_slot_buffer_new (GstTSCache * cache, Slot * slot)
{
  SlotMeta *meta;
  MmappedSlotContext *ctx;

  gpointer data = mmap (NULL, CACHE_SLOT_SIZE, PROT_READ,
      MAP_PRIVATE | MAP_POPULATE, cache->read_fd,
      slot->offset);
  if (data == NULL) {
    GST_ERROR ("mmaping timeshift buffer failed: %s", strerror (errno));
    goto err;
  }

  ctx = g_slice_new (MmappedSlotContext);
  ctx->slot = slot;
  ctx->cache = cache;
  ctx->data = data;

  GstBuffer *buffer =
      gst_buffer_new_wrapped_full (GST_MEMORY_FLAG_READONLY, data,
      CACHE_SLOT_SIZE, 0, CACHE_SLOT_SIZE, ctx,
      munmap_slot);

  meta = (SlotMeta *) gst_buffer_add_meta (buffer, SLOT_META_INFO, NULL);
  meta->cache = gst_ts_cache_ref (cache);
  meta->slot = slot;

  GST_BUFFER_OFFSET (buffer) = slot->stream_offset;
  GST_BUFFER_OFFSET_END (buffer) = slot->stream_offset + slot->size;

  return buffer;
err:
  return NULL;
}

static void
dump_cache_state (GstTSCache * cache, const gchar * title)
{
  static const gchar *state_names[] =
      { "EMPTY   ", "PART    ", "FULL    ", "POP     ", "RECYCLE " };
  gint i;
  GST_DEBUG ("---> %s \t head: %d tail: %d nslots: %d fslots: %d",
      title, cache->head, cache->tail, cache->nslots,
      g_atomic_int_get (&cache->fslots));
  GST_DEBUG ("     h_rb_offset %" G_GUINT64_FORMAT " l_rb_offset %"
      G_GUINT64_FORMAT " h_offset %" G_GUINT64_FORMAT,
      cache->h_rb_offset, cache->l_rb_offset, cache->h_offset);

  for (i = 0; i < cache->nslots; i++) {
    Slot *slot = &cache->slots[i];
    CacheState state = g_atomic_int_get (&slot->state);
    GST_LOG ("     %d. %s size %" G_GUINT32_FORMAT " offset %"
        G_GUINT64_FORMAT, i, state_names[state],
        slot->size, slot->stream_offset);
  }
}

static inline void
gst_ts_cache_flush (GstTSCache * cache)
{
  guint i;
  for (i = 0; i < cache->nslots; i++) {
    Slot *slot = &cache->slots[i];
    slot->state = STATE_EMPTY;
    slot->stream_offset = INVALID_OFFSET;
    slot->size = 0;
  }
  cache->head = cache->tail = 0;
  cache->fslots = 0;
  cache->need_discont = TRUE;
}

static int
reopen (int fd, int flags)
{
  char *filename;
  int newfd, errno_tmp;

  filename = g_strdup_printf ("/proc/%u/fd/%i", (unsigned long) getpid (), fd);
  newfd = open (filename, flags);
  errno_tmp = errno;
  g_free (filename);
  errno = errno_tmp;
  return newfd;
}

/**
 * gst_ts_cache_new:
 * @size: cache size
 *
 * Create a new cache instance.  File pointed to by fd will be used as the
 * timeshift cache backing file.
 *
 * Returns: a new #GstTSCache
 *
 */
GstTSCache *
gst_ts_cache_new (int fd)
{
  GstTSCache *cache;
  guint64 nslots;
  guint64 i;
  off64_t size, offset = 0;
  int wr_fd, rd_fd = -1;
  struct stat stat_buf;

  wr_fd = reopen (fd, O_WRONLY | O_CLOEXEC);
  rd_fd = reopen (fd, O_RDONLY | O_CLOEXEC);
  if (wr_fd == -1 || rd_fd == -1) {
    GST_ERROR ("Failed reopening fd %i: %s", fd, strerror (errno));
    goto errout;
  }
  if (fstat (fd, &stat_buf) != 0) {
    GST_ERROR ("Failed to stat fd %i: %s", fd, strerror (errno));
    goto errout;
  }
  size = stat_buf.st_size;

  cache = g_new (GstTSCache, 1);

  cache->refcount = 1;

  g_mutex_init (&cache->lock);

  cache->h_offset = 0;
  cache->l_rb_offset = 0;
  cache->h_rb_offset = 0;
  cache->write_fd = wr_fd;
  cache->read_fd = rd_fd;

  /* Ring buffer */
  nslots = size / CACHE_SLOT_SIZE;
  cache->nslots = nslots;

  cache->slots = (Slot *) g_new (Slot, nslots);
  for (i = 0; i < cache->nslots; i++) {
    cache->slots[i].offset = offset;
    offset += CACHE_SLOT_SIZE;
    cache->slots[i].stream_offset = INVALID_OFFSET;
  }

  gst_ts_cache_flush (cache);

  return cache;
errout:
  close (wr_fd);
  close (rd_fd);
  return NULL;
}

/**
 * gst_ts_cache_ref:
 * @cache: a #GstTSCache
 *
 * Increase the refcount of @cache.
 *
 */
GstTSCache *
gst_ts_cache_ref (GstTSCache * cache)
{
  g_return_val_if_fail (cache != NULL, NULL);

  g_atomic_int_inc (&cache->refcount);

  return cache;
}

static void
gst_ts_cache_free (GstTSCache * cache)
{
  int i;

  g_free (cache->slots);

  close (cache->write_fd);
  cache->write_fd = -1;
  close (cache->read_fd);
  cache->read_fd = -1;

  g_mutex_clear (&cache->lock);

  g_free (cache);
}

/**
 * gst_ts_cache_unref:
 * @cache: a #GstTSCache
 *
 * Unref @cache and free the resources when the refcount reaches 0.
 *
 */
void
gst_ts_cache_unref (GstTSCache * cache)
{
  g_return_if_fail (cache != NULL);

  if (g_atomic_int_dec_and_test (&cache->refcount))
    gst_ts_cache_free (cache);
}

static inline gboolean
gst_ts_cache_recycle (GstTSCache * cache, Slot * slot)
{
  gboolean recycle;
  recycle = g_atomic_int_compare_and_exchange (&slot->state, STATE_RECYCLE,
      STATE_EMPTY);
  if (recycle) {
    if (slot->stream_offset != INVALID_OFFSET)
      cache->l_rb_offset = slot->stream_offset + slot->size;
    slot->stream_offset = INVALID_OFFSET;
    slot->size = 0;
  }
  return recycle;
}

static inline gboolean
gst_ts_cache_rollback (GstTSCache * cache, Slot * slot)
{
  gboolean rollback;
  rollback = g_atomic_int_compare_and_exchange (&slot->state, STATE_RECYCLE,
      STATE_FULL);
  if (rollback) {
    g_atomic_int_inc (&cache->fslots);
  } else if (g_atomic_int_get (&slot->state) == STATE_FULL) {
    rollback = TRUE;
  }
  return rollback;
}

static inline gboolean
gst_ts_cache_rollforward (GstTSCache * cache, Slot * slot)
{
  gboolean rollforward;
  rollforward = g_atomic_int_compare_and_exchange (&slot->state, STATE_FULL,
      STATE_RECYCLE);
  if (rollforward) {
    g_atomic_int_add (&cache->fslots, -1);
  } else if (g_atomic_int_get (&slot->state) == STATE_RECYCLE) {
    rollforward = TRUE;
  }
  return rollforward;
}

/**
 * gst_ts_cache_pop:
 * @cache: a #GstTSCache
 *
 * Get the head of the cache.
 *
 * Returns: the head buffer of @cache or NULL when the cache is empty.
 *
 */

GstBuffer *
gst_ts_cache_pop (GstTSCache * cache, gboolean drain)
{
  GstBuffer *buffer = NULL;
  Slot *head;
  gboolean pop;

#if DEBUG_RINGBUFFER
  dump_cache_state (cache, "pre-pop");
#endif

  head = &cache->slots[cache->head];

  if (drain) {
    if (g_atomic_int_compare_and_exchange (&head->state, STATE_PART,
            STATE_FULL)) {
      cache->h_rb_offset = head->stream_offset + head->size;
      g_atomic_int_inc (&cache->fslots);
    }
  }

  pop = g_atomic_int_compare_and_exchange (&head->state, STATE_FULL, STATE_POP);

  if (pop) {
    g_atomic_int_add (&cache->fslots, -1);
    buffer = gst_slot_buffer_new (cache, head);
    if (cache->need_discont) {
      GST_BUFFER_FLAG_SET (buffer, GST_BUFFER_FLAG_DISCONT);
      cache->need_discont = FALSE;
    }

    cache->head = (cache->head + 1) % cache->nslots;
  }
#if DEBUG_RINGBUFFER
  dump_cache_state (cache, "post-pop");
#endif

  return buffer;
}

/* Does a + b being careful to wrap appropriately and taking care of negative
 * numbers, overflows, etc. */
static guint
gst_ts_cache_slot_idx_add (GstTSCache * cache, guint a, int b)
{
  guint size = cache->nslots;

  /* b_norm is positive and small but will behave as b when used in modulo
     arithmatic */
  guint b_norm = (b >= 0) ? (b % size) : size - ((-b) % size);

  return (a + b_norm) % size;
}

/* Is slot x between a and b where a is the lower bound and b the upper one? */
static gboolean
gst_ts_cache_slot_in_range (GstTSCache * cache, guint x, guint a, guint b)
{
  g_return_val_if_fail (b < cache->nslots, FALSE);
  if (a < b)
    return x >= a && x < b;
  else
    return x >= a || x < b;
}

/**
 * gst_ts_cache_push:
 * @cache: a #GstTSCache
 * @data: pointer to the data to be inserted in the cache
 * @size: size in bytes of provided data
 *
 * Cache the @buffer and takes ownership of the it.
 *
 */
gboolean
gst_ts_cache_push (GstTSCache * cache, guint8 * data, gsize size)
{
  Slot *tail;
  gsize avail;

#if DEBUG_RINGBUFFER
  dump_cache_state (cache, "pre-push");
#endif

  while (size) {
#if DEBUG_RINGBUFFER
    GST_DEBUG ("remaining size %d", size);
    dump_cache_state (cache, "pre-push");
#endif
    tail = &cache->slots[cache->tail];
    gst_ts_cache_recycle (cache, tail);
    if (slot_available (tail, &avail)) {
      GstFlowReturn write_result;
      avail = MIN (avail, size);
      write_result = slot_write (cache->write_fd, tail, data, avail,
          cache->h_rb_offset);
      if (write_result < 0) {
        /* error or flushing */
        return FALSE;
      } else if (write_result == TS_FLOW_SLOT_FILLED) {
        guint writeout_idx;

        /* Move the tail when the slot is full */
        cache->tail = (cache->tail + 1) % cache->nslots;
        cache->h_rb_offset += CACHE_SLOT_SIZE;
        g_atomic_int_inc (&cache->fslots);

        /* Instruct the kernel to start dumping the just written data to disk
           so we can later drop it from the page cache. */
        g_warn_if_fail (sync_file_range (cache->write_fd, tail->offset,
                CACHE_SLOT_SIZE, SYNC_FILE_RANGE_WRITE) == 0);

        /* Make sure the pages from a while ago have been written out by now. */
        writeout_idx =
            gst_ts_cache_slot_idx_add (cache, cache->tail,
            -PAGE_SYNC_TIME_SLOTS);
        g_warn_if_fail (sync_file_range (cache->write_fd,
                cache->slots[writeout_idx].offset, CACHE_SLOT_SIZE,
                SYNC_FILE_RANGE_WAIT_BEFORE | SYNC_FILE_RANGE_WRITE |
                SYNC_FILE_RANGE_WAIT_AFTER) == 0);

        /* And drop them from the cache unless they're going to be needed by
         * the read head soon.  e.g. if
         * (read head < writeout_idx < read_head + READ_KEEP_PAGE_SLOTS) */
        if (!gst_ts_cache_slot_in_range (cache, writeout_idx, cache->head,
                gst_ts_cache_slot_idx_add (cache, cache->head,
                    READ_KEEP_PAGE_SLOTS))) {
          g_warn_if_fail (posix_fadvise64 (cache->write_fd,
                  cache->slots[writeout_idx].offset, CACHE_SLOT_SIZE,
                  POSIX_FADV_DONTNEED) == 0);
        }
      }
      data += avail;
      size -= avail;
      cache->h_offset += avail;
    } else {
      return FALSE;
    }
  }
#if DEBUG_RINGBUFFER
  dump_cache_state (cache, "post-push");
#endif

  return TRUE;
}

/**
 * gst_ts_cache_has_offset:
 * @cache: a #GstTSCache
 * @offset: byte offset to check if is in the cache.
 *
 * Checks if an specified offset can be found on the cache.
 *
 */
gboolean
gst_ts_cache_has_offset (GstTSCache * cache, guint64 offset)
{
  gboolean ret;
  g_return_val_if_fail (cache != NULL, FALSE);

  GST_CACHE_LOCK (cache);
  dump_cache_state (cache, "has_offset");

  ret = (offset >= cache->l_rb_offset && offset < cache->h_offset);
  GST_CACHE_UNLOCK (cache);
  return ret;
}

/**
 * gst_ts_cache_get_total_bytes_received:
 * @cache: a #GstTSCache
 *
 * Returns the total number of bytes which have been written into the cache
 * equivalent to the "duration" of the cache in bytes.
 */
guint64
gst_ts_cache_get_total_bytes_received (GstTSCache * cache)
{
  guint64 offset;
  GST_CACHE_LOCK (cache);
  offset = cache->h_offset;
  GST_CACHE_UNLOCK (cache);
  return offset;
}

/**
 * gst_ts_cache_seek:
 * @cache: a #GstTSCache
 * @offset: byte offset where the cache have to be repositioned.
 *
 * Reconfigures the cache to read from the closest location to the specified
 * offset.
 *
 */
gboolean
gst_ts_cache_seek (GstTSCache * cache, guint64 offset)
{
  Slot *head;
  g_return_val_if_fail (cache != NULL, FALSE);

  GST_DEBUG ("requested seek at offset: %" G_GUINT64_FORMAT, offset);

  GST_CACHE_LOCK (cache);
  dump_cache_state (cache, "pre-seek");

  /* clamp to a min/max valid offset */
  offset = MIN (offset, cache->h_rb_offset);
  offset = MAX (offset, cache->l_rb_offset);
  GST_CACHE_UNLOCK (cache);

  GST_DEBUG ("seeking for offset: %" G_GUINT64_FORMAT, offset);

  /* First check if we can find it in the ringbuffer */
  if (offset >= cache->l_rb_offset && offset < cache->h_rb_offset) {
    guint seeker;

    GST_DEBUG ("seeking in the ringbuffer");
    seeker = cache->head;
    head = &cache->slots[seeker];

    if (offset >= head->stream_offset) {
      if (offset < head->stream_offset + head->size) {
        GST_DEBUG ("found in current head");
        /* Already in the requested position so do nothing */
        gst_ts_cache_rollback (cache, head);
        goto beach;
      }
      /* perform seek in the future */
      GST_DEBUG ("seeking in the future");
      do {
        gst_ts_cache_rollforward (cache, head);
        seeker = (seeker + 1) % cache->nslots;
        head = &cache->slots[seeker];
      } while (!(offset >= head->stream_offset &&
              offset < head->stream_offset + head->size));
      gst_ts_cache_rollback (cache, head);
    } else {
      /* perform seek in the past */
      GST_DEBUG ("seeking in the past");
      gst_ts_cache_rollback (cache, head);
      do {
        if (seeker == 0)
          seeker = cache->nslots - 1;
        else
          seeker--;
        head = &cache->slots[seeker];
        if (!gst_ts_cache_rollback (cache, head)) {
          seeker = (seeker + 1) % cache->nslots;
          break;
        }
      } while (!(offset >= head->stream_offset &&
              offset < head->stream_offset + head->size));
    }
    cache->head = seeker;
    goto beach;
  }

beach:
  dump_cache_state (cache, "post-seek");

  return TRUE;
}

/**
 * gst_ts_cache_is_empty:
 * @cache: a #GstTSCache
 *
 * Return TRUE if cache is empty.
 *
 */
gboolean
gst_ts_cache_is_empty (GstTSCache * cache)
{
  gboolean is_empty = TRUE;

  g_return_val_if_fail (cache != NULL, TRUE);

  is_empty = (g_atomic_int_get (&cache->fslots) == 0);

  return is_empty;
}

/**
 * gst_ts_cache_fullness:
 * @cache: a #GstTSCache
 *
 * Return # of bytes remaining in the ringbuffer.
 *
 */
guint64
gst_ts_cache_fullness (GstTSCache * cache)
{
  g_return_val_if_fail (cache != NULL, 0);

  if (gst_ts_cache_is_empty (cache)) {
    return 0;
  } else {
    Slot *head, *tail;
    head = &cache->slots[cache->head];
    tail = &cache->slots[cache->tail];
    return (tail->stream_offset - head->stream_offset + head->size);
  }
}

void
gst_ts_cache_buffered_range (GstTSCache * cache, guint64 * begin, guint64 * end)
{
  g_return_if_fail (cache != NULL);

  GST_CACHE_LOCK (cache);
  if (end) {
    *end = cache->h_offset;
  }

  if (begin) {
    *begin = cache->l_rb_offset;
  }
  GST_CACHE_UNLOCK (cache);
}
