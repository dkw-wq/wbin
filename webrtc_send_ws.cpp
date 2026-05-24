#include <gst/gst.h>
#include <gst/sdp/sdp.h>
#include <gst/webrtc/webrtc.h>
#include <gst/app/app.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <opencv2/opencv.hpp>
#include "face_detect.h"

GMainLoop *loop;
GstElement *pipeline, *webrtc;
GstElement *detect_sink = NULL;
SoupWebsocketConnection *ws_conn = NULL;
FaceDetector *detector = NULL;

// Data channel for sending data to browser
static GObject *data_channel = NULL;
static std::mutex dc_mutex;

static void on_data_channel_message_string(GstElement *channel, gchar *str, gpointer user_data);

// ---- JSON helpers (unchanged from C version) ----

static void send_sdp_to_server(GstWebRTCSessionDescription *desc) {
    GstSDPMessage *sdp = desc->sdp;
    gchar *text = gst_sdp_message_as_text(sdp);
    if (!text) { g_print("Failed to convert SDP to text\n"); return; }

    JsonBuilder *builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "sdp");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "offer");
    json_builder_set_member_name(builder, "sdp");
    json_builder_add_string_value(builder, text);
    json_builder_end_object(builder);
    json_builder_end_object(builder);

    JsonGenerator *gen = json_generator_new();
    JsonNode *root = json_builder_get_root(builder);
    json_generator_set_root(gen, root);
    gchar *json_str = json_generator_to_data(gen, NULL);

    g_print("Sending Offer to WS...\n");
    if (ws_conn) soup_websocket_connection_send_text(ws_conn, json_str);

    g_free(text); g_free(json_str);
    g_object_unref(builder); g_object_unref(gen); json_node_free(root);
}

static void on_ice_candidate(GstElement *element, guint mlineindex, gchar *candidate, gpointer data) {
    JsonBuilder *builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "ice");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "candidate");
    json_builder_add_string_value(builder, candidate);
    json_builder_set_member_name(builder, "sdpMLineIndex");
    json_builder_add_int_value(builder, mlineindex);
    json_builder_end_object(builder);
    json_builder_end_object(builder);

    JsonGenerator *gen = json_generator_new();
    JsonNode *root = json_builder_get_root(builder);
    json_generator_set_root(gen, root);
    gchar *json_str = json_generator_to_data(gen, NULL);

    if (ws_conn) soup_websocket_connection_send_text(ws_conn, json_str);
    g_free(json_str); g_object_unref(builder); g_object_unref(gen); json_node_free(root);
}

static void on_offer_created(GstPromise *promise, gpointer data) {
    GstWebRTCSessionDescription *offer = NULL;
    const GstStructure *reply = gst_promise_get_reply(promise);
    gst_structure_get(reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, NULL);
    gst_promise_unref(promise);

    g_signal_emit_by_name(webrtc, "set-local-description", offer, NULL);
    send_sdp_to_server(offer);
    gst_webrtc_session_description_free(offer);
}

static void on_negotiation_needed(GstElement *element, gpointer data) {
    g_print("Negotiation needed. Creating offer...\n");
    GstPromise *promise = gst_promise_new_with_change_func(on_offer_created, NULL, NULL);
    g_signal_emit_by_name(webrtc, "create-offer", NULL, promise);
}

static void on_ws_message(SoupWebsocketConnection *conn, gint type, GBytes *message, gpointer data) {
    gsize len;
    const gchar *msg_data = (const gchar*)g_bytes_get_data(message, &len);

    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, msg_data, len, NULL)) {
        g_printerr("Failed to parse JSON\n"); return;
    }

    JsonNode *root = json_parser_get_root(parser);
    JsonObject *obj = json_node_get_object(root);

    if (json_object_has_member(obj, "sdp")) {
        JsonObject *sdp_json = json_object_get_object_member(obj, "sdp");
        const gchar *sdp_type = json_object_get_string_member(sdp_json, "type");
        const gchar *sdp_str = json_object_get_string_member(sdp_json, "sdp");

        if (g_strcmp0(sdp_type, "answer") == 0) {
            g_print("Received Answer\n");
            GstSDPMessage *sdp;
            gst_sdp_message_new(&sdp);
            gst_sdp_message_parse_buffer((guint8*)sdp_str, strlen(sdp_str), sdp);
            GstWebRTCSessionDescription *answer = gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_ANSWER, sdp);
            g_signal_emit_by_name(webrtc, "set-remote-description", answer, NULL);
            gst_webrtc_session_description_free(answer);
        }
    } else if (json_object_has_member(obj, "ice")) {
        JsonObject *ice_json = json_object_get_object_member(obj, "ice");
        const gchar *candidate = json_object_get_string_member(ice_json, "candidate");
        gint mline_index = json_object_get_int_member(ice_json, "sdpMLineIndex");
        g_print("Received ICE candidate\n");
        g_signal_emit_by_name(webrtc, "add-ice-candidate", mline_index, candidate);
    } else if (json_object_has_member(obj, "type")) {
        const gchar *type = json_object_get_string_member(obj, "type");
        if (g_strcmp0(type, "request_offer") == 0) {
            g_print("Browser ready. Starting WebRTC negotiation...\n");
            on_negotiation_needed(webrtc, NULL);
        }
    }

    g_object_unref(parser);
}

