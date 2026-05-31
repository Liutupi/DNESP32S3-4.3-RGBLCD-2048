/**
 ****************************************************************************************************
 * @file        remote_ir.h
 * @brief       Universal IR remote driver using ESP32-S3 RMT (TX/RX)
 *              - TX: GPIO8 (REMOTE_OUT)
 *              - RX: GPIO2 (REMOTE_IN)
 *              - Raw symbol learning & replay (protocol agnostic)
 ****************************************************************************************************
 */

#ifndef __REMOTE_IR_H
#define __REMOTE_IR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "driver/rmt_tx.h"
#include "driver/rmt_rx.h"

/* Pin definitions from DNESP32S3 PINOUT */
#define IR_TX_GPIO_NUM          GPIO_NUM_8
#define IR_RX_GPIO_NUM          GPIO_NUM_2
#define IR_RESOLUTION_HZ        1000000     /* 1 MHz => 1 tick = 1 us */
#define IR_MAX_SYMBOLS          128
#define IR_MEM_BLOCK_SYMBOLS    128

typedef struct {
    const rmt_symbol_word_t *symbols;
    int num_symbols;
} ir_raw_payload_t;

bool remote_ir_init(void);
void remote_ir_deinit(void);

/**
 * @brief Learn an IR signal (blocking with timeout)
 * @param symbols      Output buffer for RMT symbols
 * @param num_symbols  Output: number of symbols received
 * @param timeout_ms   Maximum wait time in milliseconds
 * @return true if signal received, false on timeout
 */
bool remote_ir_learn(rmt_symbol_word_t *symbols, size_t *num_symbols, uint32_t timeout_ms);

/**
 * @brief Send raw IR symbols (blocking)
 * @param symbols      Array of RMT symbols to transmit
 * @param num_symbols  Number of symbols
 * @return true if transmission started successfully
 */
bool remote_ir_send(const rmt_symbol_word_t *symbols, size_t num_symbols);

#endif /* __REMOTE_IR_H */
