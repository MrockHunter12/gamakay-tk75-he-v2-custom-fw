#include "spi3_rf.h"
#include <string.h>

#ifdef AT32F405RCT7
#include "at32f402_405.h"
#include "board_tk75.h"
#include "tk75_pins.h"
#endif

#define SPI3_RF_DEFAULT_NAME "TK75 VIA"

static uint8_t calc_sum8(uint8_t const *buf, uint8_t count)
{
  uint8_t sum = 0U;
  for (uint8_t i = 0U; i < count; i++) {
    sum = (uint8_t)(sum + buf[i]);
  }
  return sum;
}

static uint16_t encode_hid_subframe(uint8_t subtype, uint8_t const *subpayload, uint8_t sublen,
                                    uint8_t *out_buf, uint16_t out_cap)
{
  uint8_t buf[SPI3_RF_MAX_PAYLOAD + 1U];
  if (sublen > SPI3_RF_MAX_PAYLOAD) {
    return 0U;
  }
  buf[0] = subtype;
  if (sublen > 0U && subpayload != NULL) {
    memcpy(&buf[1], subpayload, sublen);
  }
  return spi3_rf_encode_tx(SPI3_RF_CMD_HID, buf, (uint8_t)(1U + sublen), out_buf, out_cap);
}

void spi3_rf_state_init(spi3_rf_state_t *st, uint8_t target_mode, uint8_t poll_rate_idx)
{
  if (st == NULL) {
    return;
  }
  memset(st, 0, sizeof(*st));
  st->target_mode      = target_mode;
  st->reported_mode    = 0xFFU;
  st->poll_rate_idx    = (poll_rate_idx < 4U) ? poll_rate_idx : 0U;
  st->battery_pct      = 100U;
  st->last_sent_batt   = 0xFFU;
  st->mode_sync_needed = true;
  st->name_sync_needed = (target_mode <= SPI3_RF_MODE_BT3);
  st->rate_sync_needed = (target_mode != SPI3_RF_MODE_USB);
  st->batt_sync_needed = (target_mode != SPI3_RF_MODE_USB);
}

uint16_t spi3_rf_encode_tx(uint8_t cmd, uint8_t const *payload, uint8_t payload_len,
                           uint8_t *out_buf, uint16_t out_cap)
{
  if (out_buf == NULL) {
    return 0U;
  }
  if (cmd == SPI3_RF_CMD_POLL && payload_len != 0U) {
    return 0U;
  }
  if (payload_len > 65U) {
    return 0U;
  }

  uint16_t chk_idx   = (cmd == SPI3_RF_CMD_POLL) ? 0x42U : (uint16_t)(payload_len + 2U);
  uint16_t total_len = (uint16_t)((chk_idx + 4U) & ~3U);
  if (total_len > SPI3_RF_BUF_SIZE || total_len > out_cap) {
    return 0U;
  }

  memset(out_buf, 0, total_len);
  out_buf[0] = cmd;
  out_buf[1] = payload_len;
  if (payload_len > 0U && payload != NULL) {
    memcpy(&out_buf[2], payload, payload_len);
  }
  out_buf[chk_idx] = calc_sum8(&out_buf[2], payload_len);
  return total_len;
}

spi3_rf_rx_status_t spi3_rf_decode_rx(uint8_t const *rx_buf, uint16_t rx_len,
                                      spi3_rf_rx_frame_t *out_frame)
{
  if (rx_buf == NULL || out_frame == NULL || rx_len < 4U) {
    return SPI3_RF_RX_NONE;
  }

  uint8_t len = rx_buf[1];
  uint8_t cmd = rx_buf[2];

  if (len == 0xFFU && cmd == 0xFFU) {
    return SPI3_RF_RX_NONE;
  }
  if (cmd == SPI3_RF_RX_BUSY1 && (len == 0U || len > 65U)) {
    return SPI3_RF_RX_BUSY;
  }
  if (len == 0U || len > 65U || (uint16_t)(len + 2U) >= rx_len) {
    return SPI3_RF_RX_NONE;
  }

  uint8_t expected_sum = calc_sum8(&rx_buf[2], len);
  if (rx_buf[len + 2U] != expected_sum) {
    return SPI3_RF_RX_BAD_CRC;
  }
  if (cmd == SPI3_RF_RX_BUSY1 || cmd == SPI3_RF_RX_BUSY2) {
    return SPI3_RF_RX_BUSY;
  }

  memset(out_frame, 0, sizeof(*out_frame));
  out_frame->cmd         = cmd;
  out_frame->payload_len = (uint8_t)(len - 1U);
  if (out_frame->payload_len > 0U) {
    memcpy(out_frame->payload, &rx_buf[3], out_frame->payload_len);
  }
  return SPI3_RF_RX_OK;
}

