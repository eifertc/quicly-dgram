#include <gst/gst.h>
#include <glib.h>
#include <stdint.h>
#include <glib/gstdio.h>
#include <errno.h>
#include <string.h>
#include <sys/time.h>
#include <pthread.h>
#include <gst/rtp/rtp.h>
#include <stdlib.h>
#include <stdio.h>
#include <gst/rtp/gstrtpbuffer.h>
#include <gst/rtp/gstrtcpbuffer.h>
#include <time.h>

//#define DEFAULT_STAT_TIME_NS 1000000000 /* get stats every second */
#define DEFAULT_STAT_TIME_NS 500000000 /* get stats every half second */
#define DEFAULT_RTP_MTU 1200
#define DEFAULT_HOST "127.0.0.1"
#define DEFAULT_PORT 5000

int rtp_packet_num = 0;
gssize rtp_bytes = 0;
int prev_seq = 0;
int packets_lost = 0;
int num_buffers = 0;
guint num_bytes = 0;
gboolean udp_timeout = FALSE;

/* fps */
pthread_mutex_t lock_fps;
gdouble avg_fps_g = 0.0;
gdouble fps_g = 0.0;
gdouble frame_drop_rate = 0.0;

/* udp jitter */
GstClockTime prev_arrival_time = 0;
GstClockTime prev_transit = 0;
guint64 jitter = 0;
guint udp_num_packets = 0;
guint num_jitter_spikes = 0;

typedef struct {
    uint8_t ver_p_x_cc;
    uint8_t m_pt;
    uint16_t seq_nr;
    uint32_t timestamp;
    uint32_t ssrc;
} rtp_hdr_;

uint64_t last_time = 0;
uint64_t avg_time = 0;
uint64_t num_buffers_rtp = 0;
uint64_t highest_jit = 0;

typedef struct {
    GstElement *net;
    GstElement *session;
    GstElement *jitterbuf;
    GstElement *scream;
    GstElement *rtcpSink;
    GstElement *sink;
    GstElement *internal_sink;
    GObject *internal_session;
    GstClockID clockId;
    GstPipeline *pipeline;
} Gst_elements;

typedef struct {
    uint64_t packets_sent;
    uint64_t packets_lost;
    uint64_t packets_received;
    uint64_t bytes_sent;
    uint64_t bytes_received;
    uint64_t bytes_lost;
    uint64_t jitbuf_pushed;
    uint64_t jitbuf_lost;
    uint64_t jitbuf_late;
    uint64_t rtcp_bytes_sent;
    uint64_t rtcp_packets_sent;
    uint64_t bytes_received_rtp_payload;
    uint64_t dropped_late;
    uint32_t num_jit_spikes;
} Stats;

typedef struct _AppData
{
    gchar *file_path;
    gchar *cert_file;
    gchar *key_file;
    gchar *host;
    FILE *stat_file_path;
    gchar *saveToFilePath;
    gint port;
    gint rtp_mtu;
    gboolean headless;
    gboolean stream_mode;
    gboolean rtcp;
    gboolean transcode;
    gboolean debug;
    gboolean udp;
    gboolean tcp;
    gboolean aux;
    gboolean scream;
    gboolean camera;
    gboolean verbose;
    gboolean quicNoCC;
    gint quic_drop_late;
    gboolean async_sink;
    Gst_elements elements;
    Stats stats;
    GstClockTime refTime;
    gint64 stat_interval;
} AppData;

typedef struct _SessionData
{
    int ref;
    guint sessionNum;
    GstElement *input;
    GstElement *output;
    GstElement *rtpbin;
    GstCaps *caps;
} SessionData;

static SessionData *
session_ref (SessionData * data)
{
  g_atomic_int_inc (&data->ref);
  return data;
}

static void
session_unref (gpointer data)
{
  SessionData *session = (SessionData *) data;
  if (g_atomic_int_dec_and_test (&session->ref)) {

    if (G_IS_OBJECT(session->rtpbin))
        g_object_unref(session->rtpbin);

    if (G_IS_OBJECT(session->caps))
        gst_caps_unref(session->caps);
    g_free (session);
  }
}

static SessionData *
session_new (guint sessionNum)
{
  SessionData *ret = g_new0 (SessionData, 1);
  ret->sessionNum = sessionNum;
  return session_ref (ret);
}

inline uint64_t get_time() {
    struct timeval t;
    gettimeofday(&t, NULL);
    return t.tv_sec * (int)1e6 + t.tv_usec;
}

