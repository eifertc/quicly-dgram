/* GStreamer
 * Copyright (C) 2019 FIXME <fixme@example.com>
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

#ifndef _GST_QUICLYSRC_H_
#define _GST_QUICLYSRC_H_

#include <gst/gst.h>
#include <gst/base/gstpushsrc.h>
#include <gio/gio.h>
#include <quicly.h>

G_BEGIN_DECLS

#define GST_TYPE_QUICLYSRC   (gst_quiclysrc_get_type())
#define GST_QUICLYSRC(obj)   (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_QUICLYSRC,GstQuiclysrc))
#define GST_QUICLYSRC_CLASS(klass)   (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_QUICLYSRC,GstQuiclysrcClass))
#define GST_IS_QUICLYSRC(obj)   (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_QUICLYSRC))
#define GST_IS_QUICLYSRC_CLASS(obj)   (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_QUICLYSRC))
#define GST_QUICLYSRC_CAST(obj)   ((GstQuiclysrc *)(obj))

typedef struct _GstQuiclysrc GstQuiclysrc;
typedef struct _GstQuiclysrcClass GstQuiclysrcClass;

struct _GstQuiclysrc
{
  GstPushSrc base_quiclysrc;

  gboolean silent;
  gint port;
  gint bind_port;
  gchar *host;
  gchar *bind_addr;
  GSocketAddress *dst_addr;
  GstCaps *caps;
  GSocket *socket;
  GstClockID clockId;

  guint udp_mtu;
  guint quicly_mtu;

  /* private quicly */
  quicly_conn_t *conn;
  gchar *cid_key;
  quicly_cid_plaintext_t next_cid;
  quicly_transport_parameters_t resumed_transport_params;
  ptls_handshake_properties_t hs_properties;
  quicly_context_t ctx;
  ptls_context_t tlsctx;
  ptls_key_exchange_algorithm_t *key_exchanges[128];

  ptls_iovec_t resumption_token;

  /* quicly receive */
  quicly_dgram_t *dgram;
  quicly_stream_t *stream;
  gchar *recv_buf;
  gsize recv_buf_size;

  gboolean transport_close;

  /* Hack assign of buffer */
  gboolean connected;
  gsize pushed;
  GInputVector ivec;
  GInputVector reserve_vec[2];

  /* stats */
  gssize num_packets;
  gssize num_bytes;

  GCancellable *cancellable;
  gboolean made_cancel_fd;

  /* For buffer list output */
  GstAllocator *allocator;
  GstAllocationParams params;

  GstMemory **mem_list;
  GstMapInfo **map_list;
  GInputVector **vec_list;
  gint *pushed_list;

  gboolean mem_list_allocated;
  gsize mem_list_size;
  GstClockTime prev_arrival_time;
  GstClockTime prev_transit;
  guint64 jitter;
  guint64 max_jitter;
  guint num_jitter_spikes;
};

struct _GstQuiclysrcClass
{
  GstPushSrcClass base_quiclysrc_class;
};

GType gst_quiclysrc_get_type (void);

G_END_DECLS

#endif