uint16_t spi3_rf_build_poll(uint8_t *out_buf, uint16_t out_cap)
{
  return spi3_rf_encode_tx(SPI3_RF_CMD_POLL, NULL, 0U, out_buf, out_cap);
}

uint16_t spi3_rf_build_boot_kbd(uint8_t modifiers, uint8_t const keys[6], uint8_t *out_buf, uint16_t out_cap)
{
  uint8_t p[7];
  p[0] = modifiers;
  if (keys != NULL) {
    memcpy(&p[1], keys, 6U);
  } else {
    memset(&p[1], 0, 6U);
  }
  return encode_hid_subframe(SPI3_RF_HID_BOOT_KBD, p, (uint8_t)sizeof(p), out_buf, out_cap);
}

uint16_t spi3_rf_build_nkro_kbd(uint8_t const nkro15[15], uint8_t *out_buf, uint16_t out_cap)
{
  uint8_t p[15];
  if (nkro15 != NULL) {
    memcpy(p, nkro15, sizeof(p));
  } else {
    memset(p, 0, sizeof(p));
  }
  return encode_hid_subframe(SPI3_RF_HID_NKRO, p, (uint8_t)sizeof(p), out_buf, out_cap);
}

uint16_t spi3_rf_build_consumer(uint16_t usage, uint8_t *out_buf, uint16_t out_cap)
{
  uint8_t p[2] = {
    (uint8_t)(usage & 0xFFU),
    (uint8_t)((usage >> 8U) & 0xFFU),
  };
  return encode_hid_subframe(SPI3_RF_HID_CONSUMER, p, 2U, out_buf, out_cap);
}

uint16_t spi3_rf_build_system(uint8_t usage, uint8_t *out_buf, uint16_t out_cap)
{
  return encode_hid_subframe(SPI3_RF_HID_SYSTEM, &usage, 1U, out_buf, out_cap);
}

uint16_t spi3_rf_build_via(uint8_t const *pkt, uint8_t pkt_len, uint8_t *out_buf, uint16_t out_cap)
{
  uint8_t p[SPI3_RF_MAX_PAYLOAD];
  memset(p, 0, sizeof(p));
  if (pkt != NULL && pkt_len > 0U) {
    uint8_t copy_len = (pkt_len <= SPI3_RF_MAX_PAYLOAD) ? pkt_len : SPI3_RF_MAX_PAYLOAD;
    memcpy(p, pkt, copy_len);
  }
  return encode_hid_subframe(SPI3_RF_HID_VIA, p, (uint8_t)sizeof(p), out_buf, out_cap);
}

uint16_t spi3_rf_build_set_mode(uint8_t rf_mode, uint8_t *out_buf, uint16_t out_cap)
{
  return spi3_rf_encode_tx(SPI3_RF_CMD_SET_MODE, &rf_mode, 1U, out_buf, out_cap);
}

uint16_t spi3_rf_build_pair(uint8_t subcmd, uint8_t *out_buf, uint16_t out_cap)
{
  if (subcmd == 0U) {
    uint8_t p[2] = { 0U, 1U };
    return spi3_rf_encode_tx(SPI3_RF_CMD_PAIR_NAME, p, 2U, out_buf, out_cap);
  }
  return spi3_rf_encode_tx(SPI3_RF_CMD_PAIR_NAME, &subcmd, 1U, out_buf, out_cap);
}

uint16_t spi3_rf_build_ble_name(char const *name, uint8_t *out_buf, uint16_t out_cap)
{
  uint8_t p[33];
  memset(p, 0, sizeof(p));
  size_t len = (name != NULL) ? strlen(name) : 0U;
  if (len > 31U) {
    len = 31U;
  }
  p[0] = 2U;
  p[1] = (uint8_t)len;
  if (len > 0U) {
    memcpy(&p[2], name, len);
  }
  return spi3_rf_encode_tx(SPI3_RF_CMD_PAIR_NAME, p, (uint8_t)sizeof(p), out_buf, out_cap);
}

