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

#ifndef _GST_QUICLYSINK_H_
#define _GST_QUICLYSINK_H_

#include <gst/gst.h>
#include <gst/base/gstbasesink.h>
#include <gio/gio.h>
#include "quicly.h"

G_BEGIN_DECLS

#define GST_TYPE_QUICLYSINK   (gst_quiclysink_get_type())
#define GST_QUICLYSINK(obj)   (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_QUICLYSINK,GstQuiclysink))
#define GST_QUICLYSINK_CLASS(klass)   (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_QUICLYSINK,GstQuiclysinkClass))
#define GST_IS_QUICLYSINK(obj)   (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_QUICLYSINK))
#define GST_IS_QUICLYSINK_CLASS(obj)   (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_QUICLYSINK))

typedef struct _GstQuiclysink GstQuiclysink;
typedef struct _GstQuiclysinkClass GstQuiclysinkClass;

struct _GstQuiclysink
{
  GstBaseSink base_quiclysink;
  GstPad *sinkpad, *srcpad;
  gboolean silent;
  GSocket *socket;
  GSocketAddress *conn_addr;
  GSocketAddress *bind_addr;
  gchar *bind_iaddr;
  gint bind_port;
  GstCaps *caps;

  guint quicly_mtu;

    /* private quicly */
  gchar *cid_key;
  quicly_context_t ctx;
  ptls_context_t tlsctx;
  ptls_key_exchange_algorithm_t *key_exchanges[128];
  quicly_conn_t *conn;
  quicly_cid_plaintext_t next_cid;
  quicly_transport_parameters_t resumed_transport_params;
  ptls_handshake_properties_t hs_properties;

  /* certificate chain and key location */
  gchar *cert;
  gchar *key;

  /* crutch */
  gboolean received_caps_ack;
  
  quicly_dgram_t *dgram;
  quicly_stream_t *stream;
  /* quicly recv buffer */
  gchar *recv_buf;
  gsize recv_buf_size;

  gssize num_packets;
  gssize num_bytes;

  gboolean stream_mode;
};

struct _GstQuiclysinkClass
{
  GstBaseSinkClass base_quiclysink_class;
};

GType gst_quiclysink_get_type (void);

G_END_DECLS

#endif
