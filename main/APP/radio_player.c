#include "radio_player.h"

#include "board_audio.h"

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mp3_dec.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#define TAG "RADIO_PLAYER"
#define RADIO_HTTP_TIMEOUT_MS 7000
#define RADIO_HTTP_READ_CHUNK 2048
#define RADIO_RAW_BUFFER_SIZE 16384
#define RADIO_PCM_BUFFER_SIZE 8192
#define RADIO_URL_MAX 256
#define RADIO_MAX_URLS 4
#define RADIO_USER_AGENT "Mozilla/5.0 ESP32 Radio"
#define RADIO_REASON_MAX 128

typedef struct {
    char name[64];
    char urls[RADIO_MAX_URLS][RADIO_URL_MAX];
    int url_count;
    char last_error[RADIO_REASON_MAX];
    radio_player_status_cb_t cb;
    void *user_ctx;
} player_task_arg_t;

static volatile bool s_stop_requested;
static volatile bool s_running;
static TaskHandle_t s_player_task;

static void emit_status(const player_task_arg_t *arg, radio_player_state_t state, const char *message)
{
    if (message) {
        ESP_LOGI(TAG, "%s", message);
    }
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
    if (!dst || dst_size == 0) {
        return;
    }
    while (src && src[i] && i + 1 < dst_size) {
        dst[i] = (char)tolower((unsigned char)src[i]);
        ++i;
    }
    dst[i] = '\0';
}

static bool str_has(const char *haystack, const char *needle)
{
    return haystack && needle && strstr(haystack, needle) != NULL;
}

static void set_last_error(player_task_arg_t *arg, const char *fmt, ...)
{
    if (!arg || !fmt) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(arg->last_error, sizeof(arg->last_error), fmt, ap);
    va_end(ap);
}

static bool url_is_supported_mp3_candidate(const char *url)
{
    char lower[RADIO_URL_MAX];
    lower_copy(lower, sizeof(lower), url);
    return url && url[0] &&
           !str_has(lower, ".m3u8") &&
           !str_has(lower, "mpegurl") &&
           !str_has(lower, "aac") &&
           !str_has(lower, "aacp");
}

static bool content_type_is_rejected(const char *content_type)
{
    char lower[96];
    lower_copy(lower, sizeof(lower), content_type);
    return str_has(lower, "text/html") ||
           str_has(lower, "application/json") ||
           str_has(lower, "mpegurl") ||
           str_has(lower, "audio/aac") ||
           str_has(lower, "audio/aacp");
}

static bool first_bytes_look_like_mp3(const uint8_t *bytes, int len)
{
    if (!bytes || len < 2) {
        return false;
    }
    if (len >= 3 && bytes[0] == 'I' && bytes[1] == 'D' && bytes[2] == '3') {
        return true;
    }
    for (int i = 0; i + 1 < len; ++i) {
        if (bytes[i] == 0xFF && (bytes[i + 1] & 0xE0) == 0xE0) {
            return true;
        }
    }
    return false;
}

bool radio_player_audio_available(void)
{
#if CONFIG_RADIO_BOARD_ES8388_HEADLESS
    return true;
#else
    return false;
#endif
}

const char *radio_player_audio_unavailable_reason(void)
{
#if CONFIG_RADIO_BOARD_ES8388_HEADLESS
    return "RADIO_BOARD_ES8388_HEADLESS enabled";
#else
    return "RADIO_BOARD_ES8388_HEADLESS is disabled";
#endif
}

bool radio_player_is_running(void)
{
    return s_running;
}

static bool write_pcm_frame(player_task_arg_t *arg, uint8_t *pcm, uint32_t decoded_size,
                            const esp_audio_dec_info_t *info)
{
    if (!pcm || decoded_size == 0) {
        return true;
    }
    if (info && info->sample_rate > 0 && info->sample_rate != BOARD_AUDIO_SAMPLE_RATE_HZ) {
        set_last_error(arg, "unsupported_sample_rate %u", (unsigned)info->sample_rate);
        ESP_LOGW(TAG, "RADIO_URL_FAILED reason=unsupported_sample_rate sample_rate=%u",
                 (unsigned)info->sample_rate);
        return false;
    }

    int channels = (info && info->channel > 0) ? (int)info->channel : 2;
    size_t samples = decoded_size / sizeof(int16_t);
    int16_t *pcm16 = (int16_t *)pcm;
    if (channels == 1) {
        if (decoded_size * 2 > RADIO_PCM_BUFFER_SIZE) {
            set_last_error(arg, "no_i2s_write");
            ESP_LOGE(TAG, "RADIO_URL_FAILED reason=no_i2s_write detail=mono_pcm_frame_too_large");
            return false;
        }
        for (int i = (int)samples - 1; i >= 0; --i) {
            pcm16[i * 2] = pcm16[i];
            pcm16[i * 2 + 1] = pcm16[i];
        }
        samples *= 2;
    }
    if (!board_audio_i2s_write(pcm16, samples)) {
        set_last_error(arg, "no_i2s_write");
        ESP_LOGW(TAG, "RADIO_URL_FAILED reason=no_i2s_write");
        return false;
    }
    return true;
}