uint16_t spi3_rf_build_poll_rate(uint8_t rate_idx, uint8_t *out_buf, uint16_t out_cap)
{
  return spi3_rf_encode_tx(SPI3_RF_CMD_POLL_RATE, &rate_idx, 1U, out_buf, out_cap);
}

uint16_t spi3_rf_build_battery(uint8_t battery_pct, uint8_t *out_buf, uint16_t out_cap)
{
  return spi3_rf_encode_tx(SPI3_RF_CMD_BATTERY, &battery_pct, 1U, out_buf, out_cap);
}

uint16_t spi3_rf_build_sleep(uint8_t *out_buf, uint16_t out_cap)
{
  uint8_t const zeros[2] = { 0x00U, 0x00U };
  return spi3_rf_encode_tx(SPI3_RF_CMD_SLEEP, zeros, (uint8_t)sizeof(zeros), out_buf, out_cap);
}

void spi3_rf_apply_rx(spi3_rf_state_t *st, spi3_rf_rx_frame_t const *frame, uint32_t now_ms,
                      spi3_rf_via_rx_cb_t via_cb, void *via_ctx)
{
  if (st == NULL || frame == NULL) {
    return;
  }
  st->last_rx_ms = now_ms;
  bool was_up    = st->link_up;

  switch (frame->cmd) {
    case SPI3_RF_RX_VENDOR:
      if (via_cb != NULL && frame->payload_len >= 32U) {
        via_cb(frame->payload, frame->payload_len, via_ctx);
      }
      break;

    case SPI3_RF_RX_STATUS:
      if (frame->payload_len >= 1U) {
        uint8_t flags = frame->payload[0];
        st->host_leds     = (uint8_t)(flags & 0x07U);
        st->low_batt_flag = ((flags & 0x20U) != 0U);
        st->os_flag       = (uint8_t)((flags & 0x80U) >> 7U);
        st->link_up       = ((flags & 0x40U) != 0U);
      }
      if (frame->payload_len >= 2U) {
        st->link_state = frame->payload[1];
        if (st->link_state == SPI3_RF_STATE_CONNECTED) {
          st->link_up = true;
        } else if (st->link_state != SPI3_RF_STATE_RECONNECTING &&
                   st->link_state != SPI3_RF_STATE_CONNECTED) {
          st->link_up = false;
        }
      }
      if (frame->payload_len >= 3U) {
        st->reported_mode = frame->payload[2];
        if (st->target_mode != SPI3_RF_MODE_USB && st->reported_mode != st->target_mode) {
          st->mode_sync_needed = true;
        } else {
          st->mode_sync_needed = false;
        }
      }
      if (!was_up && st->link_up && st->target_mode != SPI3_RF_MODE_USB) {
        st->batt_sync_needed = true;
      }
      break;

    case SPI3_RF_RX_EXT_ACK:
      if (frame->payload_len >= 1U && frame->payload[0] == 0xC4U) {
        st->ack_pending = false;
      }
      break;

    case SPI3_RF_RX_PAIR_ACK:
      st->paired       = true;
      st->pair_pending = false;
      st->link_up      = true;
      if (!was_up && st->target_mode != SPI3_RF_MODE_USB) {
        st->batt_sync_needed = true;
      }
      st->link_state   = SPI3_RF_STATE_CONNECTED;
      break;

    default:
      break;
  }
}

void spi3_rf_note_tx(spi3_rf_state_t *st, uint8_t const *buf, uint16_t len, spi3_tx_kind_t kind)
{
  if (st == NULL || buf == NULL || len == 0U || len > SPI3_RF_BUF_SIZE) {
    return;
  }
  if (kind == SPI3_TX_NORMAL) {
    memcpy(st->last_cmd_buf, buf, len);
    st->last_cmd_len = len;
    st->retry_left   = 3U;
  }
}

bool spi3_rf_check_retry(spi3_rf_state_t *st, spi3_rf_rx_status_t rx_status)
{
  if (st == NULL) {
    return false;
  }
  if (rx_status == SPI3_RF_RX_OK) {
    st->retry_left = 0U;
    return false;
  }
  if (rx_status == SPI3_RF_RX_BUSY && st->retry_left > 0U && st->last_cmd_len > 0U) {
    st->retry_left--;
    return true;
  }
  return false;
}

#ifdef AT32F405RCT7

#define SPI3_RF_TX_Q_LEN        16U
#define SPI3_RF_BT_TX_GAP_MS    8U
#define SPI3_RF_SLEEP_WAIT_US   2000U
#define SPI3_RF_ISR_BF_MAX_SPIN 256U

