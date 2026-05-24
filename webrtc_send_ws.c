#include <gst/gst.h>
#include <gst/sdp/sdp.h>
#include <gst/webrtc/webrtc.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

GMainLoop *loop;
GstElement *pipeline, *webrtc;
SoupWebsocketConnection *ws_conn = NULL;

static void on_data_channel_message_string(GstElement *channel, gchar *str, gpointer user_data);

/* 将 SDP 转换为 JSON 字符串并发送 */
static void send_sdp_to_server(GstWebRTCSessionDescription *desc) {
    GstSDPMessage *sdp = desc->sdp;
    gchar *text = gst_sdp_message_as_text(sdp);

    if (!text) {
        g_print("Failed to convert SDP to text\n");
        return;
    }

    /* 构建 JSON: { "sdp": { "type": "offer", "sdp": "..." } } */
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
    if (ws_conn) {
        soup_websocket_connection_send_text(ws_conn, json_str);
    }

    g_free(text);
    g_free(json_str);
    g_object_unref(builder);
    g_object_unref(gen);
    json_node_free(root);
}

/* 将 ICE Candidate 转换为 JSON 并发送 */
static void on_ice_candidate(GstElement *element, guint mlineindex, gchar *candidate, gpointer data) {
    /* 构建 JSON: { "ice": { "candidate": "...", "sdpMLineIndex": 0 } } */
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

    if (ws_conn) {
        soup_websocket_connection_send_text(ws_conn, json_str);
    }
    g_free(json_str);
    g_object_unref(builder);
    g_object_unref(gen);
    json_node_free(root);
}

static void on_offer_created(GstPromise *promise, gpointer data) {
    GstWebRTCSessionDescription *offer = NULL;
    const GstStructure *reply = gst_promise_get_reply(promise);
    gst_structure_get(reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, NULL);
    gst_promise_unref(promise);

    /* 设置本地 SDP */
    g_signal_emit_by_name(webrtc, "set-local-description", offer, NULL);

    /* 发送给浏览器 */
    send_sdp_to_server(offer);
    gst_webrtc_session_description_free(offer);
}

static void on_negotiation_needed(GstElement *element, gpointer data) {
    g_print("Negotiation needed. Creating offer...\n");
    GstPromise *promise = gst_promise_new_with_change_func(on_offer_created, NULL, NULL);
    g_signal_emit_by_name(webrtc, "create-offer", NULL, promise);
}

/* 处理来自 WebSocket 的消息 (Answer 或 ICE) */
static void on_ws_message(SoupWebsocketConnection *conn, gint type, GBytes *message, gpointer data) {
    gsize len;
    const gchar *msg_data = g_bytes_get_data(message, &len);
    
    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, msg_data, len, NULL)) {
        g_printerr("Failed to parse JSON\n");
        return;
    }

    JsonNode *root = json_parser_get_root(parser);
    JsonObject *obj = json_node_get_object(root);

    if (json_object_has_member(obj, "sdp")) {
        /* 处理 Answer */
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
        /* 处理 ICE Candidate */
        JsonObject *ice_json = json_object_get_object_member(obj, "ice");
        const gchar *candidate = json_object_get_string_member(ice_json, "candidate");
        gint mline_index = json_object_get_int_member(ice_json, "sdpMLineIndex");
        
        g_print("Received ICE candidate\n");
        g_signal_emit_by_name(webrtc, "add-ice-candidate", mline_index, candidate);
    }

    g_object_unref(parser);
}


// 辅助函数：读取树莓派 CPU 温度
static double read_cpu_temp() {
    // 树莓派的 CPU 温度保存在这个文件里
    FILE *f = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
    if (!f) return -1.0;
    
    int millidegrees = 0;
    fscanf(f, "%d", &millidegrees);
    fclose(f);
    
    // 文件里读出来的是毫摄氏度 (比如 45230)，转换为摄氏度 (45.23)
    return millidegrees / 1000.0;
}

// 定时器回调函数：每隔 X 秒被调用一次
static gboolean on_timer_send_temp(gpointer user_data) {
    GObject *send_channel = (GObject *)user_data;
    if (!send_channel) return G_SOURCE_REMOVE; // 如果通道没了，停止定时器

    double temp = read_cpu_temp();
    if (temp < 0) return G_SOURCE_CONTINUE;

    // 1. 将数据打包成 JSON 字符串格式 (方便前端解析)
    gchar *json_msg = g_strdup_printf("{\"type\": \"temperature\", \"value\": %.1f}", temp);

    // 2. 核心：通过 Data Channel 发送文本数据！
    g_signal_emit_by_name(send_channel, "send-string", json_msg);

    // g_print("已发送温度: %s\n", json_msg); // 可以取消注释查看发送日志
    
    g_free(json_msg); // 释放字符串内存
    
    // 返回 G_SOURCE_CONTINUE (即 TRUE) 表示定时器继续运行
    return G_SOURCE_CONTINUE; 
}

