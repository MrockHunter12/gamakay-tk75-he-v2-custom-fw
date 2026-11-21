#ifndef SPI3_RF_H
#define SPI3_RF_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SPI3_RF_BUF_SIZE         68U
#define SPI3_RF_MAX_PAYLOAD      64U

/* Stock SPI3 RF operating modes (cmd 0x93 & status 0x10 rx[5]) */
typedef enum {
  SPI3_RF_MODE_BT1 = 0U,
  SPI3_RF_MODE_BT2 = 1U,
  SPI3_RF_MODE_BT3 = 2U,
  SPI3_RF_MODE_24G = 5U,
  SPI3_RF_MODE_USB = 6U,
} spi3_rf_mode_t;

/* Stock SPI3 RF connection states (status 0x10 rx[4]) */
typedef enum {
  SPI3_RF_STATE_DISCONNECTED = 0U,
  SPI3_RF_STATE_PAIRING      = 1U,
  SPI3_RF_STATE_RECONNECTING = 2U,
  SPI3_RF_STATE_CONNECTED    = 3U,
  SPI3_RF_STATE_SLEEP        = 4U,
} spi3_rf_link_state_t;

/* TX command opcodes */
typedef enum {
  SPI3_RF_CMD_POLL      = 0x09U,
  SPI3_RF_CMD_HID       = 0x81U,
  SPI3_RF_CMD_BATTERY   = 0x90U,
  SPI3_RF_CMD_VENDOR    = 0x91U,
  SPI3_RF_CMD_SLEEP     = 0x98U,
  SPI3_RF_CMD_STATUS    = 0x92U,
  SPI3_RF_CMD_SET_MODE  = 0x93U,
  SPI3_RF_CMD_PAIR_NAME = 0x94U,
  SPI3_RF_CMD_POLL_RATE = 0x95U,
  SPI3_RF_CMD_LED_ACK   = 0x96U,
} spi3_rf_tx_cmd_t;

/* 0x81 HID sub-packet types (matches stock TK75 HE V2 RF MCU protocol) */
typedef enum {
  SPI3_RF_HID_MOUSE    = 0U, /* 7B mouse report */
  SPI3_RF_HID_BOOT_KBD = 1U, /* 7B: modifiers + 6 keycodes */
  SPI3_RF_HID_NKRO     = 2U, /* 15B usage bitmap 0x00..0x77 */
  SPI3_RF_HID_CONSUMER = 3U, /* 2B u16 LE consumer usage */
  SPI3_RF_HID_SYSTEM   = 4U, /* 1B system control usage */
  SPI3_RF_HID_STATUS   = 5U, /* 31B status/battery */
  SPI3_RF_HID_VIA      = 6U, /* 64B vendor/VIA packet */
} spi3_rf_hid_type_t;

/* RX command opcodes */
typedef enum {
  SPI3_RF_RX_VENDOR   = 0x00U,
  SPI3_RF_RX_STATUS   = 0x10U,
  SPI3_RF_RX_EXT_ACK  = 0x11U,
  SPI3_RF_RX_PAIR_ACK = 0x12U,
  SPI3_RF_RX_BUSY1    = 0x66U,
  SPI3_RF_RX_BUSY2    = 0xFFU,
} spi3_rf_rx_cmd_t;

typedef enum {
  SPI3_RF_RX_NONE = 0,
  SPI3_RF_RX_OK,
  SPI3_RF_RX_BUSY,
  SPI3_RF_RX_BAD_CRC,
} spi3_rf_rx_status_t;

typedef struct {
  uint8_t cmd;
  uint8_t payload_len;
  uint8_t payload[SPI3_RF_MAX_PAYLOAD];
} spi3_rf_rx_frame_t;

typedef struct {
  uint8_t  target_mode;
  uint8_t  reported_mode;
  uint8_t  link_state;
  uint8_t  host_leds;
  uint8_t  os_flag;
  bool     link_up;
  bool     paired;
  bool     low_batt_flag;
  bool     mode_sync_needed;
  bool     name_sync_needed;
  bool     rate_sync_needed;
  bool     batt_sync_needed;
  bool     pair_pending;
  bool     sleeping;
  bool     ack_pending;
  uint8_t  poll_rate_idx;
  uint8_t  battery_pct;
  uint8_t  last_sent_batt;
  uint8_t  retry_left;
  uint16_t last_cmd_len;
  uint8_t  last_cmd_buf[SPI3_RF_BUF_SIZE];
  uint32_t last_tx_ms;
  uint32_t last_rx_ms;
  uint32_t last_poll_ms;
} spi3_rf_state_t;

typedef enum {
  SPI3_TX_NORMAL = 0,
  SPI3_TX_POLL   = 1,
  SPI3_TX_RETRY  = 2,
} spi3_tx_kind_t;

typedef void (*spi3_rf_via_rx_cb_t)(uint8_t const *pkt64, uint8_t len, void *ctx);

/** Initialize RF state structure with initial target mode and polling rate index. */
void spi3_rf_state_init(spi3_rf_state_t *st, uint8_t target_mode, uint8_t poll_rate_idx);