typedef struct {
  uint8_t  buf[SPI3_RF_BUF_SIZE];
  uint16_t len;
  bool     is_poll;
} spi3_rf_tx_slot_t;

static spi3_rf_state_t   g_rf;
static spi3_rf_tx_slot_t g_tx_q[SPI3_RF_TX_Q_LEN];
static uint8_t           g_q_head = 0U;
static uint8_t           g_q_tail = 0U;

static uint8_t           g_dma_tx_buf[SPI3_RF_BUF_SIZE];
static uint8_t           g_dma_rx_buf[SPI3_RF_BUF_SIZE];
static uint32_t          g_last_sync_ms = 0U;

static volatile bool     g_dma_busy = false;
static volatile bool     g_dma_tx_is_poll = false;
static volatile bool     g_rx_ready = false;

static void cs_high(void)
{
  gpio_bits_set(TK75_RF_CS_PORT, TK75_RF_CS_PIN);
}

static void cs_low(void)
{
  gpio_bits_reset(TK75_RF_CS_PORT, TK75_RF_CS_PIN);
}

static void set_pb12_run(bool run_mode)
{
  gpio_init_type gpio;
  gpio_default_para_init(&gpio);
  gpio.gpio_pins = TK75_RF_CTRL_PIN;
  if (run_mode) {
    gpio.gpio_mode           = GPIO_MODE_OUTPUT;
    gpio.gpio_out_type       = GPIO_OUTPUT_PUSH_PULL;
    gpio.gpio_pull           = GPIO_PULL_NONE;
    gpio.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
    gpio_init(TK75_RF_CTRL_PORT, &gpio);
    gpio_bits_set(TK75_RF_CTRL_PORT, TK75_RF_CTRL_PIN);
  } else {
    gpio.gpio_mode = GPIO_MODE_INPUT;
    gpio.gpio_pull = GPIO_PULL_NONE;
    gpio_init(TK75_RF_CTRL_PORT, &gpio);
  }
}

static bool enqueue_tx(uint8_t const *buf, uint16_t len, bool is_poll)
{
  if (len == 0U || len > SPI3_RF_BUF_SIZE) {
    return false;
  }
  uint8_t next = (uint8_t)((g_q_head + 1U) % SPI3_RF_TX_Q_LEN);
  if (next == g_q_tail) {
    return false;
  }
  memcpy(g_tx_q[g_q_head].buf, buf, len);
  g_tx_q[g_q_head].len     = len;
  g_tx_q[g_q_head].is_poll = is_poll;
  g_q_head = next;
  return true;
}

static void kick_dma(uint8_t const *buf, uint16_t len, spi3_tx_kind_t kind)
{
  if (g_dma_busy || buf == NULL || len == 0U || len > SPI3_RF_BUF_SIZE) {
    return;
  }

  spi3_rf_note_tx(&g_rf, buf, len, kind);
  memcpy(g_dma_tx_buf, buf, len);
  memset(g_dma_rx_buf, 0xFF, len);

  g_dma_tx_is_poll = (kind == SPI3_TX_POLL);
  g_dma_busy       = true;

  /* Drain any stale RX byte before arming DMA */
  if (spi_i2s_flag_get(TK75_RF_SPI, SPI_I2S_RDBF_FLAG) != RESET) {
    (void)spi_i2s_data_receive(TK75_RF_SPI);
  }

  dma_channel_enable(TK75_RF_RX_DMA_CH, FALSE);
  dma_channel_enable(TK75_RF_TX_DMA_CH, FALSE);
  dma_flag_clear(DMA1_FDT3_FLAG);
  dma_flag_clear(DMA1_FDT2_FLAG);

  TK75_RF_RX_DMA_CH->maddr = (uint32_t)g_dma_rx_buf;
  dma_data_number_set(TK75_RF_RX_DMA_CH, len);
  TK75_RF_TX_DMA_CH->maddr = (uint32_t)g_dma_tx_buf;
  dma_data_number_set(TK75_RF_TX_DMA_CH, len);

  cs_low();
  dma_channel_enable(TK75_RF_RX_DMA_CH, TRUE);
  dma_channel_enable(TK75_RF_TX_DMA_CH, TRUE);
}

