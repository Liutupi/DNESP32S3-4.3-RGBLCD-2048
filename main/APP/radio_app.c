#include "radio_app.h"

#include "menu.h"
#include "radio_player.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCREEN_W                 800
#define SCREEN_H                 480
#define MAX_STATIONS              32
#define MAX_URLS                   3
#define MAX_TEXT                  96
#define MAX_URL                  192
#define TAP_MOVE_MAX              28
#define SWIPE_MIN                 80
#define REMOTE_MAX_BYTES       32768

#define COL_BG              0x160E0A
#define COL_PANEL           0x2A1A14
#define COL_PANEL_2         0x3A2318
#define COL_TEXT            0xFFF2DC
#define COL_TEXT_SOFT       0xC89A72
#define COL_MUTED           0x9C765D
#define COL_ACCENT          0xE58A3A
#define COL_OK              0x79D48A
#define COL_WARN            0xF0B35A
#define COL_BAD             0xF06A4E

typedef enum {
    RADIO_STREAM_MP3 = 0,
    RADIO_STREAM_HLS,
} radio_stream_type_t;

typedef enum {
    RADIO_STATUS_WIFI_OFFLINE = 0,
    RADIO_STATUS_CONNECTING,
    RADIO_STATUS_PROBING,
    RADIO_STATUS_READY,
    RADIO_STATUS_PLAYING,
    RADIO_STATUS_FAILED,
    RADIO_STATUS_SOURCE_FAILED,
    RADIO_STATUS_HLS_UNSUPPORTED,
    RADIO_STATUS_AUDIO_UNAVAILABLE,
} radio_status_t;

typedef struct {
    const char *name;
    const char *category;
    const char *urls[MAX_URLS];
    radio_stream_type_t type;
} builtin_station_t;

typedef struct {
    char name[MAX_TEXT];
    char category[MAX_TEXT];
    char urls[MAX_URLS][MAX_URL];
    char codec_hint[16];
    bool enabled;
    radio_stream_type_t type;
} radio_station_t;

typedef struct {
    int station_index;
    int url_index;
    int status_code;
    int64_t content_length;
    char content_type[64];
    char first_magic[64];
    char final_url[MAX_URL];
    bool ok;
    bool skipped;
    bool hls;
} probe_result_t;

typedef struct {
    int generation;
    bool remote;
} loader_arg_t;

static const char *TAG = "RadioApp";
static const char *USER_AGENT = "DNESP32S3-RGBLCD-Radio/1.0";
static const char *REMOTE_STATIONS_URL =
    "https://raw.githubusercontent.com/Liutupi/DNESP32S3-4.3-RGBLCD-2048/main/stations.json";

#if 0
static const builtin_station_t k_builtin_stations[] = {
    {"Groove Salad / SomaFM", "英文", {"https://ice5.somafm.com/groovesalad-128-mp3", "https://ice6.somafm.com/groovesalad-128-mp3", NULL}, RADIO_STREAM_MP3},
    {"CNR中国之声", "新闻", {"https://lhttp.qtfm.cn/live/15318317/64k.mp3", "https://lhttp-hw.qtfm.cn/live/15318317/64k.mp3", NULL}, RADIO_STREAM_MP3},
    {"广州新闻资讯", "广东", {"http://lhttp.qingting.fm/live/4848/64k.mp3", "https://lhttp.qtfm.cn/live/4848/64k.mp3", NULL}, RADIO_STREAM_MP3},
    {"广州交通经济", "交通", {"http://lhttp.qingting.fm/live/4955/64k.mp3", "https://lhttp.qtfm.cn/live/4955/64k.mp3", NULL}, RADIO_STREAM_MP3},
    {"珠江经济电台", "广东", {"http://lhttp.qingting.fm/live/1259/64k.mp3", "https://lhttp.qtfm.cn/live/1259/64k.mp3", NULL}, RADIO_STREAM_MP3},
    {"广东音乐之声", "华语音乐", {"http://lhttp.qingting.fm/live/1260/64k.mp3", "https://lhttp.qtfm.cn/live/1260/64k.mp3", NULL}, RADIO_STREAM_MP3},
    {"广东文体广播", "广东", {"https://lhttp.qtfm.cn/live/471/64k.mp3", "https://lhttp-hw.qtfm.cn/live/471/64k.mp3", NULL}, RADIO_STREAM_MP3},
    {"深圳飞扬971", "广东", {"http://lhttp.qingting.fm/live/1271/64k.mp3", "https://lhttp.qtfm.cn/live/1271/64k.mp3", NULL}, RADIO_STREAM_MP3},
    {"亚洲粤语", "华语音乐", {"https://lhttp.qingting.fm/live/15318569/64k.mp3", "https://lhttp.qtfm.cn/live/15318569/64k.mp3", NULL}, RADIO_STREAM_MP3},
    {"500首华语经典", "华语音乐", {"https://lhttp.qtfm.cn/live/5022308/64k.mp3", "https://lhttp-hw.qtfm.cn/live/5022308/64k.mp3", "http://lhttp.qingting.fm/live/5022308/64k.mp3"}, RADIO_STREAM_MP3},
    {"清晨音乐台", "华语音乐", {"http://lhttp.qingting.fm/live/4915/64k.mp3", "https://lhttp.qtfm.cn/live/4915/64k.mp3", NULL}, RADIO_STREAM_MP3},
    {"怀旧好声音", "华语音乐", {"http://lhttp.qingting.fm/live/1223/64k.mp3", "https://lhttp.qtfm.cn/live/1223/64k.mp3", NULL}, RADIO_STREAM_MP3},
    {"959年代音乐", "华语音乐", {"http://lhttp.qingting.fm/live/5021381/64k.mp3", "https://lhttp.qtfm.cn/live/5021381/64k.mp3", NULL}, RADIO_STREAM_MP3},
    {"动听音乐台", "华语音乐", {"http://lhttp.qingting.fm/live/5022107/64k.mp3", "https://lhttp.qtfm.cn/live/5022107/64k.mp3", NULL}, RADIO_STREAM_MP3},
    {"江苏经典流行音乐", "华语音乐", {"http://lhttp.qingting.fm/live/4938/64k.mp3", "https://lhttp.qtfm.cn/live/4938/64k.mp3", NULL}, RADIO_STREAM_MP3},
    {"HLS示例-禁用", "新闻", {"https://ngcdn001.cnr.cn/live/zgzs/index.m3u8", NULL, NULL}, RADIO_STREAM_HLS},
};

