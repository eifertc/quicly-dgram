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
static gboolean gst_quiclysrc_start (GstBaseSrc * src);
static gboolean gst_quiclysrc_stop (GstBaseSrc * src);
static gboolean gst_quiclysrc_unlock (GstBaseSrc * src);
static gboolean gst_quiclysrc_unlock_stop (GstBaseSrc * src);
static gboolean gst_quiclysrc_decide_allocation (GstBaseSrc * bsrc, GstQuery * query);
static gboolean gst_quiclysrc_set_clock(GstQuiclysrc *quiclysrc, GstClock *clock);
gboolean send_async_ack_cb(GstClock *clock, GstClockTime t, GstClockID id, gpointer data);

// buffer list
static GstFlowReturn gst_quiclysrc_create(GstPushSrc *src, GstBuffer **buf);
static gboolean gst_quiclysrc_negotiate(GstBaseSrc *basesrc);
static gboolean gst_quiclysrc_free_buffer_list_mem(GstQuiclysrc *src);
static gboolean gst_quiclysrc_realloc_mem_sizes(GstQuiclysrc *src);
//static gboolean gst_quiclysrc_alloc_mem(GstQuiclysrc *src, GstMemory **pmem, GstMapInfo *map);
static gboolean gst_quiclysrc_ensure_mem(GstQuiclysrc *src);

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
static int receive_packet(GstQuiclysrc *quiclysrc, GError *err);
static GstStructure *gst_quiclysrc_create_stats(GstQuiclysrc *quiclysrc);

/* TODO: do something with that, needs to be a property */
static const char *session_file = NULL;

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
#define MAX_BUFFER_LIST_SIZE  24
#define SEND_CLOCK_TIME_NS    2000000

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
  PROP_QUICLY_MTU,
  PROP_STATS
};

/* rtp header */
typedef struct {
    uint8_t ver_p_x_cc;
    uint8_t m_pt;
    uint16_t seq_nr;
    uint32_t timestamp;
    uint32_t ssrc;
} rtp_hdr;