gboolean cb_print_stats(GstClock *cl, GstClockTime t, GstClockID id, gpointer user_data)
{
    //clock_t tf = clock();
    AppData *data = (AppData *) user_data;
    GstStructure *stats;
    float time = (gst_clock_get_time(gst_system_clock_obtain()) - data->refTime) / 1.0e9f;

    if (data->file_path) {
        /* Server stats */
        guint64 bytes_sent, packets_sent, rtcp_bytes_sent = 0, rtcp_packets_sent = 0;
        if (data->udp) {
            g_signal_emit_by_name(data->elements.net, "get-stats", 
                                    data->host, data->port, &stats);
            gst_structure_get_uint64(stats, "bytes-sent", &bytes_sent);
            gst_structure_get_uint64(stats, "packets-sent", &packets_sent);
            gst_structure_free(stats);

            if (data->rtcp) {
                g_signal_emit_by_name(data->elements.rtcpSink, "get-stats", 
                                    data->host, data->port + 1, &stats);
                gst_structure_get_uint64(stats, "bytes-sent", &rtcp_bytes_sent);
                gst_structure_get_uint64(stats, "packets-sent", &rtcp_packets_sent);
                gst_structure_free(stats);
            }

            fprintf(data->stat_file_path, "%.3f,%lu,%lu,%lu,%lu", time, 
                            packets_sent-data->stats.packets_sent, 
                            bytes_sent-data->stats.packets_sent,
                            rtcp_packets_sent-data->stats.rtcp_packets_sent,
                            rtcp_bytes_sent-data->stats.rtcp_bytes_sent);
            data->stats.rtcp_packets_sent = rtcp_packets_sent;
            data->stats.rtcp_bytes_sent = rtcp_bytes_sent;

        } else {
            guint64 packets_received, packets_lost, 
                    bytes_received, bytes_in_flight, bytes_sent_media, dropped_late;
                    guint srtt, cwnd;
            g_object_get(data->elements.net, "stats", &stats, NULL);
            gst_structure_get_uint64(stats, "packets-sent", &packets_sent);
            gst_structure_get_uint64(stats, "packets-lost", &packets_lost);
            gst_structure_get_uint64(stats, "packets-received", &packets_received);
            //gst_structure_get_uint64(stats, "acks-received", &acks_received); /* Would be just the amount of packets without ack eliciting frames send from the receiver */
            gst_structure_get_uint64(stats, "bytes-sent", &bytes_sent);
            gst_structure_get_uint64(stats, "bytes-received", &bytes_received);
            gst_structure_get_uint64(stats, "bytes-in-flight", &bytes_in_flight);
            gst_structure_get_uint64(stats, "bytes-sent-media", &bytes_sent_media);
            gst_structure_get_uint64(stats, "dropped-late", &dropped_late);
            gst_structure_get_uint(stats, "rtt-smoothed", &srtt);
            gst_structure_get_uint(stats, "cwnd", &cwnd);
            gst_structure_free(stats);
            fprintf(data->stat_file_path, "%.3f,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%u,%u", time,
                    packets_sent-data->stats.packets_sent, 
                    packets_lost-data->stats.packets_lost, 
                    packets_received-data->stats.packets_received, 
                    dropped_late-data->stats.dropped_late,
                    bytes_sent-data->stats.bytes_sent, 
                    bytes_sent_media-data->stats.rtcp_bytes_sent,
                    bytes_received-data->stats.bytes_received, 
                    bytes_in_flight, srtt, cwnd);
            
            data->stats.dropped_late = dropped_late;
            data->stats.packets_lost = packets_lost;
            data->stats.packets_received = packets_received;
            data->stats.bytes_received = bytes_received;
            data->stats.rtcp_bytes_sent = bytes_sent_media;
        }
        data->stats.bytes_sent = bytes_sent;
        data->stats.packets_sent = packets_sent;
        if (data->scream) {
            gchar *s;
            g_object_get(data->elements.scream, "stats", &s, NULL);
            fprintf(data->stat_file_path, ",%s\n", s);
        } else {
            fprintf(data->stat_file_path, "\n");
        } 
    } else {
        /* Client stats */
        if (!data->elements.jitterbuf) {
            /* Bail out if pipeline has not been fully created yet */
            return TRUE;
        }

        /* Jitterbuffer */
        g_object_get(data->elements.jitterbuf, "stats", &stats, NULL);
        guint64 pushed, rtp_lost, late, jitbuf_jitter;
        gst_structure_get_uint64(stats, "num-pushed", &pushed);
        gst_structure_get_uint64(stats, "num-lost", &rtp_lost);
        gst_structure_get_uint64(stats, "num-late", &late);
        //gst_structure_get_uint64(stats, "num-duplicate", &dup); /* weird number TODO: fix*/
        gst_structure_get_uint64(stats, "avg-jitter", &jitbuf_jitter);
        gst_structure_free(stats);

        /* RtpSource */
        GValueArray *arr = NULL;
        gboolean internal;
        guint rtpsrc_jitter = 0;
        guint64 bitrate = 0;
        guint64 packets_received, bytes_received_rtp_payload, packets_sent = 0, bytes_sent = 0;
        g_object_get(data->elements.internal_session, "sources", &arr, NULL);
        if (arr) {
            for (int i = 0; i < arr->n_values; i++) {
                GObject *rtpsrc;
                rtpsrc = g_value_get_object(arr->values + i);
                g_object_get(rtpsrc, "stats", &stats, NULL);
                gst_structure_get_boolean(stats, "internal", &internal);
                if (!internal) {
                    gst_structure_get_uint(stats, "jitter", &rtpsrc_jitter);
                    gst_structure_get_uint64(stats, "bitrate", &bitrate);
                    gst_structure_get_uint64(stats, "octets-received", &bytes_received_rtp_payload);
                    gst_structure_get_uint64(stats, "packets-received", &packets_received);
                }
                gst_structure_free(stats);
            }
        } 
        #pragma GCC diagnostic push
        #pragma GCC diagnostic ignored "-Wdeprecated-declarations"
        g_value_array_free(arr);
        #pragma GCC diagnostic pop

        if (data->udp) {
            if (data->rtcp) {
                g_signal_emit_by_name(data->elements.rtcpSink, "get-stats", 
                                    data->host, data->port + 5, &stats);
                gst_structure_get_uint64(stats, "bytes-sent", &bytes_sent);
                gst_structure_get_uint64(stats, "packets-sent", &packets_sent);
                gst_structure_free(stats);
            }
            pthread_mutex_lock(&lock_fps);
            fprintf(data->stat_file_path, "%.3f,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%u,%lu,%u,%lu,%.2f,%.2f\n", 
                    time, packets_received-data->stats.packets_received, 
                    bytes_received_rtp_payload-data->stats.bytes_received_rtp_payload,
                    packets_sent-data->stats.packets_sent,
                    bytes_sent-data->stats.bytes_sent,
                    pushed-data->stats.jitbuf_pushed, 
                    rtp_lost-data->stats.jitbuf_lost, late-data->stats.jitbuf_late, 
                    jitbuf_jitter, rtpsrc_jitter,
                    jitter, num_jitter_spikes-data->stats.num_jit_spikes,
                    bitrate/1000,
                    avg_fps_g, fps_g);
            data->stats.num_jit_spikes = num_jitter_spikes;
            pthread_mutex_unlock(&lock_fps);
        } else {
            /* quicly */
            guint jit_spikes = 0;
            g_object_get(data->elements.net, "stats", &stats, NULL);
            guint64 packets_lost, bytes_received_quic_payload, bytes_received, jitter;
            gst_structure_get_uint64(stats, "packets-sent", &packets_sent);
            gst_structure_get_uint64(stats, "packets-lost", &packets_lost);
            gst_structure_get_uint64(stats, "packets-received", &packets_received);
            gst_structure_get_uint64(stats, "bytes-sent", &bytes_sent);
            gst_structure_get_uint64(stats, "bytes-received", &bytes_received);
            gst_structure_get_uint64(stats, "bytes-received-media", &bytes_received_quic_payload);
            gst_structure_get_uint64(stats, "jitter", &jitter);
            gst_structure_get_uint(stats, "jitter-spikes", &jit_spikes);
            gst_structure_free(stats);
            pthread_mutex_lock(&lock_fps);
            fprintf(data->stat_file_path, "%.3f,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%u,%lu,%u,%lu,%.2f,%.2f\n", 
                    time, 
                    packets_sent-data->stats.packets_sent, 
                    packets_lost-data->stats.packets_lost, 
                    packets_received-data->stats.packets_received, 
                    bytes_sent-data->stats.bytes_sent,
                    bytes_received-data->stats.bytes_received, 
                    bytes_received_quic_payload-data->stats.rtcp_bytes_sent,
                    bytes_received_rtp_payload-data->stats.bytes_received_rtp_payload,
                    pushed-data->stats.jitbuf_pushed, 
                    rtp_lost-data->stats.jitbuf_lost, 
                    late-data->stats.jitbuf_late, 
                    jitbuf_jitter, 
                    rtpsrc_jitter, 
                    jitter, 
                    jit_spikes-data->stats.num_jit_spikes,
                    bitrate/1000,
                    avg_fps_g, 
                    fps_g);
            pthread_mutex_unlock(&lock_fps);
            data->stats.num_jit_spikes = jit_spikes;
            data->stats.rtcp_bytes_sent = bytes_received_quic_payload;
            data->stats.bytes_received = bytes_received;
        }
        data->stats.bytes_sent = bytes_sent;
        data->stats.bytes_received_rtp_payload = bytes_received_rtp_payload;
        data->stats.packets_lost = packets_lost;
        data->stats.packets_sent = packets_sent;
        data->stats.packets_received = packets_received;
        data->stats.jitbuf_pushed = pushed;
        data->stats.jitbuf_lost = rtp_lost;
        data->stats.jitbuf_late = late;
    }

    //g_print("stat timing: %.6f\n", ((double)(clock() - tf)) / CLOCKS_PER_SEC);
    return TRUE;
}

void create_stat_collection_callback(GstPipeline *pipe, AppData *data)
{
    GstClock *clock = gst_pipeline_get_clock(pipe);
    gint64 interval;
    if (data->stat_interval == 0)
        interval = DEFAULT_STAT_TIME_NS;
    else 
        interval = data->stat_interval * 1000000;
    data->elements.clockId = gst_clock_new_periodic_id(clock, gst_clock_get_internal_time(clock), interval);
    if (!data->elements.clockId)
        g_print("Could not setup periodic clock\n");
    if (gst_clock_id_wait_async(data->elements.clockId, cb_print_stats, data, NULL) != GST_CLOCK_OK)
        g_print("Could not register periodic stats callback");
}

/* I have to use global variables here, because the signal emmited by fpsvideosink
 * behaves weird with regards to user data.
 * TODO: Should look into that. Some strange thread issue.
 */
void cb_fps_measurement(GstElement fpsSink, gdouble fps, gdouble droprate, gdouble avg_fps, gpointer user_data)
{
    pthread_mutex_lock(&lock_fps);
    avg_fps_g = avg_fps;
    fps_g = fps;
    frame_drop_rate = droprate;
    pthread_mutex_unlock(&lock_fps);
}

void init_elements(AppData *data)
{
    data->elements.net = NULL;
    data->elements.session = NULL;
    data->elements.sink = NULL;
    /*
    GstElement *jitterbuf;
    GstElement *scream;
    GstElement *rtcpSink;
    GObject *internal_session;
    GstClockID clockId;
    GstPipeline *pipeline;
    */
}

//static void on_stream_status(GstBus *bus, GstMessage *msg, gpointer user_data)
GstBusSyncReply on_stream_status(GstBus *bus, GstMessage *msg, gpointer user_data)
{   
    if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_STREAM_STATUS) {
        GstStreamStatusType type;
        GstElement *owner;
        const GValue *val;
        //GstTask *task = NULL;
        gchar *name;

        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS) {
            GMainLoop *loop = (GMainLoop *) user_data;
            g_print("End of stream. Stopping playback...\n");
            g_main_loop_quit(loop);
        }

        gst_message_parse_stream_status(msg, &type, &owner);
        val = gst_message_get_stream_status_object(msg);
        name = gst_element_get_name(owner);

        if (G_VALUE_TYPE(val) == GST_TYPE_TASK)
            g_print("VALUE IS TASK\n");

        switch (type) {
            case GST_STREAM_STATUS_TYPE_CREATE: {
                g_print("Stream CREATE. FROM: %s. thread id; %ld\n", name, pthread_self());
                break;
            }
            case GST_STREAM_STATUS_TYPE_START: {
                g_print("Stream START. FROM: %s\n", name);
                break;
            }
            case GST_STREAM_STATUS_TYPE_ENTER:
                g_print("Thread entered loop. Element: %s. ID: %ld\n", name, pthread_self());
                break;
            case GST_STREAM_STATUS_TYPE_PAUSE:
                g_print("Thread paused. Element: %s\n", name);
                break;
            case GST_STREAM_STATUS_TYPE_LEAVE:
                g_print("Thread left loop. Element: %s. ID: %ld\n", name, pthread_self());
                break;
            case GST_STREAM_STATUS_TYPE_STOP:
                g_print("Thread stopped. Element: %s\n", name);
                break;
            default:
                g_print("UNKNOWN TYPE\n");
                break;
        }
        g_free(name);
    }
    return GST_BUS_PASS;
}