void spi3_rf_hw_init(uint8_t initial_mode, uint8_t poll_rate_idx)
{
  crm_periph_clock_enable(CRM_GPIOA_PERIPH_CLOCK, TRUE);
  crm_periph_clock_enable(CRM_GPIOB_PERIPH_CLOCK, TRUE);
  crm_periph_clock_enable(CRM_GPIOD_PERIPH_CLOCK, TRUE);
  crm_periph_clock_enable(TK75_RF_SPI_CLK, TRUE);
  crm_periph_clock_enable(TK75_RF_DMA_CLK, TRUE);

  g_q_head       = 0U;
  g_q_tail       = 0U;
  g_dma_busy     = false;
  g_rx_ready     = false;
  g_last_sync_ms = 0U;
  spi3_rf_state_init(&g_rf, initial_mode, poll_rate_idx);

  gpio_init_type gpio;
  gpio_default_para_init(&gpio);

  /* PA15 soft CS output push-pull */
  gpio.gpio_pins           = TK75_RF_CS_PIN;
  gpio.gpio_mode           = GPIO_MODE_OUTPUT;
  gpio.gpio_out_type       = GPIO_OUTPUT_PUSH_PULL;
  gpio.gpio_pull           = GPIO_PULL_NONE;
  gpio.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
  gpio_init(TK75_RF_CS_PORT, &gpio);
  cs_high();

  /* PD2 RF IRQ/Ready active-LOW input pull-up */
  gpio_default_para_init(&gpio);
  gpio.gpio_pins = TK75_RF_IRQ_PIN;
  gpio.gpio_mode = GPIO_MODE_INPUT;
  gpio.gpio_pull = GPIO_PULL_UP;
  gpio_init(TK75_RF_IRQ_PORT, &gpio);

  /* PB12 control line active HIGH in run mode */
  set_pb12_run(true);

  /* PB3/PB4/PB5 SPI3 AF6 */
  gpio_default_para_init(&gpio);
  gpio.gpio_pins           = TK75_RF_SCK_PIN | TK75_RF_MISO_PIN | TK75_RF_MOSI_PIN;
  gpio.gpio_mode           = GPIO_MODE_MUX;
  gpio.gpio_out_type       = GPIO_OUTPUT_PUSH_PULL;
  gpio.gpio_pull           = GPIO_PULL_UP;
  gpio.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
  gpio_init(TK75_RF_SPI_PORT, &gpio);
  gpio_pin_mux_config(TK75_RF_SPI_PORT, TK75_RF_SCK_SRC, GPIO_MUX_6);
  gpio_pin_mux_config(TK75_RF_SPI_PORT, TK75_RF_MISO_SRC, GPIO_MUX_6);
  gpio_pin_mux_config(TK75_RF_SPI_PORT, TK75_RF_MOSI_SRC, GPIO_MUX_6);

  /* Stock FUN_0801bacc PA15 wake/reset pulse */
  board_delay_us(1000U);
  cs_low();
  board_delay_us(1000U);
  cs_high();

  spi_i2s_reset(TK75_RF_SPI);
  spi_init_type spi;
  spi_default_para_init(&spi);
  spi.transmission_mode      = SPI_TRANSMIT_FULL_DUPLEX;
  spi.master_slave_mode      = SPI_MODE_MASTER;
  spi.mclk_freq_division     = SPI_MCLK_DIV_16; /* 108 MHz / 16 = 6.75 MHz */
  spi.first_bit_transmission = SPI_FIRST_BIT_MSB;
  spi.frame_bit_num          = SPI_FRAME_8BIT;
  spi.clock_polarity         = SPI_CLOCK_POLARITY_LOW;
  spi.clock_phase            = SPI_CLOCK_PHASE_2EDGE; /* SPI Mode 1 */
  spi.cs_mode_selection      = SPI_CS_SOFTWARE_MODE;
  spi_init(TK75_RF_SPI, &spi);
  spi_software_cs_internal_level_set(TK75_RF_SPI, SPI_SWCS_INTERNAL_LEVEL_HIGHT);

  dmamux_enable(TK75_RF_DMA, TRUE);

  dma_init_type dma;
  dma_reset(TK75_RF_RX_DMA_CH);
  dma_default_para_init(&dma);
  dma.buffer_size           = 0U;
  dma.direction             = DMA_DIR_PERIPHERAL_TO_MEMORY;
  dma.memory_base_addr      = (uint32_t)g_dma_rx_buf;
  dma.memory_data_width     = DMA_MEMORY_DATA_WIDTH_BYTE;
  dma.memory_inc_enable     = TRUE;
  dma.peripheral_base_addr  = (uint32_t)&(TK75_RF_SPI->dt);
  dma.peripheral_data_width = DMA_PERIPHERAL_DATA_WIDTH_BYTE;
  dma.peripheral_inc_enable = FALSE;
  dma.loop_mode_enable      = FALSE;
  dma.priority              = DMA_PRIORITY_HIGH;
  dma_init(TK75_RF_RX_DMA_CH, &dma);
  dma_flexible_config(TK75_RF_DMA, TK75_RF_RX_DMAMUX_CH, TK75_RF_RX_DMAMUX_REQ);

  dma_reset(TK75_RF_TX_DMA_CH);
  dma_default_para_init(&dma);
  dma.buffer_size           = 0U;
  dma.direction             = DMA_DIR_MEMORY_TO_PERIPHERAL;
  dma.memory_base_addr      = (uint32_t)g_dma_tx_buf;
  dma.memory_data_width     = DMA_MEMORY_DATA_WIDTH_BYTE;
  dma.memory_inc_enable     = TRUE;
  dma.peripheral_base_addr  = (uint32_t)&(TK75_RF_SPI->dt);
  dma.peripheral_data_width = DMA_PERIPHERAL_DATA_WIDTH_BYTE;
  dma.peripheral_inc_enable = FALSE;
  dma.loop_mode_enable      = FALSE;
  dma.priority              = DMA_PRIORITY_HIGH;
  dma_init(TK75_RF_TX_DMA_CH, &dma);
  dma_flexible_config(TK75_RF_DMA, TK75_RF_TX_DMAMUX_CH, TK75_RF_TX_DMAMUX_REQ);
  dma_interrupt_enable(TK75_RF_TX_DMA_CH, DMA_FDT_INT, TRUE);
  nvic_irq_enable(TK75_RF_TX_DMA_IRQn, 2U, 0U);

  spi_i2s_dma_receiver_enable(TK75_RF_SPI, TRUE);
  spi_i2s_dma_transmitter_enable(TK75_RF_SPI, TRUE);
  spi_enable(TK75_RF_SPI, TRUE);
}