#endif

static const builtin_station_t k_builtin_stations[] = {
    {"Groove Salad / SomaFM", "English", {"https://ice5.somafm.com/groovesalad-128-mp3", "https://ice6.somafm.com/groovesalad-128-mp3", NULL}, RADIO_STREAM_MP3},
    {"CNR China Voice", "News", {"https://lhttp.qtfm.cn/live/15318317/64k.mp3", "https://lhttp-hw.qtfm.cn/live/15318317/64k.mp3", NULL}, RADIO_STREAM_MP3},
    {"Guangzhou News", "Guangdong", {"http://lhttp.qingting.fm/live/4848/64k.mp3", "https://lhttp.qtfm.cn/live/4848/64k.mp3", NULL}, RADIO_STREAM_MP3},
    {"Guangzhou Traffic", "Traffic", {"http://lhttp.qingting.fm/live/4955/64k.mp3", "https://lhttp.qtfm.cn/live/4955/64k.mp3", NULL}, RADIO_STREAM_MP3},
    {"Pearl River Economic Radio", "Guangdong", {"http://lhttp.qingting.fm/live/1259/64k.mp3", "https://lhttp.qtfm.cn/live/1259/64k.mp3", NULL}, RADIO_STREAM_MP3},
    {"Guangdong Music", "Chinese Music", {"http://lhttp.qingting.fm/live/1260/64k.mp3", "https://lhttp.qtfm.cn/live/1260/64k.mp3", NULL}, RADIO_STREAM_MP3},
    {"Guangdong Sports Radio", "Guangdong", {"https://lhttp.qtfm.cn/live/471/64k.mp3", "https://lhttp-hw.qtfm.cn/live/471/64k.mp3", NULL}, RADIO_STREAM_MP3},
    {"Shenzhen FM971", "Guangdong", {"http://lhttp.qingting.fm/live/1271/64k.mp3", "https://lhttp.qtfm.cn/live/1271/64k.mp3", NULL}, RADIO_STREAM_MP3},
    {"Asia Cantonese", "Chinese Music", {"https://lhttp.qingting.fm/live/15318569/64k.mp3", "https://lhttp.qtfm.cn/live/15318569/64k.mp3", NULL}, RADIO_STREAM_MP3},
    {"Chinese Classics 500", "Chinese Music", {"https://lhttp.qtfm.cn/live/5022308/64k.mp3", "https://lhttp-hw.qtfm.cn/live/5022308/64k.mp3", "http://lhttp.qingting.fm/live/5022308/64k.mp3"}, RADIO_STREAM_MP3},
    {"Morning Music", "Chinese Music", {"http://lhttp.qingting.fm/live/4915/64k.mp3", "https://lhttp.qtfm.cn/live/4915/64k.mp3", NULL}, RADIO_STREAM_MP3},
    {"Nostalgic Voices", "Chinese Music", {"http://lhttp.qingting.fm/live/1223/64k.mp3", "https://lhttp.qtfm.cn/live/1223/64k.mp3", NULL}, RADIO_STREAM_MP3},
    {"959 Era Music", "Chinese Music", {"http://lhttp.qingting.fm/live/5021381/64k.mp3", "https://lhttp.qtfm.cn/live/5021381/64k.mp3", NULL}, RADIO_STREAM_MP3},
    {"Touching Music", "Chinese Music", {"http://lhttp.qingting.fm/live/5022107/64k.mp3", "https://lhttp.qtfm.cn/live/5022107/64k.mp3", NULL}, RADIO_STREAM_MP3},
    {"Jiangsu Classic Pop", "Chinese Music", {"http://lhttp.qingting.fm/live/4938/64k.mp3", "https://lhttp.qtfm.cn/live/4938/64k.mp3", NULL}, RADIO_STREAM_MP3},
    {"HLS Example Disabled", "News", {"https://ngcdn001.cnr.cn/live/zgzs/index.m3u8", NULL, NULL}, RADIO_STREAM_HLS},
};