// ---- CPU Temperature (unchanged) ----

static double read_cpu_temp() {
    FILE *f = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
    if (!f) return -1.0;
    int millidegrees = 0;
    fscanf(f, "%d", &millidegrees);
    fclose(f);
    return millidegrees / 1000.0;
}

static gboolean on_timer_send_temp(gpointer user_data) {
    if (!data_channel) return G_SOURCE_CONTINUE;
    double temp = read_cpu_temp();
    if (temp < 0) return G_SOURCE_CONTINUE;

    gchar *json_msg = g_strdup_printf("{\"type\": \"temperature\", \"value\": %.1f}", temp);
    std::lock_guard<std::mutex> lock(dc_mutex);
    g_signal_emit_by_name(data_channel, "send-string", json_msg);
    g_free(json_msg);
    return G_SOURCE_CONTINUE;
}

// ---- Face Detection Timer ----

static gboolean on_timer_face_detect(gpointer user_data) {
    if (!data_channel || !detect_sink || !detector) return G_SOURCE_CONTINUE;

    // Pull latest frame from appsink (non-blocking, drop=true so always latest)
    GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(detect_sink), 0);
    if (!sample) return G_SOURCE_CONTINUE;

    GstBuffer *buffer = gst_sample_get_buffer(sample);
    GstCaps *caps = gst_sample_get_caps(sample);

    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        gst_sample_unref(sample);
        return G_SOURCE_CONTINUE;
    }

    // Get frame dimensions from caps
    const GstStructure *s = gst_caps_get_structure(caps, 0);
    int width = 640, height = 480;
    gst_structure_get_int(s, "width", &width);
    gst_structure_get_int(s, "height", &height);

    // Wrap in cv::Mat, then clone for processing
    cv::Mat frame(height, width, CV_8UC3, map.data);
    cv::Mat frameCopy = frame.clone();
    gst_buffer_unmap(buffer, &map);
    gst_sample_unref(sample);

    // Run face detection
    auto faces = detector->detect(frameCopy);

    // Build JSON with face count and bounding boxes
    JsonBuilder *builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "faces");
    json_builder_set_member_name(builder, "count");
    json_builder_add_int_value(builder, (gint64)faces.size());

    json_builder_set_member_name(builder, "boxes");
    json_builder_begin_array(builder);
    for (auto &f : faces) {
        json_builder_begin_array(builder);
        json_builder_add_int_value(builder, (gint64)f.x1);
        json_builder_add_int_value(builder, (gint64)f.y1);
        json_builder_add_int_value(builder, (gint64)f.x2);
        json_builder_add_int_value(builder, (gint64)f.y2);
        json_builder_end_array(builder);
    }
    json_builder_end_array(builder);
    json_builder_end_object(builder);

    JsonGenerator *gen = json_generator_new();
    JsonNode *root = json_builder_get_root(builder);
    json_generator_set_root(gen, root);
    gchar *json_str = json_generator_to_data(gen, NULL);

    {
        std::lock_guard<std::mutex> lock(dc_mutex);
        g_signal_emit_by_name(data_channel, "send-string", json_str);
    }

    g_free(json_str);
    g_object_unref(builder);
    g_object_unref(gen);
    json_node_free(root);

    return G_SOURCE_CONTINUE;
}

// ---- Data Channel ----