static void
cb_timeout(GstBus *bus, GstMessage *msg, gpointer data)
{
    if (udp_timeout) {
        const GstStructure *st = gst_message_get_structure(msg);
        if (gst_structure_has_name(st, "GstUDPSrcTimeout")) {
            g_print("UDP timeout\n");
            GMainLoop *loop = (GMainLoop *) data;
            g_main_loop_quit(loop);
        }
    }
}

static void
cb_eos(GstBus *bus, GstMessage *message, gpointer data)
{
    GMainLoop *loop = (GMainLoop *) data;
    g_print("End of Stream.\n");
    g_main_loop_quit (loop);
}

static void
cb_error(GstBus *bus, GstMessage *msg, gpointer data)
{
    GMainLoop *loop = (GMainLoop *) data;
    gchar *debug;
    GError *error;

    gst_message_parse_error (msg, &error, &debug);
    g_free (debug);

    g_printerr ("Error: %s\n", error->message);
    g_error_free (error);

    g_main_loop_quit (loop);
}

static void
cb_qos(GstBus *bus, GstMessage *msg, gpointer data)
{
    guint64 processed;
    guint64 dropped;
    gint64 jitter;
    gchar *name;
    name = gst_object_get_name(msg->src);

    gst_message_parse_qos_values(msg, &jitter, NULL, NULL);
    gst_message_parse_qos_stats(msg, NULL, &processed, &dropped);
    g_print("QOS MESSAGE. From: %s. Jitter: %ld. Dropped: %lu. Processed: %lu.\n",
                name, jitter, dropped, processed);
    g_free(name);
}

static void
cb_stream_status(GstBus *bus, GstMessage *msg, gpointer data)
{
    gchar *name;
    name = gst_object_get_name(msg->src);

    GstStreamStatusType type;
    GstElement *owner;
    gst_message_parse_stream_status(msg, &type, &owner);
    gchar *oname = gst_element_get_name(owner);
    switch (type) {
        case GST_STREAM_STATUS_TYPE_CREATE:
            g_print("Thread create announced. Element: %s\n", oname);
            break;
        case GST_STREAM_STATUS_TYPE_START:
            g_print("Thread started. Element: %s\n", oname);
            break;
        case GST_STREAM_STATUS_TYPE_ENTER:
            g_print("Thread entered loop. Element: %s\n", oname);
            break;
        case GST_STREAM_STATUS_TYPE_PAUSE:
            g_print("Thread paused. Element: %s\n", oname);
            break;
        case GST_STREAM_STATUS_TYPE_LEAVE:
            g_print("Thread left loop. Element: %s\n", oname);
            break;
        case GST_STREAM_STATUS_TYPE_STOP:
            g_print("Thread stopped. Element: %s\n", oname);
            break;
        default:
            break;
    }
    g_free(oname);
    g_free(name);
}

static void
cb_state_change(GstBus *bus, GstMessage *msg, gpointer data)
{   
    gchar *name;
    name = gst_object_get_name(msg->src);
    GstState old;
    GstState new;
    gst_message_parse_state_changed(msg, &old, &new, NULL);
    g_print("Element %s changed state from %s to %s\n", name, 
            gst_element_state_get_name(old), gst_element_state_get_name(new));
    g_free(name);
}

static GstPadProbeReturn cb_udp_first_packet(GstPad *pad, GstPadProbeInfo *info, gpointer data)
{
    if (!udp_timeout)
        udp_timeout = TRUE;

    /* calc jitter */
    GstClockTime now = gst_clock_get_time(gst_system_clock_obtain());
    if (prev_arrival_time != 0) {
      guint32 transit = now - prev_arrival_time;
      guint32 tmp = prev_transit > transit ? 
                      prev_transit - transit : 
                      transit - prev_transit;
      if (tmp > 2 * jitter)
        num_jitter_spikes++;
      jitter = (udp_num_packets * jitter + tmp) / (udp_num_packets + 1);
      prev_transit = transit;
    }
    prev_arrival_time = now;
    udp_num_packets++;

    //return GST_PAD_PROBE_REMOVE;
    return GST_PAD_PROBE_OK;
}

/**
 * Debug. Inspect buffer lists passed along the gstreamer pipeline
 */
static GstPadProbeReturn cb_inspect_buf_list(GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
    GstMapInfo map;
    GstBufferList *buffer_list;
    GstBuffer *buffer;
    guint buffers, i;
    //int num_lost = 0;

    buffer_list = GST_PAD_PROBE_INFO_BUFFER_LIST(info);
    buffers = gst_buffer_list_length(buffer_list);

    for (i = 0; i < buffers; ++i) {
        num_buffers++;

        buffer = gst_buffer_list_get(buffer_list, i);
        gst_buffer_map(buffer, &map, GST_MAP_READ);
        num_bytes += map.size;

        /*
        rtp_hdr_ *hdr = (rtp_hdr_ *) map.data;
        g_print("%i ", hdr->seq_nr);
        */
    
        /*
        if (hdr->seq_nr != prev_seq + 256) {
            if (hdr->seq_nr < prev_seq) {
                num_lost += ((65535 - prev_seq) + hdr->seq_nr) / 256; 
            } else {
                num_lost += (hdr->seq_nr - prev_seq) / 256;
            }
            if (fPtr == NULL) {
                g_print("Seq NR: %i. Number lost: %i\n", hdr->seq_nr, num_lost);
            } else {
                fprintf(fPtr, "%s %i %s %i %s", "Seq nr: ", hdr->seq_nr, "Num lost: ", num_lost, "\n");
            }
        }
        if (hdr->seq_nr + 256 > 65535) {
            prev_seq = hdr->seq_nr - 65535;
        } else {
            prev_seq = hdr->seq_nr;
        }

        rtp_packet_num++;
        rtp_bytes += map.size;
        */
        gst_buffer_unmap(buffer, &map);
    }
    //packets_lost++;

    return GST_PAD_PROBE_OK;
}

/**
 * Debug. Inspect buffers passed along the gstreamer pipeline
 */
static GstPadProbeReturn cb_inspect_buf(GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
    
    GstMapInfo map;
    GstBuffer *buffer;
    //int num_lost = 0;

    buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    gst_buffer_map(buffer, &map, GST_MAP_READ);
    num_bytes += map.size;
    gst_buffer_unmap(buffer, &map);

    /*
    rtp_hdr_ *hdr = (rtp_hdr_ *) map.data;
    g_print("%i ", hdr->seq_nr);
    */
    
    /*
    if (hdr->seq_nr != prev_seq + 256) {
        if (hdr->seq_nr < prev_seq) {
            num_lost = ((65535 - prev_seq) + hdr->seq_nr) / 256; 
        } else {
            num_lost = (hdr->seq_nr - prev_seq) / 256;
        }
        if (fPtr == NULL) {
            g_print("Seq NR: %i. Number lost: %i\n", hdr->seq_nr, num_lost);
        } else {
            fprintf(fPtr, "%s %i %s %i %s", "Seq nr: ", hdr->seq_nr, "Num lost: ", num_lost, "\n");
        }
    }
    if (hdr->seq_nr + 256 > 65535) {
        prev_seq = hdr->seq_nr - 65535;
    } else {
        prev_seq = hdr->seq_nr;
    }

    rtp_packet_num++;
    rtp_bytes += map.size;
    gst_buffer_unmap(buffer, &map);
    */

    /*
    if (last_time != 0) {
        uint64_t t = get_time();
        uint64_t dif = t - last_time;
        avg_time += dif;
        last_time = t;
        ++num_buffers_rtp;
        if (dif > highest_jit)
            highest_jit = dif;
    } else {
        last_time = get_time();
    }
    */
    num_buffers++;

    return GST_PAD_PROBE_OK;
}

/* Feedback data from quicly. Forward to encoder or congestion control 
 * TODO: Report only the newly lost ones.
 */ 
static void 
cb_on_feedback_report(GstElement *ele, 
                      guint64 packets_sent, guint64 packets_lost, guint64 packets_acked,
                      guint64 bytes_sent, guint64 bytes_lost, guint64 bytes_acked,
                      guint64 latest_ack_send_time, guint64 latest_ack_recv_time,
                      guint64 bytes_in_flight, guint32 cwnd, gpointer user_data)
{
    g_print("Diff: %lu\n", latest_ack_recv_time - latest_ack_send_time);
}

