#ifndef RADIO_PLAYER_H
#define RADIO_PLAYER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RADIO_PLAYER_IDLE = 0,
    RADIO_PLAYER_CONNECTING,
    RADIO_PLAYER_PLAYING,
    RADIO_PLAYER_FAILED,
    RADIO_PLAYER_STOPPED,
} radio_player_state_t;

typedef void (*radio_player_status_cb_t)(radio_player_state_t state, const char *message, void *user_ctx);

typedef struct {
    bool url_supported;
    bool http_open_ok;
    int http_status;
    char content_type[96];
    bool first_bytes_mp3;
    bool decoder_open_ok;
    bool decode_ok;
    int sample_rate;
    int channels;
    int decoded_size;
    bool i2s_write_ok;
    bool playable;
    char reason[128];
} radio_diag_result_t;

typedef void (*radio_player_diag_cb_t)(const radio_diag_result_t *result, void *user_ctx);

typedef struct {
    const char *name;
    const char *url;
    const char *fallback_urls[3];
    int fallback_count;
    const char *codec_hint;
    radio_player_status_cb_t status_cb;
    void *user_ctx;
} radio_player_request_t;

bool radio_player_audio_available(void);
const char *radio_player_audio_unavailable_reason(void);
bool radio_player_is_running(void);
void radio_player_stop(void);
bool radio_player_play(const radio_player_request_t *request);
bool radio_player_test_beep(radio_player_status_cb_t cb, void *user_ctx);
bool radio_player_diag_test_url(const char *url, bool write_i2s,
                                radio_player_diag_cb_t cb, void *user_ctx,
                                radio_diag_result_t *out_result);

#ifdef __cplusplus
}
#endif

#endif