void spi3_rf_set_target_mode(uint8_t rf_mode)
{
  if (g_rf.target_mode != rf_mode) {
    g_rf.target_mode      = rf_mode;
    g_rf.mode_sync_needed = true;
    g_rf.name_sync_needed = (rf_mode <= SPI3_RF_MODE_BT3);
    g_rf.rate_sync_needed = (rf_mode != SPI3_RF_MODE_USB);
    g_rf.batt_sync_needed = (rf_mode != SPI3_RF_MODE_USB);
    g_rf.last_sent_batt   = 0xFFU;
    g_rf.link_up          = false;
    g_rf.link_state       = SPI3_RF_STATE_RECONNECTING;
    g_last_sync_ms        = 0U;
  }
}

void spi3_rf_request_pair(void)
{
  if (g_rf.target_mode != SPI3_RF_MODE_USB) {
    g_rf.pair_pending = true;
    g_rf.link_state   = SPI3_RF_STATE_PAIRING;
    g_rf.link_up      = false;
  }
}

void spi3_rf_set_poll_rate(uint8_t rate_idx)
{
  g_rf.poll_rate_idx    = (rate_idx < 4U) ? rate_idx : 0U;
  g_rf.rate_sync_needed = (g_rf.target_mode != SPI3_RF_MODE_USB);
}

void spi3_rf_enter_sleep(uint8_t battery_pct)
{
  if (g_rf.sleeping) {
    return;
  }
  uint8_t pkt[SPI3_RF_BUF_SIZE];
  uint16_t batt_len  = spi3_rf_build_battery(battery_pct, pkt, (uint16_t)sizeof(pkt));
  /* Drain active DMA with bounded microsecond timeout before sending battery & sleep */
  uint32_t t0 = board_micros();
  while (g_dma_busy && (uint32_t)(board_micros() - t0) < SPI3_RF_SLEEP_WAIT_US) {
  }
  if (!g_dma_busy && batt_len > 0U) {
    kick_dma(pkt, batt_len, SPI3_TX_NORMAL);
    g_rf.last_sent_batt = battery_pct;
    t0 = board_micros();
    while (g_dma_busy && (uint32_t)(board_micros() - t0) < SPI3_RF_SLEEP_WAIT_US) {
    }
  }
  uint16_t sleep_len = spi3_rf_build_sleep(pkt, (uint16_t)sizeof(pkt));
  if (!g_dma_busy && sleep_len > 0U) {
    kick_dma(pkt, sleep_len, SPI3_TX_NORMAL);
    t0 = board_micros();
    while (g_dma_busy && (uint32_t)(board_micros() - t0) < SPI3_RF_SLEEP_WAIT_US) {
    }
  }
  g_rf.sleeping   = true;
  g_rf.link_state = SPI3_RF_STATE_SLEEP;
  g_rf.link_up    = false;
  set_pb12_run(false);
}