// 新增：当数据通道真正建立连接时触发
static void on_data_channel_opened(GObject *channel, gpointer user_data) {
    g_print(">>> Data Channel really OPEN !now we start to send cpu temperature information...\n");
    
    // 把之前放在 on_ws_connected 里的定时器移到这里来！
    g_object_ref(channel);
    g_timeout_add_seconds(2, on_timer_send_temp, channel);
    g_object_unref(channel);
}

static void on_ws_connected(SoupSession *session, GAsyncResult *res, gpointer data) {
    GError *error = NULL;
    ws_conn = soup_session_websocket_connect_finish(session, res, &error);
    
    if (error) {
        g_printerr("WS Connect Error: %s\n", error->message);
        return;
    }
    
    g_print("WebSocket Connected! Starting Pipeline...\n");
    g_signal_connect(ws_conn, "message", G_CALLBACK(on_ws_message), NULL);

    // =========================================================================
    // 【关键修复】: 必须先让管道进入 READY 状态，WebRTC 内部才会初始化！
    //  否则就会报 'is_closed' 错误。
    // =========================================================================
    gst_element_set_state(pipeline, GST_STATE_READY);

    /* 使用全局变量 webrtc (在 main 中已经获取了)，不需要再 get_by_name 了 */
    /* 如果你 main 里没把 webrtc 设为全局，可以用 gst_bin_get_by_name */
    GObject *send_channel = NULL;

    // 参数含义: 信号名, 通道名(control), 配置项(NULL), 返回的通道对象指针
    g_signal_emit_by_name(webrtc, "create-data-channel", "control", NULL, &send_channel);
    
    if (send_channel) {
        g_print("SUCCESS: Data Channel 'control' created by C!\n");
        g_signal_connect(send_channel, "on-message-string", G_CALLBACK(on_data_channel_message_string), NULL);
        
        // 【修改这里】：绑定 on-open 信号，等通道通了再开定时器
        g_signal_connect(send_channel, "on-open", G_CALLBACK(on_data_channel_opened), NULL);
        
        g_object_unref(send_channel);
    } else {
        g_print("ERROR: Failed to create Data Channel!\n");
    }
    
    /* 最后再全速运行 */
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
}

// 处理收到的字符串消息
static void on_data_channel_message_string(GstElement *channel, gchar *str, gpointer user_data) {
    g_print("【get command】: %s\n", str);

    // TODO: 这里即使是你展示嵌入式能力的地方！
    // 比如：if (strcmp(str, "light_on") == 0) {system("echo 1 > /sys/class/gpio...");}
}

// 当浏览器创建 Data Channel 时，这个函数会被触发
static void on_data_channel(GstElement *webrtc, GObject *channel, gpointer user_data) {
    g_print("Data Channel established!\n");
    
    // 订阅 "on-message-string" 信号，专门处理文本消息
    g_signal_connect(channel, "on-message-string", G_CALLBACK(on_data_channel_message_string), NULL);
}

int main(int argc, char *argv[]) {
    gst_init(&argc, &argv);
    loop = g_main_loop_new(NULL, FALSE);

    /* 1. 设置 WebSocket 连接 (使用 Libsoup 3.0) */
    SoupSession *soup_session = soup_session_new();
    
    /* 注意：Soup 3.0 这里多了一个 G_PRIORITY_DEFAULT 参数 */
    soup_session_websocket_connect_async(soup_session, 
        soup_message_new(SOUP_METHOD_GET, "ws://localhost:8443"), 
        NULL, 
        NULL, 
        G_PRIORITY_DEFAULT, 
        NULL, 
        (GAsyncReadyCallback)on_ws_connected, 
        NULL);

    /* 2. 创建 GStreamer 管道 (发送端) */
    /* 2. 创建 GStreamer 管道 (发送端) */
    pipeline = gst_parse_launch(
        "webrtcbin name=send_webrtc stun-server=stun://stun.l.google.com:19302 "
        
        // 1. 采集
        "v4l2src device=/dev/video8 ! "
        "video/x-raw,width=640,height=480,framerate=30/1 ! "

        // 2. 转换
        "videoconvert ! "

        // 3. 编码 (openh264 — 比 x264 快数倍, 兼容性好)
        "openh264enc usage-type=camera bitrate=800000 complexity=low "
        "multi-thread=4 gop-size=30 ! "

        // 【关键】：constrained-baseline 保证浏览器兼容性
        "video/x-h264,profile=constrained-baseline ! "

        // 4. 打包
        "rtph264pay config-interval=1 ! "
        "application/x-rtp,media=video,encoding-name=H264,payload=96 ! "

        "send_webrtc.",
        NULL);
    
    webrtc = gst_bin_get_by_name(GST_BIN(pipeline), "send_webrtc");

    g_signal_connect(webrtc, "on-data-channel", G_CALLBACK(on_data_channel), NULL);
    /* 3. 绑定 WebRTC 信号 */
    g_signal_connect(webrtc, "on-negotiation-needed", G_CALLBACK(on_negotiation_needed), NULL);
    g_signal_connect(webrtc, "on-ice-candidate", G_CALLBACK(on_ice_candidate), NULL);

    g_print("Waiting for websocket connection...\n");
    g_main_loop_run(loop);

    return 0;
}
