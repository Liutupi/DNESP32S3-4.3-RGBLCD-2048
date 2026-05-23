#include "radio_player.h"

#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mp3_dec.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if CONFIG_RADIO_EXTERNAL_I2S_DAC
#include "driver/i2s_std.h"
#endif

#define TAG "RADIO"
#define TAG_I2S "RADIO_I2S"
#define TAG_BEEP "RADIO_BEEP"
#define TAG_HTTP "RADIO_HTTP"
#define TAG_DECODER "RADIO_DECODER"

#define RADIO_SAMPLE_RATE_HZ       44100
#define RADIO_BEEP_HZ                440
#define RADIO_BEEP_MS                350
#define RADIO_HTTP_TIMEOUT_MS       7000
#define RADIO_HTTP_READ_CHUNK       2048
#define RADIO_RAW_BUFFER_SIZE      16384
#define RADIO_PCM_BUFFER_SIZE       8192
#define RADIO_PLAYLIST_MAX_BYTES    4096
#define RADIO_URL_MAX                256
#define RADIO_USER_AGENT "DNESP32S3-RGBLCD-Radio/2.0"

static volatile bool s_stop_requested;
static TaskHandle_t s_player_task;

#if CONFIG_RADIO_EXTERNAL_I2S_DAC
static i2s_chan_handle_t s_i2s_tx;
static int s_i2s_rate;
#endif

typedef struct {
    char name[64];
    char url[RADIO_URL_MAX];
    char urls[3][RADIO_URL_MAX];
    int url_count;
    char codec_hint[16];
    radio_player_status_cb_t cb;
    void *user_ctx;
} player_task_arg_t;

typedef struct {
    int status;
    int64_t content_length;
    char content_type[96];
    char final_url[RADIO_URL_MAX];
    uint8_t first_bytes[16];
    int first_len;
} stream_probe_t;

static void status_emit(const player_task_arg_t *arg, radio_player_state_t state, const char *message)
{
    ESP_LOGI(TAG, "%s", message ? message : "");
    if (arg && arg->cb) {
        arg->cb(state, message, arg->user_ctx);
    }
}

static bool wifi_is_connected(void)
{
    wifi_ap_record_t ap;
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
}

static void lower_copy(char *dst, size_t dst_size, const char *src)
{
    size_t i = 0;
    if (!dst || dst_size == 0) return;
    while (src && src[i] && i + 1 < dst_size) {
        dst[i] = (char)tolower((unsigned char)src[i]);
        i++;
    }
    dst[i] = '\0';
}

static bool str_has(const char *haystack, const char *needle)
{
    return haystack && needle && strstr(haystack, needle) != NULL;
}

static bool url_or_type_is_hls(const char *url, const char *content_type)
{
    char lower[RADIO_URL_MAX];
    lower_copy(lower, sizeof(lower), url);
    if (str_has(lower, ".m3u8")) return true;
    lower_copy(lower, sizeof(lower), content_type);
    return str_has(lower, "application/vnd.apple.mpegurl") ||
           str_has(lower, "application/x-mpegurl");
}

static bool url_or_type_is_playlist(const char *url, const char *content_type)
{
    char lower[RADIO_URL_MAX];
    lower_copy(lower, sizeof(lower), url);
    if (str_has(lower, ".m3u") || str_has(lower, ".pls")) return true;
    lower_copy(lower, sizeof(lower), content_type);
    return str_has(lower, "mpegurl") || str_has(lower, "scpls") ||
           str_has(lower, "audio/x-scpls");
}

static bool content_type_is_audio(const char *content_type)
{
    char lower[96];
    if (!content_type || !content_type[0]) return false;
    lower_copy(lower, sizeof(lower), content_type);
    return str_has(lower, "audio/mpeg") ||
           str_has(lower, "audio/mp3") ||
           str_has(lower, "audio/aac") ||
           str_has(lower, "audio/aacp") ||
           str_has(lower, "application/octet-stream");
}

static bool content_type_is_rejectable_text(const char *content_type)
{
    char lower[96];
    lower_copy(lower, sizeof(lower), content_type);
    return str_has(lower, "text/html") ||
           str_has(lower, "application/json") ||
           str_has(lower, "text/plain");
}

static bool first_bytes_look_like_audio(const uint8_t *bytes, int len)
{
    if (!bytes || len < 2) return false;
    if (len >= 3 && bytes[0] == 'I' && bytes[1] == 'D' && bytes[2] == '3') return true;
    for (int i = 0; i + 1 < len; ++i) {
        if (bytes[i] == 0xFF && (bytes[i + 1] & 0xE0) == 0xE0) return true;
    }
    if (len >= 7 && memcmp(bytes, "ADIF", 4) == 0) return true;
    for (int i = 0; i + 1 < len; ++i) {
        if (bytes[i] == 0xFF && (bytes[i + 1] & 0xF0) == 0xF0) return true;
    }
    return false;
}