static void on_pad_added(GstElement *ele, GstPad *pad, gpointer data)
{
    GstPad *sinkpad;
    GstElement *sink = (GstElement *) data;
    sinkpad = gst_element_get_static_pad(sink, "sink");

    if (gst_pad_link(pad, sinkpad) != 0) {
        gchar *name;
        name = gst_element_get_name (ele);
        g_print ("Could not link %s pad\n", name);
        g_free (name);
    }
    gst_object_unref(sinkpad);
}

static void 
cb_new_jitterbuf(GstBin *rtpbin, GstElement *jitterbuf, guint session, guint ssrc, gpointer data)
{
    AppData *adata = (AppData *) data;
    if (!adata->elements.jitterbuf)
        adata->elements.jitterbuf = jitterbuf;
}

static GstCaps *
cb_request_pt_map (GstElement * rtpbin, guint session, guint pt,
    gpointer user_data)
{
  SessionData *data = (SessionData *) user_data;
  gchar *caps_str;
  //g_print ("Looking for caps for pt %u in session %u, have %u\n", pt, session, data->sessionNum);
  if (session == data->sessionNum) {
    caps_str = gst_caps_to_string (data->caps);
    //g_print ("Returning %s\n", caps_str);
    g_free (caps_str);
    return gst_caps_ref (data->caps);
  }
  return NULL;
}

static void
cb_handle_new_stream(GstElement *ele, GstPad *pad, gpointer data)
{
    SessionData *session = (SessionData *) data;
    gchar *padName;
    gchar *myPrefix;

    padName = gst_pad_get_name (pad);
    myPrefix = g_strdup_printf ("recv_rtp_src_%u", session->sessionNum);

    //g_print ("New pad: %s, looking for %s_*\n", padName, myPrefix);

    if (g_str_has_prefix (padName, myPrefix)) {
    GstPad *outputSinkPad;
    GstElement *parent;

    parent = GST_ELEMENT (gst_element_get_parent (session->rtpbin));
    gst_bin_add (GST_BIN (parent), session->output);
    gst_element_sync_state_with_parent (session->output);
    gst_object_unref (parent);

    outputSinkPad = gst_element_get_static_pad (session->output, "sink");
    g_assert_cmpint (gst_pad_link (pad, outputSinkPad), ==, GST_PAD_LINK_OK);
    gst_object_unref (outputSinkPad);

    //g_print ("Linked!\n");
    }
    g_free (myPrefix);
    g_free (padName);
}

static GstElement *cb_request_aux_receiver(GstElement *rtpBin, guint sessid, SessionData *session)
{
    GstElement *rtx, *bin;
    GstPad *pad;
    gchar *name;
    GstStructure *pt_map;

    bin = gst_bin_new("aux_bin");
    rtx = gst_element_factory_make("rtprtxreceive", NULL);
    pt_map = gst_structure_new ("application/x-rtp-pt-map",
      "8", G_TYPE_UINT, 98, "96", G_TYPE_UINT, 99, NULL);
    g_object_set (rtx, "payload-type-map", pt_map, NULL);

    gst_structure_free (pt_map);
    gst_bin_add (GST_BIN (bin), rtx);

    pad = gst_element_get_static_pad (rtx, "src");
    name = g_strdup_printf ("src_%u", sessid);
    gst_element_add_pad (bin, gst_ghost_pad_new (name, pad));
    g_free (name);
    gst_object_unref (pad);

    pad = gst_element_get_static_pad (rtx, "sink");
    name = g_strdup_printf ("sink_%u", sessid);
    gst_element_add_pad (bin, gst_ghost_pad_new (name, pad));
    g_free (name);
    gst_object_unref (pad);

    return bin;
}

static GstElement *cb_request_aux_sender(GstElement *rtpbin, guint sessid, SessionData *session)
{
    GstElement *rtx, *bin;
    GstPad *pad;
    gchar *name;
    GstStructure *pt_map;

    GST_INFO ("creating AUX sender");
    bin = gst_bin_new (NULL);
    rtx = gst_element_factory_make ("rtprtxsend", NULL);
    pt_map = gst_structure_new ("application/x-rtp-pt-map",
      "8", G_TYPE_UINT, 98, "96", G_TYPE_UINT, 99, NULL);
    g_object_set (rtx, "payload-type-map", pt_map, NULL);
    gst_structure_free (pt_map);
    gst_bin_add (GST_BIN (bin), rtx);

    pad = gst_element_get_static_pad (rtx, "src");
    name = g_strdup_printf ("src_%u", sessid);
    gst_element_add_pad (bin, gst_ghost_pad_new (name, pad));
    g_free (name);
    gst_object_unref (pad);

    pad = gst_element_get_static_pad (rtx, "sink");
    name = g_strdup_printf ("sink_%u", sessid);
    gst_element_add_pad (bin, gst_ghost_pad_new (name, pad));
    g_free (name);
    gst_object_unref (pad);

    return bin;
}

static void cb_on_recv_rtcp(GObject *sess, GstBuffer *buffer, gpointer data)
{
  g_print("received RTCP\n");
  GstRTCPBuffer buf = { NULL, };
  if (!gst_rtcp_buffer_map(buffer, GST_MAP_READ, &buf)) {
    g_print("Unable to map rtcp buffer\n");
    return;
  }
  guint num = gst_rtcp_buffer_get_packet_count(&buf);
  g_print("Num RTCP packets: %u\n", num);

  GstRTCPPacket packet;
  if (!gst_rtcp_buffer_get_first_packet(&buf, &packet)) {
    g_print("No first packet\n");
    return;
  }

  GstRTCPType type = gst_rtcp_packet_get_type(&packet);
  g_print("Packet type: %i\n", type);

  if (type == 201) {
    guint blocks = gst_rtcp_packet_get_rb_count(&packet);
    g_print("Block count: %i\n", blocks);
    if (blocks >= 1) {

        guint32 ssrc, exthighestseq, jitter, lsr, dlsr;
        guint8 fractionlost;
        gint32 packetslost;
        gst_rtcp_packet_get_rb(&packet, 0, &ssrc, &fractionlost, &packetslost, &exthighestseq,
                              &jitter, &lsr, &dlsr);
        g_print("Fractionlost: %i, Packets lost: %i, ExtHighestSeq: %u, Jitter: %u, LSR: %u, DLSR: %u\n",
                 fractionlost, packetslost, exthighestseq, jitter, lsr, dlsr);
    }

    if (!gst_rtcp_packet_move_to_next(&packet))
      return;

    type = gst_rtcp_packet_get_type(&packet);
    g_print("Second Packet type: %i\n", type);

    if (!gst_rtcp_packet_move_to_next(&packet))
      return;

    type = gst_rtcp_packet_get_type(&packet);
    g_print("Third Packet type: %i\n", type);
  }
}

static void cb_on_send_rtcp(GObject *sess, GstBuffer *buffer, gboolean early, gpointer data)
{
  g_print("Sending RTCP\n");
}

static void add_server_stream(GstPipeline *pipe, GstElement *rtpBin, SessionData *session, AppData *sdata)
{
    GstElement *rtpSink;
    gchar *padName;

    if (sdata->udp) {
        g_print("UDP transport for rtp stream\n");
        rtpSink = gst_element_factory_make("udpsink", "rtpsink");
        g_object_set (rtpSink, "port", sdata->port, "host", sdata->host, "sync", !sdata->async_sink, NULL);
    } else {
        g_print("QUIC transport for rtp stream\n");
        rtpSink = gst_element_factory_make("quiclysink", "rtpsink");
        g_object_set(rtpSink, "bind-port", sdata->port, 
                          "cert", sdata->cert_file,
                          "key", sdata->key_file, 
                          "sync", !sdata->async_sink, NULL);
        if (sdata->rtp_mtu != 0)
             g_object_set(rtpSink, "quicly-mtu", sdata->rtp_mtu, NULL);

        if (sdata->quicNoCC)
            g_object_set(rtpSink, "app-cc", TRUE, NULL);

        if (sdata->scream) 
            g_object_set(rtpSink, "app-cc", TRUE, "feedback", TRUE, NULL);

        if (sdata->stream_mode)
            g_object_set(rtpSink, "stream-mode", TRUE, NULL);

        if (sdata->debug) {
            g_signal_connect(rtpSink, "on-feedback-report", G_CALLBACK(cb_on_feedback_report), NULL);
        }

        if (sdata->quic_drop_late != -1)
            g_object_set(rtpSink, "drop-late", TRUE, NULL);
    }
    sdata->elements.net = rtpSink;

    if (sdata->rtcp) {
        GstElement *rtcpSink = gst_element_factory_make ("udpsink", NULL);
        GstElement *rtcpSrc = gst_element_factory_make ("udpsrc", NULL);
        
        g_object_set (rtcpSink, "port", sdata->port + 1, "host", sdata->host, "sync",
                        FALSE, "async", FALSE, NULL);
        g_object_set (rtcpSrc, "port", sdata->port + 5, NULL);

        gst_bin_add_many(GST_BIN(pipe), rtpSink, rtcpSink, rtcpSrc, session->input, NULL);

        padName = g_strdup_printf ("send_rtcp_src_%u", session->sessionNum);
        gst_element_link_pads (rtpBin, padName, rtcpSink, "sink");
        g_free (padName);

        padName = g_strdup_printf ("recv_rtcp_sink_%u", session->sessionNum);
        gst_element_link_pads (rtcpSrc, "src", rtpBin, padName);
        g_free (padName);

        sdata->elements.rtcpSink = rtcpSink;
    } else {
        gst_bin_add_many(GST_BIN(pipe), rtpSink, session->input, NULL);
    }

    if (sdata->aux) {
        g_signal_connect(rtpBin, "request-aux-sender", 
            G_CALLBACK(cb_request_aux_sender), session);
    }

    padName = g_strdup_printf ("send_rtp_sink_%u", session->sessionNum);
    gst_element_link_pads (session->input, "src", rtpBin, padName);
    g_free (padName);

    padName = g_strdup_printf ("send_rtp_src_%u", session->sessionNum);
    gst_element_link_pads (rtpBin, padName, rtpSink, "sink");
    g_free (padName);
    
    session_unref(session);
}