static radio_station_t g_stations[MAX_STATIONS];
static int g_station_count;
static int g_station_index;
static int g_url_index;
static int g_generation;
static bool g_probe_busy;
static bool g_remote_attempted;
static bool g_remote_loaded;
static radio_status_t g_status = RADIO_STATUS_CONNECTING;
static probe_result_t g_last_probe;

static lv_obj_t *g_screen;
static lv_obj_t *g_title_label;
static lv_obj_t *g_name_label;
static lv_obj_t *g_category_label;
static lv_obj_t *g_status_label;
static lv_obj_t *g_index_label;
static lv_obj_t *g_http_label;
static lv_obj_t *g_url_label;
static lv_obj_t *g_audio_label;
static lv_obj_t *g_remote_label;
static lv_obj_t *g_touch_layer;
static lv_coord_t g_press_x;
static lv_coord_t g_press_y;
static char g_player_message[160];

static bool wifi_is_connected(void)
{
    wifi_ap_record_t ap;
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
}

static const char *status_text(radio_status_t status)
{
    switch (status) {
        case RADIO_STATUS_WIFI_OFFLINE: return "WiFi Offline";
        case RADIO_STATUS_CONNECTING: return "Connecting";
        case RADIO_STATUS_PROBING: return "Probing";
        case RADIO_STATUS_READY: return "Ready";
        case RADIO_STATUS_PLAYING: return "Playing";
        case RADIO_STATUS_FAILED: return "Failed";
        case RADIO_STATUS_SOURCE_FAILED: return "Source failed";
        case RADIO_STATUS_HLS_UNSUPPORTED: return "HLS unsupported";
        case RADIO_STATUS_AUDIO_UNAVAILABLE: return "Audio unavailable";
        default: return "Connecting";
    }
}

static const char *stream_type_text(radio_stream_type_t type)
{
    return type == RADIO_STREAM_MP3 ? "MP3 direct" : "HLS m3u8";
}

static void copy_text(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) return;
    snprintf(dst, dst_size, "%s", src ? src : "");
}

static int station_url_count(const radio_station_t *station)
{
    int count = 0;
    for (int i = 0; i < MAX_URLS; ++i) {
        if (station->urls[i][0]) count++;
    }
    return count > 0 ? count : 1;
}

static void strtolower_copy(char *dst, size_t dst_size, const char *src)
{
    size_t out = 0;
    if (dst_size == 0) return;
    while (src && *src && out + 1 < dst_size) {
        dst[out++] = (char)tolower((unsigned char)*src++);
    }
    dst[out] = '\0';
}

static bool url_looks_playable_mp3(const char *url)
{
    char lower[MAX_URL];
    if (!url || !url[0]) return false;
    strtolower_copy(lower, sizeof(lower), url);
    return strstr(lower, ".mp3") &&
           !strstr(lower, ".m3u8") &&
           !strstr(lower, ".aac") &&
           !strstr(lower, ".flv") &&
           !strstr(lower, "token=");
}

static bool url_looks_hls(const char *url)
{
    char lower[MAX_URL];
    strtolower_copy(lower, sizeof(lower), url);
    return strstr(lower, ".m3u8") != NULL;
}

static bool content_type_allowed(const char *content_type)
{
    char lower[96];
    strtolower_copy(lower, sizeof(lower), content_type ? content_type : "");
    return strstr(lower, "audio/mpeg") ||
           strstr(lower, "audio/mp3") ||
           strstr(lower, "audio/aac") ||
           strstr(lower, "audio/aacp") ||
           strstr(lower, "application/octet-stream");
}

static bool content_type_rejected(const char *content_type)
{
    char lower[96];
    strtolower_copy(lower, sizeof(lower), content_type ? content_type : "");
    return strstr(lower, "text/html") ||
           strstr(lower, "application/json") ||
           strstr(lower, "mpegurl") ||
           strstr(lower, "text/plain");
}

static bool bytes_look_like_audio(const uint8_t *bytes, int len)
{
    if (!bytes || len < 2) return false;
    if (len >= 3 && bytes[0] == 'I' && bytes[1] == 'D' && bytes[2] == '3') return true;
    for (int i = 0; i + 1 < len; ++i) {
        if (bytes[i] == 0xFF && (bytes[i + 1] & 0xE0) == 0xE0) return true;
    }
    if (len >= 4 && memcmp(bytes, "ADIF", 4) == 0) return true;
    return false;
}

static void bytes_to_hex(const uint8_t *bytes, int len, char *out, size_t out_size)
{
    size_t used = 0;
    if (!out || out_size == 0) return;
    out[0] = '\0';
    for (int i = 0; bytes && i < len && used + 4 < out_size; ++i) {
        int wrote = snprintf(out + used, out_size - used, "%s%02X", i ? " " : "", bytes[i]);
        if (wrote <= 0) break;
        used += (size_t)wrote;
    }
}