static void first_bytes_to_hex(const uint8_t *bytes, int len, char *out, size_t out_size)
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

bool radio_player_audio_available(void)
{
#if CONFIG_RADIO_EXTERNAL_I2S_DAC
    return true;
#else
    return false;
#endif
}

const char *radio_player_audio_unavailable_reason(void)
{
#if CONFIG_RADIO_EXTERNAL_I2S_DAC
    return "external I2S DAC enabled";
#else
    return "External I2S DAC disabled. Enable Network Radio -> Enable external I2S DAC for Radio.";
#endif
}

#if CONFIG_RADIO_EXTERNAL_I2S_DAC
static bool gpio_conflicts_with_rgb_lcd(int gpio)
{
    return gpio == 3 || gpio == 9 || gpio == 10 || gpio == 14 || gpio == 46;
}

static esp_err_t i2s_start_tx(int sample_rate)
{
    ESP_LOGI(TAG_I2S, "init start");
    ESP_LOGI(TAG_I2S, "bclk=%d lrck=%d dout=%d mclk=%d",
             CONFIG_RADIO_I2S_BCLK_GPIO, CONFIG_RADIO_I2S_LRCK_GPIO,
             CONFIG_RADIO_I2S_DOUT_GPIO, CONFIG_RADIO_I2S_MCLK_GPIO);

    if (gpio_conflicts_with_rgb_lcd(CONFIG_RADIO_I2S_BCLK_GPIO) ||
        gpio_conflicts_with_rgb_lcd(CONFIG_RADIO_I2S_LRCK_GPIO) ||
        gpio_conflicts_with_rgb_lcd(CONFIG_RADIO_I2S_DOUT_GPIO) ||
        gpio_conflicts_with_rgb_lcd(CONFIG_RADIO_I2S_MCLK_GPIO)) {
        ESP_LOGE(TAG_I2S, "hardware pin conflict: external I2S config uses RGB LCD/ES8388 GPIO");
        return ESP_ERR_INVALID_ARG;
    }

    if (s_i2s_tx && s_i2s_rate == sample_rate) {
        ESP_LOGI(TAG_I2S, "init ok");
        return ESP_OK;
    }
    if (s_i2s_tx) {
        i2s_channel_disable(s_i2s_tx);
        i2s_del_channel(s_i2s_tx);
        s_i2s_tx = NULL;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_i2s_tx, NULL), TAG, "i2s new tx channel failed");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = CONFIG_RADIO_I2S_MCLK_GPIO >= 0 ? (gpio_num_t)CONFIG_RADIO_I2S_MCLK_GPIO : I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)CONFIG_RADIO_I2S_BCLK_GPIO,
            .ws = (gpio_num_t)CONFIG_RADIO_I2S_LRCK_GPIO,
            .dout = (gpio_num_t)CONFIG_RADIO_I2S_DOUT_GPIO,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_i2s_tx, &std_cfg), TAG, "i2s std init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_i2s_tx), TAG, "i2s enable failed");
    s_i2s_rate = sample_rate;
    ESP_LOGI(TAG_I2S, "init ok");
    ESP_LOGI(TAG_I2S, "rate=%d bits=16 channels=2 signed_pcm=yes", sample_rate);
    return ESP_OK;
}

static void i2s_stop_tx(void)
{
    if (s_i2s_tx) {
        i2s_channel_disable(s_i2s_tx);
        i2s_del_channel(s_i2s_tx);
        s_i2s_tx = NULL;
        s_i2s_rate = 0;
    }
}

static bool i2s_write_pcm(const int16_t *samples, size_t sample_count)
{
    size_t bytes_written = 0;
    esp_err_t err = i2s_channel_write(s_i2s_tx, samples, sample_count * sizeof(int16_t),
                                      &bytes_written, pdMS_TO_TICKS(500));
    if (err != ESP_OK || bytes_written == 0) {
        ESP_LOGE(TAG_I2S, "write failed: %s bytes=%u", esp_err_to_name(err), (unsigned)bytes_written);
        return false;
    }
    ESP_LOGI(TAG_I2S, "wrote pcm bytes=%u", (unsigned)bytes_written);
    return true;
}
#endif

