/**
 ****************************************************************************************************
 * @file        remote_ir.c
 * @brief       RMT-based raw IR transceiver for DNESP32S3
 ****************************************************************************************************
 */

#include "remote_ir.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include <string.h>

static const char *TAG = "remote_ir";

static rmt_channel_handle_t s_rx_channel = NULL;
static rmt_channel_handle_t s_tx_channel = NULL;
static rmt_encoder_handle_t s_raw_encoder = NULL;
static QueueHandle_t s_rx_queue = NULL;

/* Raw encoder internal structure */
typedef struct {
    rmt_encoder_t base;
    rmt_encoder_t *copy_encoder;
    int state;
} rmt_ir_raw_encoder_t;

static size_t rmt_encode_ir_raw(rmt_encoder_t *encoder, rmt_channel_handle_t channel,
                                const void *primary_data, size_t data_size,
                                rmt_encode_state_t *ret_state)
{
    rmt_ir_raw_encoder_t *raw_encoder = __containerof(encoder, rmt_ir_raw_encoder_t, base);
    rmt_encode_state_t session_state = RMT_ENCODING_RESET;
    rmt_encode_state_t state = RMT_ENCODING_RESET;
    size_t encoded_symbols = 0;

    const ir_raw_payload_t *payload = (const ir_raw_payload_t *)primary_data;
    const rmt_symbol_word_t *symbols = payload->symbols;
    int num_symbols = payload->num_symbols;

    while (raw_encoder->state < num_symbols) {
        encoded_symbols += raw_encoder->copy_encoder->encode(
            raw_encoder->copy_encoder, channel,
            &symbols[raw_encoder->state], sizeof(rmt_symbol_word_t),
            &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) {
            raw_encoder->state++;
        }
        if (session_state & RMT_ENCODING_MEM_FULL) {
            state |= RMT_ENCODING_MEM_FULL;
            goto out;
        }
    }

    state |= RMT_ENCODING_COMPLETE;
    raw_encoder->state = 0; /* reset for next transmit */

out:
    *ret_state = state;
    return encoded_symbols;
}

static esp_err_t rmt_del_ir_raw_encoder(rmt_encoder_t *encoder)
{
    rmt_ir_raw_encoder_t *raw_encoder = __containerof(encoder, rmt_ir_raw_encoder_t, base);
    rmt_del_encoder(raw_encoder->copy_encoder);
    free(raw_encoder);
    return ESP_OK;
}

static esp_err_t rmt_ir_raw_encoder_reset(rmt_encoder_t *encoder)
{
    rmt_ir_raw_encoder_t *raw_encoder = __containerof(encoder, rmt_ir_raw_encoder_t, base);
    rmt_encoder_reset(raw_encoder->copy_encoder);
    raw_encoder->state = 0;
    return ESP_OK;
}

static esp_err_t rmt_new_ir_raw_encoder(rmt_encoder_handle_t *ret_encoder)
{
    rmt_ir_raw_encoder_t *raw_encoder = calloc(1, sizeof(rmt_ir_raw_encoder_t));
    if (!raw_encoder) {
        return ESP_ERR_NO_MEM;
    }
    raw_encoder->base.encode = rmt_encode_ir_raw;
    raw_encoder->base.del = rmt_del_ir_raw_encoder;
    raw_encoder->base.reset = rmt_ir_raw_encoder_reset;

    rmt_copy_encoder_config_t copy_encoder_config = {};
    esp_err_t ret = rmt_new_copy_encoder(&copy_encoder_config, &raw_encoder->copy_encoder);
    if (ret != ESP_OK) {
        free(raw_encoder);
        return ret;
    }

    *ret_encoder = &raw_encoder->base;
    return ESP_OK;
}

void remote_ir_deinit(void)
{
    if (s_raw_encoder) {
        rmt_del_encoder(s_raw_encoder);
        s_raw_encoder = NULL;
    }
    if (s_tx_channel) {
        rmt_disable(s_tx_channel);
        rmt_del_channel(s_tx_channel);
        s_tx_channel = NULL;
    }
    if (s_rx_channel) {
        rmt_disable(s_rx_channel);
        rmt_del_channel(s_rx_channel);
        s_rx_channel = NULL;
    }
    if (s_rx_queue) {
        vQueueDelete(s_rx_queue);
        s_rx_queue = NULL;
    }
}

static bool rx_done_callback(rmt_channel_handle_t channel, const rmt_rx_done_event_data_t *edata, void *user_data)
{
    BaseType_t high_task_wakeup = pdFALSE;
    QueueHandle_t queue = (QueueHandle_t)user_data;
    xQueueSendFromISR(queue, edata, &high_task_wakeup);
    return high_task_wakeup == pdTRUE;
}

