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
 * Free Software Foundation, Inc., 51 Franklin Street, Suite 500,
 * Boston, MA 02110-1335, USA.
 */
/**
 * SECTION:element-gstquiclysrc
 *
 * The quiclysrc element does FIXME stuff.
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch -v fakesrc ! quiclysrc ! FIXME ! fakesink
 * ]|
 * FIXME Describe what the pipeline does.
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <sys/types.h>
#include <sys/socket.h>

#include "quicly/defaults.h"
#include "quicly/streambuf.h"
#include "../deps/picotls/t/util.h"

#include "gstquiclysrc.h"

GST_DEBUG_CATEGORY_STATIC (gst_quiclysrc_debug_category);
#define GST_CAT_DEFAULT gst_quiclysrc_debug_category

static void
gst_quiclysrc_create_cancellable (GstQuiclysrc *quiclysrc)
{
  GPollFD pollfd;

  quiclysrc->cancellable = g_cancellable_new ();
  quiclysrc->made_cancel_fd = g_cancellable_make_pollfd (quiclysrc->cancellable, &pollfd);
}

static void
gst_quiclysrc_free_cancellable (GstQuiclysrc *quiclysrc)
{
  if (quiclysrc->made_cancel_fd) {
    g_cancellable_release_fd (quiclysrc->cancellable);
    quiclysrc->made_cancel_fd = FALSE;
  }
  g_object_unref (quiclysrc->cancellable);
  quiclysrc->cancellable = NULL;
}


/* prototypes */
static void gst_quiclysrc_set_property (GObject * object,
    guint property_id, const GValue * value, GParamSpec * pspec);
static void gst_quiclysrc_get_property (GObject * object,
    guint property_id, GValue * value, GParamSpec * pspec);
static void gst_quiclysrc_finalize (GObject * object);

static GstCaps *gst_quiclysrc_get_caps (GstBaseSrc * src, GstCaps * filter);
//static gboolean gst_quiclysrc_decide_allocation (GstBaseSrc * src, GstQuery * query);
static gboolean gst_quiclysrc_start (GstBaseSrc * src);
static gboolean gst_quiclysrc_stop (GstBaseSrc * src);
static gboolean gst_quiclysrc_unlock (GstBaseSrc * src);
static gboolean gst_quiclysrc_unlock_stop (GstBaseSrc * src);
static GstFlowReturn gst_quiclysrc_fill (GstPushSrc * src, GstBuffer * buf);
static gboolean gst_quiclysrc_decide_allocation (GstBaseSrc * bsrc, GstQuery * query);


/* quicly prototypes */
static int save_ticket_cb(ptls_save_ticket_t *_self, ptls_t *tls, ptls_iovec_t src);
static void on_closed_by_peer(quicly_closed_by_peer_t *self, quicly_conn_t *conn, int err, uint64_t frame_type, const char *reason,
                              size_t reason_len);
static int on_dgram_open(quicly_dgram_open_t *self, quicly_dgram_t *dgram);
static int on_stream_open(quicly_stream_open_t *self, quicly_stream_t *stream);
static int on_stop_sending(quicly_stream_t *stream, int err);
static int on_receive_dgram(quicly_dgram_t *dgram, const void *src, size_t len);
static int on_receive_stream(quicly_stream_t *stream, size_t off, const void *src, size_t len);
static int on_receive_reset(quicly_stream_t *stream, int err);
static int send_pending(GstQuiclysrc *quiclysrc);
//static int receive_packet(GstQuiclysrc *quiclysink);
//static void write_dgram_buffer(quicly_dgram_t *dgram, const void *src, size_t len);
static int receive_packet(GstQuiclysrc *quiclysrc, GError *err);

/* TODO: do something with that, needs to be a property */
static const char *ticket_file = NULL;

/* quicly callbacks */
static ptls_save_ticket_t save_ticket = {save_ticket_cb};
static quicly_dgram_open_t dgram_open = {&on_dgram_open};
static quicly_stream_open_t stream_open = {&on_stream_open};
static quicly_closed_by_peer_t closed_by_peer = {&on_closed_by_peer};

static const quicly_stream_callbacks_t stream_callbacks = {quicly_streambuf_destroy,
                                                           quicly_streambuf_egress_shift,
                                                           quicly_streambuf_egress_emit,
                                                           on_stop_sending,
                                                           on_receive_stream,
                                                           on_receive_reset};
static const quicly_dgram_callbacks_t dgram_callbacks = {quicly_dgrambuf_destroy,
                                                         quicly_dgrambuf_egress_shift,
                                                         quicly_dgrambuf_egress_emit,
                                                         on_receive_dgram};

#define DEFAULT_BIND_ADDRESS  "0.0.0.0"
#define DEFAULT_BIND_PORT     17001
#define DEFAULT_CAPS          NULL
#define DEFAULT_BUFFER_SIZE   0
#define UDP_DEFAULT_MTU       (1492)
#define QUICLY_DEFAULT_MTU    1280
#define DEFAULT_HOST          "127.0.0.1"
#define DEFAULT_PORT          5000