void radio_player_stop(void)
{
    s_stop_requested = true;
    for (int i = 0; s_player_task && i < 50; ++i) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
#if CONFIG_RADIO_EXTERNAL_I2S_DAC
    i2s_stop_tx();
#endif
}

static bool parse_playlist_url(const char *body, char *out_url, size_t out_size, bool *is_hls)
{
    const char *p = body;
    if (is_hls) *is_hls = false;
    if (!body || !out_url || out_size == 0) return false;
    if (strstr(body, "#EXT-X-STREAM-INF") || strstr(body, "#EXT-X-TARGETDURATION")) {
        if (is_hls) *is_hls = true;
        return false;
    }
    while (*p) {
        while (*p == '\r' || *p == '\n' || *p == ' ' || *p == '\t') p++;
        const char *line = p;
        while (*p && *p != '\r' && *p != '\n') p++;
        size_t len = (size_t)(p - line);
        if (len > 5) {
            if (strncmp(line, "File", 4) == 0) {
                const char *eq = memchr(line, '=', len);
                if (eq && (size_t)(eq + 1 - line) < len) {
                    len -= (size_t)((eq + 1) - line);
                    line = eq + 1;
                }
            }
            if (len >= 4 && strncmp(line, "http", 4) == 0) {
                if (len >= out_size) len = out_size - 1;
                memcpy(out_url, line, len);
                out_url[len] = '\0';
                return true;
            }
        }
    }
    return false;
}

static bool fetch_playlist(const char *url, char *out_url, size_t out_size, bool *is_hls)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = RADIO_HTTP_TIMEOUT_MS,
        .buffer_size = 1024,
        .disable_auto_redirect = false,
        .max_redirection_count = 5,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;
    esp_http_client_set_header(client, "User-Agent", RADIO_USER_AGENT);
    esp_http_client_set_header(client, "Accept", "*/*");

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "playlist open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return false;
    }
    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    char *ct = NULL;
    esp_http_client_get_header(client, "Content-Type", &ct);
    ESP_LOGI(TAG, "opening URL %s", url);
    ESP_LOGI(TAG, "HTTP status=%d content-type=%s final-url=%s", status, ct ? ct : "(none)", url);

    char *body = calloc(1, RADIO_PLAYLIST_MAX_BYTES + 1);
    int total = 0;
    if (body) {
        while (total < RADIO_PLAYLIST_MAX_BYTES) {
            int got = esp_http_client_read(client, body + total, RADIO_PLAYLIST_MAX_BYTES - total);
            if (got <= 0) break;
            total += got;
        }
        body[total] = '\0';
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    bool ok = body && status >= 200 && status < 400 && parse_playlist_url(body, out_url, out_size, is_hls);
    free(body);
    return ok;
}

