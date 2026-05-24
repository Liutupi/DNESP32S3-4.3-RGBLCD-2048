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
#define RADIO_HTTP_TIMEOUT_MS 2500
#define RADIO_HTTP_READ_CHUNK 2048
#define RADIO_RAW_BUFFER_SIZE 16384
#define RADIO_PCM_BUFFER_SIZE 8192
#define RADIO_URL_MAX 256
#define RADIO_MAX_URLS 4
#define RADIO_USER_AGENT "Mozilla/5.0 ESP32 Radio"
#define RADIO_REASON_MAX 128
#define RADIO_DIAG_MAX_DECODE_FAILURES 12
#define RADIO_PLAY_MAX_DECODE_FAILURES 12
#define RADIO_PLAY_PREROLL_FRAMES 4
#define RADIO_PLAY_GOOD_FRAMES_BEFORE_ANNOUNCE 8
#define RADIO_STOP_WAIT_MS 5000
#define RADIO_STOP_POLL_MS 20

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

static void first_bytes_to_hex(const uint8_t *bytes, int len, char *out, size_t out_size)
{
    size_t used = 0;
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';
    for (int i = 0; bytes && i < len && used + 4 < out_size; ++i) {
        int written = snprintf(out + used, out_size - used, "%s%02X",
                               i ? " " : "", bytes[i]);
        if (written <= 0) {
            break;
        }
        used += (size_t)written;
    }
}

static int mp3_frame_length_from_header(const uint8_t *h)
{
    if (!h || h[0] != 0xFF || (h[1] & 0xE0) != 0xE0) {
        return -1;
    }

    int version = (h[1] >> 3) & 0x03;
    int layer = (h[1] >> 1) & 0x03;
    int bitrate_index = (h[2] >> 4) & 0x0F;
    int sample_index = (h[2] >> 2) & 0x03;
    int padding = (h[2] >> 1) & 0x01;
    if (version == 1 || layer == 0 || bitrate_index == 0 || bitrate_index == 15 ||
        sample_index == 3) {
        return -1;
    }

    static const int sample_rates[3] = {44100, 48000, 32000};
    int sample_rate = sample_rates[sample_index];
    if (version == 2) {
        sample_rate /= 2;
    } else if (version == 0) {
        sample_rate /= 4;
    }

    static const int br_mpeg1_l1[16]  = {0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448, 0};
    static const int br_mpeg1_l2[16]  = {0, 32, 48, 56,  64,  80,  96, 112, 128, 160, 192, 224, 256, 320, 384, 0};
    static const int br_mpeg1_l3[16]  = {0, 32, 40, 48,  56,  64,  80,  96, 112, 128, 160, 192, 224, 256, 320, 0};
    static const int br_mpeg2_l1[16]  = {0, 32, 48, 56,  64,  80,  96, 112, 128, 144, 160, 176, 192, 224, 256, 0};
    static const int br_mpeg2_l23[16] = {0,  8, 16, 24,  32,  40,  48,  56,  64,  80,  96, 112, 128, 144, 160, 0};

    int bitrate_kbps = 0;
    if (version == 3) {
        if (layer == 3) {
            bitrate_kbps = br_mpeg1_l1[bitrate_index];
        } else if (layer == 2) {
            bitrate_kbps = br_mpeg1_l2[bitrate_index];
        } else {
            bitrate_kbps = br_mpeg1_l3[bitrate_index];
        }
    } else {
        if (layer == 3) {
            bitrate_kbps = br_mpeg2_l1[bitrate_index];
        } else {
            bitrate_kbps = br_mpeg2_l23[bitrate_index];
        }
    }
    if (bitrate_kbps <= 0 || sample_rate <= 0) {
        return -1;
    }

    int bitrate = bitrate_kbps * 1000;
    if (layer == 3) {
        return ((12 * bitrate) / sample_rate + padding) * 4;
    }
    if (layer == 1 && version != 3) {
        return (72 * bitrate) / sample_rate + padding;
    }
    return (144 * bitrate) / sample_rate + padding;
}

static bool mp3_header_valid(const uint8_t *bytes, int len)
{
    return len >= 4 && mp3_frame_length_from_header(bytes) > 0;
}