bool remote_ir_init(void)
{
    if (s_tx_channel) return true; /* already initialized */

    esp_err_t ret;

    /* Create RX channel */
    rmt_rx_channel_config_t rx_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = IR_RESOLUTION_HZ,
        .mem_block_symbols = IR_MEM_BLOCK_SYMBOLS,
        .gpio_num = IR_RX_GPIO_NUM,
        .flags.with_dma = 1,
    };
    ret = rmt_new_rx_channel(&rx_cfg, &s_rx_channel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "RX channel create failed: %s", esp_err_to_name(ret));
        goto fail;
    }

    s_rx_queue = xQueueCreate(2, sizeof(rmt_rx_done_event_data_t));
    if (!s_rx_queue) {
        ESP_LOGE(TAG, "RX queue create failed");
        goto fail;
    }

    rmt_rx_event_callbacks_t rx_cbs = {
        .on_recv_done = rx_done_callback,
    };
    ret = rmt_rx_register_event_callbacks(s_rx_channel, &rx_cbs, s_rx_queue);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "RX callback register failed: %s", esp_err_to_name(ret));
        goto fail;
    }
    ret = rmt_enable(s_rx_channel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "RX enable failed: %s", esp_err_to_name(ret));
        goto fail;
    }

    /* Create TX channel — reset GPIO first to disconnect LCD IO MUX */
    gpio_reset_pin(IR_TX_GPIO_NUM);
    gpio_set_direction(IR_TX_GPIO_NUM, GPIO_MODE_OUTPUT);

    rmt_tx_channel_config_t tx_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = IR_RESOLUTION_HZ,
        .mem_block_symbols = IR_MEM_BLOCK_SYMBOLS,
        .trans_queue_depth = 4,
        .gpio_num = IR_TX_GPIO_NUM,
        .flags.with_dma = 1,
    };
    ret = rmt_new_tx_channel(&tx_cfg, &s_tx_channel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TX channel create failed: %s", esp_err_to_name(ret));
        goto fail;
    }

    /* 38 kHz carrier for typical IR modulation */
    rmt_carrier_config_t carrier_cfg = {
        .frequency_hz = 38000,
        .duty_cycle = 0.33f,
    };
    ret = rmt_apply_carrier(s_tx_channel, &carrier_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Carrier apply failed: %s", esp_err_to_name(ret));
        goto fail;
    }
    ret = rmt_enable(s_tx_channel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TX enable failed: %s", esp_err_to_name(ret));
        goto fail;
    }

    /* Create raw encoder */
    ret = rmt_new_ir_raw_encoder(&s_raw_encoder);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Raw encoder create failed: %s", esp_err_to_name(ret));
        goto fail;
    }

    ESP_LOGI(TAG, "IR remote initialized (TX=GPIO%d, RX=GPIO%d)", IR_TX_GPIO_NUM, IR_RX_GPIO_NUM);
    return true;

fail:
    remote_ir_deinit();
    return false;
}

bool remote_ir_learn(rmt_symbol_word_t *symbols, size_t *num_symbols, uint32_t timeout_ms)
{
    if (!s_rx_channel || !symbols || !num_symbols) return false;

    rmt_symbol_word_t rx_symbols[IR_MAX_SYMBOLS];
    rmt_rx_done_event_data_t rx_data;

    /* Start receive */
    rmt_receive_config_t recv_config = {
        .signal_range_min_ns = 1000,      /* 1 us: ignore very short glitches */
        .signal_range_max_ns = 20000000,  /* 20 ms: long enough for any frame + silence */
    };
    ESP_ERROR_CHECK(rmt_receive(s_rx_channel, rx_symbols, sizeof(rx_symbols), &recv_config));

    /* Wait for RX done */
    if (xQueueReceive(s_rx_queue, &rx_data, pdMS_TO_TICKS(timeout_ms)) == pdPASS) {
        size_t count = rx_data.num_symbols;
        if (count > IR_MAX_SYMBOLS) count = IR_MAX_SYMBOLS;
        memcpy(symbols, rx_data.received_symbols, count * sizeof(rmt_symbol_word_t));
        *num_symbols = count;
        ESP_LOGI(TAG, "Learned %d symbols", (int)count);
        if (count > 0) {
            ESP_LOGI(TAG, "Sym[0]: L%d/%dus L%d/%dus",
                     symbols[0].level0, symbols[0].duration0,
                     symbols[0].level1, symbols[0].duration1);
            if (count > 1) {
                ESP_LOGI(TAG, "Sym[1]: L%d/%dus L%d/%dus",
                         symbols[1].level0, symbols[1].duration0,
                         symbols[1].level1, symbols[1].duration1);
            }
        }
        return true;
    }

    *num_symbols = 0;
    return false;
}

bool remote_ir_send(const rmt_symbol_word_t *symbols, size_t num_symbols)
{
    if (!s_tx_channel || !s_raw_encoder || !symbols || num_symbols == 0) return false;

    /* IR receiver output is inverted: active-low when carrier present.
     * We must invert levels before TX so the emitted waveform matches original. */
    rmt_symbol_word_t tx_symbols[IR_MAX_SYMBOLS];
    for (size_t i = 0; i < num_symbols; i++) {
        tx_symbols[i] = symbols[i];
        tx_symbols[i].level0 = !symbols[i].level0;
        tx_symbols[i].level1 = !symbols[i].level1;
    }

    ir_raw_payload_t payload = {
        .symbols = tx_symbols,
        .num_symbols = (int)num_symbols,
    };

    rmt_transmit_config_t tx_config = {
        .loop_count = 0,
    };

    /* Reset encoder state before each transmit */
    rmt_encoder_reset(s_raw_encoder);

    ESP_LOGI(TAG, "Sending %d symbols", (int)num_symbols);
    if (num_symbols > 0) {
        ESP_LOGI(TAG, "TX[0]: L%d/%dus L%d/%dus",
                 tx_symbols[0].level0, tx_symbols[0].duration0,
                 tx_symbols[0].level1, tx_symbols[0].duration1);
    }
    esp_err_t ret = rmt_transmit(s_tx_channel, s_raw_encoder, &payload, sizeof(payload), &tx_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "RMT transmit failed: %s", esp_err_to_name(ret));
        return false;
    }

    /* Wait for transmission to complete (blocking for simplicity) */
    ret = rmt_tx_wait_all_done(s_tx_channel, 500);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "RMT transmit timeout");
        return false;
    }

    return true;
}