/**
 * Encode one TX frame according to verified stock SPI3 checksum & 4-byte DMA padding:
 * - out[0]=cmd, out[1]=payload_len, out[2..1+payload_len]=payload
 * - out[chk_idx]=sum8(out[2..1+payload_len]), chk_idx = (cmd==0x09) ? 0x42 : (payload_len+2)
 * - returns 4-byte aligned total DMA length ((chk_idx + 4) & ~3), or 0 on error.
 */
uint16_t spi3_rf_encode_tx(uint8_t cmd, uint8_t const *payload, uint8_t payload_len,
                           uint8_t *out_buf, uint16_t out_cap);

/** Decode & verify one 68-byte RX DMA capture shifted during a 0x09 poll. */
spi3_rf_rx_status_t spi3_rf_decode_rx(uint8_t const *rx_buf, uint16_t rx_len,
                                      spi3_rf_rx_frame_t *out_frame);

/** High-level TX builders (return aligned wire length or 0 on capacity error). */
uint16_t spi3_rf_build_poll(uint8_t *out_buf, uint16_t out_cap);
uint16_t spi3_rf_build_boot_kbd(uint8_t modifiers, uint8_t const keys[6], uint8_t *out_buf, uint16_t out_cap);
uint16_t spi3_rf_build_nkro_kbd(uint8_t const nkro15[15], uint8_t *out_buf, uint16_t out_cap);
uint16_t spi3_rf_build_consumer(uint16_t usage, uint8_t *out_buf, uint16_t out_cap);
uint16_t spi3_rf_build_system(uint8_t usage, uint8_t *out_buf, uint16_t out_cap);
uint16_t spi3_rf_build_via(uint8_t const *pkt, uint8_t pkt_len, uint8_t *out_buf, uint16_t out_cap);
uint16_t spi3_rf_build_set_mode(uint8_t rf_mode, uint8_t *out_buf, uint16_t out_cap);
uint16_t spi3_rf_build_pair(uint8_t subcmd, uint8_t *out_buf, uint16_t out_cap);
uint16_t spi3_rf_build_ble_name(char const *name, uint8_t *out_buf, uint16_t out_cap);
uint16_t spi3_rf_build_poll_rate(uint8_t rate_idx, uint8_t *out_buf, uint16_t out_cap);
uint16_t spi3_rf_build_battery(uint8_t battery_pct, uint8_t *out_buf, uint16_t out_cap);
uint16_t spi3_rf_build_sleep(uint8_t *out_buf, uint16_t out_cap);

/** Update RF protocol state from a validated RX frame and invoke via_cb for 0x00 RX frames. */
void spi3_rf_apply_rx(spi3_rf_state_t *st, spi3_rf_rx_frame_t const *frame, uint32_t now_ms,
                      spi3_rf_via_rx_cb_t via_cb, void *via_ctx);

/** Record an outgoing SPI3 frame for busy-retry tracking (ignores polls and retries). */
void spi3_rf_note_tx(spi3_rf_state_t *st, uint8_t const *buf, uint16_t len, spi3_tx_kind_t kind);

/** Handle poll RX status and return true if last_cmd_buf should be re-transmitted on BUSY. */
bool spi3_rf_check_retry(spi3_rf_state_t *st, spi3_rf_rx_status_t rx_status);

#ifdef AT32F405RCT7
/** Configure SPI3 (PB3/4/5 AF6, PA15 CS, PD2 IRQ, PB12 ctrl) + DMA1 Ch2/Ch3 and initial state. */
void spi3_rf_hw_init(uint8_t initial_mode, uint8_t poll_rate_idx);

/** Set desired wireless/USB target mode (0/1/2=BT1..3, 5=2.4G, 6=USB) and queue sync. */
void spi3_rf_set_target_mode(uint8_t rf_mode);

/** Request BLE / 2.4G pairing (`0x94` subcmd 3) or channel reconnect. */
void spi3_rf_request_pair(void);

/** Update wireless polling rate index (`0..3`, sent via `0x95`). */
void spi3_rf_set_poll_rate(uint8_t rate_idx);

/** Send sleep (`0x90`) command and float PB12 for low-power battery idle. */
void spi3_rf_enter_sleep(uint8_t battery_pct);

/** Re-assert PB12 + toggle PA15 wake pulse and re-sync active mode after idle sleep. */
void spi3_rf_wake(void);

/** Non-blocking queued HID & VIA transmitters over SPI3 RF link. */
bool spi3_rf_send_boot_kbd(uint8_t modifiers, uint8_t const keys[6]);
bool spi3_rf_send_nkro_kbd(uint8_t const nkro15[15]);
bool spi3_rf_send_consumer(uint16_t usage);
bool spi3_rf_send_system(uint8_t usage);
bool spi3_rf_send_via(uint8_t const *pkt32);

/** Service DMA completion, PD2 IRQ polling, auto-resync retries, and VIA RX callback. */
void spi3_rf_service(uint32_t now_ms, uint8_t battery_pct, spi3_rf_via_rx_cb_t via_cb, void *via_ctx);

/** Return read-only pointer to live RF connection/LED state. */
spi3_rf_state_t const *spi3_rf_get_state(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* SPI3_RF_H */