static int mp3_decodable_offset_from(const uint8_t *bytes, int len, int start)
{
    if (!bytes || len < 2) {
        return -1;
    }
    int scan_start = start > 0 ? start : 0;
    if (start <= 0 && len >= 10 && bytes[0] == 'I' && bytes[1] == 'D' && bytes[2] == '3') {
        int tag_size = ((bytes[6] & 0x7F) << 21) |
                       ((bytes[7] & 0x7F) << 14) |
                       ((bytes[8] & 0x7F) << 7) |
                       (bytes[9] & 0x7F);
        int total = 10 + tag_size + ((bytes[5] & 0x10) ? 10 : 0);
        if (total > 0 && total < len) {
            scan_start = total;
        }
    }
    for (int i = scan_start; i + 3 < len; ++i) {
        if (!mp3_header_valid(bytes + i, len - i)) {
            continue;
        }
        int frame_len = mp3_frame_length_from_header(bytes + i);
        if (frame_len > 0 && i + frame_len + 3 < len &&
            mp3_header_valid(bytes + i + frame_len, len - i - frame_len)) {
            return i;
        }
    }
    return -1;
}

static int mp3_decodable_offset(const uint8_t *bytes, int len)
{
    return mp3_decodable_offset_from(bytes, len, 0);
}

static void discard_raw_prefix(uint8_t *raw, int *raw_len, int offset)
{
    if (!raw || !raw_len || offset <= 0 || offset >= *raw_len) {
        return;
    }
    memmove(raw, raw + offset, *raw_len - offset);
    *raw_len -= offset;
}

static void diag_set_reason(radio_diag_result_t *result, const char *fmt, ...)
{
    if (!result || !fmt) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(result->reason, sizeof(result->reason), fmt, ap);
    va_end(ap);
}