/* rtp header with framing for stream mode */
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
  //GstElementClass *gstelement_class = (GstElementClass *) klass;

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
  //push_src_class->fill = GST_DEBUG_FUNCPTR (gst_quiclysrc_fill);

  /* buffer list */
  push_src_class->create = GST_DEBUG_FUNCPTR (gst_quiclysrc_create);
  base_src_class->negotiate = GST_DEBUG_FUNCPTR (gst_quiclysrc_negotiate);

  //gstelement_class->set_clock = GST_DEBUG_FUNCPTR(gst_quiclysrc_set_clock);

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
  g_object_class_install_property(gobject_class, PROP_STATS,
          g_param_spec_boxed("stats", "Statistics", "Various Statistics",
          GST_TYPE_STRUCTURE, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
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
  quiclysrc->clockId = NULL;

  gst_base_src_set_live(GST_BASE_SRC(quiclysrc), TRUE);
  gst_base_src_set_format (GST_BASE_SRC (quiclysrc), GST_FORMAT_TIME);
  //gst_base_src_set_do_timestamp (GST_BASE_SRC (quiclysrc), TRUE);

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

  quiclysrc->conn = NULL;
  quiclysrc->dgram = NULL;
  quiclysrc->stream = NULL;
  /* -------- end context init --------------*/

  quiclysrc->num_packets = 0;
  quiclysrc->num_bytes = 0;
  quiclysrc->pushed = 0;
  quiclysrc->silent = FALSE;
  quiclysrc->connected = FALSE;
  quiclysrc->recv_buf = malloc(sizeof(gchar) * (2048 + 1));
  quiclysrc->recv_buf_size = 2048;

  /* init mem alloc for buffer list */
  quiclysrc->mem_list = NULL;
  quiclysrc->map_list = NULL;
  quiclysrc->vec_list = NULL;
  quiclysrc->pushed_list = NULL;
  quiclysrc->mem_list_allocated = FALSE;
  quiclysrc->mem_list_size = MAX_BUFFER_LIST_SIZE;
  quiclysrc->allocator = NULL;
  gst_allocation_params_init(&quiclysrc->params);
  gst_quiclysrc_realloc_mem_sizes(quiclysrc);
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
    case PROP_QUICLY_MTU: {
      guint tmp = g_value_get_uint(value);
      quiclysrc->quicly_mtu = (tmp + 28 > 1280) ? 1252 : tmp;
    }
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
    case PROP_STATS:
      g_value_take_boxed(value, gst_quiclysrc_create_stats(quiclysrc));
      break;
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

  if (quiclysrc->clockId != NULL) {
    gst_clock_id_unschedule(quiclysrc->clockId);
    gst_clock_id_unref(quiclysrc->clockId);
  }

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

  gst_quiclysrc_free_buffer_list_mem(quiclysrc);

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

  /* Quicly connect */
  int64_t timeout_at;
  int64_t delta;
  int64_t wait = 0;
  err = NULL;
  int ret;

  if ((ret = quicly_connect(&quiclysrc->conn, &quiclysrc->ctx,
                            quiclysrc->host,
                            &native_sa,
                            NULL,
                            &quiclysrc->next_cid,
                            quiclysrc->resumption_token,
                            &quiclysrc->hs_properties,
                            &quiclysrc->resumed_transport_params)) != 0) {
    g_printerr("Quicly connect failed\n");
    return FALSE;
  }
  ++quiclysrc->next_cid.master_id;

  if ((ret = send_pending(quiclysrc)) != 0)
    g_printerr("Could not send inital packets.\n");

  g_print("Connecting...");
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
  /* Schedule async callback to send acks */
  GstClock *clock = gst_system_clock_obtain();
  if (!gst_quiclysrc_set_clock(quiclysrc, clock))
    return FALSE;
  gst_object_unref(clock);

  /* set connected, for the on_receive functions */
  quiclysrc->connected = TRUE;
  /* set application context */
  quicly_set_data(quiclysrc->conn, (void*) quiclysrc);

  g_print("Done\n");
  return TRUE;
}

static int receive_packet(GstQuiclysrc *quiclysrc, GError *err)
{
  gssize rret;
  size_t off, plen;
  GSocketAddress *in_addr;
  struct sockaddr native_sa;

  if ((rret = g_socket_receive_from(quiclysrc->socket, &in_addr, 
                                    quiclysrc->recv_buf,
                                    quiclysrc->recv_buf_size,
                                    quiclysrc->cancellable,
                                    &err)) < 0) {
    g_printerr("Error receiving from socket: %s\n", err->message);
    return -1;
  }
  /* TODO: REMOVE THAT CONVERSION, best by using recvfrom directly instead of the g_socket variant? */
  gssize len = g_socket_address_get_native_size(in_addr);
  if (!g_socket_address_to_native(in_addr, &native_sa, len, &err)) {
    g_printerr("Could not convert GSocketAddress to native. Error: %s\n", err->message);
    g_object_unref(in_addr);
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
    GST_OBJECT_LOCK(quiclysrc);
    quicly_receive(quiclysrc->conn, NULL, &native_sa, &packet);
    GST_OBJECT_UNLOCK(quiclysrc);
    off += plen;
  }
  g_object_unref(in_addr);

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
      /* TODO: unnecessary? */
      if (num_packets > 0) {
        addr = g_socket_address_new_from_native(&packets[0]->dest.sa,
                                                 quicly_get_socklen(&packets[0]->dest.sa));
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
    }
  } while(ret == 0 && num_packets == sizeof(packets) / sizeof(packets[0]));
  if (addr)
    g_object_unref(addr);
  return ret;
}