static void on_data_channel_opened(GObject *channel, gpointer user_data) {
    g_print(">>> Data Channel OPEN! Starting temperature + face detection...\n");
    g_object_ref(channel);
    // Send temperature every 2 seconds
    g_timeout_add_seconds(2, on_timer_send_temp, channel);
    // Run face detection every 500ms (2x per second)
    g_timeout_add(500, on_timer_face_detect, channel);
    g_object_unref(channel);
}

static void on_ws_connected(SoupSession *session, GAsyncResult *res, gpointer data) {
    GError *error = NULL;
    ws_conn = soup_session_websocket_connect_finish(session, res, &error);
    if (error) { g_printerr("WS Connect Error: %s\n", error->message); return; }

    g_print("WebSocket Connected! Starting Pipeline...\n");
    g_signal_connect(ws_conn, "message", G_CALLBACK(on_ws_message), NULL);

    gst_element_set_state(pipeline, GST_STATE_READY);

    GObject *send_channel = NULL;
    g_signal_emit_by_name(webrtc, "create-data-channel", "control", NULL, &send_channel);

    if (send_channel) {
        g_print("SUCCESS: Data Channel 'control' created by C!\n");
        data_channel = send_channel;
        g_signal_connect(send_channel, "on-message-string", G_CALLBACK(on_data_channel_message_string), NULL);
        g_signal_connect(send_channel, "on-open", G_CALLBACK(on_data_channel_opened), NULL);
        g_object_unref(send_channel);
    } else {
        g_print("ERROR: Failed to create Data Channel!\n");
    }

    gst_element_set_state(pipeline, GST_STATE_PLAYING);
}

static void on_data_channel_message_string(GstElement *channel, gchar *str, gpointer user_data) {
    g_print("【get command】: %s\n", str);
}

static void on_data_channel_cb(GstElement *webrtc, GObject *channel, gpointer user_data) {
    g_print("Data Channel established!\n");
    g_signal_connect(channel, "on-message-string", G_CALLBACK(on_data_channel_message_string), NULL);
}

// ---- Main ----

int main(int argc, char *argv[]) {
    gst_init(&argc, &argv);
    loop = g_main_loop_new(NULL, FALSE);

    // Init face detector
    detector = new FaceDetector("/home/dkw/dir1/wbin/models/det_500m.onnx");
    g_print("Face detector loaded.\n");

    /* 1. Set up WebSocket */
    SoupSession *soup_session = soup_session_new();
    soup_session_websocket_connect_async(soup_session,
        soup_message_new(SOUP_METHOD_GET, "ws://localhost:8443"),
        NULL, NULL, G_PRIORITY_DEFAULT, NULL,
        (GAsyncReadyCallback)on_ws_connected, NULL);

    /* 2. Create GStreamer pipeline with tee for face detection */
    pipeline = gst_parse_launch(
        "webrtcbin name=send_webrtc stun-server=stun://stun.l.google.com:19302 "

        "v4l2src device=/dev/video8 ! "
        "video/x-raw,width=640,height=480,framerate=30/1 ! "
        "videoconvert ! tee name=t "

        // Branch 1: face detection (BGR format for OpenCV)
        "t. ! queue ! videoconvert ! video/x-raw,format=BGR ! "
        "appsink name=detect_sink max-buffers=1 drop=true "

        // Branch 2: video encoding + WebRTC
        "t. ! queue ! videoconvert ! "
        "openh264enc usage-type=camera bitrate=800000 complexity=low "
        "multi-thread=4 gop-size=30 ! "
        "video/x-h264,profile=constrained-baseline ! "
        "rtph264pay config-interval=1 ! "
        "application/x-rtp,media=video,encoding-name=H264,payload=96 ! "
        "send_webrtc.",
        NULL);

    webrtc = gst_bin_get_by_name(GST_BIN(pipeline), "send_webrtc");
    detect_sink = gst_bin_get_by_name(GST_BIN(pipeline), "detect_sink");

    g_signal_connect(webrtc, "on-data-channel", G_CALLBACK(on_data_channel_cb), NULL);
    g_signal_connect(webrtc, "on-negotiation-needed", G_CALLBACK(on_negotiation_needed), NULL);
    g_signal_connect(webrtc, "on-ice-candidate", G_CALLBACK(on_ice_candidate), NULL);

    g_print("Waiting for websocket connection...\n");
    g_main_loop_run(loop);

    delete detector;
    return 0;
}