static bool play_one_url(player_task_arg_t *arg, const char *url, int url_index)
{
    esp_http_client_handle_t client = NULL;
    void *decoder = NULL;
    uint8_t *raw = NULL;
    uint8_t *pcm = NULL;
    int raw_len = 0;
    int decode_failures = 0;
    uint32_t frames = 0;
    bool announced = false;
    bool ok = false;
    bool fatal_url_failure = false;

    if (!url_is_supported_mp3_candidate(url)) {
        set_last_error(arg, "unsupported_stream_type");
        ESP_LOGW(TAG, "RADIO_URL_FAILED reason=unsupported_stream_type url=%s", url ? url : "(null)");
        return false;
    }
    if (url_index == 0) {
        ESP_LOGI(TAG, "RADIO_URL_TRY primary url=%s", url);
    } else {
        ESP_LOGI(TAG, "RADIO_TRY_FALLBACK index=%d url=%s", url_index, url);
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
        set_last_error(arg, "http_open failed");
        ESP_LOGW(TAG, "RADIO_URL_FAILED reason=http_open failed detail=http_client_init url=%s", url);
        return false;
    }
    esp_http_client_set_header(client, "User-Agent", RADIO_USER_AGENT);
    esp_http_client_set_header(client, "Accept", "audio/mpeg,audio/mp3,application/octet-stream,*/*");
    esp_http_client_set_header(client, "Icy-MetaData", "0");

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        set_last_error(arg, "http_open failed");
        ESP_LOGW(TAG, "RADIO_URL_FAILED reason=http_open failed err=%s url=%s", esp_err_to_name(err), url);
        goto cleanup;
    }
    ESP_LOGI(TAG, "MP3_HTTP_OPEN_OK");

    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    int64_t content_length = esp_http_client_get_content_length(client);
    char *content_type = NULL;
    esp_http_client_get_header(client, "Content-Type", &content_type);
    ESP_LOGI(TAG, "HTTP status=%d content-type=%s content-length=%" PRId64 " url=%s",
             status, content_type ? content_type : "(none)", content_length, url);
    if (status < 200 || status >= 400) {
        set_last_error(arg, "http_status %d", status);
        ESP_LOGW(TAG, "RADIO_URL_FAILED reason=http_status %d url=%s", status, url);
        goto cleanup;
    }
    if (content_type_is_rejected(content_type)) {
        set_last_error(arg, "content_type %s", content_type ? content_type : "(none)");
        ESP_LOGW(TAG, "RADIO_URL_FAILED reason=content_type %s url=%s",
                 content_type ? content_type : "(none)", url);
        goto cleanup;
    }

    raw = malloc(RADIO_RAW_BUFFER_SIZE);
    pcm = malloc(RADIO_PCM_BUFFER_SIZE);
    if (!raw || !pcm) {
        set_last_error(arg, "buffer_alloc");
        ESP_LOGW(TAG, "RADIO_URL_FAILED reason=buffer_alloc");
        goto cleanup;
    }

    raw_len = esp_http_client_read(client, (char *)raw, RADIO_HTTP_READ_CHUNK);
    if (raw_len <= 0 || !first_bytes_look_like_mp3(raw, raw_len)) {
        set_last_error(arg, "first_bytes_not_mp3");
        ESP_LOGW(TAG, "RADIO_URL_FAILED reason=first_bytes_not_mp3 read=%d url=%s", raw_len, url);
        goto cleanup;
    }

    esp_audio_err_t dec_err = esp_mp3_dec_open(NULL, 0, &decoder);
    if (dec_err != ESP_AUDIO_ERR_OK) {
        set_last_error(arg, "decoder_open failed");
        ESP_LOGW(TAG, "RADIO_URL_FAILED reason=decoder_open failed err=%d url=%s", (int)dec_err, url);
        goto cleanup;
    }
    ESP_LOGI(TAG, "MP3_DECODER_START_OK");

    while (!s_stop_requested && !fatal_url_failure) {
        if (raw_len < RADIO_RAW_BUFFER_SIZE - RADIO_HTTP_READ_CHUNK) {
            int got = esp_http_client_read(client, (char *)raw + raw_len, RADIO_HTTP_READ_CHUNK);
            if (got <= 0) {
                set_last_error(arg, "stream_read %d", got);
                ESP_LOGW(TAG, "RADIO_URL_FAILED reason=stream_read read=%d url=%s", got, url);
                break;
            }
            raw_len += got;
        }

        bool decoded_any = false;
        while (raw_len > 0 && !s_stop_requested) {
            esp_audio_dec_in_raw_t in = {.buffer = raw, .len = raw_len};
            esp_audio_dec_out_frame_t out = {.buffer = pcm, .len = RADIO_PCM_BUFFER_SIZE};
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
            raw_len -= (int)in.consumed;
            if (out.decoded_size == 0) {
                continue;
            }
            if (!write_pcm_frame(arg, pcm, out.decoded_size, &info)) {
                fatal_url_failure = true;
                break;
            }
            frames++;
            if (!announced) {
                emit_status(arg, RADIO_PLAYER_PLAYING, "RADIO_PLAYING");
                announced = true;
            }
        }

        if (!decoded_any && raw_len >= RADIO_RAW_BUFFER_SIZE - RADIO_HTTP_READ_CHUNK) {
            raw_len = 0;
            decode_failures++;
            set_last_error(arg, "decode_wait %d", (int)dec_err);
            ESP_LOGW(TAG, "RADIO_URL_FAILED reason=decode_wait err=%d failures=%d",
                     (int)dec_err, decode_failures);
            if (decode_failures >= 3) {
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    ok = frames > 0;

cleanup:
    if (decoder) {
        esp_mp3_dec_close(decoder);
    }
    free(raw);
    free(pcm);
    if (client) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    if (!ok && !s_stop_requested) {
        ESP_LOGW(TAG, "RADIO_URL_FAILED url=%s", url);
    }
    return ok || s_stop_requested;
}

static void player_task(void *param)
{
    player_task_arg_t *arg = (player_task_arg_t *)param;
    bool played = false;

    s_player_task = xTaskGetCurrentTaskHandle();
    s_running = true;
    s_stop_requested = false;

    if (!radio_player_audio_available()) {
        emit_status(arg, RADIO_PLAYER_FAILED, radio_player_audio_unavailable_reason());
        goto done;
    }
    if (!wifi_is_connected()) {
        emit_status(arg, RADIO_PLAYER_FAILED, "RADIO_URL_FAILED reason=wifi_not_connected");
        goto done;
    }

    emit_status(arg, RADIO_PLAYER_CONNECTING, "RADIO_CONNECTING");
    set_last_error(arg, "unknown");
    for (int i = 0; i < arg->url_count && !s_stop_requested; ++i) {
        if (!arg->urls[i][0]) {
            continue;
        }
        if (play_one_url(arg, arg->urls[i], i)) {
            played = true;
            break;
        }
    }

    if (!played && !s_stop_requested) {
        char message[RADIO_REASON_MAX + 32];
        snprintf(message, sizeof(message), "RADIO_URL_FAILED reason=%s",
                 arg->last_error[0] ? arg->last_error : "unknown");
        emit_status(arg, RADIO_PLAYER_FAILED, message);
    }

done:
    if (s_stop_requested) {
        emit_status(arg, RADIO_PLAYER_STOPPED, "RADIO_STOPPED");
    }
    s_running = false;
    s_player_task = NULL;
    free(arg);
    vTaskDelete(NULL);
}

void radio_player_stop(void)
{
    s_stop_requested = true;
    for (int i = 0; s_running && i < 100; ++i) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

bool radio_player_play(const radio_player_request_t *request)
{
    if (!request || !request->url || !request->url[0]) {
        return false;
    }

    radio_player_stop();

    player_task_arg_t *arg = calloc(1, sizeof(*arg));
    if (!arg) {
        return false;
    }

    snprintf(arg->name, sizeof(arg->name), "%s", request->name ? request->name : "Radio");
    snprintf(arg->urls[arg->url_count++], sizeof(arg->urls[0]), "%s", request->url);
    for (int i = 0; i < request->fallback_count && i < 3 && arg->url_count < RADIO_MAX_URLS; ++i) {
        if (request->fallback_urls[i] && request->fallback_urls[i][0]) {
            snprintf(arg->urls[arg->url_count++], sizeof(arg->urls[0]), "%s", request->fallback_urls[i]);
        }
    }
    arg->cb = request->status_cb;
    arg->user_ctx = request->user_ctx;

    if (xTaskCreate(player_task, "radio_player", 12288, arg, 5, &s_player_task) != pdPASS) {
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
        .url_count = 0,
        .cb = cb,
        .user_ctx = user_ctx,
    };
    if (!radio_player_audio_available()) {
        emit_status(&arg, RADIO_PLAYER_FAILED, radio_player_audio_unavailable_reason());
        return false;
    }
    bool ok = board_audio_play_beep_440hz_500ms();
    emit_status(&arg, ok ? RADIO_PLAYER_PLAYING : RADIO_PLAYER_FAILED, ok ? "BEEP_OK" : "BEEP_FAILED");
    return ok;
}