static SessionData *make_server_video_session(guint sessionNum, AppData *sdata)
{
    SessionData *session;
    GstElement *demux, *lastEle;
    GstBin *videoBin = GST_BIN(gst_bin_new("videobin"));
    GstElement *filesrc = gst_element_factory_make("filesrc", "fs");
    GstElement *rtph264pay = gst_element_factory_make("rtph264pay", "rtppay");
    GstElement *queue2_1 = gst_element_factory_make("queue2", "queue2_1");
    GstElement *queue2_2 = gst_element_factory_make("queue2", "queue2_2");

    /* Identity element sync's on the clock and basically makes a live stream
     * out of my filesrc. Without that, scream would not work
     */
    GstElement *identity = gst_element_factory_make("identity", "identity");
    lastEle = rtph264pay;

    /* Choose demuxer based on video container type*/
    char comp_str[strlen(sdata->file_path)];
    memcpy(comp_str, sdata->file_path, strlen(sdata->file_path)); 
    char *type = "mkv";
    char *ptr = &sdata->file_path[strlen(sdata->file_path)-3];
    if (strncmp(ptr, type, 3) == 0) {
        demux = gst_element_factory_make("matroskademux", "demux");
    } else {
        demux = gst_element_factory_make("qtdemux", "demux");
    }

    if (!videoBin || !filesrc || !demux || !rtph264pay || !identity) {
        g_printerr ("One element could not be created. Exiting.\n");
        return NULL;
    }

    g_object_set(identity, "sync", TRUE, NULL);
    g_object_set(G_OBJECT(filesrc), "location", sdata->file_path, NULL);
    if (sdata->rtp_mtu > 1252)
        sdata->rtp_mtu = 1252;
    g_object_set(G_OBJECT(rtph264pay), "mtu", sdata->rtp_mtu == 0 ? DEFAULT_RTP_MTU : sdata->rtp_mtu,
    "config-interval", 2, NULL);
    //g_object_set(G_OBJECT(rtpmp4gpay), "mtu", 1200, NULL);

    /* Link with or without transcoding */
    if (sdata->transcode || sdata->scream) {
        GstElement *decoder = gst_element_factory_make("avdec_h264", "decode");
        //GstElement *queue = gst_element_factory_make("queue", "decode_queue");
        GstElement *encoder = gst_element_factory_make("x264enc", "video");
        /* TODO: add bitrate option */
        g_object_set(encoder,"tune", 4, NULL);

        if (sdata->scream) {
            GstElement *scream = gst_element_factory_make("gscreamtx", "scream");
            //GstElement *cam = gst_element_factory_make("v412src", "camsrc");
            g_object_set(scream, "media-src", 0, NULL);
            sdata->elements.scream = scream;

            if (!sdata->udp)
                g_object_set(scream, "quic", TRUE, NULL);

            gst_bin_add_many (videoBin, filesrc, demux, decoder, identity,
                                encoder, scream, rtph264pay, queue2_1, queue2_2, NULL);
            gst_element_link_many(decoder, identity, queue2_1, encoder, queue2_2, rtph264pay, scream, NULL);
            lastEle = scream;
        } else {
            /* Transcode only */
            g_object_set(encoder, "bitrate", 5000, NULL);
            gst_bin_add_many (videoBin, filesrc, demux, decoder, identity,
                              queue2_1, queue2_2, encoder, rtph264pay, NULL);
            gst_element_link_many(decoder, identity, queue2_1, encoder, queue2_2, rtph264pay, NULL);
        }
        
        if (!gst_element_link(filesrc, demux))
            g_warning("Failed to link filesrc\n");
        g_signal_connect(demux, "pad-added", G_CALLBACK(on_pad_added), decoder);
        
    } else {
        gst_bin_add_many(videoBin, filesrc, demux, identity, queue2_1, rtph264pay, NULL);
        if (!gst_element_link(filesrc, demux))
            g_warning("Failed to link filesrc\n");
        g_signal_connect(demux, "pad-added", G_CALLBACK(on_pad_added), identity);
        gst_element_link_many(identity, queue2_1, rtph264pay, NULL);
    }

    if (sdata->debug) {
        /* get rtp source pad */
        GstPad *pad;
        pad = gst_element_get_static_pad(rtph264pay, "src");
        gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER_LIST, 
                         (GstPadProbeCallback) cb_inspect_buf_list,
                         NULL, NULL);
        gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER,
                         (GstPadProbeCallback) cb_inspect_buf,
                          NULL, NULL);
        gst_object_unref(pad);
    }

    /* create src ghost pad on bin */
    GstPad *srcPad = gst_element_get_static_pad(lastEle, "src");
    GstPad *binPad = gst_ghost_pad_new("src", srcPad);
    gst_element_add_pad(GST_ELEMENT(videoBin), binPad);

    session = session_new(sessionNum);
    session->input = GST_ELEMENT(videoBin);

    return session;
}

/* Print verbose server stats */
void print_server_stats(AppData *sdata)
{
    GstStructure *stats;
    gchar *str;
    g_object_get(gst_bin_get_by_name(GST_BIN(sdata->elements.pipeline), "rtppay"), "stats", &stats, NULL);
    str = gst_structure_to_string(stats);
    g_print("##### RTP Payloader stats:\n%s\n", str);
    gst_structure_free(stats);
    g_free(str);

    g_object_get(sdata->elements.session, "stats", &stats, NULL);
    str = gst_structure_to_string(stats);
    g_print("##### RTPSession stats:\n%s\n", str);
    gst_structure_free(stats);
    g_free(str);

    if (sdata->udp) {
        g_signal_emit_by_name(gst_bin_get_by_name(GST_BIN(sdata->elements.pipeline), "rtpsink"),
                              "get-stats", sdata->host, sdata->port, &stats);
        str = gst_structure_to_string(stats);
        g_print("##### UdpSink stats:\n%s\n", str);
        gst_structure_free(stats);
        g_free(str);
    } else {
        g_object_get(gst_bin_get_by_name(GST_BIN(sdata->elements.pipeline), "rtpsink"), "stats", &stats, NULL);
        str = gst_structure_to_string(stats);
        g_print("##### QuiclySink stats:\n%s\n", str);
        gst_structure_free(stats);
        g_free(str);
    }

    gint64 rate;
    GstElement *qu;
    if ((qu = gst_bin_get_by_name(GST_BIN(sdata->elements.pipeline), "queue2_1")) != NULL) {
        g_object_get(qu, "avg-in-rate", &rate, NULL);
        g_print("##### Rate queue2_1 (Mbit/s): %lu\n", rate / 125000);
        gst_object_unref(qu);
    }
    if ((qu = gst_bin_get_by_name(GST_BIN(sdata->elements.pipeline), "queue2_2")) != NULL) {
        g_object_get(qu, "avg-in-rate", &rate, NULL);
        g_print("##### Rate queue2_2 (Mbit/s): %lu\n", rate / 125000);
        gst_object_unref(qu);
    }

    if (sdata->debug)
        g_print("Num buffers at sink: %i. Num bytes: %u\n", num_buffers, num_bytes);
}