static bool station_duplicate(const char *name, const char *url)
{
    for (int i = 0; i < g_station_count; ++i) {
        if (name && g_stations[i].name[0] && strcmp(g_stations[i].name, name) == 0) {
            return true;
        }
        for (int u = 0; u < MAX_URLS; ++u) {
            if (url && g_stations[i].urls[u][0] && strcmp(g_stations[i].urls[u], url) == 0) {
                return true;
            }
        }
    }
    return false;
}

static bool append_station(const char *name, const char *category, const char * const urls[MAX_URLS],
                           radio_stream_type_t type)
{
    if (!name || !name[0] || !urls || !urls[0] || !urls[0][0]) return false;
    if (g_station_count >= MAX_STATIONS) return false;
    if (station_duplicate(name, urls[0])) return false;

    radio_station_t *station = &g_stations[g_station_count++];
    memset(station, 0, sizeof(*station));
    copy_text(station->name, sizeof(station->name), name);
    copy_text(station->category, sizeof(station->category), category ? category : "电台");
    station->type = type;
    station->enabled = true;
    copy_text(station->codec_hint, sizeof(station->codec_hint),
              type == RADIO_STREAM_MP3 ? "mp3" : "hls");
    for (int i = 0; i < MAX_URLS; ++i) {
        copy_text(station->urls[i], sizeof(station->urls[i]), urls[i]);
    }
    return true;
}

static void load_builtin_stations(void)
{
    g_station_count = 0;
    for (size_t i = 0; i < sizeof(k_builtin_stations) / sizeof(k_builtin_stations[0]); ++i) {
        append_station(k_builtin_stations[i].name, k_builtin_stations[i].category,
                       k_builtin_stations[i].urls, k_builtin_stations[i].type);
    }
    if (g_station_index >= g_station_count) g_station_index = 0;
    g_url_index = 0;
}

static radio_stream_type_t parse_stream_type(const char *type, const char *url)
{
    char lower[32];
    strtolower_copy(lower, sizeof(lower), type ? type : "");
    if (strstr(lower, "hls") || strstr(lower, "m3u8") || url_looks_hls(url)) {
        return RADIO_STREAM_HLS;
    }
    return RADIO_STREAM_MP3;
}

static bool append_json_station(const cJSON *item)
{
    const cJSON *enabled = cJSON_GetObjectItem(item, "enabled");
    const cJSON *name = cJSON_GetObjectItem(item, "name");
    const cJSON *category = cJSON_GetObjectItem(item, "category");
    const cJSON *url = cJSON_GetObjectItem(item, "url");
    const cJSON *type = cJSON_GetObjectItem(item, "type");
    const cJSON *codec_hint = cJSON_GetObjectItem(item, "codec_hint");
    const char *urls[MAX_URLS] = {0};
    int out = 0;

    if (!cJSON_IsObject(item)) return false;
    if (cJSON_IsBool(enabled) && !cJSON_IsTrue(enabled)) return false;
    if (!cJSON_IsString(name) || !cJSON_IsString(url)) return false;

    const char *hint = cJSON_IsString(codec_hint) ? codec_hint->valuestring :
                       (cJSON_IsString(type) ? type->valuestring : NULL);
    radio_stream_type_t stream_type = parse_stream_type(hint, url->valuestring);
    if (stream_type != RADIO_STREAM_MP3 || !url_looks_playable_mp3(url->valuestring)) {
        ESP_LOGW(TAG, "Skip unsupported station from JSON: name=%s url=%s",
                 name->valuestring, url->valuestring);
        return false;
    }

    urls[out++] = url->valuestring;
    const cJSON *fallbacks = cJSON_GetObjectItem(item, "fallback_urls");
    if (cJSON_IsArray(fallbacks)) {
        const cJSON *fallback = NULL;
        cJSON_ArrayForEach(fallback, fallbacks) {
            if (out >= MAX_URLS) break;
            if (cJSON_IsString(fallback) && url_looks_playable_mp3(fallback->valuestring)) {
                urls[out++] = fallback->valuestring;
            }
        }
    }

    bool ok = append_station(name->valuestring,
                             cJSON_IsString(category) ? category->valuestring : "Radio",
                             urls, stream_type);
    if (ok && hint) {
        copy_text(g_stations[g_station_count - 1].codec_hint,
                  sizeof(g_stations[g_station_count - 1].codec_hint), hint);
    }
    return ok;
}