enum
{
  PROP_0,
  PROP_BIND_ADDRESS,
  PROP_BIND_PORT,
  PROP_HOST,
  PROP_PORT,
  PROP_CAPS,
  PROP_DEFAULT_BUFFER_SIZE,
  PROP_UDP_MTU,
  PROP_QUICLY_MTU
};

/* rtp header and framing for stream mode */
typedef struct {
    uint16_t framing;
    uint8_t ver_p_x_cc;
    uint8_t m_pt;
    uint16_t seq_nr;
    uint32_t timestamp;
    uint32_t ssrc;
} rtp_hdr_;

/* pad templates */

static GstStaticPadTemplate gst_quiclysrc_src_template =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);


/* class initialization */

G_DEFINE_TYPE_WITH_CODE (GstQuiclysrc, gst_quiclysrc, GST_TYPE_PUSH_SRC,
  GST_DEBUG_CATEGORY_INIT (gst_quiclysrc_debug_category, "quiclysrc", 0,
  "debug category for quiclysrc element"));

static void
gst_quiclysrc_class_init (GstQuiclysrcClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstBaseSrcClass *base_src_class = GST_BASE_SRC_CLASS (klass);
  GstPushSrcClass *push_src_class = (GstPushSrcClass *) klass;

  /* Setting up pads and setting metadata should be moved to
     base_class_init if you intend to subclass this class. */
  gst_element_class_add_pad_template (GST_ELEMENT_CLASS(klass),
      gst_static_pad_template_get (&gst_quiclysrc_src_template));

  gst_element_class_set_static_metadata (GST_ELEMENT_CLASS(klass),
      "FIXME Long name", "Generic", "FIXME Description",
      "FIXME <fixme@example.com>");

  gobject_class->set_property = gst_quiclysrc_set_property;
  gobject_class->get_property = gst_quiclysrc_get_property;
  gobject_class->finalize = gst_quiclysrc_finalize;

  base_src_class->get_caps = GST_DEBUG_FUNCPTR (gst_quiclysrc_get_caps);
  //base_src_class->decide_allocation = GST_DEBUG_FUNCPTR (gst_quiclysrc_decide_allocation);
  base_src_class->start = GST_DEBUG_FUNCPTR (gst_quiclysrc_start);
  base_src_class->stop = GST_DEBUG_FUNCPTR (gst_quiclysrc_stop);
  base_src_class->decide_allocation = gst_quiclysrc_decide_allocation;
  base_src_class->unlock = GST_DEBUG_FUNCPTR (gst_quiclysrc_unlock);
  base_src_class->unlock_stop = GST_DEBUG_FUNCPTR (gst_quiclysrc_unlock_stop);

  push_src_class->fill = GST_DEBUG_FUNCPTR (gst_quiclysrc_fill);

  g_object_class_install_property(gobject_class, PROP_HOST,
                                  g_param_spec_string("host", 
                                  "Host", 
                                  "Address to connect to", 
                                  DEFAULT_HOST, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property(gobject_class, PROP_BIND_ADDRESS,
                                  g_param_spec_string("bind-address", 
                                  "Bind Address", 
                                  "Local address to bind to", 
                                  DEFAULT_BIND_ADDRESS, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property(gobject_class, PROP_BIND_PORT, 
                                  g_param_spec_int("bind-port", "Bind Port", 
                                  "The port to bind to packets from", 
                                  1, 65535, DEFAULT_BIND_PORT, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property(gobject_class, PROP_PORT, 
                                  g_param_spec_int("port", "Port", 
                                  "The port to connect to", 
                                  1, 65535, DEFAULT_PORT, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_CAPS,
                                   g_param_spec_boxed ("caps", "Caps",
                                   "The caps of the source pad", GST_TYPE_CAPS,
                                   G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (G_OBJECT_CLASS (klass), PROP_DEFAULT_BUFFER_SIZE,
                                   g_param_spec_int ("buffer-size", "Buffer Size",
                                   "Size of the kernel receive buffer in bytes, 0=default", 
                                   0, G_MAXINT,
                                   DEFAULT_BUFFER_SIZE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_UDP_MTU,
          g_param_spec_uint ("udp-mtu", "Expected Maximum Transmission Unit",
          "Maximum expected packet size. This directly defines the allocation"
          "size of the receive buffer pool.",
          0, G_MAXINT, UDP_DEFAULT_MTU,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_QUICLY_MTU,
          g_param_spec_uint ("quicly-mtu", "Expected Max Quicly packet size",
          "Maximum expected packet size of quicly packets.",
          0, G_MAXINT, QUICLY_DEFAULT_MTU,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));


}

static void
gst_quiclysrc_init (GstQuiclysrc *quiclysrc)
{
  GST_DEBUG_OBJECT (quiclysrc, "init");

  quiclysrc->port = DEFAULT_PORT;
  quiclysrc->host = g_strdup(DEFAULT_HOST);
  quiclysrc->bind_addr = g_strdup (DEFAULT_BIND_ADDRESS);
  quiclysrc->bind_port = DEFAULT_BIND_PORT;
  quiclysrc->transport_close = FALSE;

  gst_base_src_set_live(GST_BASE_SRC(quiclysrc), TRUE);
  gst_base_src_set_format (GST_BASE_SRC (quiclysrc), GST_FORMAT_TIME);
  gst_base_src_set_do_timestamp (GST_BASE_SRC (quiclysrc), TRUE);

  quiclysrc->udp_mtu = UDP_DEFAULT_MTU;
  quiclysrc->quicly_mtu = QUICLY_DEFAULT_MTU;

   /* Setup quicly and tls context */
  quiclysrc->tlsctx.random_bytes = ptls_openssl_random_bytes;
  quiclysrc->tlsctx.get_time = &ptls_get_time;
  quiclysrc->tlsctx.key_exchanges = quiclysrc->key_exchanges;
  quiclysrc->tlsctx.cipher_suites = ptls_openssl_cipher_suites;
  quiclysrc->tlsctx.require_dhe_on_psk = 1;
  quiclysrc->tlsctx.save_ticket = &save_ticket;

  quiclysrc->ctx = quicly_spec_context;
  quiclysrc->ctx.tls = &quiclysrc->tlsctx;
  quiclysrc->ctx.stream_open = &stream_open;
  quiclysrc->ctx.dgram_open = &dgram_open;
  quiclysrc->ctx.closed_by_peer = &closed_by_peer;

  setup_session_cache(quiclysrc->ctx.tls);
  quicly_amend_ptls_context(quiclysrc->ctx.tls);

  /* key exchange */
  quiclysrc->key_exchanges[0] = &ptls_openssl_secp256r1;

  quiclysrc->ctx.event_log.cb = quicly_new_default_event_logger(stderr);
  //_ctx->event_log.mask = UINT64_MAX;
  quiclysrc->ctx.event_log.mask = ((uint64_t)1 << QUICLY_EVENT_TYPE_PACKET_LOST) | 
                                  ((uint64_t)1 << QUICLY_EVENT_TYPE_TRANSPORT_CLOSE_RECEIVE) |
                                  ((uint64_t)1 << QUICLY_EVENT_TYPE_APPLICATION_CLOSE_RECEIVE) |
                                  ((uint64_t)1 << QUICLY_EVENT_TYPE_DGRAM_LOST);
                         //((uint64_t)1 << QUICLY_EVENT_TYPE_DGRAM_ACKED);

  quiclysrc->conn = NULL;
  quiclysrc->dgram = NULL;
  quiclysrc->stream = NULL;
  /* -------- end context init --------------*/

  quiclysrc->num_packets = 0;
  quiclysrc->num_bytes = 0;
  quiclysrc->silent = FALSE;
  quiclysrc->connected = FALSE;
  quiclysrc->recv_buf = malloc(sizeof(gchar) * (2048 + 1));
  quiclysrc->recv_buf_size = 2048;
}

void
gst_quiclysrc_set_property (GObject * object, guint property_id,
    const GValue * value, GParamSpec * pspec)
{
  GstQuiclysrc *quiclysrc = GST_QUICLYSRC (object);

  GST_DEBUG_OBJECT (quiclysrc, "set_property");

  switch (property_id) {
    case PROP_BIND_ADDRESS:
      g_free(quiclysrc->bind_addr);
      if (g_value_get_string(value) == NULL)
        quiclysrc->bind_addr = g_strdup (DEFAULT_BIND_ADDRESS);
      else
        quiclysrc->bind_addr = g_value_dup_string(value);
      break;
    case PROP_HOST:
      g_free(quiclysrc->host);
      if (g_value_get_string(value) == NULL)
        quiclysrc->host = g_strdup (DEFAULT_HOST);
      else
        quiclysrc->host = g_value_dup_string(value);
      break;
    case PROP_PORT:
      quiclysrc->port = g_value_get_int(value);
      break;
    case PROP_BIND_PORT:
      quiclysrc->bind_port = g_value_get_int(value);
      break;
    case PROP_CAPS: {
      const GstCaps *new_caps_val = gst_value_get_caps(value);
      GstCaps *new_caps;
      GstCaps *old_caps;

      if (new_caps_val == NULL) {
        new_caps = gst_caps_new_any();
      } else {
        new_caps = gst_caps_copy(new_caps_val);
      }

      GST_OBJECT_LOCK (quiclysrc);
      old_caps = quiclysrc->caps;
      quiclysrc->caps = new_caps;
      GST_OBJECT_UNLOCK (quiclysrc);
      if (old_caps)
        gst_caps_unref(old_caps);

      gst_pad_mark_reconfigure(GST_BASE_SRC_PAD(quiclysrc));
      break;
    }
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

void
gst_quiclysrc_get_property (GObject * object, guint property_id,
    GValue * value, GParamSpec * pspec)
{
  GstQuiclysrc *quiclysrc = GST_QUICLYSRC (object);

  GST_DEBUG_OBJECT (quiclysrc, "get_property");

  switch (property_id) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

void
gst_quiclysrc_finalize (GObject * object)
{
  GstQuiclysrc *quiclysrc = GST_QUICLYSRC (object);

  GST_DEBUG_OBJECT (quiclysrc, "finalize");

  /* clean up object here */
  if (quiclysrc->socket)
    g_object_unref(quiclysrc->socket);

  if (quiclysrc->caps)
    gst_caps_unref(quiclysrc->caps);
  quiclysrc->caps = NULL;

  g_free(quiclysrc->host);
  quiclysrc->host = NULL;

  g_free(quiclysrc->bind_addr);
  quiclysrc->bind_addr = NULL;

  if (quiclysrc->conn != NULL)
    free(quiclysrc->conn);
  quiclysrc->conn = NULL;

  if (quiclysrc->dgram != NULL)
    free(quiclysrc->dgram);
  quiclysrc->dgram = NULL;

  g_free(quiclysrc->recv_buf);
  quiclysrc->recv_buf = NULL;

  G_OBJECT_CLASS (gst_quiclysrc_parent_class)->finalize (object);
}

/* get caps from subclass */
static GstCaps *
gst_quiclysrc_get_caps (GstBaseSrc * src, GstCaps * filter)
{
  GstQuiclysrc *quiclysrc = GST_QUICLYSRC (src);

  GST_DEBUG_OBJECT (quiclysrc, "get_caps");

  GstCaps *caps, *result;
  GST_OBJECT_LOCK(quiclysrc);
  if ((caps = quiclysrc->caps))
    gst_caps_ref(caps);
  GST_OBJECT_UNLOCK(quiclysrc);
  
  if (caps) {
    if (filter) {
      result = gst_caps_intersect_full (filter, caps, GST_CAPS_INTERSECT_FIRST);
      gst_caps_unref(caps);
    } else {
      result = caps;
    }
  } else {
    result = (filter) ? gst_caps_ref(filter) : gst_caps_new_any();
  }

  return result;
}

/* start and stop processing, ideal for opening/closing the resource */
static gboolean
gst_quiclysrc_start (GstBaseSrc * src)
{
  GstQuiclysrc *quiclysrc = GST_QUICLYSRC (src);

  GST_DEBUG_OBJECT (quiclysrc, "start");

  gst_quiclysrc_create_cancellable(quiclysrc);

  GError *err = NULL;
  GInetAddress *iaddr;
  GSocketAddress *bind_addr_t;

  iaddr = g_inet_address_new_from_string(quiclysrc->host);
  if (!iaddr) {
    g_printerr("Could not resolve address\n");
    return FALSE;
  }

  quiclysrc->dst_addr = g_inet_socket_address_new(iaddr, quiclysrc->port);
  g_object_unref(iaddr);

  if ((quiclysrc->socket = g_socket_new(G_SOCKET_FAMILY_IPV4, 
                           G_SOCKET_TYPE_DATAGRAM, 
                           G_SOCKET_PROTOCOL_UDP, &err)) == NULL) {
    g_printerr("Could not create socket\n");
    return FALSE;
  }

  iaddr = g_inet_address_new_from_string(quiclysrc->bind_addr);
  if (!iaddr) {
    g_printerr("Could not resolve bind address\n");
    return FALSE;
  }

  bind_addr_t = g_inet_socket_address_new(iaddr, quiclysrc->bind_port);
  g_object_unref(iaddr);

  if (!g_socket_bind(quiclysrc->socket, bind_addr_t, TRUE, &err)) {
    g_printerr("Could not bind socket\n");
    return FALSE;
  }
  g_object_unref(bind_addr_t);

    /* convert to native for quicly_connect */
  gssize len = g_socket_address_get_native_size(quiclysrc->dst_addr);
  struct sockaddr native_sa;
  if(!g_socket_address_to_native(quiclysrc->dst_addr, &native_sa,
                             len,
                             &err)) {
    g_printerr("Could not convert GSocketAddress to native. Error: %s\n", err->message);
    return FALSE;
  }
  socklen_t salen = sizeof(native_sa);


  /* Quicly connect */
  int64_t timeout_at;
  int64_t delta;
  int64_t wait = 0;
  err = NULL;
  int ret;

  if ((ret = quicly_connect(&quiclysrc->conn, &quiclysrc->ctx,
                            quiclysrc->host,
                            &native_sa,
                            salen,
                            &quiclysrc->next_cid,
                            &quiclysrc->hs_properties,
                            &quiclysrc->resumed_transport_params)) != 0) {
    g_printerr("Quicly connect failed\n");
    return FALSE;
  }
  ++quiclysrc->next_cid.master_id;

  if ((ret = send_pending(quiclysrc)) != 0)
    g_printerr("Could not send inital packets.\n");


  g_print("Connecting...\n");
  while(quicly_connection_is_ready(quiclysrc->conn) == 0) {
    timeout_at = quiclysrc->conn != NULL ? quicly_get_first_timeout(quiclysrc->conn) : INT64_MAX;
    if (timeout_at != INT64_MAX) {
      delta = timeout_at - quiclysrc->ctx.now->cb(quiclysrc->ctx.now);
      if (delta > 0) {
        wait = delta * 1000;
      } else {
        wait = 0;
      }
    }
    if (g_socket_condition_timed_wait(quiclysrc->socket, G_IO_IN, wait, NULL, &err)) {
      if (receive_packet(quiclysrc, err) != 0) {
        g_printerr("Error in receive_packet\n");
      }
    }
    err = NULL;
    if ((quiclysrc->conn != NULL)) {
      if (send_pending(quiclysrc) != 0) {
        quicly_free(quiclysrc->conn);
        g_print("Connection closed while sending\n");
        quiclysrc->conn = NULL;
        return FALSE;
      }
    }
  }


  /* set connected, for the on_receive functions */
  quiclysrc->connected = TRUE;
  /* set application context */
  quicly_set_data(quiclysrc->conn, (void*) quiclysrc);

  g_print("Connection established\n");
  return TRUE;
}

static int receive_packet(GstQuiclysrc *quiclysrc, GError *err)
{
  gssize rret;
  size_t off, plen;

  if ((rret = g_socket_receive_from(quiclysrc->socket, NULL, 
                                    quiclysrc->recv_buf,
                                    quiclysrc->recv_buf_size,
                                    quiclysrc->cancellable,
                                    &err)) < 0) {
    g_printerr("Error receiving from socket: %s\n", err->message);
    return -1;
  }
  off = 0;
  while (off != rret) {
    quicly_decoded_packet_t packet;
    plen = quicly_decode_packet(&quiclysrc->ctx, &packet, 
                               (uint8_t *)quiclysrc->recv_buf + off,
                                rret - off);
    if (plen == SIZE_MAX)
      break;
    quicly_receive(quiclysrc->conn, &packet);
  
    off += plen;
  }

  return 0;
}

static int send_pending(GstQuiclysrc *quiclysrc) {
  quicly_datagram_t *packets[16];
  size_t num_packets, i;
  gssize rret;
  int ret;
  GError *err = NULL;
  GSocketAddress *addr = NULL;

  do {
    num_packets = sizeof(packets) / sizeof(packets[0]);
    if ((ret = quicly_send(quiclysrc->conn, packets, &num_packets)) == 0) {
      addr = g_socket_address_new_from_native(&packets[0]->sa,
                                               packets[0]->salen);
      if (!addr) {
        g_printerr("Could not convert to native address in send\n");
        return -1;
      }
      for (i = 0; i != num_packets; ++i) {
        if ((rret = g_socket_send_to(quiclysrc->socket, addr, 
                                    (gchar *)packets[i]->data.base,
                                    packets[i]->data.len,
                                    quiclysrc->cancellable, &err)) < 0) {
          g_printerr("Send to failed. Error: %s\n", err->message);
          err = NULL;
        }
        quicly_packet_allocator_t *pa = quiclysrc->ctx.packet_allocator;
        pa->free_packet(pa, packets[i]);
      }
    }
  } while(ret == 0 && num_packets == sizeof(packets) / sizeof(packets[0]));
  if (addr)
    g_object_unref(addr);
  return ret;
}

static void dump_stats(FILE *fp, quicly_conn_t *conn)
{
    quicly_stats_t stats;

    quicly_get_stats(conn, &stats);
    fprintf(fp,
            "packets-received: %" PRIu64 ", packets-sent: %" PRIu64 ", packets-lost: %" PRIu64 ", ack-received: %" PRIu64
            ", bytes-received: %" PRIu64 ", bytes-sent: %" PRIu64 ", srtt: %" PRIu32 "\n",
            stats.num_packets.received, stats.num_packets.sent, stats.num_packets.lost, stats.num_packets.ack_received,
            stats.num_bytes.received, stats.num_bytes.sent, stats.rtt.smoothed);
}

static gboolean
gst_quiclysrc_stop (GstBaseSrc * src)
{
  GstQuiclysrc *quiclysrc = GST_QUICLYSRC (src);

  GST_DEBUG_OBJECT (quiclysrc, "stop");

  /* Print stats */
  g_print("###################### Quicly Stats ######################\n");
  dump_stats(stdout, quiclysrc->conn);
  g_print("\nQuiclysrc. Packets received: %lu. Kilobytes received: %lu\n.", 
          quiclysrc->num_packets, quiclysrc->num_bytes / 1000);
  g_print("###################### Quicly Stats ######################\n");
  
  gst_quiclysrc_free_cancellable(quiclysrc);

  return TRUE;
}

/* unlock any pending access to the resource. subclasses should unlock
 * any function ASAP. */

static gboolean
gst_quiclysrc_unlock (GstBaseSrc * src)
{
  GstQuiclysrc *quiclysrc = GST_QUICLYSRC (src);

  GST_DEBUG_OBJECT (quiclysrc, "unlock");
  g_cancellable_cancel (quiclysrc->cancellable);


  return TRUE;
}


/* Clear any pending unlock request, as we succeeded in unlocking */

static gboolean
gst_quiclysrc_unlock_stop (GstBaseSrc * src)
{
  GstQuiclysrc *quiclysrc = GST_QUICLYSRC (src);

  GST_DEBUG_OBJECT (quiclysrc, "unlock_stop");

  gst_quiclysrc_free_cancellable(quiclysrc);
  gst_quiclysrc_create_cancellable(quiclysrc);

  return TRUE;
}

/* ask the subclass to fill the buffer with data from offset and size */
static GstFlowReturn
gst_quiclysrc_fill (GstPushSrc * src, GstBuffer * buf)
{
  GstQuiclysrc *quiclysrc = GST_QUICLYSRC (src);

  GST_DEBUG_OBJECT (quiclysrc, "fill");

  if (quiclysrc->transport_close) {
    g_print("END OF STREAM\n");
    return GST_FLOW_EOS;
  }

  GstMapInfo info;
  GError *err = NULL;
  int ret;

  if (!gst_buffer_map(buf, &info, GST_MAP_READWRITE)) {
    g_printerr("Failed to map buffer\n");
    return GST_FLOW_ERROR;
  }

  if (info.size < 1200) {
    g_printerr("Buffer too small\n");
    return GST_FLOW_ERROR;
  }

  /* Check if we have stored frames from previous receives */
  if (quiclysrc->dgram != NULL) {
    if (quicly_dgram_can_get_data(quiclysrc->dgram) > 0) {
      gsize len = info.size;
      quicly_dgrambuf_ingress_get(quiclysrc->dgram, info.data, &len);
      gst_buffer_unmap(buf, &info);

      /* update stats */
      ++quiclysrc->num_packets;
      quiclysrc->num_bytes += len;

      /* Resize the buffer to the size of the received payload */
      gst_buffer_resize(buf, 0, len);
      quicly_dgrambuf_ingress_shift(quiclysrc->dgram, 1);
      return GST_FLOW_OK;
    }
  } else if (quiclysrc->stream != NULL) {
    ptls_iovec_t input;

    if ((input = quicly_streambuf_ingress_get(quiclysrc->stream)).len != 0) {
      /* TODO: Check rtp header version field */
      rtp_hdr_ *hdr = (rtp_hdr_*) input.base;
      if ((hdr->framing <= (input.len - 2)) && (input.len >= 2)) {

        /* Make sure rtp packet fits in buffer */
        if (hdr->framing > 1200) {
          g_printerr("RTP packet too large.\n");
          goto error;
        }

        memcpy(info.data, input.base + 2, hdr->framing);
        //g_print("FILL. Len: %li. Pushed data: %i\n", input.len, hdr->framing);
        /* stats */
        ++quiclysrc->num_packets;
        quiclysrc->num_bytes += hdr->framing;

        quicly_streambuf_ingress_shift(quiclysrc->stream, hdr->framing + 2);
        return GST_FLOW_OK;
      } 
    }
  }

  quiclysrc->ivec.buffer = info.data;
  quiclysrc->ivec.size = info.size;
  do {
    if (!g_socket_condition_timed_wait(quiclysrc->socket, G_IO_IN | G_IO_PRI, 6000000, quiclysrc->cancellable, &err)) {
      if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_BUSY) ||
          g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        goto stopped;
      } else if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_TIMED_OUT)) {
        g_printerr("Timeout in fill\n");
        goto end_stream;
      } else {
        goto error;
      }
    }
    if ((ret = receive_packet(quiclysrc, err)) != 0) {
      g_printerr("receive_packet failed in fill\n");
      if (err != NULL) {
        g_printerr("Error while receiving: %s\n", err->message);
        goto error;
      }
    }
  } while (quiclysrc->pushed == 0 && !quiclysrc->transport_close);

  if (quiclysrc->transport_close)
    return GST_FLOW_EOS;
  
  gst_buffer_unmap(buf, &info);

  /* Resize the buffer to the size of the received payload */
  gst_buffer_resize(buf, 0, quiclysrc->pushed);

  quiclysrc->pushed = 0;
  
  /* Check if there is something to send */
  if (quicly_get_first_timeout(quiclysrc->conn) <= 
                    quiclysrc->ctx.now->cb(quiclysrc->ctx.now)) {
    if (send_pending(quiclysrc) != 0) {
      g_printerr("Failed to send in fill\n");
    }
  }

  /* TODO: could add buffer metadata -> gst_buffer_add_net_address_meta(outbuf, saddr) */
  GST_LOG_OBJECT (quiclysrc, "Filled buffer");
  return GST_FLOW_OK;

  error:
    {
      gst_buffer_unmap(buf, &info);
      g_printerr("ERROR FILL\n");
      GST_DEBUG_OBJECT(quiclysrc, "Error in fill");
      g_clear_error(&err);
      return GST_FLOW_ERROR;
    }
  stopped:
    {
      gst_buffer_unmap(buf, &info);
      g_printerr("FLUSHING...\n");
      GST_DEBUG_OBJECT(quiclysrc, "FLUSHIN in fill");
      g_clear_error(&err);
      return GST_FLOW_FLUSHING;
    }
  end_stream:
    {
      gst_buffer_unmap(buf, &info);
      g_printerr("End of Stream...\n");
      GST_DEBUG_OBJECT(quiclysrc, "FLUSHIN in fill");
      g_clear_error(&err);
      return GST_FLOW_EOS;
    }
}



static int on_receive_dgram(quicly_dgram_t *dgram, const void *src, size_t len)
{
  GstQuiclysrc *quiclysrc = GST_QUICLYSRC (*quicly_get_data(dgram->conn));

  /* We already pushed a buffer from this packet, store this frame */
  if (quiclysrc->pushed > 0) {
    quicly_dgrambuf_ingress_receive(dgram, src, len);
    return 0;
  }

  /* write data to buffer */
  if (quiclysrc->connected && (quiclysrc->ivec.size >= len)) {
    memcpy(quiclysrc->ivec.buffer, src, len);
    quiclysrc->pushed = len;

    /* stats */
    ++quiclysrc->num_packets;
    quiclysrc->num_bytes += len;
  } else if (!quiclysrc->connected) {
    g_printerr("Received dgram without connection\n");
  } else {
    g_print("PACKET TOO LARGE\n: %ld", len);
  }

  return 0;
}

static void send_caps_event(GstQuiclysrc *quiclysrc)
{
  GstPad *pad = GST_BASE_SRC_PAD(quiclysrc);
  GST_DEBUG_OBJECT(quiclysrc, "Setting caps downstream.");
  if (!gst_pad_set_caps(pad, quiclysrc->caps))
    g_printerr("Could not set caps downstream\n");
}

/*
 * Would be better to differentiate by stream id. e.g. always send caps on stream id 1
 **/
static int on_receive_stream(quicly_stream_t *stream, size_t off, const void *src, size_t len)
{
  GstQuiclysrc *quiclysrc = GST_QUICLYSRC (*quicly_get_data(stream->conn));
  ptls_iovec_t input;
  char msg[] = "MSG";
  int ret;

  if ((ret = quicly_streambuf_ingress_receive(stream, off, src, len)) != 0)
    return ret;

  if ((input = quicly_streambuf_ingress_get(stream)).len != 0) {
    char head[4] = {input.base[0], input.base[1], input.base[2], '\0'};
    if (strcmp(head, msg) == 0) {
      char str[input.len];
      memcpy(str, input.base, input.len);
      char delim[] = ":;\n";
      char *token;
      char check[] = "DATA";
      GstStructure *cp;
      GstCaps *_caps;
      gboolean found = FALSE;

      token = strtok(str, delim);

      while (token != NULL) {
        if (found) {
          cp = gst_structure_from_string(token, NULL);
          _caps = gst_caps_new_full(cp, NULL);
          if (GST_IS_CAPS(_caps)) {
            if (quiclysrc->caps)
              gst_caps_unref(quiclysrc->caps);
            quiclysrc->caps = _caps;
            send_caps_event(quiclysrc);
            g_print("Caps set from source.\n");
            GST_DEBUG_OBJECT(quiclysrc, "Caps: %s", gst_caps_to_string(_caps));
          }
          break;
        }
        if (strcmp(token, check) == 0)
          found = TRUE;
        token = strtok(NULL, delim);
      }
      quicly_streambuf_ingress_shift(stream, input.len);
    } else {
      /* else: data stream */
      if (quiclysrc->stream == NULL)
        quiclysrc->stream = stream;

      /* already pushed to this buffer, skip */
      if (quiclysrc->pushed)
        return 0;

      /* TODO: Check rtp header version field */
      rtp_hdr_ *hdr = (rtp_hdr_*) input.base;
      
      if ((hdr->framing <= (input.len - 2)) && (input.len >= 2)) {
        if (quiclysrc->connected && (quiclysrc->ivec.size >= len)) {
          memcpy(quiclysrc->ivec.buffer, input.base + 2, hdr->framing);
          quiclysrc->pushed = hdr->framing;
          /* stats */
          ++quiclysrc->num_packets;
          quiclysrc->num_bytes += hdr->framing;
          quicly_streambuf_ingress_shift(stream, hdr->framing + 2);
        }
      } else {
        /* skip, not a complete rtp packet */
        return 0;
      }
    }
  }
  return 0;
}

static int on_dgram_open(quicly_dgram_open_t *self, quicly_dgram_t *dgram)
{
  GstQuiclysrc *quiclysrc = GST_QUICLYSRC (*quicly_get_data(dgram->conn));
  int ret;
  if ((ret = quicly_dgrambuf_create(dgram, sizeof(quicly_dgrambuf_t))) != 0)
      return ret;
  dgram->callbacks = &dgram_callbacks;
  quiclysrc->dgram = dgram;
  return 0;
}

static int on_stream_open(quicly_stream_open_t *self, quicly_stream_t *stream)
{
  int ret;

  if ((ret = quicly_streambuf_create(stream, sizeof(quicly_streambuf_t))) != 0)
      return ret;
  stream->callbacks = &stream_callbacks;
  return 0;
}

static void on_closed_by_peer(quicly_closed_by_peer_t *self, quicly_conn_t *conn, int err, uint64_t frame_type, const char *reason,
                              size_t reason_len)
{
    GstQuiclysrc *quiclysrc = GST_QUICLYSRC (*quicly_get_data(conn));
    quiclysrc->transport_close = TRUE;

    if (QUICLY_ERROR_IS_QUIC_TRANSPORT(err)) {
        fprintf(stderr, "transport close: code=%d\n", err);
    } else if (QUICLY_ERROR_IS_QUIC_APPLICATION(err)) {
        g_printerr("application close: code=%d\n", err);
    } else if (err == QUICLY_ERROR_RECEIVED_STATELESS_RESET) {
        g_printerr("stateless reset\n");
    } else {
        g_printerr("unexpected close:code=%d\n", err);
    }
}

static int on_stop_sending(quicly_stream_t *stream, int err)
{
    assert(QUICLY_ERROR_IS_QUIC_APPLICATION(err));
    g_printerr("received STOP_SENDING\n");
    return 0;
}

static int on_receive_reset(quicly_stream_t *stream, int err)
{
    assert(QUICLY_ERROR_IS_QUIC_APPLICATION(err));
    g_printerr("received RESET_STREAM\n");
    return 0;
}

int save_ticket_cb(ptls_save_ticket_t *_self, ptls_t *tls, ptls_iovec_t src)
{
    quicly_conn_t *conn = *ptls_get_data_ptr(tls);
    ptls_buffer_t buf;
    FILE *fp = NULL;
    int ret;

    if (ticket_file == NULL)
        return 0;

    ptls_buffer_init(&buf, "", 0);

    /* build data (session ticket and transport parameters) */
    ptls_buffer_push_block(&buf, 2, { ptls_buffer_pushv(&buf, src.base, src.len); });
    ptls_buffer_push_block(&buf, 2, {
        if ((ret = quicly_encode_transport_parameter_list(&buf, 1, quicly_get_peer_transport_parameters(conn), NULL, NULL)) != 0)
            goto Exit;
    });

    /* write file */
    if ((fp = fopen(ticket_file, "wb")) == NULL) {
        fprintf(stderr, "failed to open file:%s:%s\n", ticket_file, strerror(errno));
        ret = PTLS_ERROR_LIBRARY;
        goto Exit;
    }
    fwrite(buf.base, 1, buf.off, fp);

    ret = 0;
Exit:
    if (fp != NULL)
        fclose(fp);
    ptls_buffer_dispose(&buf);
    return 0;
}

static gboolean gst_quiclysrc_decide_allocation (GstBaseSrc * bsrc, GstQuery * query)
{
  GstQuiclysrc *quiclysrc;
  GstBufferPool *pool;
  gboolean update;
  GstStructure *config;
  GstCaps *caps = NULL;

  quiclysrc = GST_QUICLYSRC (bsrc);

  if (gst_query_get_n_allocation_pools(query) > 0) {
    update = TRUE;
  } else {
    update = FALSE;
  }

  pool = gst_buffer_pool_new();
  config = gst_buffer_pool_get_config(pool);
  gst_query_parse_allocation(query, &caps, NULL);
  gst_buffer_pool_config_set_params(config, caps, quiclysrc->udp_mtu, 0, 0);
  gst_buffer_pool_set_config(pool, config);

  if (update)
    gst_query_set_nth_allocation_pool(query, 0, pool, quiclysrc->udp_mtu, 0, 0);
  else
    gst_query_add_allocation_pool(query, pool, quiclysrc->udp_mtu, 0, 0);

  gst_object_unref(pool);

  return TRUE;
}


static gboolean
plugin_init (GstPlugin * plugin)
{

  /* FIXME Remember to set the rank if it's an element that is meant
     to be autoplugged by decodebin. */
  return gst_element_register (plugin, "quiclysrc", GST_RANK_NONE,
      GST_TYPE_QUICLYSRC);
}

/* FIXME: these are normally defined by the GStreamer build system.
   If you are creating an element to be included in gst-plugins-*,
   remove these, as they're always defined.  Otherwise, edit as
   appropriate for your external plugin package. */
#ifndef VERSION
#define VERSION "0.0.FIXME"
#endif
#ifndef PACKAGE
#define PACKAGE "FIXME_package"
#endif
#ifndef PACKAGE_NAME
#define PACKAGE_NAME "FIXME_package_name"
#endif
#ifndef GST_PACKAGE_ORIGIN
#define GST_PACKAGE_ORIGIN "http://FIXME.org/"
#endif

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    quiclysrc,
    "FIXME plugin description",
    plugin_init, VERSION, "LGPL", PACKAGE_NAME, GST_PACKAGE_ORIGIN)