int run_server(AppData *sdata)
{
    g_print("Starting as server.\n");

    if (sdata->file_path == NULL) {
        g_printerr("Missing source video file path\n");
        return -1;
    }

    if ((sdata->cert_file == NULL || sdata->key_file == NULL) && !sdata->udp) {
        g_printerr("Missing key/cert files\n");
        return -1;
    }

    GstPipeline *pipe;
    GstBus *bus;
    SessionData *videoSession;
    GstElement *rtpBin;
    GMainLoop *loop;

    loop = g_main_loop_new(NULL, FALSE);

    pipe = GST_PIPELINE(gst_pipeline_new("mainPipeline"));

    /* message handler */
    bus = gst_element_get_bus (GST_ELEMENT (pipe));
    //g_signal_connect (bus, "message::state-changed", G_CALLBACK (cb_state_change), pipe);
    g_signal_connect(bus, "message::eos", G_CALLBACK(cb_eos), loop);
    g_signal_connect(bus, "message::qos", G_CALLBACK(cb_qos), NULL);
    g_signal_connect(bus, "message::error", G_CALLBACK(cb_error), loop);
    if (sdata->debug) {
        g_signal_connect(bus, "message::state_changed", G_CALLBACK(cb_state_change), loop);
        g_signal_connect(bus, "message::stream_status", G_CALLBACK(cb_stream_status), loop);
    }
    gst_bus_add_signal_watch(bus);
    gst_object_unref (bus);

    if ((rtpBin = gst_element_factory_make("rtpbin", "rtpbin")) == NULL) {
        g_print("Could not create RtpBin.\n");
        return -1;
    }
    g_object_set(rtpBin, "rtp-profile", GST_RTP_PROFILE_AVPF, NULL);

    gst_bin_add (GST_BIN (pipe), rtpBin);

    videoSession = make_server_video_session(0, sdata);
    add_server_stream(pipe, rtpBin, videoSession, sdata);

    GstElement *session;
    g_signal_emit_by_name(rtpBin, "get-session", 0, &session);
    GObject *int_session;
    g_signal_emit_by_name(rtpBin, "get-internal-session", 0, &int_session);
    sdata->elements.session = session;
    sdata->elements.internal_session = int_session;
    sdata->elements.pipeline = pipe;


    if (sdata->rtcp && sdata->debug) {
        g_signal_connect(int_session, "on_receiving_rtcp", G_CALLBACK(cb_on_recv_rtcp), NULL);
        g_signal_connect(int_session, "on_send_rtcp", G_CALLBACK(cb_on_send_rtcp), NULL);
    }

    /* start the pipeline */
    gst_element_set_state(GST_ELEMENT(pipe), GST_STATE_PLAYING);

    /* Gather statistics periodically */
    sdata->refTime = gst_clock_get_time(gst_system_clock_obtain());
    if (sdata->stat_file_path) 
        create_stat_collection_callback(pipe, sdata);

    g_main_loop_run(loop);

    /* Out of the main loop */
    if (sdata->stat_file_path) {
        gst_clock_id_unschedule(sdata->elements.clockId);
        gst_clock_id_unref(sdata->elements.clockId);
    }

    if (sdata->verbose)
        print_server_stats(sdata);

    gst_element_set_state (GST_ELEMENT(pipe), GST_STATE_NULL);

    g_print ("Deleting pipeline\n");
    gst_object_unref(pipe);
    g_main_loop_unref(loop);
    return 0;
}

static void
add_client_stream(GstElement *pipe, GstElement *rtpBin, SessionData *session, AppData *adata)
{
    GstElement *rtpSrc;
    gchar *padName;

    session->rtpbin = g_object_ref(rtpBin);

    if (adata->udp) {
        rtpSrc = gst_element_factory_make("udpsrc", "rtpsrc");
        // timeout: 1550000000
        g_object_set(rtpSrc, "port", adata->port, "caps", 
                        session->caps, "timeout", 1550000000, NULL);

        /* Use a probe pad to recognize when we first receive a packet.
         * After the first packet is received, the timeout activates
         */
        GstPad *pad;
        pad = gst_element_get_static_pad(rtpSrc, "src");
        gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, 
                         (GstPadProbeCallback) cb_udp_first_packet,
                         NULL, NULL);
    } else {
        rtpSrc = gst_element_factory_make("quiclysrc", "rtpsrc");
        g_object_set(G_OBJECT(rtpSrc), "host", adata->host, "port", adata->port, NULL);
    }
    adata->elements.net = rtpSrc;

    if (adata->rtcp) {
        GstElement *rtcpSrc = gst_element_factory_make("udpsrc", NULL);
        GstElement *rtcpSink = gst_element_factory_make("udpsink", NULL);
        g_object_set(rtcpSink, "port", adata->port + 5, "host", adata->host, "sync",
                    FALSE, "async", FALSE, NULL);
        g_object_set(rtcpSrc, "port", adata->port + 1, NULL);
        gst_bin_add_many(GST_BIN(pipe), rtpSrc, rtcpSrc, rtcpSink, NULL);

        padName = g_strdup_printf("recv_rtcp_sink_%u", session->sessionNum);
        gst_element_link_pads(rtcpSrc, "src", rtpBin, padName);
        g_free (padName);

        padName = g_strdup_printf("send_rtcp_src_%u", session->sessionNum);
        gst_element_link_pads(rtpBin, padName, rtcpSink, "sink");
        g_free(padName);

        adata->elements.rtcpSink = rtcpSink;
    } else {
        gst_bin_add(GST_BIN(pipe), rtpSrc);
    }

    if (adata->aux) {
        g_signal_connect (rtpBin, "request-aux-receiver",
                         G_CALLBACK(cb_request_aux_receiver), session);
    }

    g_signal_connect_data(rtpBin, "pad-added", G_CALLBACK(cb_handle_new_stream),
      session_ref(session), (GClosureNotify) session_unref, 0);

    g_signal_connect_data(rtpBin, "request-pt-map", G_CALLBACK(cb_request_pt_map),
      session_ref(session), (GClosureNotify) session_unref, 0);

    padName = g_strdup_printf("recv_rtp_sink_%u", session->sessionNum);
    gst_element_link_pads(rtpSrc, "src", rtpBin, padName);
    g_free(padName);

    session_unref(session);
}

static SessionData *make_client_video_session(guint sessionNum, AppData *cdata)
{
    GstElement *depay, *decoder, *sink, *queue, *ghostSink;

    SessionData *ret = session_new(sessionNum);
    GstBin *bin = GST_BIN(gst_bin_new("videobin"));
    depay = gst_element_factory_make("rtph264depay", "rtp");
    decoder = gst_element_factory_make ("avdec_h264", NULL);
    queue = gst_element_factory_make("queue", "thread_queue");
    ghostSink = queue;

    if (cdata->headless) {
        sink = gst_element_factory_make("fpsdisplaysink", "sink");
        GstElement *internal_sink = gst_element_factory_make("fakesink", "internal_sink");
        g_object_set(internal_sink, "sync", FALSE, "qos", FALSE, NULL);
        g_object_set(sink, "signal-fps-measurements", TRUE, "video-sink", internal_sink, 
                    "sync", FALSE, NULL);
        g_signal_connect(sink, "fps-measurements", G_CALLBACK(cb_fps_measurement), NULL);
        cdata->elements.internal_sink = internal_sink;
    } else if (cdata->saveToFilePath != NULL) {
        sink = gst_element_factory_make("filesink", "sink");
        g_object_set(sink, "location", cdata->saveToFilePath, NULL);
    } else {
        sink = gst_element_factory_make("fpsdisplaysink", "sink");
        GstElement *internal_sink;
        if ((internal_sink = gst_element_factory_make("glimagesink", "internal_sink")) == NULL)
            internal_sink = gst_element_factory_make("autovideosink", "internal_sink");

        //g_object_set(internal_sink, "sync", FALSE, "async", FALSE, NULL);
        //g_object_set(sink, "sync", TRUE, NULL);
        g_object_set(sink, "signal-fps-measurements", TRUE, "video-sink", internal_sink, NULL);
        
        g_signal_connect(sink, "fps-measurements", G_CALLBACK(cb_fps_measurement), NULL);
        cdata->elements.internal_sink = internal_sink;
    }
    cdata->elements.sink = sink;

    if (!bin || !depay || !decoder || !sink || !queue) {
        g_printerr ("One element could not be created. Exiting.\n");
        return NULL;
    }

    if (cdata->scream && cdata->udp) {
        GstElement *scream = gst_element_factory_make("gscreamrx", "scream");

        gst_bin_add_many(bin, scream, depay, decoder, sink, NULL);
        gst_element_link_many(scream, depay, decoder, sink, NULL);
        ghostSink = scream;
    } else if (cdata->saveToFilePath != NULL) {
        GstElement *mux = gst_element_factory_make("matroskamux", "mux");
        GstElement *parse = gst_element_factory_make("h264parse", "parse");
        gst_bin_add_many(bin, depay, parse, mux, sink, queue, NULL);
        gst_element_link_many(queue, depay, parse, mux, sink, NULL);
    } else {
        gst_bin_add_many(bin, depay, decoder, queue, sink, NULL);
        gst_element_link_many(queue, depay, decoder, sink, NULL);
    }
    
    GstPad *sinkPad = gst_element_get_static_pad(ghostSink, "sink");
    GstPad *binPad = gst_ghost_pad_new ("sink", sinkPad);
    gst_element_add_pad (GST_ELEMENT (bin), binPad);

    if (cdata->debug) {
        /* get rtp sink pad */
        GstPad *pad;
        pad = gst_element_get_static_pad(depay, "sink");
        gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER_LIST, 
                         (GstPadProbeCallback) cb_inspect_buf_list,
                         NULL, NULL);
        gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER,
                         (GstPadProbeCallback) cb_inspect_buf,
                          NULL, NULL);
        gst_object_unref(pad);
    }

    ret->output = GST_ELEMENT(bin);
    ret->caps = gst_caps_new_simple ("application/x-rtp",
      "media", G_TYPE_STRING, "video",
      "clock-rate", G_TYPE_INT, 90000,
      "encoding-name", G_TYPE_STRING, "H264", NULL);

    return ret;
}

