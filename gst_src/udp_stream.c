#include <gst/gst.h>
#include <glib.h>
#include <sys/types.h>
#include <stdint.h>
#include <sys/time.h>

GstElement *stats_element;

static gboolean msg_handler(GstBus *bus, GstMessage *msg, gpointer data)
{
    GMainLoop *loop = (GMainLoop *) data;
    const GstStructure *st = gst_message_get_structure (msg);

    switch (GST_MESSAGE_TYPE(msg)) {
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

int run_server(gchar *file_path, gchar *host, gint *port, gboolean debug, GMainLoop *loop)
{
    GstElement *filesrc, *qtdemux, *rtph264pay, *udpsink;
    GstElement *pipeline;
    GstBus *bus;
    guint bus_watch_id;

    /* inspect rtp data */
    //GstPad *pad;

    pipeline = gst_pipeline_new("streamer");
    filesrc = gst_element_factory_make("filesrc", "fs");
    qtdemux = gst_element_factory_make("qtdemux", "demux");
    rtph264pay = gst_element_factory_make("rtph264pay", "rtp");
    udpsink = gst_element_factory_make("udpsink", "udp");

    if (!pipeline || !filesrc || !qtdemux || !rtph264pay || !udpsink) {
        g_printerr ("One element could not be created. Exiting.\n");
        return -1;
    }

    if (host == NULL || port == NULL || file_path == NULL) {
        g_print("Specify video file path, host and port\n");
        return -1;
    }

    g_object_set(G_OBJECT(filesrc), "location", file_path, NULL);
    g_object_set(G_OBJECT(udpsink), "host", host, "port", port, NULL);
    g_object_set(G_OBJECT(rtph264pay), "mtu", 1200, NULL);

    /* message handler */
    bus = gst_pipeline_get_bus (GST_PIPELINE (pipeline));
    bus_watch_id = gst_bus_add_watch (bus, msg_handler, loop);
    gst_object_unref (bus);

    gst_bin_add_many (GST_BIN (pipeline), filesrc, qtdemux, rtph264pay, udpsink, NULL);

    if (!gst_element_link(filesrc, qtdemux))
        g_warning("Failed to link filesrc");
    if (!gst_element_link(rtph264pay, udpsink))
        g_warning("Failed to link rtp");

    g_signal_connect(qtdemux, "pad-added", G_CALLBACK(on_pad_added), rtph264pay);

    /* get rtp source pad */
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

int run_client(gboolean headless, gboolean bunny, gboolean debug, GMainLoop *loop)
{
    GstElement *udpsrc, *rtp, *decodebin, *sink, *jitterbuf;

    GstElement *pipeline;
    GstCaps *caps;
    GstBus *bus;
    guint bus_watch_id;

    if (bunny) {
        caps = gst_caps_new_simple("application/x-rtp",
                                   "media", G_TYPE_STRING, "video",
                                   "clock-rate", G_TYPE_INT, 90000,
                                   "encoding-name", G_TYPE_STRING, "H264",
                                   "packetization-mode", G_TYPE_STRING, "1",
                                   "profile-level-id", G_TYPE_STRING, "640033",
                                   "sprop-parameter-sets", G_TYPE_STRING, "Z2QAM6xyhEB4AiflwEQAAAMABAAAAwDwPGDGEYA\\=\\,aOhDiSyL",
                                   "payload", G_TYPE_INT, 96,
                                    NULL);
    } else {
        caps = gst_caps_new_simple("application/x-rtp",
                                   "media", G_TYPE_STRING, "video",
                                   "clock-rate", G_TYPE_INT, 90000,
                                   "encoding-name", G_TYPE_STRING, "H264",
                                   "packetization-mode", G_TYPE_STRING, "1",
                                   "profile-level-id", G_TYPE_STRING, "42c01f",
                                   "sprop-parameter-sets", G_TYPE_STRING, "Z0LAH9kAUAW7/wB4AFsQAAADABAAAAMDAPGDJIA\\=\\,aMuBcsg\\=",
                                   "payload", G_TYPE_INT, 96,
                                    NULL);
    }
    

    if (!GST_IS_CAPS(caps))
        g_printerr("caps not valid\n");

    // create elements
    pipeline = gst_pipeline_new("streamer");
    udpsrc = gst_element_factory_make("udpsrc", "fs");
    rtp = gst_element_factory_make("rtph264depay", "rtp");
    decodebin = gst_element_factory_make("decodebin", "dec");
    //decodebin = gst_element_factory_make("avdec_h264", "dec");

    if (headless) {
        sink = gst_element_factory_make("fakesink", "sink");
    } else {
        sink = gst_element_factory_make("autovideosink", "sink");
    }
    jitterbuf = gst_element_factory_make("rtpjitterbuffer", "jitterbuf");

    if (!pipeline || !udpsrc || !rtp || !decodebin || !sink || !jitterbuf) {
        g_printerr ("One element could not be created. Exiting.\n");
        return -1;
    }

    g_object_set(G_OBJECT(udpsrc), "uri", "udp://0.0.0.0:5000",
                 "timeout", 1550000000, "caps", caps, NULL);

    g_object_set(G_OBJECT(jitterbuf), "latency", 600, NULL);

    /* message handler */
    bus = gst_pipeline_get_bus (GST_PIPELINE (pipeline));
    bus_watch_id = gst_bus_add_watch (bus, msg_handler, loop);
    gst_object_unref (bus);

    gst_bin_add_many (GST_BIN (pipeline), udpsrc, jitterbuf, rtp, decodebin, sink, NULL);

    if (!gst_element_link_many(udpsrc, jitterbuf, rtp, decodebin, NULL))
        g_warning("Failed to link many");

    g_signal_connect(decodebin, "pad-added", G_CALLBACK(on_pad_added), sink);

    stats_element = jitterbuf;

    if (debug) {
        /* get rtp source pad */
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
    gchar *host = NULL;
    gint *port = NULL;
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
        {NULL}
    };

    ctx = g_option_context_new("UDP streaming");
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
        ret = run_server(file_path, host, port, debug, loop);
    else
        ret = run_client(headless, bunny, debug, loop);

    if (ret != 0) {
        g_printerr("Failed init. Exit...\n");
        return 1;
    }

    g_main_loop_unref (loop);

    return 0;
}