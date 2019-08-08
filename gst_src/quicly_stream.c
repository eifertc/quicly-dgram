#include <gst/gst.h>
#include <glib.h>
#include <stdint.h>

int rtp_packet_num = 0;
gssize rtp_bytes = 0;

static gboolean msg_handler(GstBus *bus, GstMessage *msg, gpointer data)
{
    GMainLoop *loop = (GMainLoop *) data;

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
        case GST_MESSAGE_QOS: {
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

static GstPadProbeReturn cb_inspect_buf_list(GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
    GstMapInfo map;
    GstBufferList *buffer_list;
    GstBuffer *buffer;
    guint num_buffers, i;

    buffer_list = GST_PAD_PROBE_INFO_BUFFER_LIST(info);
    num_buffers = gst_buffer_list_length(buffer_list);

    for (i = 0; i < num_buffers; ++i) {
        buffer = gst_buffer_list_get(buffer_list, i);
        gst_buffer_map(buffer, &map, GST_MAP_READ);
        //rtp_hdr_ *hdr = (rtp_hdr_ *) map.data;
        //g_print("RTP FRAME SIZE: %lu. Seq NR: %i\n", map.size, hdr->seq_nr);
        rtp_packet_num++;
        rtp_bytes += map.size;
        gst_buffer_unmap(buffer, &map);
    }

    return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn cb_inspect_buf(GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
    GstMapInfo map;
    GstBuffer *buffer;;

    buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    gst_buffer_map(buffer, &map, GST_MAP_READ);
    //rtp_hdr_ *hdr = (rtp_hdr_ *) map.data;
    //g_print("RTP FRAME SIZE: %lu. Seq NR: %i\n", map.size, hdr->seq_nr);
    rtp_packet_num++;
    rtp_bytes += map.size;
    gst_buffer_unmap(buffer, &map);

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

int run_server(gchar *file_path, gchar *cert_file, gchar *key_file, gboolean stream_mode, gboolean debug, GMainLoop *loop)
{
    GstElement *filesrc, *qtdemux, *rtph264pay, *quiclysink;
    GstElement *pipeline;
    GstBus *bus;
    guint bus_watch_id;

        // create elements
    pipeline = gst_pipeline_new("streamer");
    filesrc = gst_element_factory_make("filesrc", "fs");
    qtdemux = gst_element_factory_make("qtdemux", "demux");
    rtph264pay = gst_element_factory_make("rtph264pay", "rtp");
    quiclysink = gst_element_factory_make("quiclysink", "quicly");

    if (!pipeline || !filesrc || !qtdemux || !rtph264pay || !quiclysink) {
        g_printerr ("One element could not be created. Exiting.\n");
        return -1;
    }

    if (cert_file == NULL || key_file == NULL) {
        g_printerr("Missing key/cert files\n");
        return -1;
    }

    if (file_path == NULL) {
        g_printerr("Missing source video file\n");
        return -1;
    }

    g_object_set(G_OBJECT(filesrc), "location", file_path, NULL);
    g_object_set(G_OBJECT(quiclysink), "bind-port", 5000, 
                          "cert", cert_file,
                          "key", key_file, NULL);
    if (stream_mode)
        g_object_set(G_OBJECT(quiclysink), "stream-mode", TRUE, NULL);

    g_object_set(G_OBJECT(rtph264pay), "mtu", 1200, NULL);

    /* message handler */
    bus = gst_pipeline_get_bus (GST_PIPELINE (pipeline));
    bus_watch_id = gst_bus_add_watch (bus, msg_handler, loop);
    gst_object_unref (bus);

    //add elements to pipeline
    gst_bin_add_many (GST_BIN (pipeline), filesrc, qtdemux, rtph264pay, quiclysink, NULL);

    // link
    if (!gst_element_link(filesrc, qtdemux))
        g_warning("Failed to link filesrc");
    if (!gst_element_link(rtph264pay, quiclysink))
        g_warning("Failed to link rtp to quiclysink");

    g_signal_connect(qtdemux, "pad-added", G_CALLBACK(on_pad_added), rtph264pay);

    if (debug) {
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
    

    /* start the pipeline */
    g_print ("Start...\n");
    gst_element_set_state(GST_ELEMENT (pipeline), GST_STATE_PLAYING);

    g_main_loop_run(loop);

    /* Out of the main loop, clean up nicely */
    g_print ("Returned, stopping playback\n");

    //stats
    GstStructure *stats;
    gchar *str;
    g_object_get(rtph264pay, "stats", &stats, NULL);
    str = gst_structure_to_string(stats);
    g_print("stats: %s\n", str);
    gst_structure_free(stats);
    g_free(str);

    if (debug)
        g_print("RTP packets created: %i. Bytes: %lu\n", rtp_packet_num, rtp_bytes);

    gst_element_set_state (pipeline, GST_STATE_NULL);

    g_print ("Deleting pipeline\n");
    gst_object_unref (GST_OBJECT (pipeline));
    g_source_remove (bus_watch_id);

    return 0;
}

int run_client(gchar *host, gint *port, gboolean headless, gboolean debug, GMainLoop *loop)
{
    GstElement *quiclysrc, *rtp, *decodebin, *sink, *jitterbuf;
    GstElement *pipeline;

    GstBus *bus;
    guint bus_watch_id;

    pipeline = gst_pipeline_new("streamer");
    quiclysrc = gst_element_factory_make("quiclysrc", "quicsrc");
    rtp = gst_element_factory_make("rtph264depay", "rtp");
    decodebin = gst_element_factory_make("decodebin", "dec");

    if (headless) {
        sink = gst_element_factory_make("fakesink", "sink");
    } else {
        sink = gst_element_factory_make("autovideosink", "sink");
    }

    jitterbuf = gst_element_factory_make("rtpjitterbuffer", "jitterbuf");

    if (!pipeline || !quiclysrc || !rtp || !decodebin || !sink || !jitterbuf) {
        g_printerr ("One element could not be created. Exiting.\n");
        return -1;
    }

    if (host == NULL || port == NULL) {
        g_print("Specify host and port\n");
        return -1;
    }

    g_object_set(G_OBJECT(quiclysrc), "host", host, "port", port, NULL);

    /* message handler */
    bus = gst_pipeline_get_bus (GST_PIPELINE (pipeline));
    bus_watch_id = gst_bus_add_watch (bus, msg_handler, loop);
    gst_object_unref (bus);

    gst_bin_add_many (GST_BIN (pipeline), quiclysrc, jitterbuf, rtp, decodebin, sink, NULL);

    // link
    if (!gst_element_link_many(quiclysrc, jitterbuf, rtp, decodebin, NULL))
        g_warning("Failed to link many");

    g_signal_connect(decodebin, "pad-added", G_CALLBACK(on_pad_added), sink);

    if (debug) {
        /* get rtp source pad */
        GstPad *pad;
        pad = gst_element_get_static_pad(quiclysrc, "src");
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
    
    //stats
    GstStructure *stats;
    gchar *str;
    g_object_get(jitterbuf, "stats", &stats, NULL);
    str = gst_structure_to_string(stats);
    g_print("stats: %s\n", str);
    gst_structure_free(stats);
    g_free(str);

    if (debug)
        g_print("Quiclysrc src pad. Packets pushed: %i. Bytes: %lu\n", rtp_packet_num, rtp_bytes);

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
    gchar *host = NULL;
    gint *port = NULL;
    gboolean headless = FALSE;
    gboolean debug = FALSE;
    gboolean stream_mode = FALSE;
    gchar *file_path = NULL;
    gchar *cert_file = NULL;
    gchar *key_file = NULL;
    gchar *plugins = NULL;
    GOptionContext *ctx;
    GError *err = NULL;
    GOptionEntry entries[] = {
        {"server", 's', 0, G_OPTION_ARG_NONE, &server,
         "Start as server", NULL},
        {"file", 'f', 0, G_OPTION_ARG_STRING, &file_path,
         "Server. Video file path", NULL},
        {"cert", 'c', 0, G_OPTION_ARG_STRING, &cert_file,
         "Server. Certificate file path", NULL},
        {"key", 'k', 0, G_OPTION_ARG_STRING, &key_file,
         "Server. Key file path", NULL},
        {"plugin-path", 'r', 0, G_OPTION_ARG_STRING, &plugins,
         "custom plugin folder", NULL},
        {"stream_mode", 'm', 0, G_OPTION_ARG_NONE, &stream_mode,
         "Server. Use streams instead of datagrams", NULL},
        {"debug", 'd', 0, G_OPTION_ARG_NONE, &debug,
         "Print debug info", NULL},
        {"host", 'h', 0, G_OPTION_ARG_STRING, &host,
         "Client. Host to connect to", NULL},
        {"port", 'p', 0, G_OPTION_ARG_INT, &port,
         "Client. Port to connect to", NULL},
        {"headless", 'l', 0, G_OPTION_ARG_NONE, &headless,
         "Client. Use fakesink", NULL},
        {NULL}
    };

    ctx = g_option_context_new("Quicly stream server");
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

    /* SET PLUGIN PATH */
    if (plugins == NULL)
        plugins = "./libgst";
    GstRegistry *reg;
    reg = gst_registry_get();
    gst_registry_scan_path(reg, plugins);

    int ret;
    if (server)
        ret = run_server(file_path, cert_file, key_file, stream_mode, debug, loop);
    else 
        ret = run_client(host, port, headless, debug, loop);

    if (ret != 0) {
        g_printerr("Init failed. Exit...\n");
        return 1;
    }

    g_main_loop_unref (loop);

    return 0;
}