static void __attribute__((unused)) player_task(void *param)
{
    player_task_arg_t *arg = (player_task_arg_t *)param;
    char url[RADIO_URL_MAX];
    char resolved_url[RADIO_URL_MAX];

    s_player_task = xTaskGetCurrentTaskHandle();
    s_stop_requested = false;
    snprintf(url, sizeof(url), "%s", arg->url);

    if (!radio_player_audio_available()) {
        status_emit(arg, RADIO_PLAYER_FAILED, radio_player_audio_unavailable_reason());
        goto done;
    }
    if (!wifi_is_connected()) {
        status_emit(arg, RADIO_PLAYER_FAILED, "RADIO: WiFi not connected");
        goto done;
    }

    status_emit(arg, RADIO_PLAYER_CONNECTING, "RADIO: WiFi connected");
    status_emit(arg, RADIO_PLAYER_CONNECTING, "RADIO: opening URL ...");

    char hint[16];
    lower_copy(hint, sizeof(hint), arg->codec_hint);
    if (str_has(hint, "hls") || url_or_type_is_hls(url, NULL)) {
        status_emit(arg, RADIO_PLAYER_FAILED, "该电台为 HLS 流，当前版本暂不支持");
        goto done;
    }

    if (url_or_type_is_playlist(url, NULL)) {
        bool is_hls = false;
        if (!fetch_playlist(url, resolved_url, sizeof(resolved_url), &is_hls)) {
            status_emit(arg, RADIO_PLAYER_FAILED,
                        is_hls ? "该电台为 HLS 流，当前版本暂不支持" : "RADIO: playlist parse failed");
            goto done;
        }
        snprintf(url, sizeof(url), "%s", resolved_url);
        ESP_LOGI(TAG, "playlist resolved URL: %s", url);
    }

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = RADIO_HTTP_TIMEOUT_MS,
        .buffer_size = RADIO_HTTP_READ_CHUNK,
        .buffer_size_tx = 512,
        .disable_auto_redirect = false,
        .max_redirection_count = 5,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        status_emit(arg, RADIO_PLAYER_FAILED, "RADIO: HTTP client init failed");
        goto done;
    }
    esp_http_client_set_header(client, "User-Agent", RADIO_USER_AGENT);
    esp_http_client_set_header(client, "Accept", "audio/mpeg,audio/aac,*/*");
    esp_http_client_set_header(client, "Icy-MetaData", "0");

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        char msg[96];
        snprintf(msg, sizeof(msg), "RADIO: HTTP open failed: %s", esp_err_to_name(err));
        status_emit(arg, RADIO_PLAYER_FAILED, msg);
        esp_http_client_cleanup(client);
        goto done;
    }
    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    char *ct = NULL;
    esp_http_client_get_header(client, "Content-Type", &ct);
    int64_t len = esp_http_client_get_content_length(client);
    ESP_LOGI(TAG, "HTTP status=%d content-type=%s content-length=%lld final-url=%s",
             status, ct ? ct : "(none)", (long long)len, url);

    if (status < 200 || status >= 400) {
        status_emit(arg, RADIO_PLAYER_FAILED, "RADIO: HTTP status code abnormal");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        goto done;
    }
    if (url_or_type_is_hls(url, ct)) {
        status_emit(arg, RADIO_PLAYER_FAILED, "该电台为 HLS 流，当前版本暂不支持");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        goto done;
    }
    if (url_or_type_is_playlist(url, ct)) {
        status_emit(arg, RADIO_PLAYER_FAILED, "RADIO: playlist URL did not resolve to audio");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        goto done;
    }
    if (!content_type_is_audio(ct)) {
        status_emit(arg, RADIO_PLAYER_FAILED, "RADIO: URL is not an audio stream");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        goto done;
    }
    if (ct && (strstr(ct, "aac") || strstr(ct, "aacp"))) {
        status_emit(arg, RADIO_PLAYER_FAILED, "RADIO: AAC decoder unsupported in current build");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        goto done;
    }
    ESP_LOGI(TAG, "content-type %s", ct ? ct : "(none)");

#if CONFIG_RADIO_EXTERNAL_I2S_DAC
    err = i2s_start_tx(RADIO_SAMPLE_RATE_HZ);
    if (err != ESP_OK) {
        status_emit(arg, RADIO_PLAYER_FAILED, "RADIO: I2S init failed or pin conflict");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        goto done;
    }