void spi3_rf_wake(void)
{
  if (!g_rf.sleeping) {
    return;
  }
  set_pb12_run(true);
  board_delay_us(500U);
  cs_low();
  board_delay_us(500U);
  cs_high();
  g_rf.sleeping         = false;
  g_rf.mode_sync_needed = true;
  g_rf.rate_sync_needed = (g_rf.target_mode != SPI3_RF_MODE_USB);
  g_rf.batt_sync_needed = (g_rf.target_mode != SPI3_RF_MODE_USB);
  g_rf.link_state       = SPI3_RF_STATE_RECONNECTING;
  g_last_sync_ms        = 0U;
}

bool spi3_rf_send_boot_kbd(uint8_t modifiers, uint8_t const keys[6])
{
  uint8_t pkt[SPI3_RF_BUF_SIZE];
  uint16_t len = spi3_rf_build_boot_kbd(modifiers, keys, pkt, (uint16_t)sizeof(pkt));
  return enqueue_tx(pkt, len, false);
}

bool spi3_rf_send_nkro_kbd(uint8_t const nkro15[15])
{
  uint8_t pkt[SPI3_RF_BUF_SIZE];
  uint16_t len = spi3_rf_build_nkro_kbd(nkro15, pkt, (uint16_t)sizeof(pkt));
  return enqueue_tx(pkt, len, false);
}

bool spi3_rf_send_consumer(uint16_t usage)
{
  uint8_t pkt[SPI3_RF_BUF_SIZE];
  uint16_t len = spi3_rf_build_consumer(usage, pkt, (uint16_t)sizeof(pkt));
  return enqueue_tx(pkt, len, false);
}

bool spi3_rf_send_system(uint8_t usage)
{
  uint8_t pkt[SPI3_RF_BUF_SIZE];
  uint16_t len = spi3_rf_build_system(usage, pkt, (uint16_t)sizeof(pkt));
  return enqueue_tx(pkt, len, false);
}

bool spi3_rf_send_via(uint8_t const *pkt32)
{
  uint8_t pkt[SPI3_RF_BUF_SIZE];
  uint16_t len = spi3_rf_build_via(pkt32, 32U, pkt, (uint16_t)sizeof(pkt));
  return enqueue_tx(pkt, len, false);
}

