#include <gst/gst.h>
#include <glib.h>
#include <gio/gio.h>
#include <sys/types.h>
#include <stdint.h>
#include <sys/time.h>
#include <stdio.h>

#include <sys/socket.h>

GstElement *stats_element;
gboolean pipeline_ready = FALSE;

static gboolean msg_handler(GstBus *bus, GstMessage *msg, gpointer data)
{
    GMainLoop *loop = (GMainLoop *) data;
    const GstStructure *st = gst_message_get_structure (msg);
    /*
    const gchar *type, *src;
    type = GST_MESSAGE_TYPE_NAME(msg);
    src = GST_MESSAGE_SRC_NAME(msg);
    g_print("MSG FROM: %s. TYPE: %s\n", src, type);
    */
    gchar *ob_name = GST_OBJECT_NAME(msg->src);
    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_BUFFERING: {
            gint percent = 0;
            gst_message_parse_buffering(msg, &percent);
            g_print("Buffering: %i\n", percent);
        }
        case GST_MESSAGE_EOS:
            g_print("End of stream\n");
            g_main_loop_quit(loop);
            break;
        case GST_MESSAGE_ERROR: {
            gchar *debug;
            GError *error;

            gst_message_parse_error (msg, &error, &debug);
            g_free (debug);

            g_printerr ("Error: %s\n", error->message);
            g_error_free (error);

            g_main_loop_quit (loop);
          break;
        }
        case GST_MESSAGE_ELEMENT:
            if (!gst_structure_has_name(st, "GstUDPSrcTimeout"))
                break;

            g_print("UDP Timeout. Stopping...\n");
            g_main_loop_quit(loop);
            break;
        case GST_MESSAGE_STATE_CHANGED: {
            /*
            GstState old;
            GstState new;
            //GstState pending;
            gst_message_parse_state_changed(msg, &old, &new, NULL);
            g_print("Element %s changed state from %s to %s\n", ob_name, 
                    gst_element_state_get_name(old), gst_element_state_get_name(new));
            if ((strcmp(ob_name, "streamer") == 0) && (new == GST_STATE_READY)) {
                g_print("Pipeline Ready\n");
                pipeline_ready = TRUE;
            }
            */
            break;
        }
        case GST_MESSAGE_STREAM_STATUS: {
            GstStreamStatusType type;
            GstElement *owner;
            gst_message_parse_stream_status(msg, &type, &owner);
            gchar *name = gst_element_get_name(owner);
            switch (type) {
                case GST_STREAM_STATUS_TYPE_CREATE:
                    g_print("Thread create announced. Element: %s\n", name);
                    break;
                case GST_STREAM_STATUS_TYPE_START:
                    g_print("Thread started. Element: %s\n", name);
                    break;
                case GST_STREAM_STATUS_TYPE_ENTER:
                    g_print("Thread entered loop. Element: %s\n", name);
                    break;
                case GST_STREAM_STATUS_TYPE_PAUSE:
                    g_print("Thread paused. Element: %s\n", name);
                    break;
                case GST_STREAM_STATUS_TYPE_LEAVE:
                    g_print("Thread left loop. Element: %s\n", name);
                    break;
                case GST_STREAM_STATUS_TYPE_STOP:
                    g_print("Thread stopped. Element: %s\n", name);
                    break;
                default:
                    break;
            }
            g_free(name);
            break;
        }
        default:
            break;
    }
    return TRUE;
}

typedef struct {
    uint8_t ver_p_x_cc;
    uint8_t m_pt;
    uint16_t seq_nr;
    uint32_t timestamp;
    uint32_t ssrc;
} rtp_hdr_;

inline uint64_t get_time() {
    struct timeval t;
    gettimeofday(&t, NULL);
    return t.tv_sec * (int)1e6 + t.tv_usec;
}

uint64_t last_time = 0;
uint64_t avg_time = 0;
uint64_t num_buffers = 0;
uint64_t highest_jit = 0;
int num_packets = 0;

struct connection {
    gchar *host;
    gint port;
    GstElement *pipeline;
};