/* Print verbose stats */
void print_client_stats(AppData *cdata)
{
    GstStructure *stats;
    gchar *str;
    if (cdata->elements.jitterbuf) {
        g_object_get(cdata->elements.jitterbuf, "stats", &stats, NULL);
        str = gst_structure_to_string(stats);
        g_print("##### Jitterbuffer stats:\n%s\n", str);
        gst_structure_free(stats);
        g_free(str);

        //g_object_get(session, "stats", &stats, NULL);
        //str = gst_structure_to_string(stats);
        //g_print("##### RTPSession stats:\n%s\n", str);
        //gst_structure_free(stats);
        //g_free(str);

        GValueArray *arr = NULL;
        g_object_get(cdata->elements.internal_session, "sources", &arr, NULL);

        gboolean internal;
        if (arr) {
            for (int i = 0; i < arr->n_values; i++) {
                GObject *rtpsrc;
                rtpsrc = g_value_get_object(arr->values + i);
                g_object_get(rtpsrc, "stats", &stats, NULL);
                g_print("RTPSRC NUMBER: %i\n", i);
                gst_structure_get_boolean(stats, "internal", &internal);
                if (!internal) {
                    guint jitter;
                    guint64 bitrate;
                    gst_structure_get_uint(stats, "jitter", &jitter);
                    gst_structure_get_uint64(stats, "bitrate", &bitrate);

                    g_print("##### RTPSrc Nr.%i stats:\n", i);
                    g_print("Bitrate: %luKbit/s. Jitter: %ums\n", bitrate/1000, jitter);

                    guint64 bytes_received;
                    guint64 bytes_sent;
                    guint64 packets_sent;
                    guint64 packets_received;
                    gst_structure_get_uint64(stats, "octets-received", &bytes_received);
                    gst_structure_get_uint64(stats, "octets-sent", &bytes_sent);
                    gst_structure_get_uint64(stats, "packets-sent", &packets_sent);
                    gst_structure_get_uint64(stats, "packets-received", &packets_received);
                    g_print("Bytes_sent: %lu. Bytes_received: %lu\n", bytes_sent, bytes_received);
                    g_print("Packets_sent: %lu. Packets_received: %lu\n", packets_sent, packets_received);

                    gst_structure_free(stats);
                }
            }
        }
        #pragma GCC diagnostic push
        #pragma GCC diagnostic ignored "-Wdeprecated-declarations"
        g_value_array_free(arr);
        #pragma GCC diagnostic pop
    }
    g_print("Src avg jitter: %lu, Num. jitter spikes: %u, num_packets: %u\n", jitter, num_jitter_spikes, udp_num_packets);

    if(!cdata->udp) {
        g_object_get(gst_bin_get_by_name(GST_BIN(cdata->elements.pipeline), "rtpsrc"), "stats", &stats, NULL);
        str = gst_structure_to_string(stats);
        g_print("##### QuiclySrc stats:\n%s\n", str);
        gst_structure_free(stats);
        g_free(str);
    }

    if (cdata->debug) {
        g_print("Num rtp buffers: %i. Num bytes: %u\n", num_buffers, num_bytes);
        /*
        g_print("Quiclysrc src pad. Packets pushed: %i. Packets lost: %i. Bytes: %lu\n", rtp_packet_num, packets_lost, rtp_bytes);
        g_print("\nAvg time between pushed buffers (in micro seconds): %lu. Highest: %lu\n", 
                avg_time / num_buffers_rtp, highest_jit);
        */
    }
}

int run_client(AppData *cdata)
{
    g_print("Starting as client.\n");

    GstPipeline *pipe;
    SessionData *videoSession;
    GstElement *rtpBin;
    GstBus *bus;
    GMainLoop *loop;

    loop = g_main_loop_new(NULL, FALSE);
    pipe = GST_PIPELINE(gst_pipeline_new("mainPipeline"));

    /* message handlers */
    bus = gst_element_get_bus (GST_ELEMENT (pipe));
    g_signal_connect(bus, "message::eos", G_CALLBACK(cb_eos), loop);
    g_signal_connect(bus, "message::error", G_CALLBACK(cb_error), loop);
    g_signal_connect(bus, "message::element", G_CALLBACK(cb_timeout), loop);
    g_signal_connect(bus, "message::qos", G_CALLBACK(cb_qos), NULL);
    //g_signal_connect(bus, "message::state-changed", G_CALLBACK(cb_state_change), NULL);
    gst_bus_add_signal_watch(bus);
    gst_object_unref (bus);
    
    rtpBin = gst_element_factory_make("rtpbin", "rtpbin");
    gst_bin_add(GST_BIN(pipe), rtpBin);
    g_object_set (rtpBin, "latency", 100, "do-retransmission", cdata->aux,
      "rtp-profile", GST_RTP_PROFILE_AVPF, NULL);

    g_signal_connect(rtpBin, "new-jitterbuffer", G_CALLBACK(cb_new_jitterbuf), cdata);

    videoSession = make_client_video_session(0, cdata);
    add_client_stream(GST_ELEMENT(pipe), rtpBin, videoSession, cdata);

    GstElement *session;
    g_signal_emit_by_name(rtpBin, "get-session", 0, &session);
    GObject *int_session;
    g_signal_emit_by_name(rtpBin, "get-internal-session", 0, &int_session);
    cdata->elements.session = session;
    cdata->elements.internal_session = int_session;
    cdata->elements.pipeline = pipe;

    /* start the pipeline */
    gst_element_set_state(GST_ELEMENT(pipe), GST_STATE_PLAYING);

    /* Setup callback for periodic stat receive */
    cdata->refTime = gst_clock_get_time(gst_system_clock_obtain());
    if (cdata->stat_file_path)
        create_stat_collection_callback(pipe, cdata);

    g_main_loop_run(loop);

    // print graph
    //GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(pipe), GST_DEBUG_GRAPH_SHOW_ALL, "mainPipeline");

    /* Out of the main loop */
    if (cdata->stat_file_path) {
        gst_clock_id_unschedule(cdata->elements.clockId);
        gst_clock_id_unref(cdata->elements.clockId);
    }

    if (cdata->verbose)
        print_client_stats(cdata);

    gst_element_set_state(GST_ELEMENT(pipe), GST_STATE_NULL);

    g_print("Deleting pipeline\n");
    gst_object_unref (GST_OBJECT(pipe));
    g_main_loop_unref(loop);

    return 0;
}