#endif

    void *decoder = NULL;
    esp_audio_err_t dec_err = esp_mp3_dec_open(NULL, 0, &decoder);
    if (dec_err != ESP_AUDIO_ERR_OK) {
        status_emit(arg, RADIO_PLAYER_FAILED, "RADIO: decoder unsupported or failed");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        goto done;
    }
    ESP_LOGI(TAG, "decoder started");

    uint8_t *raw = malloc(RADIO_RAW_BUFFER_SIZE);
    uint8_t *pcm = malloc(RADIO_PCM_BUFFER_SIZE);
    if (!raw || !pcm) {
        status_emit(arg, RADIO_PLAYER_FAILED, "RADIO: audio buffer alloc failed");
        free(raw);
        free(pcm);
        esp_mp3_dec_close(decoder);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        goto done;
    }

    int raw_len = 0;
    int decode_failures = 0;
    uint32_t frames = 0;
    bool announced_playing = false;

    while (!s_stop_requested) {
        if (raw_len < RADIO_RAW_BUFFER_SIZE - RADIO_HTTP_READ_CHUNK) {
            int got = esp_http_client_read(client, (char *)raw + raw_len, RADIO_HTTP_READ_CHUNK);
            if (got <= 0) {
                status_emit(arg, RADIO_PLAYER_FAILED, got == 0 ? "RADIO: stream ended" : "RADIO: stream read failed");
                break;
            }
            raw_len += got;
        }

        bool decoded_any = false;
        while (raw_len > 0 && !s_stop_requested) {
            esp_audio_dec_in_raw_t in = {
                .buffer = raw,
                .len = raw_len,
            };
            esp_audio_dec_out_frame_t out = {
                .buffer = pcm,
                .len = RADIO_PCM_BUFFER_SIZE,
            };
            esp_audio_dec_info_t info = {0};
            dec_err = esp_mp3_dec_decode(decoder, &in, &out, &info);
            if (dec_err != ESP_AUDIO_ERR_OK || in.consumed == 0) {
                break;
            }
            decoded_any = true;
            decode_failures = 0;
            if (in.consumed < (uint32_t)raw_len) {
                memmove(raw, raw + in.consumed, raw_len - in.consumed);
            }
            raw_len -= in.consumed;
            if (out.decoded_size == 0) continue;

#if CONFIG_RADIO_EXTERNAL_I2S_DAC
            if (info.sample_rate > 0 && info.sample_rate != s_i2s_rate) {
                ESP_LOGI(TAG, "MP3 sample_rate=%d channels=%d bitrate=%d", info.sample_rate, info.channel, info.bitrate);
                if (i2s_start_tx(info.sample_rate) != ESP_OK) {
                    status_emit(arg, RADIO_PLAYER_FAILED, "RADIO: I2S reinit failed");
                    s_stop_requested = true;
                    break;
                }
            }
            if (!i2s_write_pcm((const int16_t *)pcm, out.decoded_size / sizeof(int16_t))) {
                status_emit(arg, RADIO_PLAYER_FAILED, "RADIO: I2S write failed");
                s_stop_requested = true;
                break;
            }
#endif
            frames++;
            if (!announced_playing) {
                status_emit(arg, RADIO_PLAYER_PLAYING, "RADIO: playing");
                ESP_LOGI(TAG, "i2s write ok");
                announced_playing = true;
            }
        }

        if (!decoded_any && raw_len >= RADIO_RAW_BUFFER_SIZE - RADIO_HTTP_READ_CHUNK) {
            raw_len = 0;
            if (++decode_failures >= 3) {
                status_emit(arg, RADIO_PLAYER_FAILED, "RADIO: MP3 decoder failed to find frames");
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    if (frames == 0 && !s_stop_requested) {
        status_emit(arg, RADIO_PLAYER_FAILED, "RADIO: decoder produced no audio");
    }

    free(raw);
    free(pcm);
    esp_mp3_dec_close(decoder);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

done:
#if CONFIG_RADIO_EXTERNAL_I2S_DAC
    i2s_stop_tx();
#endif
    if (s_stop_requested) {
        status_emit(arg, RADIO_PLAYER_STOPPED, "RADIO: stopped");
    }
    s_player_task = NULL;
    free(arg);
    vTaskDelete(NULL);
}

static bool player_try_url(player_task_arg_t *arg, const char *original_url, int attempt, int total_attempts)
{
    char url[RADIO_URL_MAX];
    char resolved_url[RADIO_URL_MAX] = {0};
    char msg[160];
    char first_hex[64];
    esp_http_client_handle_t client = NULL;
    void *decoder = NULL;
    uint8_t *raw = NULL;
    uint8_t *pcm = NULL;
    int raw_len = 0;
    int decode_failures = 0;
    uint32_t frames = 0;
    bool announced_playing = false;
    bool ok = false;

    snprintf(url, sizeof(url), "%s", original_url);
    ESP_LOGI(TAG_HTTP, "name=%s", arg->name);
    ESP_LOGI(TAG_HTTP, "original url=%s", url);

    char hint[16];
    lower_copy(hint, sizeof(hint), arg->codec_hint);
    if (str_has(hint, "hls") || url_or_type_is_hls(url, NULL)) {
        status_emit(arg, RADIO_PLAYER_FAILED, "Failed: HLS/m3u8 unsupported");
        return false;
    }

    if (url_or_type_is_playlist(url, NULL)) {
        bool is_hls = false;
        if (!fetch_playlist(url, resolved_url, sizeof(resolved_url), &is_hls)) {
            status_emit(arg, RADIO_PLAYER_FAILED,
                        is_hls ? "Failed: HLS/m3u8 unsupported" : "Failed: playlist parse failed");
            return false;
        }
        snprintf(url, sizeof(url), "%s", resolved_url);
        ESP_LOGI(TAG_HTTP, "playlist resolved url=%s", url);
    }

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = RADIO_HTTP_TIMEOUT_MS,
        .buffer_size = RADIO_HTTP_READ_CHUNK,
        .buffer_size_tx = 512,
        .disable_auto_redirect = false,
        .max_redirection_count = 5,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    client = esp_http_client_init(&cfg);
    if (!client) {
        status_emit(arg, RADIO_PLAYER_FAILED, "Failed: HTTP client init failed");
        return false;
    }
    esp_http_client_set_header(client, "User-Agent", RADIO_USER_AGENT);
    esp_http_client_set_header(client, "Accept", "audio/mpeg,audio/mp3,audio/aac,application/octet-stream,*/*");
    esp_http_client_set_header(client, "Icy-MetaData", "0");

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        snprintf(msg, sizeof(msg), "Failed: HTTP open %s", esp_err_to_name(err));
        status_emit(arg, RADIO_PLAYER_FAILED, msg);
        esp_http_client_cleanup(client);
        return false;
    }

    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    int64_t len = esp_http_client_get_content_length(client);
    char *ct = NULL;
    esp_http_client_get_header(client, "Content-Type", &ct);
    esp_http_client_get_url(client, resolved_url, sizeof(resolved_url));
    ESP_LOGI(TAG_HTTP, "final url after redirect=%s", resolved_url[0] ? resolved_url : url);
    ESP_LOGI(TAG_HTTP, "HTTP status=%d", status);
    ESP_LOGI(TAG_HTTP, "Content-Type=%s", ct ? ct : "(none)");
    ESP_LOGI(TAG_HTTP, "Content-Length=%lld", (long long)len);
    ESP_LOGI(TAG_HTTP, "codec_hint=%s", arg->codec_hint);

    raw = malloc(RADIO_RAW_BUFFER_SIZE);
    pcm = malloc(RADIO_PCM_BUFFER_SIZE);
    if (!raw || !pcm) {
        status_emit(arg, RADIO_PLAYER_FAILED, "Failed: audio buffer alloc failed");
        goto cleanup;
    }

    raw_len = esp_http_client_read(client, (char *)raw, RADIO_HTTP_READ_CHUNK);
    if (raw_len <= 0) {
        status_emit(arg, RADIO_PLAYER_FAILED, "Failed: stream read failed");
        goto cleanup;
    }
    first_bytes_to_hex(raw, raw_len < 16 ? raw_len : 16, first_hex, sizeof(first_hex));
    ESP_LOGI(TAG_HTTP, "first bytes magic=%s", first_hex);
    ESP_LOGI(TAG_HTTP, "received bytes=%d", raw_len);

    if (status < 200 || status >= 400) {
        snprintf(msg, sizeof(msg), "Failed: HTTP status %d", status);
        status_emit(arg, RADIO_PLAYER_FAILED, msg);
        goto cleanup;
    }
    if (url_or_type_is_hls(url, ct)) {
        status_emit(arg, RADIO_PLAYER_FAILED, "Failed: HLS/m3u8 unsupported");
        goto cleanup;
    }
    if (url_or_type_is_playlist(url, ct)) {
        status_emit(arg, RADIO_PLAYER_FAILED, "Failed: playlist did not resolve to direct audio");
        goto cleanup;
    }
    if (content_type_is_rejectable_text(ct) || !content_type_is_audio(ct) ||
        !first_bytes_look_like_audio(raw, raw_len)) {
        status_emit(arg, RADIO_PLAYER_FAILED, "Failed: not direct audio stream");
        goto cleanup;
    }
    if (ct && (strstr(ct, "aac") || strstr(ct, "aacp"))) {
        status_emit(arg, RADIO_PLAYER_FAILED, "Failed: AAC decoder unsupported");
        goto cleanup;
    }

#if CONFIG_RADIO_EXTERNAL_I2S_DAC
    if (i2s_start_tx(RADIO_SAMPLE_RATE_HZ) != ESP_OK) {
        status_emit(arg, RADIO_PLAYER_FAILED, "Failed: I2S init failed or pin conflict");
        goto cleanup;
    }
#endif

    esp_audio_err_t dec_err = esp_mp3_dec_open(NULL, 0, &decoder);
    if (dec_err != ESP_AUDIO_ERR_OK) {
        snprintf(msg, sizeof(msg), "Failed: decoder open err=%d", (int)dec_err);
        status_emit(arg, RADIO_PLAYER_FAILED, msg);
        goto cleanup;
    }
    ESP_LOGI(TAG_DECODER, "started");

    while (!s_stop_requested) {
        if (raw_len < RADIO_RAW_BUFFER_SIZE - RADIO_HTTP_READ_CHUNK) {
            int got = esp_http_client_read(client, (char *)raw + raw_len, RADIO_HTTP_READ_CHUNK);
            if (got <= 0) {
                status_emit(arg, RADIO_PLAYER_FAILED, got == 0 ? "Failed: stream ended" : "Failed: stream read failed");
                break;
            }
            raw_len += got;
            ESP_LOGI(TAG_HTTP, "received bytes=%d", got);
        }

        bool decoded_any = false;
        while (raw_len > 0 && !s_stop_requested) {
            esp_audio_dec_in_raw_t in = {.buffer = raw, .len = raw_len};
            esp_audio_dec_out_frame_t out = {.buffer = pcm, .len = RADIO_PCM_BUFFER_SIZE};
            esp_audio_dec_info_t info = {0};
            dec_err = esp_mp3_dec_decode(decoder, &in, &out, &info);
            if (dec_err != ESP_AUDIO_ERR_OK || in.consumed == 0) {
                ESP_LOGW(TAG_DECODER, "decode wait/fail err=%d consumed=%u raw_len=%d",
                         (int)dec_err, (unsigned)in.consumed, raw_len);
                break;
            }
            decoded_any = true;
            decode_failures = 0;
            if (in.consumed < (uint32_t)raw_len) {
                memmove(raw, raw + in.consumed, raw_len - in.consumed);
            }
            raw_len -= in.consumed;
            if (out.decoded_size == 0) continue;

            ESP_LOGI(TAG_DECODER, "frame decoded, pcm bytes=%u sample_rate=%u bits=%u channels=%u bitrate=%u",
                     (unsigned)out.decoded_size, (unsigned)info.sample_rate,
                     (unsigned)info.bits_per_sample, (unsigned)info.channel,
                     (unsigned)info.bitrate);
            if (info.bits_per_sample && info.bits_per_sample != 16) {
                status_emit(arg, RADIO_PLAYER_FAILED, "Failed: decoder PCM is not 16-bit");
                s_stop_requested = true;
                break;
            }
            if (info.channel && info.channel != 1 && info.channel != 2) {
                status_emit(arg, RADIO_PLAYER_FAILED, "Failed: decoder channel count unsupported");
                s_stop_requested = true;
                break;
            }

#if CONFIG_RADIO_EXTERNAL_I2S_DAC
            if (info.sample_rate > 0 && info.sample_rate != s_i2s_rate) {
                if (info.sample_rate != 44100 && info.sample_rate != 48000) {
                    status_emit(arg, RADIO_PLAYER_FAILED, "Failed: MP3 sample rate unsupported");
                    s_stop_requested = true;
                    break;
                }
                if (i2s_start_tx(info.sample_rate) != ESP_OK) {
                    status_emit(arg, RADIO_PLAYER_FAILED, "Failed: I2S reinit failed");
                    s_stop_requested = true;
                    break;
                }
            }
            const int16_t *i2s_pcm = (const int16_t *)pcm;
            size_t i2s_samples = out.decoded_size / sizeof(int16_t);
            if (info.channel == 1) {
                if (out.decoded_size * 2 > RADIO_PCM_BUFFER_SIZE) {
                    status_emit(arg, RADIO_PLAYER_FAILED, "Failed: mono PCM frame too large");
                    s_stop_requested = true;
                    break;
                }
                int16_t *mono = (int16_t *)pcm;
                for (int i = (int)i2s_samples - 1; i >= 0; --i) {
                    mono[i * 2] = mono[i];
                    mono[i * 2 + 1] = mono[i];
                }
                i2s_samples *= 2;
            }
            if (!i2s_write_pcm(i2s_pcm, i2s_samples)) {
                status_emit(arg, RADIO_PLAYER_FAILED, "Failed: I2S write failed");
                s_stop_requested = true;
                break;
            }
#endif
            frames++;
            if (!announced_playing) {
                status_emit(arg, RADIO_PLAYER_PLAYING, attempt > 0 ? "Playing fallback" : "Playing");
                announced_playing = true;
            }
        }

        if (!decoded_any && raw_len >= RADIO_RAW_BUFFER_SIZE - RADIO_HTTP_READ_CHUNK) {
            raw_len = 0;
            if (++decode_failures >= 3) {
                snprintf(msg, sizeof(msg), "Failed: MP3 decoder err=%d", (int)dec_err);
                status_emit(arg, RADIO_PLAYER_FAILED, msg);
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    ok = frames > 0 || s_stop_requested;
    if (frames == 0 && !s_stop_requested) {
        status_emit(arg, RADIO_PLAYER_FAILED, "Failed: decoder produced no audio");
    }

cleanup:
    if (decoder) esp_mp3_dec_close(decoder);
    free(raw);
    free(pcm);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
#if CONFIG_RADIO_EXTERNAL_I2S_DAC
    i2s_stop_tx();
#endif
    if (!ok && !s_stop_requested && attempt + 1 < total_attempts) {
        snprintf(msg, sizeof(msg), "Trying fallback %d/%d...", attempt + 1, total_attempts - 1);
        status_emit(arg, RADIO_PLAYER_CONNECTING, msg);
    }
    return ok;
}

static void player_task_with_fallbacks(void *param)
{
    player_task_arg_t *arg = (player_task_arg_t *)param;

    s_player_task = xTaskGetCurrentTaskHandle();
    s_stop_requested = false;

    if (!radio_player_audio_available()) {
        status_emit(arg, RADIO_PLAYER_FAILED, radio_player_audio_unavailable_reason());
        goto done;
    }
    if (!wifi_is_connected()) {
        status_emit(arg, RADIO_PLAYER_FAILED, "Failed: WiFi not connected");
        goto done;
    }

    status_emit(arg, RADIO_PLAYER_CONNECTING, "RADIO: WiFi connected");
    for (int i = 0; i < arg->url_count && !s_stop_requested; ++i) {
        if (!arg->urls[i][0]) continue;
        if (player_try_url(arg, arg->urls[i], i, arg->url_count)) {
            break;
        }
    }

done:
    if (s_stop_requested) {
        status_emit(arg, RADIO_PLAYER_STOPPED, "RADIO: stopped");
    }
    s_player_task = NULL;
    free(arg);
    vTaskDelete(NULL);
}

bool radio_player_play(const radio_player_request_t *request)
{
    if (!request || !request->url || !request->url[0]) return false;
    radio_player_stop();
    player_task_arg_t *arg = calloc(1, sizeof(*arg));
    if (!arg) return false;
    snprintf(arg->name, sizeof(arg->name), "%s", request->name ? request->name : "Radio");
    snprintf(arg->url, sizeof(arg->url), "%s", request->url);
    snprintf(arg->urls[arg->url_count++], sizeof(arg->urls[0]), "%s", request->url);
    for (int i = 0; i < request->fallback_count && i < 2; ++i) {
        if (request->fallback_urls[i] && request->fallback_urls[i][0]) {
            snprintf(arg->urls[arg->url_count++], sizeof(arg->urls[0]), "%s", request->fallback_urls[i]);
        }
    }
    snprintf(arg->codec_hint, sizeof(arg->codec_hint), "%s", request->codec_hint ? request->codec_hint : "unknown");
    arg->cb = request->status_cb;
    arg->user_ctx = request->user_ctx;
    s_stop_requested = false;
    if (xTaskCreate(player_task_with_fallbacks, "radio_player", 12288, arg, 5, &s_player_task) != pdPASS) {
        free(arg);
        s_player_task = NULL;
        return false;
    }
    return true;
}

bool radio_player_test_beep(radio_player_status_cb_t cb, void *user_ctx)
{
    player_task_arg_t arg = {
        .name = "Beep",
        .url = "",
        .codec_hint = "pcm",
        .cb = cb,
        .user_ctx = user_ctx,
    };
    if (!radio_player_audio_available()) {
        status_emit(&arg, RADIO_PLAYER_FAILED, radio_player_audio_unavailable_reason());
        return false;
    }

#if CONFIG_RADIO_EXTERNAL_I2S_DAC
    if (i2s_start_tx(RADIO_SAMPLE_RATE_HZ) != ESP_OK) {
        status_emit(&arg, RADIO_PLAYER_FAILED, "RADIO: I2S init failed or pin conflict");
        return false;
    }
    ESP_LOGI(TAG_BEEP, "start 440Hz");
    status_emit(&arg, RADIO_PLAYER_CONNECTING, "RADIO_BEEP: start 440Hz");
    int16_t frame[128 * 2];
    uint32_t total = (RADIO_SAMPLE_RATE_HZ * RADIO_BEEP_MS) / 1000;
    uint32_t pos = 0;
    size_t total_written = 0;
    bool ok = true;
    while (pos < total) {
        uint32_t n = total - pos;
        if (n > 128) n = 128;
        for (uint32_t i = 0; i < n; ++i) {
            float phase = 2.0f * (float)M_PI * RADIO_BEEP_HZ * (float)(pos + i) / (float)RADIO_SAMPLE_RATE_HZ;
            int16_t sample = (int16_t)(sinf(phase) * 9000.0f);
            frame[i * 2] = sample;
            frame[i * 2 + 1] = sample;
        }
        if (!i2s_write_pcm(frame, n * 2)) {
            ok = false;
            break;
        }
        total_written += n * 2 * sizeof(int16_t);
        pos += n;
    }
    i2s_stop_tx();
    ESP_LOGI(TAG_BEEP, "written bytes=%u", (unsigned)total_written);
    ESP_LOGI(TAG_BEEP, "done");
    status_emit(&arg, ok ? RADIO_PLAYER_PLAYING : RADIO_PLAYER_FAILED,
                ok ? "RADIO_BEEP: done" : "RADIO_BEEP: I2S write failed");
    return ok;
#else
    return false;
#endif
}