static void get_static_caps(gboolean bunny, GstCaps **caps)
{
    if (bunny) {
        *caps = gst_caps_new_simple("application/x-rtp",
                                   "media", G_TYPE_STRING, "video",
                                   "clock-rate", G_TYPE_INT, 90000,
                                   "encoding-name", G_TYPE_STRING, "H264",
                                   "packetization-mode", G_TYPE_STRING, "1",
                                   "profile-level-id", G_TYPE_STRING, "640033",
                                   "sprop-parameter-sets", G_TYPE_STRING, "Z2QAM6xyhEB4AiflwEQAAAMABAAAAwDwPGDGEYA\\=\\,aOhDiSyL",
                                   "payload", G_TYPE_INT, 96,
                                    NULL);
    } else {
        *caps = gst_caps_new_simple("application/x-rtp",
                                    "encoding-name", G_TYPE_STRING, "H264",
                                    "payload", G_TYPE_INT, 96,
                                    NULL);
        /*
        *caps = gst_caps_new_simple("application/x-rtp",
                                   "media", G_TYPE_STRING, "video",
                                   "clock-rate", G_TYPE_INT, 90000,
                                   "encoding-name", G_TYPE_STRING, "H264",
                                   "packetization-mode", G_TYPE_STRING, "1",
                                   "profile-level-id", G_TYPE_STRING, "42c01f",
                                   "sprop-parameter-sets", G_TYPE_STRING, "Z0LAH9kAUAW7/wB4AFsQAAADABAAAAMDAPGDJIA\\=\\,aMuBcsg\\=",
                                   "payload", G_TYPE_INT, 96,
                                    NULL);
        */
    }
}

static int exchange_caps(gchar *host, gint port, GstCaps **caps)
{
    g_print("Getting caps...");
    GSocket *sock;
    GError *err = NULL;
    if ((sock = g_socket_new(G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_STREAM, 0, &err)) == NULL) {
        g_printerr("Could not create socket\n");
        g_error_free(err);
        return -1;
    }
    GSocketAddress *addr;
    GInetAddress *iaddr;
    iaddr = g_inet_address_new_from_string(host);
    if (!iaddr) {
        g_printerr("Could not resolve host address\n");
        return -1;
    }
    addr = g_inet_socket_address_new(iaddr, port + 1);

    if (!g_socket_connect(sock, addr, NULL, &err)) {
        g_printerr("Could not connect to host\n");
        g_error_free(err);
        return -1;
    }
    gssize bytes_received;
    gchar buf[2048];
    gint off = 0;
    do {
        bytes_received = g_socket_receive(sock, buf+off, 2048, NULL, &err);
        if (bytes_received < 0) {
            g_printerr("Error on receive caps\n");
            g_error_free(err);
            return -1;
        }
        off += bytes_received;
    } while (bytes_received > 0);
    if (off == 0)
        return -1;

    gchar str[off];
    memcpy(str, buf, off);
    //g_print("STRING COPIED: %s\n", str);
    
    char delim[] = "\n";
    char *ptr = strtok(str, delim);
    //g_print("DELIM STRING: %s\n", ptr);
    GstStructure *cp;
    
    cp = gst_structure_from_string(ptr, NULL);
    *caps = gst_caps_new_full(cp, NULL);
    //g_print("CAPS: %s", gst_caps_to_string(*caps));

    g_socket_close(sock, NULL);
    g_print("Done\n");

    return 0;
}

