#ifndef UART7_TFT_H
#define UART7_TFT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UART7_TX_SYNC       0x55U
#define UART7_RX_SYNC       0x44U /* 'D' */
#define UART7_MAX_PAYLOAD   96U
#define UART7_MAX_WIRE      (UART7_MAX_PAYLOAD + 5U)

typedef struct {
  uint8_t        cmd;
  uint16_t       payload_len;
  uint8_t const *payload;
} uart7_tx_frame_t;

typedef struct {
  uint8_t cmd;
  uint8_t payload_len;
  uint8_t payload[UART7_MAX_PAYLOAD];
} uart7_rx_frame_t;

typedef struct {
  uint8_t  state;
  uint8_t  expected_len;
  uint16_t idx;
  uint8_t  sum8;
  uint8_t  buf[UART7_MAX_WIRE];
} uart7_rx_parser_t;

/**
 * Encode one STM32 -> TFT frame matching stock UART7 framing:
 * wire[0] = 0x55, wire[1..2] = le16(payload_len + 2), wire[3] = cmd,
 * wire[4..3+payload_len] = payload, wire[4+payload_len] = sum8(wire[1..3+payload_len]).
 * Returns total encoded bytes or 0 if buffer capacity exceeded.
 */
uint16_t uart7_tft_encode_frame(uart7_tx_frame_t const *frame, uint8_t *out_wire, uint16_t out_cap);

/** Reset RX stream parser state. */
void uart7_tft_rx_init(uart7_rx_parser_t *parser);

/** Feed one received byte into `'D'` RX parser; returns true when a checksummed frame finishes. */
bool uart7_tft_rx_feed(uart7_rx_parser_t *parser, uint8_t byte, uart7_rx_frame_t *out_frame);

#ifdef AT32F405RCT7
/** Configure PF7 (TX) / PF6 (RX) AF9 @ 921600 8N1, DMA1 Ch4/Ch5, and enable PA12 TFT power. */
void uart7_tft_hw_init(void);

/** Power-gate UART7 TX/RX pins LOW when TFT rail is off (prevents ESD back-powering U24),
 *  or cleanly re-enable UART7 + reset DMA RX ring when TFT rail powers back on. */
void uart7_tft_set_bus_power(bool enabled);

/** Returns true if non-blocking TX DMA is currently in flight. */
bool uart7_tft_tx_busy(void);

/** Encode and launch async DMA transmission of one TFT frame. */
bool uart7_tft_send_frame(uart7_tx_frame_t const *frame);

/** Poll RX circular DMA ring and yield one validated frame if ready. */
bool uart7_tft_poll_rx(uart7_rx_frame_t *out_frame);
#endif

#ifdef __cplusplus
}
#endif

#endif /* UART7_TFT_H */