static GstStructure *gst_quiclysrc_create_stats(GstQuiclysrc *quiclysrc)
{
  quicly_stats_t stats;
  GST_OBJECT_LOCK(quiclysrc);
  quicly_get_stats(quiclysrc->conn, &stats);
  GST_OBJECT_UNLOCK(quiclysrc);
  GstStructure *s;

  s = gst_structure_new("quiclysrc-stats",
      "packets-received", G_TYPE_UINT64, stats.num_packets.received,
      "packets-sent", G_TYPE_UINT64, stats.num_packets.sent,
      "packets-lost", G_TYPE_UINT64, stats.num_packets.lost,
      "acks-received", G_TYPE_UINT64, stats.num_packets.acked,
      "bytes-received", G_TYPE_UINT64, stats.num_bytes.received,
      "bytes-sent", G_TYPE_UINT64, stats.num_bytes.sent,
      "rtt-smoothed", G_TYPE_UINT, stats.rtt.smoothed,
      "rtt-latest", G_TYPE_UINT, stats.rtt.latest,
      "rtt-minimum", G_TYPE_UINT, stats.rtt.minimum,
      "rtt-variance", G_TYPE_UINT, stats.rtt.variance, NULL);

  return s;
}

static gboolean
gst_quiclysrc_stop (GstBaseSrc * src)
{
  GstQuiclysrc *quiclysrc = GST_QUICLYSRC (src);

  GST_DEBUG_OBJECT(quiclysrc, "Stop. Packets received: %lu. Kilobytes received: %lu\n.", 
          quiclysrc->num_packets, quiclysrc->num_bytes / 1000);
  
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

static void gst_quiclysrc_reset_memory_allocator(GstQuiclysrc *src)
{
  if (src->mem_list != NULL) {
    for (int i = 0; i < src->mem_list_size; i++) {
      if (src->mem_list[i] != NULL) {
        gst_memory_unmap(src->mem_list[i], src->map_list[i]);
        gst_memory_unref(src->mem_list[i]);
        src->mem_list[i] = NULL;
      }
      src->vec_list[i]->buffer = NULL;
      src->vec_list[i]->size = 0;
    }
  }

  if (src->allocator != NULL) {
    gst_object_unref(src->allocator);
    src->allocator = NULL;
  }
}

static gboolean gst_quiclysrc_negotiate(GstBaseSrc *basesrc)
{
  GstQuiclysrc *src = GST_QUICLYSRC_CAST(basesrc);
  gboolean ret;

  ret = GST_BASE_SRC_CLASS(gst_quiclysrc_parent_class)->negotiate(basesrc);

  if (ret) {
    GstAllocationParams new_params;
    GstAllocator *new_allocator = NULL;

    gst_base_src_get_allocator(basesrc, &new_allocator, &new_params);
    if (new_allocator == NULL)

    if (src->allocator != new_allocator || memcmp(&src->params, &new_params, sizeof(GstAllocationParams)) != 0) {
      gst_quiclysrc_reset_memory_allocator(src);

      src->allocator = new_allocator;
      src->params = new_params;

      GST_INFO_OBJECT(src, "new mem allocator\n");
    }
  }

  return ret;
}

static gboolean gst_quiclysrc_free_buffer_list_mem(GstQuiclysrc *src)
{
  if (src->mem_list != NULL) {
    for (int i = 0; i < src->mem_list_size; i++) {
      if (src->mem_list[i] != NULL) {
        gst_memory_unmap(src->mem_list[i], src->map_list[i]);
        gst_memory_unref(src->mem_list[i]);
        src->mem_list[i] = NULL;
      }
      free(src->map_list[i]);
      free(src->vec_list[i]);
    }
    free(src->map_list);
    free(src->vec_list);
    free(src->pushed_list);
  }

   if (src->mem_list != NULL) {
    free(src->mem_list);
  }

  return TRUE;
}

static gboolean gst_quiclysrc_realloc_mem_sizes(GstQuiclysrc *src)
{
  if (src->mem_list != NULL)
    gst_quiclysrc_free_buffer_list_mem(src);

  src->map_list = malloc(src->mem_list_size * sizeof(GstMapInfo *));
  src->vec_list = malloc(src->mem_list_size * sizeof(GInputVector *));
  src->pushed_list = (gint *) malloc(src->mem_list_size * sizeof(gint));
  src->mem_list = malloc(src->mem_list_size * sizeof(GstMemory *));
  for (int i = 0; i < src->mem_list_size; i++) {
    src->map_list[i] = malloc(sizeof(GstMapInfo));
    src->vec_list[i] = malloc(sizeof(GInputVector));
    src->mem_list[i] = NULL;
  }

  return TRUE;
}

static gboolean gst_quiclysrc_alloc_mem(GstQuiclysrc *src, GstMemory **pmem, GstMapInfo *map)
{
  GstMemory *mem;
  mem = gst_allocator_alloc(src->allocator, src->quicly_mtu, &src->params);

  if (!gst_memory_map(mem, map, GST_MAP_WRITE)) {
    gst_memory_unref(mem);
    memset(map, 0, sizeof(GstMapInfo));
    return FALSE;
  }
  *pmem = mem;
  return TRUE;
}

/* 
 * Allocate new memory for buffers
 */
static gboolean gst_quiclysrc_ensure_mem(GstQuiclysrc *src)
{
  if (!src->mem_list_allocated) {
    /* or use a parameter in the function call? */
    for (int i = 0; i < src->mem_list_size; i++) {
      if (src->mem_list[i] == NULL) {
        if (!gst_quiclysrc_alloc_mem(src, &src->mem_list[i], src->map_list[i]))
          return FALSE;
        src->vec_list[i]->buffer = src->map_list[i]->data;
        src->vec_list[i]->size = src->map_list[i]->size;
      }
    }
  }
  src->mem_list_allocated = TRUE;
  return TRUE;
}

static GstFlowReturn gst_quiclysrc_create(GstPushSrc *src, GstBuffer **buf)
{
  GstBaseSrc *base = GST_BASE_SRC_CAST(src);
  GstQuiclysrc *quiclysrc = GST_QUICLYSRC_CAST(src);

  if (quiclysrc->transport_close) {
    return GST_FLOW_EOS;
  }
  
  if (!gst_quiclysrc_ensure_mem(quiclysrc))
    return GST_FLOW_ERROR;
  
  gsize written = 0;
  gsize ret;
  GError *err = NULL;

  /**
   * Check if we have stored frames from previous receives. 
   * For dgrams, that should currently almost never happen, as we cap the amount of frames we receive
   * in one run to the size of the buffer list.
   * Only happens if the last packet we receive before the list is full contains multiple small frames.
   */
  if (quiclysrc->dgram != NULL) {
    while ((quicly_dgram_can_get_data(quiclysrc->dgram)) > 0 && (written < quiclysrc->mem_list_size)) {
      gsize len = quiclysrc->vec_list[written]->size;
      quicly_dgrambuf_ingress_get(quiclysrc->dgram, quiclysrc->vec_list[written]->buffer, &len);
      quiclysrc->pushed_list[written] = len;
      quiclysrc->pushed++;
      written++;

      /* update stats */
      ++quiclysrc->num_packets;
      quiclysrc->num_bytes += len;
      quicly_dgrambuf_ingress_shift(quiclysrc->dgram, 1);
    }
  } else if (quiclysrc->stream != NULL) {
    ptls_iovec_t input;
    gboolean skip = FALSE;
    while (((input = quicly_streambuf_ingress_get(quiclysrc->stream)).len != 0) && 
                                            (written < quiclysrc->mem_list_size) && !skip) {
      rtp_hdr_ *hdr = (rtp_hdr_*) input.base;
      if ((hdr->framing <= (input.len - 2)) && (input.len >= 2) &&
                      quiclysrc->vec_list[written]->size >= hdr->framing) {
        memcpy(quiclysrc->vec_list[written]->buffer, input.base + 2, hdr->framing);
        quiclysrc->pushed_list[written] = hdr->framing;
        quiclysrc->pushed++;
        written++;

        /* update stats */
        ++quiclysrc->num_packets;
        quiclysrc->num_bytes += hdr->framing;
        quicly_streambuf_ingress_shift(quiclysrc->stream, hdr->framing + 2);
      } else {
        /* skip, not a complete rtp package */
        skip = TRUE;
      }
    }
  }

  /* receive packets */
  GIOCondition cond = G_IO_IN | G_IO_PRI;
  GIOCondition out_cond;
  if (!g_socket_condition_timed_wait(quiclysrc->socket, G_IO_IN | G_IO_PRI, 6000000, quiclysrc->cancellable, &err)) {
    if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_BUSY) ||
        g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      goto stopped;
    } else if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_TIMED_OUT)) {
        g_printerr("Timeout in receive socket wait\n");
        goto end_stream;
    } else {
      goto error;
    }
  }
  do {
    if ((ret = receive_packet(quiclysrc, err)) != 0) {
      g_printerr("receive_packet failed in fill\n");
      if (err != NULL) {
        g_printerr("Error while receiving: %s\n", err->message);
        goto error;
      }
    }

    out_cond = g_socket_condition_check(quiclysrc->socket, cond);
    if (!(cond & out_cond) && (quiclysrc->pushed != 0)) {
      break;
    }

  } while ((!quiclysrc->transport_close) && (quiclysrc->pushed < quiclysrc->mem_list_size));

  /* Check if there is something to send */
  /*
  if (quicly_get_first_timeout(quiclysrc->conn) <= 
                  quiclysrc->ctx.now->cb(quiclysrc->ctx.now)) {
    if (send_pending(quiclysrc) != 0) {
        g_printerr("Failed to send in create\n");
    }
  }
  */

  GstBufferList *buf_list;
  GstBuffer *out_buf = NULL;
  
  buf_list = gst_buffer_list_new_sized(quiclysrc->pushed);
  for (int i = 0; i < quiclysrc->pushed; i++) {
    out_buf = gst_buffer_new();
    gst_buffer_append_memory(out_buf, quiclysrc->mem_list[i]);
    gst_memory_unmap(quiclysrc->mem_list[i], quiclysrc->map_list[i]);
    quiclysrc->vec_list[i]->buffer = NULL;
    quiclysrc->vec_list[i]->size = 0;
    gst_buffer_resize(out_buf, 0, quiclysrc->pushed_list[i]);
    quiclysrc->pushed_list[i] = 0;
    gst_buffer_list_insert(buf_list, -1, out_buf);
    quiclysrc->mem_list[i] = NULL;
  }

  gst_base_src_submit_buffer_list(base, buf_list);
  quiclysrc->pushed = 0;
  quiclysrc->mem_list_allocated = FALSE;
  *buf = NULL;

  if (quiclysrc->transport_close)
    goto end_stream;

  return GST_FLOW_OK;

  error:
    {
      GST_DEBUG_OBJECT(quiclysrc, "Error in create");
      g_clear_error(&err);
      return GST_FLOW_ERROR;
    }
  stopped:
    {
      GST_DEBUG_OBJECT(quiclysrc, "FLUSHING in create");
      g_clear_error(&err);
      return GST_FLOW_FLUSHING;
    }
  end_stream:
    {
      GST_DEBUG_OBJECT(quiclysrc, "End of stream in create");
      g_clear_error(&err);
      return GST_FLOW_EOS;
    }
}