static bool download_text(const char *url, char **out_text, int *out_len)
{
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 4500,
        .buffer_size = 2048,
        .buffer_size_tx = 512,
        .disable_auto_redirect = false,
        .max_redirection_count = 5,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return false;

    char *text = calloc(1, REMOTE_MAX_BYTES + 1);
    if (!text) {
        esp_http_client_cleanup(client);
        return false;
    }

    esp_http_client_set_header(client, "User-Agent", USER_AGENT);
    esp_http_client_set_header(client, "Accept", "application/json,*/*");

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Remote stations open failed: %s", esp_err_to_name(err));
        free(text);
        esp_http_client_cleanup(client);
        return false;
    }

    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "Remote stations HTTP status=%d url=%s", status, url);
        free(text);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    int total = 0;
    while (total < REMOTE_MAX_BYTES) {
        int read = esp_http_client_read(client, text + total, REMOTE_MAX_BYTES - total);
        if (read <= 0) break;
        total += read;
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (total <= 0 || total >= REMOTE_MAX_BYTES) {
        free(text);
        return false;
    }
    text[total] = '\0';
    *out_text = text;
    *out_len = total;
    return true;
}

static bool load_remote_stations(void)
{
    char *json = NULL;
    int len = 0;
    if (!download_text(REMOTE_STATIONS_URL, &json, &len)) {
        return false;
    }

    cJSON *root = cJSON_ParseWithLength(json, len);
    free(json);
    if (!root) {
        ESP_LOGW(TAG, "Remote stations JSON parse failed");
        return false;
    }

    cJSON *array = cJSON_GetObjectItem(root, "stations");
    if (!cJSON_IsArray(array)) array = root;
    if (!cJSON_IsArray(array)) {
        ESP_LOGW(TAG, "Remote stations JSON has no stations array");
        cJSON_Delete(root);
        return false;
    }

    int before = g_station_count;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, array) {
        append_json_station(item);
    }
    cJSON_Delete(root);

    ESP_LOGI(TAG, "Remote stations merged: added=%d total=%d source=%s",
             g_station_count - before, g_station_count, REMOTE_STATIONS_URL);
    return g_station_count > before;
}

static radio_station_t *current_station(void)
{
    if (g_station_count <= 0) return NULL;
    if (g_station_index < 0) g_station_index = 0;
    if (g_station_index >= g_station_count) g_station_index = g_station_count - 1;
    return &g_stations[g_station_index];
}

static void label_set(lv_obj_t *label, const char *text)
{
    if (label) lv_label_set_text(label, text ? text : "");
}

static void update_ui(void)
{
    radio_station_t *station = current_station();
    char buf[256];
    int url_count = station ? station_url_count(station) : 1;

    if (!g_screen || !station) return;

    snprintf(buf, sizeof(buf), "%s", station->name);
    label_set(g_name_label, buf);
    snprintf(buf, sizeof(buf), "%s  |  %s", station->category, stream_type_text(station->type));
    label_set(g_category_label, buf);
    snprintf(buf, sizeof(buf), "Station %d/%d   URL %d/%d",
             g_station_index + 1, g_station_count, g_url_index + 1, url_count);
    label_set(g_index_label, buf);
    label_set(g_status_label, status_text(g_status));

    if (g_last_probe.status_code > 0 || g_last_probe.skipped) {
        snprintf(buf, sizeof(buf), "HTTP %d   type: %s   len: %lld   magic: %s",
                 g_last_probe.status_code,
                 g_last_probe.content_type[0] ? g_last_probe.content_type : "(none)",
                 (long long)g_last_probe.content_length,
                 g_last_probe.first_magic[0] ? g_last_probe.first_magic : "--");
    } else {
        snprintf(buf, sizeof(buf), "HTTP --   content-type: --   length: --");
    }
    label_set(g_http_label, buf);

    snprintf(buf, sizeof(buf), "%s", station->urls[g_url_index][0] ? station->urls[g_url_index] : "(no URL)");
    label_set(g_url_label, buf);

    if ((g_status == RADIO_STATUS_FAILED || g_status == RADIO_STATUS_PLAYING) && g_player_message[0]) {
        label_set(g_audio_label, g_player_message);
    } else if (radio_player_audio_available()) {
        label_set(g_audio_label, "Audio output available");
    } else if (g_last_probe.ok) {
        label_set(g_audio_label, "Stream OK, audio unavailable on RGB LCD build");
    } else {
        label_set(g_audio_label, radio_player_audio_unavailable_reason());
    }

    snprintf(buf, sizeof(buf), "%s%s",
             wifi_is_connected() ? "WiFi connected" : "WiFi offline",
             g_remote_loaded ? "  |  remote stations merged" :
             (g_remote_attempted ? "  |  using built-in stations" : ""));
    label_set(g_remote_label, buf);

    lv_obj_set_style_text_color(g_status_label,
                                lv_color_hex((g_status == RADIO_STATUS_PLAYING || g_last_probe.ok) ? COL_OK :
                                             (g_status == RADIO_STATUS_SOURCE_FAILED ||
                                              g_status == RADIO_STATUS_FAILED ||
                                              g_status == RADIO_STATUS_HLS_UNSUPPORTED ||
                                              g_status == RADIO_STATUS_WIFI_OFFLINE ? COL_BAD : COL_WARN)),
                                0);
}

static void update_ui_async(void *arg)
{
    (void)arg;
    update_ui();
}