static GstPadProbeReturn cb_get_caps_event(GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
    GstEvent *event;
    event = gst_pad_probe_info_get_event(info);
    //const gchar *event_type;
    //event_type = GST_EVENT_TYPE_NAME(event);
    //g_print("EVENT: %s\n", event_type);
    if (GST_EVENT_TYPE(event) == GST_EVENT_CAPS) {
        struct connection *con_data = (struct connection *) user_data;
        gst_element_set_state(GST_ELEMENT (con_data->pipeline), GST_STATE_PAUSED);
        GstCaps *caps;
        gst_event_parse_caps(event, &caps);
        gchar *cp = gst_caps_to_string(caps);
        gchar *end = "\n";
        gchar send_str[strlen(cp) + strlen(end)];
        sprintf(send_str, "%s%s", cp, end);
        //g_print("Caps to send: %s\n", send_str);

        GError *err;
        GSocketListener *listen = g_socket_listener_new();

        if (!g_socket_listener_add_inet_port(listen, con_data->port + 1, NULL, &err)) {
            g_printerr("Could not create Socket\n");
            g_error_free(err);
            return GST_PAD_PROBE_REMOVE;
        }

        GSocket *sock;
        g_print("Waiting for client to exchange caps...");
        if ((sock = g_socket_listener_accept_socket(listen, NULL, NULL, &err)) == NULL) {
            g_printerr("Error accepting connection\n");
            g_error_free(err);
            return GST_PAD_PROBE_REMOVE;
        }

        gsize len = strlen(send_str);
        gint off = 0;
        while (len > 0) {
            gssize ret = g_socket_send(sock, send_str + off, len, NULL, &err);
            if (ret < 0){
                g_printerr("Could not send on socket\n");
                g_error_free(err);
                return GST_PAD_PROBE_REMOVE;
            }
            off += ret;
            len -= ret;
        }

        g_socket_close(sock, NULL);
        g_socket_listener_close(listen);
        g_print("Done.\n");

        /* sleep 500ms. For debugging */
        /*
        struct timespec ts;
        ts.tv_sec = 1500 / 1000;
        ts.tv_nsec = (1500 % 1000) * 1000000;
        nanosleep(&ts, &ts);
        */
        sleep(1);
        g_print("end sleep\n");

        gst_element_set_state(GST_ELEMENT (con_data->pipeline), GST_STATE_PLAYING);
        return GST_PAD_PROBE_REMOVE;
    } 

    
    return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn cb_inspect_buf_list(GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
    //GstMapInfo map;
    GstBufferList *buffer_list;
    //GstBuffer *buffer;
    guint num_buffers, i;

    buffer_list = GST_PAD_PROBE_INFO_BUFFER_LIST(info);
    num_buffers = gst_buffer_list_length(buffer_list);

    for (i = 0; i < num_buffers; ++i) {
        /*
        buffer = gst_buffer_list_get(buffer_list, i);
        gst_buffer_map(buffer, &map, GST_MAP_READ);
        rtp_hdr_ *hdr = (rtp_hdr_ *) map.data;
        g_print("RTP FRAME SIZE (buffer_list): %lu. Seq NR: %i\n", map.size, hdr->seq_nr);
        gst_buffer_unmap(buffer, &map);
        */
        num_packets++;
    }

    return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn cb_inspect_buf(GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
    /*
    GstMapInfo map;
    GstBuffer *buffer;

    buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    gst_buffer_map(buffer, &map, GST_MAP_READ);
    rtp_hdr_ *hdr = (rtp_hdr_ *) map.data;
    g_print("RTP FRAME SIZE (buffer): %lu. Seq NR: %i\n", map.size, hdr->seq_nr);
    gst_buffer_unmap(buffer, &map);
    */
    /*
    if (last_time != 0) {
        uint64_t t = get_time();
        uint64_t dif = t - last_time;
        avg_time += dif;
        last_time = t;
        ++num_buffers;
        if (dif > highest_jit)
            highest_jit = dif;
    } else {
        last_time = get_time();
    }
    */
    num_packets++;

    return GST_PAD_PROBE_OK;
}

static void on_pad_added(GstElement *ele, GstPad *pad, gpointer data)
{
    gchar *name;

    name = gst_pad_get_name (pad);
    g_print ("A new pad %s was created\n", name);
    g_free (name);

    GstPad *sinkpad;
    GstElement *sink = (GstElement *) data;

    sinkpad = gst_element_get_static_pad(sink, "sink");

    gst_pad_link(pad, sinkpad);
    gst_object_unref(sinkpad);
}

int run_server(gchar *file_path, gchar *host, gint port, gboolean auto_caps, gboolean debug, GMainLoop *loop)
{
    GstElement *filesrc, *demux, *rtph264pay, *udpsink;
    GstElement *pipeline;
    GstBus *bus;
    guint bus_watch_id;
    pipeline = gst_pipeline_new("streamer");
    filesrc = gst_element_factory_make("filesrc", "fs");
    rtph264pay = gst_element_factory_make("rtph264pay", "rtp");
    udpsink = gst_element_factory_make("udpsink", "udp");

    if (host == NULL || file_path == NULL) {
        g_print("Specify video file path, host and port\n");
        return -1;
    }

    char comp_str[strlen(file_path)];
    memcpy(comp_str, file_path, strlen(file_path)); 
    char *type = "mkv";
    //char delim[] = ".";
    //char *ptr = strtok(comp_str, delim);
    //ptr = strtok(NULL, delim);
    char *ptr = &file_path[strlen(file_path)-3];
    if (strncmp(ptr, type, 3) == 0) {
        demux = gst_element_factory_make("matroskademux", "demux");
    } else {
        demux = gst_element_factory_make("qtdemux", "demux");
    }

    if (!pipeline || !filesrc || !demux || !rtph264pay || !udpsink) {
        g_printerr ("One element could not be created. Exiting.\n");
        return -1; 
    }

    g_object_set(G_OBJECT(filesrc), "location", file_path, NULL);
    g_object_set(G_OBJECT(udpsink), "host", host, "port", port, NULL);
    g_object_set(G_OBJECT(rtph264pay), "mtu", 1200, "config-interval", 1, NULL);

    /* message handler */
    bus = gst_pipeline_get_bus (GST_PIPELINE (pipeline));
    bus_watch_id = gst_bus_add_watch (bus, msg_handler, loop);
    gst_object_unref (bus);

    gst_bin_add_many (GST_BIN (pipeline), filesrc, demux, rtph264pay, udpsink, NULL);

    if (!gst_element_link(filesrc, demux))
        g_warning("Failed to link filesrc");
    if (!gst_element_link(rtph264pay, udpsink))
        g_warning("Failed to link rtp");

    g_signal_connect(demux, "pad-added", G_CALLBACK(on_pad_added), rtph264pay);

    if (auto_caps) {
        /* Get rtp source pad for caps event */
        struct connection conn;
        
        conn.host = host;
        conn.port = port;
        conn.pipeline = pipeline;
        
        GstPad *spad;
        spad = gst_element_get_static_pad(rtph264pay, "src");

        gst_pad_add_probe(spad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
                          (GstPadProbeCallback) cb_get_caps_event, &conn, NULL);
    }

    /* get rtp source pad for buffer inspection */
    if (debug) {
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

    /* start the pipeline */
    //gst_element_set_state(GST_ELEMENT (pipeline), GST_STATE_READY);
    g_print ("Start...\n");
    gst_element_set_state(GST_ELEMENT (pipeline), GST_STATE_PLAYING);

    g_main_loop_run(loop);

    /* Out of the main loop, clean up nicely */
    g_print ("Returned, stopping playback\n");

    if (debug)
        g_print("Packets send: %i\n", num_packets);

    guint64 served;
    g_object_get(udpsink, "bytes-served", &served, NULL);
    g_print("Bytes served: %lu\n", served);

    gst_element_set_state (pipeline, GST_STATE_NULL);

    g_print ("Deleting pipeline\n");
    gst_object_unref (GST_OBJECT (pipeline));
    g_source_remove (bus_watch_id);

    return 0;
}

int run_client(gboolean headless, gchar *host, gint port, gboolean bunny, gboolean auto_caps, gboolean debug, GMainLoop *loop)
{
    GstElement *udpsrc, *rtp, *decodebin, *sink, *jitterbuf, *queue;

    GstElement *pipeline;
    GstBus *bus;
    guint bus_watch_id;

    GstCaps *caps = NULL;

    // create elements
    pipeline = gst_pipeline_new("streamer");
    udpsrc = gst_element_factory_make("udpsrc", "udps");
    rtp = gst_element_factory_make("rtph264depay", "rtp");
    decodebin = gst_element_factory_make("decodebin", "dec");
    //decodebin = gst_element_factory_make("avdec_h264", "dec");
    queue = gst_element_factory_make("queue2", "thread_queue");

    if (headless) {
        sink = gst_element_factory_make("fakesink", "sink");
    } else {
        sink = gst_element_factory_make("autovideosink", "sink");
    }
    jitterbuf = gst_element_factory_make("rtpjitterbuffer", "jitterbuf");

    if (!pipeline || !udpsrc || !rtp || !decodebin || !sink || !jitterbuf || !queue) {
        g_printerr ("One element could not be created. Exiting.\n");
        return -1;
    }

    g_object_set(G_OBJECT(udpsrc), "uri", "udp://0.0.0.0:5000",
                 "timeout", 1550000000, "buffer-size", 10000000, NULL);
    //g_object_set(G_OBJECT(jitterbuf), "latency", 400, "mode", 0, NULL);

    /* message handler */
    bus = gst_pipeline_get_bus (GST_PIPELINE (pipeline));
    bus_watch_id = gst_bus_add_watch (bus, msg_handler, loop);
    gst_object_unref (bus);

    gst_bin_add_many (GST_BIN (pipeline), udpsrc, jitterbuf, rtp, decodebin, sink, NULL);
    //gst_bin_add_many (GST_BIN (pipeline), udpsrc, jitterbuf, sink, NULL);

    /* Add queue after jitterbuffer, so delay values are not falsified by buffering */
    //if (!gst_element_link_many(udpsrc, jitterbuf, sink, NULL))
    if (!gst_element_link_many(udpsrc, jitterbuf, rtp, decodebin, NULL))
        g_warning("Failed to link many");

    g_signal_connect(decodebin, "pad-added", G_CALLBACK(on_pad_added), sink);

    stats_element = jitterbuf;

    if (debug) {
        /* get udp source pad */
        GstPad *pad;
        pad = gst_element_get_static_pad(udpsrc, "src");
        gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER_LIST, 
                         (GstPadProbeCallback) cb_inspect_buf_list,
                         NULL, NULL);
        gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER,
                         (GstPadProbeCallback) cb_inspect_buf,
                          NULL, NULL);
        gst_object_unref(pad);
    }

    /*
    GstState state;
    GstStateChangeReturn ret;
    if (auto_caps) {
        ret = gst_element_set_state(GST_ELEMENT(pipeline), GST_STATE_PAUSED);
        g_print("State Change: %i\n", ret);
        g_print("set state playing\n");
        ret = gst_element_get_state(pipeline, &state, NULL, GST_CLOCK_TIME_NONE);
        switch (ret) {
            case GST_STATE_CHANGE_FAILURE:
                g_print("State change failed\n");
                return -1;
                break;
            case GST_STATE_CHANGE_ASYNC:
                g_print("Async state change\n");
                return -1;
                break;
            case GST_STATE_CHANGE_SUCCESS:
            case GST_STATE_CHANGE_NO_PREROLL:
                sleep(1);
                g_print("State: %s, exchanging caps\n", gst_element_state_get_name(state));
                if (exchange_caps(host, port, &caps) != 0) {
                    g_printerr("Caps exchange failed\n");
                    return -1;
                }
                break;
            default:
                break;
        }
    } else {
        get_static_caps(bunny, &caps);
    }
    */

    
    if (auto_caps) {
        if (exchange_caps(host, port, &caps) != 0) {
            g_printerr("Caps exchange failed\n");
            return -1;
        }
        g_print("Auto exchanged caps successfull.\n");
    } else {
        get_static_caps(bunny, &caps);
    }
    if (GST_IS_CAPS(caps)) {
        g_object_set(G_OBJECT(udpsrc), "caps", caps, NULL);
    } else {
        g_printerr("Caps not valid\n");
    }
    
    /* start the pipeline */
    g_print ("Start...\n");
    gst_element_set_state(GST_ELEMENT (pipeline), GST_STATE_PLAYING);

    g_main_loop_run(loop);

    /* Out of the main loop, clean up nicely */
    g_print ("Returned, stopping playback\n");


    GstStructure *stats;
    gchar *str;
    g_object_get(jitterbuf, "stats", &stats, NULL);
    str = gst_structure_to_string(stats);
    g_print("stats: %s\n", str);
    gst_structure_free(stats);
    g_free(str);

    if (debug)
        g_print("Packets pushed: %i\n", num_packets);
        /*
        g_print("\nAvg time between pushed buffers (in micro seconds): %lu. Highest: %lu\n", 
                avg_time / num_buffers, highest_jit);
                */

    gst_element_set_state (pipeline, GST_STATE_NULL);

    g_print ("Deleting pipeline\n");
    gst_object_unref (GST_OBJECT (pipeline));
    g_source_remove (bus_watch_id);

    return 0;
}

int main (int argc, char *argv[])
{
    GMainLoop *loop;

    loop = g_main_loop_new(NULL, FALSE);
    
    /* Parse command line options */
    gboolean server = FALSE;
    gboolean bunny = FALSE;
    gboolean headless = FALSE;
    gboolean debug = FALSE;
    gboolean auto_caps = FALSE;
    gchar *host = NULL;
    gint port = 5000;
    gchar *file_path = NULL;
    GOptionContext *ctx;
    GError *err = NULL;
    GOptionEntry entries[] = {
        {"server", 's', 0, G_OPTION_ARG_NONE, &server,
         "start as server", NULL},
        {"file", 'f', 0, G_OPTION_ARG_STRING, &file_path,
         "video file path", NULL},
        {"host", 'h', 0, G_OPTION_ARG_STRING, &host,
         "host to connect to", NULL},
        {"port", 'p', 0, G_OPTION_ARG_INT, &port,
         "port to connect to", NULL},
        {"headless", 'l', 0, G_OPTION_ARG_NONE, &headless,
         "use fakesink", NULL},
        {"bunny", 'b', 0, G_OPTION_ARG_NONE, &bunny,
         "use big buck bunny caps", NULL},
        {"debug", 'd', 0, G_OPTION_ARG_NONE, &debug,
         "print debug info", NULL},
        {"auto", 'a', 0, G_OPTION_ARG_NONE, &auto_caps,
         "automatic caps exchange", NULL},
        {NULL}
    };

    ctx = g_option_context_new("-h IP -p PORT -f VIDEO_FILE");
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

    //init
    gst_init(NULL, NULL);
    int ret;
    if (server)
        ret = run_server(file_path, host, port, auto_caps, debug, loop);
    else
        ret = run_client(headless, host, port, bunny, auto_caps, debug, loop);

    if (ret != 0) {
        g_printerr("Failed init. Exit...\n");
        return 1;
    }

    g_main_loop_unref (loop);

    return 0;
}