/* buffer list version */
static int on_receive_dgram(quicly_dgram_t *dgram, const void *src, size_t len)
{
  GstQuiclysrc *quiclysrc = GST_QUICLYSRC (*quicly_get_data(dgram->conn));

  if (quiclysrc->pushed >= quiclysrc->mem_list_size) {
    quicly_dgrambuf_ingress_receive(dgram, src, len);
    return 0;
  }
  
  if (quiclysrc->connected && (quiclysrc->vec_list[quiclysrc->pushed]->size >= len)) {
    memcpy(quiclysrc->vec_list[quiclysrc->pushed]->buffer, src, len);
    quiclysrc->pushed_list[quiclysrc->pushed] = len;
    quiclysrc->pushed++;

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

/* Send ack for caps to server */
static void ack_caps_receive(GstQuiclysrc *quiclysrc)
{
  quicly_stream_t *stream;
  if (quicly_open_stream(quiclysrc->conn, &stream, 0) == 0) {
    gchar send[] = "MSG:CAPS;DATA:OK\n";
    quicly_streambuf_egress_write(stream, send, strlen(send));
    quicly_streambuf_egress_shutdown(stream);
    if (send_pending(quiclysrc) == 0)
      GST_INFO_OBJECT (quiclysrc, "Caps ack send");
  }
}

static void send_caps_event(GstQuiclysrc *quiclysrc)
{
  GstPad *pad = GST_BASE_SRC_PAD(quiclysrc);
  GST_DEBUG_OBJECT(quiclysrc, "Setting caps downstream.");
  if (gst_pad_set_caps(pad, quiclysrc->caps)) {
    ack_caps_receive(quiclysrc);
  } else {
    g_printerr("Could not set caps downstream\n");
  }
}

static void handle_meta_data_stream(GstQuiclysrc *quiclysrc, quicly_stream_t *stream, ptls_iovec_t *input)
{
  char str[input->len];
  memcpy(str, input->base, input->len);
  GST_DEBUG_OBJECT(quiclysrc, "Received MSG string: %s", str);
  char delim[] = ":;\n";
  char *token;
  char check[] = "DATA";
  GstStructure *cp;
  GstCaps *_caps;
  gboolean found = FALSE;

  token = strtok(str, delim);

  while (token != NULL) {
    if (found) {
      if ((cp = gst_structure_from_string(token, NULL)) == NULL) {
        /* Didn't get the whole caps string or broken. Skip */
        return;
      }
      if (((_caps = gst_caps_new_full(cp, NULL)) != NULL) && GST_IS_CAPS(_caps)) {
        if (quiclysrc->caps)
          gst_caps_unref(quiclysrc->caps);
        quiclysrc->caps = _caps;
        send_caps_event(quiclysrc);
        GST_INFO_OBJECT(quiclysrc, "Caps received: %s", gst_caps_to_string(_caps));
      }
      break;
    }
    if (strcmp(token, check) == 0)
      found = TRUE;
    token = strtok(NULL, delim);
  }
  g_print("SKIPPING STUFF\n");
  quicly_streambuf_ingress_shift(stream, input->len);
}

/*
 * Would be better to differentiate by stream id. e.g. always send caps on stream id 1
 * TODO: Regardless -> split the function up
 **/
static int on_receive_stream(quicly_stream_t *stream, size_t off, const void *src, size_t len)
{
  GstQuiclysrc *quiclysrc = GST_QUICLYSRC (*quicly_get_data(stream->conn));
  ptls_iovec_t input;
  int ret;

  if ((ret = quicly_streambuf_ingress_receive(stream, off, src, len)) != 0)
    return ret;

  if ((input = quicly_streambuf_ingress_get(stream)).len != 0) {
    if (quiclysrc->stream != stream) {
      char msg[] = "MSG";
      char head[4] = {input.base[0], input.base[1], input.base[2], '\0'};
      if (strcmp(head, msg) == 0) {
        /* Meta data stream */
        handle_meta_data_stream(quiclysrc, stream, &input);
        return 0;
      } else {
        /* New media data stream */
        quiclysrc->stream = stream;
      }
    }

    rtp_hdr_ *hdr = (rtp_hdr_*) input.base;
    uint8_t version = hdr->ver_p_x_cc >> 6;
    if (version != 2) {
      /* Not an rtp packet with stream framing. Skip */
      g_printerr("ILLEGAL PACKET FORMAT\n");
      return 0;
    }

    if (quiclysrc->pushed >= quiclysrc->mem_list_size) {
      /* skip, buffer list full */
      return 0;
    }
    
    /* Check if we can get a complete rtp packet */
    if ((hdr->framing <= (input.len - 2)) && (input.len >= 2)) {
      /* Check if the current buffer has enough space. Should always be true, if we set rtp payloader to 1200bytes */
      if (quiclysrc->connected && (quiclysrc->vec_list[quiclysrc->pushed]->size >= hdr->framing)) {
        memcpy(quiclysrc->vec_list[quiclysrc->pushed]->buffer, input.base + 2, hdr->framing);
        quiclysrc->pushed_list[quiclysrc->pushed] = hdr->framing;
        quiclysrc->pushed++;
        /* stats */
        ++quiclysrc->num_packets;
        quiclysrc->num_bytes += hdr->framing;
        quicly_streambuf_ingress_shift(stream, hdr->framing + 2);
      }
    } else {
      /* skip, not a complete rtp packet */
      g_print("RECEIVED. NOT COMPLETE RTP\n");
      return 0;
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

    /* Unref ack callback */
    if (quiclysrc->clockId != NULL) {
      gst_clock_id_unschedule(quiclysrc->clockId);
      gst_clock_id_unref(quiclysrc->clockId);
    }
    quiclysrc->clockId = NULL;

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

    if (session_file == NULL)
        return 0;

    ptls_buffer_init(&buf, "", 0);

    /* build data (session ticket and transport parameters) */
    ptls_buffer_push_block(&buf, 2, { ptls_buffer_pushv(&buf, src.base, src.len); });
    ptls_buffer_push_block(&buf, 2, {
        if ((ret = quicly_encode_transport_parameter_list(&buf, 1, quicly_get_peer_transport_parameters(conn), NULL, NULL, 0)) != 0)
            goto Exit;
    });

    /* write file */
    if ((fp = fopen(session_file, "wb")) == NULL) {
        fprintf(stderr, "failed to open file:%s:%s\n", session_file, strerror(errno));
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
  gst_buffer_pool_config_set_params(config, caps, quiclysrc->quicly_mtu, 0, 0);
  gst_buffer_pool_set_config(pool, config);

  if (update)
    gst_query_set_nth_allocation_pool(query, 0, pool, quiclysrc->quicly_mtu, 0, 0);
  else
    gst_query_add_allocation_pool(query, pool, quiclysrc->quicly_mtu, 0, 0);

  gst_object_unref(pool);

  return TRUE;
}

gboolean send_async_ack_cb(GstClock *clock, GstClockTime t, GstClockID id, gpointer data)
{
  GstQuiclysrc *quiclysrc = GST_QUICLYSRC(data);

    /* Check if there is something to send */
  if (quiclysrc->conn != NULL) {
    if (quicly_get_first_timeout(quiclysrc->conn) <= 
                    quiclysrc->ctx.now->cb(quiclysrc->ctx.now)) {
      GST_OBJECT_LOCK(quiclysrc);
      if (send_pending(quiclysrc) != 0) {
          g_printerr("Failed to send in create\n");
      }
      GST_OBJECT_UNLOCK(quiclysrc);
    }
  }

  return TRUE;
}

/* Obtain the pipeline clock and schedule a callback for receving quic packets */
static gboolean gst_quiclysrc_set_clock(GstQuiclysrc *quiclysrc, GstClock *clock)
{
  //GstQuiclysrc *quiclysrc = GST_QUICLYSRC(ele);
  if ((quiclysrc->clockId = gst_clock_new_periodic_id(clock, 
                                      gst_clock_get_internal_time(clock), 
                                      SEND_CLOCK_TIME_NS)) == NULL)
      return FALSE;

  if (gst_clock_id_wait_async(quiclysrc->clockId, send_async_ack_cb, quiclysrc, NULL) != GST_CLOCK_OK)
      return FALSE;
    
  return TRUE;
  //return GST_ELEMENT_CLASS(gst_quiclysrc_parent_class)->set_clock(ele, clock);
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