static probe_result_t probe_url(const radio_station_t *station, const char *url, int station_index,
                                int station_count, int url_index)
{
    probe_result_t result = {
        .station_index = station_index,
        .url_index = url_index,
        .status_code = 0,
        .content_length = -1,
        .ok = false,
        .skipped = false,
        .hls = false,
    };

    if (!url_looks_playable_mp3(url)) {
        result.skipped = true;
        result.hls = url_looks_hls(url);
        ESP_LOGW(TAG, "probe skipped station=%d/%d name=%s category=%s url_index=%d url=%s reason=%s",
                 station_index + 1, station_count, station->name, station->category, url_index + 1, url,
                 result.hls ? "HLS unsupported" : "unsupported URL");
        return result;
    }

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 3000,
        .buffer_size = 1024,
        .buffer_size_tx = 512,
        .disable_auto_redirect = false,
        .max_redirection_count = 5,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return result;

    esp_http_client_set_header(client, "User-Agent", USER_AGENT);
    esp_http_client_set_header(client, "Accept", "audio/mpeg,*/*");
    esp_http_client_set_header(client, "Icy-MetaData", "0");

    esp_err_t err = esp_http_client_open(client, 0);
    if (err == ESP_OK) {
        esp_http_client_fetch_headers(client);
        result.status_code = esp_http_client_get_status_code(client);
        result.content_length = esp_http_client_get_content_length(client);
        char *content_type = NULL;
        if (esp_http_client_get_header(client, "Content-Type", &content_type) == ESP_OK && content_type) {
            copy_text(result.content_type, sizeof(result.content_type), content_type);
        }
        esp_http_client_get_url(client, result.final_url, sizeof(result.final_url));
        uint8_t first[512] = {0};
        int first_len = esp_http_client_read(client, (char *)first, sizeof(first));
        if (first_len > 0) {
            bytes_to_hex(first, first_len < 16 ? first_len : 16, result.first_magic, sizeof(result.first_magic));
        }
        result.ok = result.status_code >= 200 && result.status_code < 400 &&
                    content_type_allowed(result.content_type) &&
                    !content_type_rejected(result.content_type) &&
                    bytes_look_like_audio(first, first_len);
        ESP_LOGI(TAG, "probe station=%d/%d name=%s category=%s url_index=%d original_url=%s "
                      "final_url=%s using_fallback=%s HTTP status=%d content-type=%s "
                      "content-length=%lld codec_hint=%s first bytes magic=%s ok=%s reason=%s",
                 station_index + 1, station_count, station->name, station->category, url_index + 1, url,
                 result.final_url[0] ? result.final_url : url,
                 url_index > 0 ? "yes" : "no", result.status_code,
                 result.content_type[0] ? result.content_type : "(none)",
                 (long long)result.content_length, station->codec_hint,
                 result.first_magic[0] ? result.first_magic : "(none)",
                 result.ok ? "yes" : "no",
                 result.ok ? "direct audio stream" : "not direct audio stream");
    } else {
        ESP_LOGW(TAG, "probe open failed station=%d/%d name=%s category=%s url_index=%d url=%s err=%s",
                 station_index + 1, station_count, station->name, station->category, url_index + 1, url,
                 esp_err_to_name(err));
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return result;
}

static void probe_task(void *arg)
{
    int generation = (int)(intptr_t)arg;
    radio_station_t station_copy;
    int station_index;
    int station_count;
    bool found = false;
    probe_result_t result = {0};

    if (generation != g_generation || !current_station()) {
        g_probe_busy = false;
        vTaskDelete(NULL);
        return;
    }

    memcpy(&station_copy, current_station(), sizeof(station_copy));
    station_index = g_station_index;
    station_count = g_station_count;

    if (station_copy.type == RADIO_STREAM_HLS) {
        result.station_index = station_index;
        result.url_index = 0;
        result.skipped = true;
        result.hls = true;
        g_last_probe = result;
        g_status = RADIO_STATUS_HLS_UNSUPPORTED;
        ESP_LOGW(TAG, "station is HLS and unsupported: station=%d/%d name=%s",
                 station_index + 1, station_count, station_copy.name);
    } else if (!wifi_is_connected()) {
        g_status = RADIO_STATUS_WIFI_OFFLINE;
        ESP_LOGW(TAG, "probe skipped: WiFi offline");
    } else {
        for (int i = 0; i < MAX_URLS; ++i) {
            if (!station_copy.urls[i][0]) continue;
            result = probe_url(&station_copy, station_copy.urls[i], station_index, station_count, i);
            g_last_probe = result;
            if (result.hls) {
                g_status = RADIO_STATUS_HLS_UNSUPPORTED;
                break;
            }
            if (result.ok) {
                g_url_index = i;
                found = true;
                break;
            }
        }
        if (found) {
            g_status = radio_player_audio_available() ? RADIO_STATUS_READY : RADIO_STATUS_AUDIO_UNAVAILABLE;
        } else if (g_status != RADIO_STATUS_HLS_UNSUPPORTED) {
            g_status = RADIO_STATUS_SOURCE_FAILED;
        }
    }

    g_probe_busy = false;
    if (generation == g_generation && g_screen) {
        lv_async_call(update_ui_async, NULL);
    }
    vTaskDelete(NULL);
}

static void __attribute__((unused)) start_probe(void)
{
    if (g_probe_busy) return;
    memset(&g_last_probe, 0, sizeof(g_last_probe));
    g_status = wifi_is_connected() ? RADIO_STATUS_PROBING : RADIO_STATUS_WIFI_OFFLINE;
    update_ui();
    g_probe_busy = true;
    if (xTaskCreate(probe_task, "radio_probe", 6144, (void *)(intptr_t)g_generation, 5, NULL) != pdPASS) {
        g_probe_busy = false;
        g_status = RADIO_STATUS_SOURCE_FAILED;
        update_ui();
    }
}

static void player_ui_async(void *arg)
{
    (void)arg;
    update_ui();
}

static void player_status_cb(radio_player_state_t state, const char *message, void *user_ctx)
{
    (void)user_ctx;
    copy_text(g_player_message, sizeof(g_player_message), message ? message : "");
    switch (state) {
        case RADIO_PLAYER_CONNECTING:
            g_status = RADIO_STATUS_CONNECTING;
            break;
        case RADIO_PLAYER_PLAYING:
            g_status = RADIO_STATUS_PLAYING;
            break;
        case RADIO_PLAYER_FAILED:
            g_status = RADIO_STATUS_FAILED;
            break;
        case RADIO_PLAYER_STOPPED:
            g_status = RADIO_STATUS_READY;
            break;
        default:
            break;
    }
    if (g_screen) {
        lv_async_call(player_ui_async, NULL);
    }
}

static void start_playback(void)
{
    radio_station_t *station = current_station();
    if (!station) return;

    radio_player_stop();
    g_status = RADIO_STATUS_CONNECTING;
    copy_text(g_player_message, sizeof(g_player_message), "Connecting");
    update_ui();

    radio_player_request_t req = {
        .name = station->name,
        .url = station->urls[g_url_index][0] ? station->urls[g_url_index] : station->urls[0],
        .codec_hint = station->codec_hint,
        .status_cb = player_status_cb,
        .user_ctx = NULL,
    };
    int fallback_out = 0;
    for (int i = 0; i < MAX_URLS && fallback_out < 2; ++i) {
        if (i == g_url_index || !station->urls[i][0]) continue;
        req.fallback_urls[fallback_out++] = station->urls[i];
    }
    req.fallback_count = fallback_out;
    if (!radio_player_play(&req)) {
        g_status = RADIO_STATUS_FAILED;
        copy_text(g_player_message, sizeof(g_player_message), "RADIO: player task create failed");
        update_ui();
    }
}

static void start_beep_test(void)
{
    radio_player_stop();
    g_status = RADIO_STATUS_CONNECTING;
    copy_text(g_player_message, sizeof(g_player_message), "Testing 440Hz beep");
    update_ui();
    if (!radio_player_test_beep(player_status_cb, NULL)) {
        g_status = RADIO_STATUS_FAILED;
        copy_text(g_player_message, sizeof(g_player_message), radio_player_audio_unavailable_reason());
        update_ui();
    }
}

static void switch_station(int delta)
{
    if (g_station_count <= 0) return;
    radio_player_stop();
    g_station_index = (g_station_index + delta + g_station_count) % g_station_count;
    g_url_index = 0;
    memset(&g_last_probe, 0, sizeof(g_last_probe));
    g_status = current_station()->type == RADIO_STREAM_HLS ? RADIO_STATUS_HLS_UNSUPPORTED :
               (wifi_is_connected() ? RADIO_STATUS_READY : RADIO_STATUS_WIFI_OFFLINE);
    update_ui();
}

static void loader_task(void *arg)
{
    loader_arg_t *loader = (loader_arg_t *)arg;
    int generation = loader->generation;
    free(loader);

    if (wifi_is_connected()) {
        g_remote_attempted = true;
        g_remote_loaded = load_remote_stations();
    } else {
        g_status = RADIO_STATUS_WIFI_OFFLINE;
    }

    if (generation == g_generation && g_screen) {
        lv_async_call(update_ui_async, NULL);
    }
    vTaskDelete(NULL);
}

static void start_remote_loader(void)
{
    if (g_remote_attempted) return;
    loader_arg_t *arg = calloc(1, sizeof(*arg));
    if (!arg) return;
    arg->generation = g_generation;
    if (xTaskCreate(loader_task, "radio_stations", 6144, arg, 4, NULL) != pdPASS) {
        free(arg);
    }
}

static void back_cb(lv_event_t *e)
{
    (void)e;
    g_generation++;
    radio_player_stop();
    g_screen = NULL;
    menu_go_back();
}

static lv_obj_t *panel(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color, lv_opa_t opa, int r)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, opa, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, r, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                       uint32_t color, int x, int y, int w)
{
    lv_obj_t *obj = lv_label_create(parent);
    lv_label_set_text(obj, text);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_width(obj, w);
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_text_letter_space(obj, 0, 0);
    lv_label_set_long_mode(obj, LV_LABEL_LONG_DOT);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

static void touch_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_get_act();
    lv_point_t p;

    if (!indev) return;
    lv_indev_get_point(indev, &p);

    if (code == LV_EVENT_PRESSED) {
        g_press_x = p.x;
        g_press_y = p.y;
        return;
    }

    if (code != LV_EVENT_RELEASED) return;

    int dx = p.x - g_press_x;
    int dy = p.y - g_press_y;
    if (LV_ABS(dx) > SWIPE_MIN && LV_ABS(dx) > LV_ABS(dy)) {
        switch_station(dx > 0 ? -1 : 1);
        return;
    }
    if (LV_ABS(dx) > TAP_MOVE_MAX || LV_ABS(dy) > TAP_MOVE_MAX) return;

    if (p.x < 250) {
        switch_station(-1);
    } else if (p.x > 550) {
        switch_station(1);
    } else if (p.y > 380) {
        start_beep_test();
    } else {
        start_playback();
    }
}