void spi3_rf_service(uint32_t now_ms, uint8_t battery_pct, spi3_rf_via_rx_cb_t via_cb, void *via_ctx)
{
  g_rf.battery_pct = battery_pct;
  if (g_rf.target_mode != SPI3_RF_MODE_USB && battery_pct != g_rf.last_sent_batt) {
    g_rf.batt_sync_needed = true;
  }

  if (g_rx_ready) {
    g_rx_ready = false;
    spi3_rf_rx_frame_t frame;
    spi3_rf_rx_status_t st = spi3_rf_decode_rx(g_dma_rx_buf, SPI3_RF_BUF_SIZE, &frame);
    if (st == SPI3_RF_RX_OK) {
      (void)spi3_rf_check_retry(&g_rf, st);
      spi3_rf_apply_rx(&g_rf, &frame, now_ms, via_cb, via_ctx);
    } else if (spi3_rf_check_retry(&g_rf, st)) {
      if (!g_dma_busy) {
        kick_dma(g_rf.last_cmd_buf, g_rf.last_cmd_len, SPI3_TX_RETRY);
      }
    }
  }

  if (g_dma_busy || g_rf.sleeping) {
    return;
  }

  if (g_q_tail != g_q_head) {
    uint32_t min_gap_ms = (g_rf.target_mode <= SPI3_RF_MODE_BT3) ? SPI3_RF_BT_TX_GAP_MS : 0U;
    if ((uint32_t)(now_ms - g_rf.last_tx_ms) < min_gap_ms) {
      return;
    }
    spi3_rf_tx_slot_t const *slot = &g_tx_q[g_q_tail];
    g_q_tail = (uint8_t)((g_q_tail + 1U) % SPI3_RF_TX_Q_LEN);
    g_rf.last_tx_ms = now_ms;
    kick_dma(slot->buf, slot->len, slot->is_poll ? SPI3_TX_POLL : SPI3_TX_NORMAL);
    return;
  }

  uint8_t pkt[SPI3_RF_BUF_SIZE];
  if (g_rf.mode_sync_needed && (uint32_t)(now_ms - g_last_sync_ms) >= 150U) {
    g_last_sync_ms = now_ms;
    uint16_t len = spi3_rf_build_set_mode(g_rf.target_mode, pkt, (uint16_t)sizeof(pkt));
    if (g_rf.target_mode == SPI3_RF_MODE_USB) {
      g_rf.mode_sync_needed = false;
    }
    kick_dma(pkt, len, SPI3_TX_NORMAL);
    return;
  }

  if (g_rf.name_sync_needed && g_rf.target_mode <= SPI3_RF_MODE_BT3 &&
      (uint32_t)(now_ms - g_last_sync_ms) >= 60U) {
    g_last_sync_ms        = now_ms;
    g_rf.name_sync_needed = false;
    uint16_t len          = spi3_rf_build_ble_name(SPI3_RF_DEFAULT_NAME, pkt, (uint16_t)sizeof(pkt));
    kick_dma(pkt, len, SPI3_TX_NORMAL);
    return;
  }

  if (g_rf.rate_sync_needed && g_rf.target_mode != SPI3_RF_MODE_USB &&
      (uint32_t)(now_ms - g_last_sync_ms) >= 60U) {
    g_last_sync_ms        = now_ms;
    g_rf.rate_sync_needed = false;
    uint16_t len          = spi3_rf_build_poll_rate(g_rf.poll_rate_idx, pkt, (uint16_t)sizeof(pkt));
    kick_dma(pkt, len, SPI3_TX_NORMAL);
    return;
  }

  if (g_rf.pair_pending && g_rf.target_mode != SPI3_RF_MODE_USB &&
      (uint32_t)(now_ms - g_last_sync_ms) >= 60U) {
    g_last_sync_ms    = now_ms;
    g_rf.pair_pending = false;
    uint16_t len      = spi3_rf_build_pair(3U, pkt, (uint16_t)sizeof(pkt));
    kick_dma(pkt, len, SPI3_TX_NORMAL);
    return;
  }

  if (g_rf.batt_sync_needed && g_rf.target_mode != SPI3_RF_MODE_USB &&
      (uint32_t)(now_ms - g_last_sync_ms) >= 60U) {
    g_last_sync_ms        = now_ms;
    g_rf.batt_sync_needed = false;
    g_rf.last_sent_batt   = g_rf.battery_pct;
    uint16_t len          = spi3_rf_build_battery(g_rf.battery_pct, pkt, (uint16_t)sizeof(pkt));
    kick_dma(pkt, len, SPI3_TX_NORMAL);
    return;
  }

  bool irq_low = (gpio_input_data_bit_read(TK75_RF_IRQ_PORT, TK75_RF_IRQ_PIN) == RESET);
  if (g_rf.target_mode != SPI3_RF_MODE_USB &&
      (irq_low || (uint32_t)(now_ms - g_rf.last_poll_ms) >= 25U)) {
    g_rf.last_poll_ms = now_ms;
    uint16_t len      = spi3_rf_build_poll(pkt, (uint16_t)sizeof(pkt));
    kick_dma(pkt, len, SPI3_TX_POLL);
  }
}

spi3_rf_state_t const *spi3_rf_get_state(void)
{
  return &g_rf;
}

void DMA1_Channel2_IRQHandler(void)
{
  if (dma_flag_get(DMA1_FDT2_FLAG) != RESET) {
    dma_flag_clear(DMA1_FDT2_FLAG);
    uint32_t spin = 0U;
    while (spi_i2s_flag_get(TK75_RF_SPI, SPI_I2S_BF_FLAG) != RESET && spin < SPI3_RF_ISR_BF_MAX_SPIN) {
      spin++;
    }
    dma_channel_enable(TK75_RF_RX_DMA_CH, FALSE);
    dma_channel_enable(TK75_RF_TX_DMA_CH, FALSE);
    dma_flag_clear(DMA1_FDT3_FLAG);
    cs_high();

    if (g_dma_tx_is_poll) {
      g_rx_ready = true;
    }
    g_dma_busy = false;
  }
}

#endif /* AT32F405RCT7 */