static void diag_emit(const radio_diag_result_t *result, radio_player_diag_cb_t cb, void *user_ctx)
{
    if (cb) {
        cb(result, user_ctx);
    }
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

static uint32_t pcm_average_abs(const int16_t *samples, size_t sample_count, uint32_t *peak_out)
{
    uint64_t total = 0;
    uint32_t peak = 0;
    for (size_t i = 0; samples && i < sample_count; ++i) {
        int32_t value = samples[i];
        uint32_t mag = value < 0 ? (uint32_t)(-value) : (uint32_t)value;
        if (mag > peak) {
            peak = mag;
        }
        total += (uint32_t)mag;
    }
    if (peak_out) {
        *peak_out = peak;
    }
    return sample_count ? (uint32_t)(total / sample_count) : 0;
}

static void log_decoded_frame(uint32_t frame_index, uint32_t consumed,
                              uint32_t decoded_size, const esp_audio_dec_info_t *info,
                              const uint8_t *pcm)
{
    if (frame_index > 5 && frame_index % 200 != 0) {
        return;
    }
    size_t samples = decoded_size / sizeof(int16_t);
    uint32_t peak = 0;
    uint32_t avg_abs = pcm_average_abs((const int16_t *)pcm, samples, &peak);
    ESP_LOGI(TAG,
             "PCM_FRAME frame=%" PRIu32 " consumed=%" PRIu32 " decoded=%" PRIu32
             " rate=%u bits=%u ch=%u bitrate=%u avg_abs=%" PRIu32 " peak=%u",
             frame_index, consumed, decoded_size,
             info ? (unsigned)info->sample_rate : 0,
             info ? (unsigned)info->bits_per_sample : 0,
             info ? (unsigned)info->channel : 0,
             info ? (unsigned)info->bitrate : 0,
             avg_abs, peak);
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
    if (info && info->bits_per_sample > 0 && info->bits_per_sample != 16) {
        set_last_error(arg, "unsupported_bits %u", (unsigned)info->bits_per_sample);
        ESP_LOGW(TAG, "RADIO_URL_FAILED reason=unsupported_bits bits=%u",
                 (unsigned)info->bits_per_sample);
        return false;
    }
    if (channels != 1 && channels != 2) {
        set_last_error(arg, "unsupported_channels %d", channels);
        ESP_LOGW(TAG, "RADIO_URL_FAILED reason=unsupported_channels channels=%d", channels);
        return false;
    }
    if ((decoded_size % sizeof(int16_t)) != 0 ||
        (channels == 2 && (decoded_size % (sizeof(int16_t) * 2)) != 0)) {
        set_last_error(arg, "bad_pcm_alignment size=%u ch=%d",
                       (unsigned)decoded_size, channels);
        ESP_LOGW(TAG, "RADIO_URL_FAILED reason=bad_pcm_alignment decoded=%u channels=%d",
                 (unsigned)decoded_size, channels);
        return false;
    }
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
    uint32_t decoded_frames = 0;
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

    while (raw_len < RADIO_HTTP_READ_CHUNK * 2) {
        int got = esp_http_client_read(client, (char *)raw + raw_len,
                                       RADIO_HTTP_READ_CHUNK);
        if (got <= 0) {
            break;
        }
        raw_len += got;
    }
    if (raw_len <= 0 || !first_bytes_look_like_mp3(raw, raw_len)) {
        set_last_error(arg, "first_bytes_not_mp3");
        ESP_LOGW(TAG, "RADIO_URL_FAILED reason=first_bytes_not_mp3 read=%d url=%s", raw_len, url);
        goto cleanup;
    }
    int sync_offset = mp3_decodable_offset(raw, raw_len);
    if (sync_offset < 0) {
        char head_hex[64];
        first_bytes_to_hex(raw, raw_len < 16 ? raw_len : 16, head_hex, sizeof(head_hex));
        set_last_error(arg, "no_valid_mp3_sync raw=%d head=%s", raw_len, head_hex);
        ESP_LOGW(TAG, "RADIO_URL_FAILED reason=no_valid_mp3_sync raw=%d head=%s url=%s",
                 raw_len, head_hex, url);
        goto cleanup;
    }
    if (sync_offset > 0) {
        ESP_LOGI(TAG, "MP3_SYNC_SKIP offset=%d raw_len=%d", sync_offset, raw_len);
        discard_raw_prefix(raw, &raw_len, sync_offset);
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
            if (dec_err == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH && out.needed_size > RADIO_PCM_BUFFER_SIZE) {
                set_last_error(arg, "pcm_buffer_too_small need=%u", (unsigned)out.needed_size);
                ESP_LOGW(TAG, "RADIO_URL_FAILED reason=pcm_buffer_too_small need=%u",
                         (unsigned)out.needed_size);
                fatal_url_failure = true;
                break;
            }
            if (dec_err == ESP_AUDIO_ERR_DATA_LACK) {
                break;
            }
            if (dec_err != ESP_AUDIO_ERR_OK || in.consumed == 0) {
                int next_sync = mp3_decodable_offset_from(raw, raw_len, 1);
                if (next_sync >= 0) {
                    discard_raw_prefix(raw, &raw_len, next_sync);
                    ESP_LOGI(TAG, "MP3_SYNC_SKIP_AFTER_ERR offset=%d raw_len=%d",
                             next_sync, raw_len);
                } else if (in.consumed > 0 && in.consumed < (uint32_t)raw_len) {
                    memmove(raw, raw + in.consumed, raw_len - in.consumed);
                    raw_len -= (int)in.consumed;
                } else if (raw_len > 1) {
                    discard_raw_prefix(raw, &raw_len, 1);
                }
                set_last_error(arg, "decode_step err=%d raw=%d sync=%d",
                               (int)dec_err, raw_len, mp3_decodable_offset(raw, raw_len));
                ESP_LOGW(TAG, "RADIO_URL_FAILED reason=decode_step err=%d consumed=%u raw=%d sync=%d",
                         (int)dec_err, (unsigned)in.consumed, raw_len,
                         mp3_decodable_offset(raw, raw_len));
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
            decoded_frames++;
            log_decoded_frame(decoded_frames, in.consumed, out.decoded_size, &info, pcm);
            if (decoded_frames <= RADIO_PLAY_PREROLL_FRAMES) {
                continue;
            }
            if (!write_pcm_frame(arg, pcm, out.decoded_size, &info)) {
                fatal_url_failure = true;
                break;
            }
            if (frames == 0) {
                ESP_LOGI(TAG, "I2S_WRITE_OK");
            }
            frames++;
            if (!announced && frames >= RADIO_PLAY_GOOD_FRAMES_BEFORE_ANNOUNCE) {
                emit_status(arg, RADIO_PLAYER_PLAYING, "RADIO_PLAYING");
                announced = true;
            }
        }

        if (!decoded_any) {
            decode_failures++;
            set_last_error(arg, "decode_wait err=%d raw=%d sync=%d tries=%d",
                           (int)dec_err, raw_len, mp3_decodable_offset(raw, raw_len),
                           decode_failures);
            ESP_LOGW(TAG, "RADIO_URL_FAILED reason=decode_wait err=%d raw=%d sync=%d failures=%d",
                     (int)dec_err, raw_len, mp3_decodable_offset(raw, raw_len),
                     decode_failures);
            if (raw_len >= RADIO_RAW_BUFFER_SIZE - RADIO_HTTP_READ_CHUNK) {
                raw_len = 0;
            }
            if (decode_failures >= RADIO_PLAY_MAX_DECODE_FAILURES) {
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    ok = frames >= RADIO_PLAY_GOOD_FRAMES_BEFORE_ANNOUNCE;

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

bool radio_player_diag_test_url(const char *url, bool write_i2s,
                                radio_player_diag_cb_t cb, void *user_ctx,
                                radio_diag_result_t *out_result)
{
    radio_diag_result_t result = {0};
    esp_http_client_handle_t client = NULL;
    void *decoder = NULL;
    uint8_t *raw = NULL;
    uint8_t *pcm = NULL;
    int raw_len = 0;
    esp_audio_err_t dec_err = ESP_AUDIO_ERR_FAIL;
    int decode_failures = 0;
    uint32_t decoded_frames = 0;
    uint32_t played_samples = 0;

    snprintf(result.reason, sizeof(result.reason), "%s", "starting");
    diag_emit(&result, cb, user_ctx);

    if (!url_is_supported_mp3_candidate(url)) {
        diag_set_reason(&result, "unsupported URL");
        ESP_LOGW(TAG, "RADIO_URL_FAILED reason=unsupported_stream_type url=%s", url ? url : "(null)");
        goto cleanup;
    }
    result.url_supported = true;
    diag_set_reason(&result, "URL supported");
    diag_emit(&result, cb, user_ctx);

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
        diag_set_reason(&result, "HTTP failed: client init");
        ESP_LOGW(TAG, "RADIO_URL_FAILED reason=http_open failed detail=http_client_init url=%s", url);
        goto cleanup;
    }
    esp_http_client_set_header(client, "User-Agent", RADIO_USER_AGENT);
    esp_http_client_set_header(client, "Accept", "audio/mpeg,audio/mp3,application/octet-stream,*/*");
    esp_http_client_set_header(client, "Icy-MetaData", "0");

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        diag_set_reason(&result, "HTTP failed: %s", esp_err_to_name(err));
        ESP_LOGW(TAG, "RADIO_URL_FAILED reason=http_open failed err=%s url=%s",
                 esp_err_to_name(err), url);
        goto cleanup;
    }
    result.http_open_ok = true;
    diag_set_reason(&result, "HTTP OK");
    ESP_LOGI(TAG, "MP3_HTTP_OPEN_OK");
    diag_emit(&result, cb, user_ctx);

    esp_http_client_fetch_headers(client);
    result.http_status = esp_http_client_get_status_code(client);
    char *content_type = NULL;
    esp_http_client_get_header(client, "Content-Type", &content_type);
    snprintf(result.content_type, sizeof(result.content_type), "%s",
             content_type ? content_type : "(none)");
    ESP_LOGI(TAG, "HTTP status=%d content-type=%s url=%s",
             result.http_status, result.content_type, url);
    if (result.http_status < 200 || result.http_status >= 400) {
        diag_set_reason(&result, "http_status %d", result.http_status);
        diag_emit(&result, cb, user_ctx);
        goto cleanup;
    }
    if (content_type_is_rejected(content_type)) {
        diag_set_reason(&result, "content_type %s", result.content_type);
        diag_emit(&result, cb, user_ctx);
        goto cleanup;
    }
    diag_set_reason(&result, "status=%d", result.http_status);
    diag_emit(&result, cb, user_ctx);

    raw = malloc(RADIO_RAW_BUFFER_SIZE);
    pcm = malloc(RADIO_PCM_BUFFER_SIZE);
    if (!raw || !pcm) {
        diag_set_reason(&result, "buffer_alloc");
        ESP_LOGW(TAG, "RADIO_URL_FAILED reason=buffer_alloc");
        goto cleanup;
    }

    while (raw_len < RADIO_HTTP_READ_CHUNK * 2) {
        int got = esp_http_client_read(client, (char *)raw + raw_len,
                                       RADIO_HTTP_READ_CHUNK);
        if (got <= 0) {
            break;
        }
        raw_len += got;
    }
    if (raw_len <= 0 || !first_bytes_look_like_mp3(raw, raw_len)) {
        diag_set_reason(&result, "first bytes not MP3");
        ESP_LOGW(TAG, "RADIO_URL_FAILED reason=first_bytes_not_mp3 read=%d url=%s", raw_len, url);
        diag_emit(&result, cb, user_ctx);
        goto cleanup;
    }
    int sync_offset = mp3_decodable_offset(raw, raw_len);
    if (sync_offset < 0) {
        char head_hex[64];
        first_bytes_to_hex(raw, raw_len < 16 ? raw_len : 16, head_hex, sizeof(head_hex));
        diag_set_reason(&result, "no valid MP3 sync raw=%d head=%s", raw_len, head_hex);
        ESP_LOGW(TAG, "RADIO_URL_FAILED reason=no_valid_mp3_sync raw=%d head=%s url=%s",
                 raw_len, head_hex, url);
        diag_emit(&result, cb, user_ctx);
        goto cleanup;
    }
    if (sync_offset > 0) {
        ESP_LOGI(TAG, "MP3_SYNC_SKIP offset=%d raw_len=%d", sync_offset, raw_len);
        discard_raw_prefix(raw, &raw_len, sync_offset);
    }
    result.first_bytes_mp3 = true;
    diag_set_reason(&result, "first bytes MP3 sync=%d raw=%d", sync_offset, raw_len);
    diag_emit(&result, cb, user_ctx);

    dec_err = esp_mp3_dec_open(NULL, 0, &decoder);
    if (dec_err != ESP_AUDIO_ERR_OK) {
        diag_set_reason(&result, "decoder failed");
        ESP_LOGW(TAG, "RADIO_URL_FAILED reason=decoder_open failed err=%d url=%s",
                 (int)dec_err, url);
        diag_emit(&result, cb, user_ctx);
        goto cleanup;
    }
    result.decoder_open_ok = true;
    diag_set_reason(&result, "decoder OK");
    ESP_LOGI(TAG, "MP3_DECODER_START_OK");
    diag_emit(&result, cb, user_ctx);

    while (decode_failures < RADIO_DIAG_MAX_DECODE_FAILURES) {
        if (raw_len < RADIO_RAW_BUFFER_SIZE - RADIO_HTTP_READ_CHUNK) {
            int got = esp_http_client_read(client, (char *)raw + raw_len, RADIO_HTTP_READ_CHUNK);
            if (got > 0) {
                raw_len += got;
            }
        }

        sync_offset = mp3_decodable_offset(raw, raw_len);
        if (sync_offset > 0) {
            ESP_LOGI(TAG, "MP3_SYNC_SKIP offset=%d raw_len=%d", sync_offset, raw_len);
            discard_raw_prefix(raw, &raw_len, sync_offset);
        }

        bool decoded_any = false;
        while (raw_len > 0) {
            esp_audio_dec_in_raw_t in = {.buffer = raw, .len = raw_len};
            esp_audio_dec_out_frame_t out = {.buffer = pcm, .len = RADIO_PCM_BUFFER_SIZE};
            esp_audio_dec_info_t info = {0};
            dec_err = esp_mp3_dec_decode(decoder, &in, &out, &info);
            if (dec_err == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH && out.needed_size > RADIO_PCM_BUFFER_SIZE) {
                diag_set_reason(&result, "pcm buffer too small need=%u", (unsigned)out.needed_size);
                ESP_LOGW(TAG, "RADIO_URL_FAILED reason=pcm_buffer_too_small need=%u",
                         (unsigned)out.needed_size);
                diag_emit(&result, cb, user_ctx);
                goto cleanup;
            }
            if (dec_err == ESP_AUDIO_ERR_DATA_LACK) {
                break;
            }
            if (dec_err != ESP_AUDIO_ERR_OK || in.consumed == 0) {
                int next_sync = mp3_decodable_offset_from(raw, raw_len, 1);
                if (next_sync >= 0) {
                    discard_raw_prefix(raw, &raw_len, next_sync);
                    ESP_LOGI(TAG, "MP3_SYNC_SKIP_AFTER_ERR offset=%d raw_len=%d",
                             next_sync, raw_len);
                } else if (in.consumed > 0 && in.consumed < (uint32_t)raw_len) {
                    memmove(raw, raw + in.consumed, raw_len - in.consumed);
                    raw_len -= (int)in.consumed;
                } else if (raw_len > 1) {
                    discard_raw_prefix(raw, &raw_len, 1);
                }
                ESP_LOGW(TAG, "RADIO_URL_FAILED reason=decode_step err=%d consumed=%u raw=%d sync=%d",
                         (int)dec_err, (unsigned)in.consumed, raw_len,
                         mp3_decodable_offset(raw, raw_len));
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

            decoded_frames++;
            log_decoded_frame(decoded_frames, in.consumed, out.decoded_size, &info, pcm);

            result.decode_ok = true;
            result.sample_rate = (int)info.sample_rate;
            result.channels = info.channel > 0 ? (int)info.channel : 2;
            result.decoded_size = (int)out.decoded_size;
            diag_set_reason(&result, "decode OK");
            diag_emit(&result, cb, user_ctx);

            if (result.sample_rate != BOARD_AUDIO_SAMPLE_RATE_HZ) {
                diag_set_reason(&result, "unsupported sample_rate=%d", result.sample_rate);
                ESP_LOGW(TAG, "RADIO_URL_FAILED reason=unsupported_sample_rate sample_rate=%d",
                         result.sample_rate);
                diag_emit(&result, cb, user_ctx);
                goto cleanup;
            }
            if (info.bits_per_sample > 0 && info.bits_per_sample != 16) {
                diag_set_reason(&result, "unsupported bits=%u", (unsigned)info.bits_per_sample);
                ESP_LOGW(TAG, "RADIO_URL_FAILED reason=unsupported_bits bits=%u",
                         (unsigned)info.bits_per_sample);
                diag_emit(&result, cb, user_ctx);
                goto cleanup;
            }
            if (result.channels != 1 && result.channels != 2) {
                diag_set_reason(&result, "unsupported channels=%d", result.channels);
                ESP_LOGW(TAG, "RADIO_URL_FAILED reason=unsupported_channels channels=%d",
                         result.channels);
                diag_emit(&result, cb, user_ctx);
                goto cleanup;
            }
            if ((out.decoded_size % sizeof(int16_t)) != 0 ||
                (result.channels == 2 && (out.decoded_size % (sizeof(int16_t) * 2)) != 0)) {
                diag_set_reason(&result, "bad PCM alignment size=%u ch=%d",
                                (unsigned)out.decoded_size, result.channels);
                ESP_LOGW(TAG, "RADIO_URL_FAILED reason=bad_pcm_alignment decoded=%u channels=%d",
                         (unsigned)out.decoded_size, result.channels);
                diag_emit(&result, cb, user_ctx);
                goto cleanup;
            }
            if (!write_i2s) {
                result.playable = true;
                goto cleanup;
            }

            player_task_arg_t temp = {0};
            if (!write_pcm_frame(&temp, pcm, out.decoded_size, &info)) {
                diag_set_reason(&result, "no_i2s_write");
                diag_emit(&result, cb, user_ctx);
                goto cleanup;
            }
            result.i2s_write_ok = true;
            result.playable = true;
            ESP_LOGI(TAG, "RADIO_TEST_PLAYING");
            diag_set_reason(&result, "I2S_WRITE_OK");
            diag_emit(&result, cb, user_ctx);

            uint32_t channels = result.channels > 0 ? (uint32_t)result.channels : 2;
            played_samples += (out.decoded_size / sizeof(int16_t)) / channels;
            if (played_samples >= BOARD_AUDIO_SAMPLE_RATE_HZ * 2) {
                goto cleanup;
            }
        }

        if (!decoded_any) {
            decode_failures++;
            diag_set_reason(&result, "decode wait err=%d raw=%d sync=%d tries=%d",
                            (int)dec_err, raw_len, mp3_decodable_offset(raw, raw_len),
                            decode_failures);
            diag_emit(&result, cb, user_ctx);
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    if (!result.decode_ok) {
        diag_set_reason(&result, "decode failed err=%d raw=%d sync=%d",
                        (int)dec_err, raw_len, mp3_decodable_offset(raw, raw_len));
        ESP_LOGW(TAG, "RADIO_URL_FAILED reason=decode_failed err=%d raw=%d sync=%d url=%s",
                 (int)dec_err, raw_len, mp3_decodable_offset(raw, raw_len), url);
        diag_emit(&result, cb, user_ctx);
    }

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
    if (out_result) {
        *out_result = result;
    }
    return write_i2s ? result.i2s_write_ok : result.playable;
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
    int tries = RADIO_STOP_WAIT_MS / RADIO_STOP_POLL_MS;
    for (int i = 0; s_running && i < tries; ++i) {
        vTaskDelay(pdMS_TO_TICKS(RADIO_STOP_POLL_MS));
    }
    if (s_running) {
        ESP_LOGW(TAG, "RADIO_STOP_TIMEOUT waited_ms=%d", RADIO_STOP_WAIT_MS);
    }
}

bool radio_player_play(const radio_player_request_t *request)
{
    if (!request || !request->url || !request->url[0]) {
        return false;
    }

    radio_player_stop();
    if (s_running) {
        ESP_LOGW(TAG, "RADIO_URL_FAILED reason=previous_player_still_running");
        return false;
    }
    s_stop_requested = false;

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