void radio_app_start(void)
{
    g_generation++;
    load_builtin_stations();
    memset(&g_last_probe, 0, sizeof(g_last_probe));
    memset(g_player_message, 0, sizeof(g_player_message));
    g_status = wifi_is_connected() ? RADIO_STATUS_CONNECTING : RADIO_STATUS_WIFI_OFFLINE;
    g_probe_busy = false;
    g_remote_attempted = false;
    g_remote_loaded = false;

    g_screen = lv_obj_create(NULL);
    lv_scr_load(g_screen);
    lv_obj_set_style_bg_color(g_screen, lv_color_hex(COL_BG), 0);
    lv_obj_clear_flag(g_screen, LV_OBJ_FLAG_SCROLLABLE);

    panel(g_screen, 30, 112, 740, 254, COL_PANEL, LV_OPA_COVER, 24);
    panel(g_screen, 560, -72, 260, 170, COL_ACCENT, LV_OPA_20, 44);
    panel(g_screen, -80, 355, 250, 110, COL_PANEL_2, LV_OPA_60, 36);

    g_title_label = label(g_screen, "Radio / 网络电台", &lv_font_montserrat_28, COL_TEXT, 42, 30, 500);
    label(g_screen, "MP3 direct first . fallback . HLS unsupported", &lv_font_montserrat_14,
          COL_TEXT_SOFT, 44, 68, 460);

    lv_obj_t *back = lv_btn_create(g_screen);
    lv_obj_set_size(back, 112, 42);
    lv_obj_set_pos(back, 640, 32);
    lv_obj_set_style_bg_color(back, lv_color_hex(COL_PANEL_2), 0);
    lv_obj_set_style_radius(back, 8, 0);
    lv_obj_add_event_cb(back, back_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_t *back_text = label(back, "Back", &lv_font_montserrat_16, COL_TEXT, 0, 10, 112);
    lv_obj_set_style_text_align(back_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_event_cb(back_text, back_cb, LV_EVENT_RELEASED, NULL);

    g_index_label = label(g_screen, "Station --/--   URL --/--", &lv_font_montserrat_14,
                          COL_ACCENT, 70, 130, 660);
    g_name_label = label(g_screen, "--", &lv_font_montserrat_28, COL_TEXT, 70, 165, 660);
    g_category_label = label(g_screen, "--", &lv_font_montserrat_16, COL_TEXT_SOFT, 72, 206, 650);
    g_status_label = label(g_screen, "Connecting", &lv_font_montserrat_24, COL_WARN, 70, 246, 660);
    g_http_label = label(g_screen, "HTTP --   content-type: --   length: --", &lv_font_montserrat_14,
                         COL_TEXT_SOFT, 72, 292, 650);
    g_url_label = label(g_screen, "--", &lv_font_montserrat_12, COL_MUTED, 72, 324, 650);

    g_audio_label = label(g_screen, radio_player_audio_unavailable_reason(), &lv_font_montserrat_16,
                          COL_BAD, 46, 386, 710);
    g_remote_label = label(g_screen, "WiFi checking", &lv_font_montserrat_12, COL_TEXT_SOFT, 46, 416, 710);
    label(g_screen, "< left station     center play     bottom center beep test     right station",
          &lv_font_montserrat_14, COL_TEXT_SOFT, 46, 448, 710);

    g_touch_layer = lv_obj_create(g_screen);
    lv_obj_set_pos(g_touch_layer, 0, 0);
    lv_obj_set_size(g_touch_layer, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_opa(g_touch_layer, LV_OPA_0, 0);
    lv_obj_set_style_border_width(g_touch_layer, 0, 0);
    lv_obj_set_style_pad_all(g_touch_layer, 0, 0);
    lv_obj_add_flag(g_touch_layer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(g_touch_layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(g_touch_layer, touch_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(g_touch_layer, touch_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_move_background(g_touch_layer);
    lv_obj_move_foreground(back);

    update_ui();
    start_remote_loader();
}