int main (int argc, char *argv[])
{
    /* Parse command line options */
    AppData data;
    data.host = DEFAULT_HOST;
    data.port = DEFAULT_PORT;
    data.rtp_mtu = 0;
    data.headless = FALSE;
    data.debug = FALSE;
    data.stream_mode = FALSE;
    data.udp = FALSE;
    data.tcp = FALSE;
    data.transcode = FALSE;
    data.rtcp = FALSE;
    data.aux = FALSE;
    data.scream = FALSE;
    data.camera = FALSE;
    data.quic_drop_late = -1;
    data.file_path = NULL;
    data.cert_file = NULL;
    data.key_file = NULL;;
    data.elements.jitterbuf = NULL;
    data.stat_file_path = NULL;
    data.verbose = FALSE;
    data.quicNoCC = FALSE;
    data.saveToFilePath = NULL;
    data.stat_interval = 0;
    data.async_sink = FALSE;
    gchar *logfile = NULL;
    GOptionContext *ctx;
    GError *err = NULL;
    gchar **plugins = NULL;
    init_elements(&data);
    memset(&data.stats, 0, sizeof(Stats));
    pthread_mutex_init(&lock_fps, NULL);
    
    GOptionEntry entries[] = {
        {"scream", 's', 0, G_OPTION_ARG_NONE, &data.scream,
         "Use rmcat scream cc. Default: False", NULL},
        {"udp", 'U', 0, G_OPTION_ARG_NONE, &data.udp,
         "Use udp transport. Default: Quic.", NULL},
        {"tcp", 'T', 0, G_OPTION_ARG_NONE, &data.tcp,
         "Use tcp transport. Default: Quic.", NULL},
        {"rtcp", 'r', 0, G_OPTION_ARG_NONE, &data.rtcp,
         "Enable RTCP messages. Default: False.", NULL},
        {"aux", 'a', 0, G_OPTION_ARG_NONE, &data.aux,
         "Enable rtp retransmission. Default: False.", NULL},
        {"transcode", 't', 0, G_OPTION_ARG_NONE, &data.transcode,
         "Enable transcoding for dynamic bitrate. Default: False.", NULL},
        {"file", 'f', 0, G_OPTION_ARG_STRING, &data.file_path,
         "Server. Video file path", NULL},
        {"cert", 'c', 0, G_OPTION_ARG_STRING, &data.cert_file,
         "Server (Quic). Certificate file path", NULL},
        {"key", 'k', 0, G_OPTION_ARG_STRING, &data.key_file,
         "Server (Quic). Key file path", NULL},
        {"plugin-path", 'P', 0, G_OPTION_ARG_STRING_ARRAY, &plugins,
         "custom gstreamer plugin folder", NULL},
        {"stream_mode", 'm', 0, G_OPTION_ARG_NONE, &data.stream_mode,
         "Server (Quic). Use streams instead of datagrams", NULL},
        {"rtp-mtu", 'M', 0, G_OPTION_ARG_INT, &data.rtp_mtu,
         "MTU in rtp payloader. Default: 1200. Max value is 1252 currently, because of hardcoded macro in quicly",
          NULL},
        {"debug", 'd', 0, G_OPTION_ARG_NONE, &data.debug,
         "Print debug info", NULL},
        {"stat-interval", 'i', 0, G_OPTION_ARG_INT64, &data.stat_interval,
         "Time between stat collection (in ms). Default: 500", NULL},
        {"disableCC", 'D', 0, G_OPTION_ARG_NONE, &data.quicNoCC,
         "Disable quic CC. Default: False.", NULL},
        {"host", 'h', 0, G_OPTION_ARG_STRING, &data.host,
         "Host to connect to.", NULL},
        {"port", 'p', 0, G_OPTION_ARG_INT, &data.port,
         "Port to connect to", NULL},
        {"headless", 'H', 0, G_OPTION_ARG_NONE, &data.headless,
         "Client. Use fakesink", NULL},
        {"logfile", 'l', 0, G_OPTION_ARG_STRING, &logfile,
         "Log stats. Filepath or stdout", NULL}, 
        {"dropLate", 'L', 0, G_OPTION_ARG_INT, &data.quic_drop_late,
         "Quic. Drop late packets. -1: Never, 0: Drop immediately if can't send, >0: use expiration time", NULL}, 
        {"filesink", 'S', 0, G_OPTION_ARG_STRING, &data.saveToFilePath,
         "Client. Save video to file", NULL},
        {"verbose", 'v', 0, G_OPTION_ARG_NONE, &data.verbose,
         "Print additional information", NULL}, 
        {"async", 'y', 0, G_OPTION_ARG_NONE, &data.async_sink,
         "Server. Don't sync on the clock in the sink. Default: False", NULL},
        {NULL}
    };

    ctx = g_option_context_new("-c CERT_FILE -k KEY_FILE -f VIDEO_FILE");
    g_option_context_set_summary(ctx, "Supported encoding: H264\nSupported container: avi, mkv, mp4");
    g_option_context_add_main_entries(ctx, entries, NULL);
    g_option_context_add_group(ctx, gst_init_get_option_group());
    if (!g_option_context_parse(ctx, &argc, &argv, &err)) {
        g_printerr("Failed to init %s\n", err->message);
        g_clear_error(&err);
        g_option_context_free(ctx);
        return 1;
    }
    g_option_context_free(ctx);

    gst_init(NULL, NULL);

    /* Set plugin paths */
    if (plugins == NULL) {
        g_print("No plugin paths specified. Trying default dev paths.\n");
        plugins = malloc(4 * sizeof(gchar*));
        plugins[0] = "./libgst";
        plugins[1] = "../../scream/code/gscream/gst-gscreamtx/gst-plugin/src/.libs";
        plugins[2] = "../../scream/code/gscream/gst-gscreamrx/gst-plugin/src/.libs";
        plugins[3] = NULL;
    } 

    GstRegistry *reg;
    reg = gst_registry_get();
    for (int i = 0; plugins[i] != NULL; i++) {
        if (gst_registry_scan_path(reg, plugins[i]) == FALSE) {
            g_print("Invalid Plugin Path: %s\n", plugins[i]);
            return -1;
        }
    }

    if (logfile != NULL) {
        if (strcmp(logfile, "stdout") == 0) {
            data.stat_file_path = stdout;
        } else {
            data.stat_file_path = fopen(logfile, "a");
            if (data.stat_file_path == NULL) {
                g_printerr("Could not open file. Err: %s\n", strerror(errno));
                return -1;
            }
            /* Print application info */
            fprintf(data.stat_file_path, "#app:%s,transport:%s,cc:%s,rtp-mtu:%i,drop-late:%i,output-file:%s,video:%s\n", 
                                        data.file_path ? "server" : "client", 
                                        data.udp ? "udp" : (data.stream_mode ? "quic-stream" : "quic-dgram"),
                                        data.scream ? "scream" : data.udp ? "none" : ((data.quicNoCC ? "none" : "quic")),
                                        data.rtp_mtu == 0 ? DEFAULT_RTP_MTU : data.rtp_mtu, 
                                        data.quic_drop_late,
                                        data.saveToFilePath != NULL ? data.saveToFilePath : "none",
                                        data.file_path ? data.file_path : "none");
            if (data.file_path) {
                /* Print value explanation for server */
                char *s = (char *) malloc(500);
                char *s2 = (char *) malloc(500);
                if (data.udp)
                    sprintf(s, "#args:time, u-packets-sent, u-bytes-sent, rtcp-packets-sent, "
                                "rtcp-bytes-sent");
                else
                    sprintf(s, "#args:time, q-packets-sent, q-packets-lost, q-packets-received, "
                                "packets-dropped-late, q-bytes-sent, q-bytes-sent-media, q-bytes-received, "
                                "q-bytes-in-flight, q-srtt, q-cwnd");

                if (data.scream)
                    sprintf(s2, ", sc-queue-delay, sc-owd, sc-srtt, sc-cwnd, sc-bytes-in-flight, "
                                "sc-rate-transmitted, sc-target-bitrate, sc-rtp-rate, sc-rate-lost\n");
                else
                    sprintf(s2, "\n");

                strcat(s, s2);
                fprintf(data.stat_file_path, "%s", s);
                free(s);
                free(s2);
            } else {
                /* Print value explanation for client */
                if (data.udp)
                    fprintf(data.stat_file_path, "#args:time, packets-received, bytes-received-rtp-payload, "
                                                 "rtcp-packets-sent, rtcp-bytes-sent, jitbuf-pushed, jitbuf_lost, "
                                                 "jitbuf_late, jitbuf-jitter(ns), rtpsrc-jitter, src-jitter, "
                                                 "jitter-spikes, rtpsrc-bitrate(kbit/s), avg-fps, fps\n");
                else
                    fprintf(data.stat_file_path, "#args:time, packets-sent, packets-lost, packets-received, bytes-sent, bytes-received, bytes-received-quic-payload, "
                                                 "bytes-received-rtp-payload, jitbuf-pushed, jitbuf_lost, jitbuf_late, jitbuf-jitter(ns), rtpsrc-jitter, src-jitter, "
                                                 "jitter-spikes, rtpsrc-bitrate(kbit/s), avg-fps, fps\n");
            }
        }
        g_free(logfile);                   
    }
    /* handle different modes */
    if (data.scream) {
        data.rtcp = data.udp ? TRUE : FALSE;
        data.aux = FALSE;
        data.transcode = TRUE;
    }

    int ret;
    if (data.file_path) 
        ret = run_server(&data);
    else  
        ret = run_client(&data);

    if (ret != 0) {
        g_printerr("Init failed. Exit...\n");
        return 1;
    }

    if (data.stat_file_path)
        fclose(data.stat_file_path);

    return 0;
}