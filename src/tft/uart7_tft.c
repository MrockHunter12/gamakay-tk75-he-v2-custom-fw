#include "uart7_tft.h"
#include <stddef.h>
#include <string.h>

#ifdef AT32F405RCT7
#include "at32f402_405.h"
#include "board_tk75.h"
#include "tk75_pins.h"
#endif

uint16_t uart7_tft_encode_frame(uart7_tx_frame_t const *frame, uint8_t *out_wire, uint16_t out_cap)
{
  if (frame == NULL || out_wire == NULL || frame->payload_len > UART7_MAX_PAYLOAD) {
    return 0U;
  }

  uint16_t total_len = (uint16_t)(frame->payload_len + 5U);
  if (out_cap < total_len || (frame->payload_len > 0U && frame->payload == NULL)) {
    return 0U;
  }

  uint16_t len_field = (uint16_t)(frame->payload_len + 2U);
  out_wire[0] = UART7_TX_SYNC;
  out_wire[1] = (uint8_t)(len_field & 0xFFU);
  out_wire[2] = (uint8_t)(len_field >> 8U);
  out_wire[3] = frame->cmd;

  if (frame->payload_len > 0U) {
    memcpy(&out_wire[4], frame->payload, frame->payload_len);
  }

  uint8_t  sum     = 0U;
  uint16_t sum_end = (uint16_t)(frame->payload_len + 4U);
  for (uint16_t i = 1U; i < sum_end; i++) {
    sum = (uint8_t)(sum + out_wire[i]);
  }
  out_wire[sum_end] = sum;
  return total_len;
}

void uart7_tft_rx_init(uart7_rx_parser_t *parser)
{
  if (parser != NULL) {
    memset(parser, 0, sizeof(*parser));
  }
}

bool uart7_tft_rx_feed(uart7_rx_parser_t *parser, uint8_t byte, uart7_rx_frame_t *out_frame)
{
  if (parser == NULL || out_frame == NULL) {
    return false;
  }

  switch (parser->state) {
    case 0U: /* Wait for 'D' sync */
      if (byte == UART7_RX_SYNC) {
        parser->buf[0] = byte;
        parser->state  = 1U;
      }
      return false;

    case 1U: /* Length byte (protocol covers rx[1..len], checksum at rx[len+1]) */
      if (byte < 2U || byte > (uint8_t)(UART7_MAX_PAYLOAD + 1U)) {
        parser->state = (byte == UART7_RX_SYNC) ? 1U : 0U;
        return false;
      }
      parser->expected_len = byte;
      parser->buf[1]       = byte;
      parser->sum8         = byte;
      parser->idx          = 2U;
      parser->state        = 2U;
      return false;

    case 2U: /* Cmd + payload bytes rx[2..expected_len] */
      parser->buf[parser->idx] = byte;
      parser->sum8             = (uint8_t)(parser->sum8 + byte);
      parser->idx++;
      if (parser->idx > parser->expected_len) {
        parser->state = 3U;
      }
      return false;

    case 3U: /* Checksum byte */
    {
      bool valid = (byte == parser->sum8);
      if (valid) {
        uint8_t plen = (uint8_t)(parser->expected_len - 2U);
        out_frame->cmd         = parser->buf[2];
        out_frame->payload_len = plen;
        if (plen > 0U) {
          memcpy(out_frame->payload, &parser->buf[3], plen);
        }
      }
      parser->state = 0U;
      return valid;
    }

    default:
      parser->state = 0U;
      return false;
  }
}

#ifdef AT32F405RCT7

#define UART7_RX_RING_SIZE 256U

static uint8_t           g_tx_buf[UART7_MAX_WIRE];
static uint8_t           g_rx_ring[UART7_RX_RING_SIZE];
static uint16_t          g_rx_tail = 0U;
static volatile bool     g_tx_busy = false;
static uart7_rx_parser_t g_rx_parser;

static void init_uart7_gpio(void)
{
  gpio_init_type gpio;

  crm_periph_clock_enable(CRM_GPIOF_PERIPH_CLOCK, TRUE);
  gpio_default_para_init(&gpio);
  gpio.gpio_pins           = TK75_TFT_TX_PIN | TK75_TFT_RX_PIN;
  gpio.gpio_mode           = GPIO_MODE_MUX;
  gpio.gpio_out_type       = GPIO_OUTPUT_PUSH_PULL;
  gpio.gpio_pull           = GPIO_PULL_UP;
  gpio.gpio_drive_strength = GPIO_DRIVE_STRENGTH_MODERATE;
  gpio_init(TK75_TFT_GPIO_PORT, &gpio);

  gpio_pin_mux_config(TK75_TFT_GPIO_PORT, TK75_TFT_TX_SRC, TK75_TFT_PIN_MUX);
  gpio_pin_mux_config(TK75_TFT_GPIO_PORT, TK75_TFT_RX_SRC, TK75_TFT_PIN_MUX);
}

static void init_uart7_dma(void)
{
  dma_init_type dma;

  crm_periph_clock_enable(CRM_DMA1_PERIPH_CLOCK, TRUE);
  dmamux_enable(DMA1, TRUE);

  dma_reset(DMA1_CHANNEL4);
  dma_default_para_init(&dma);
  dma.buffer_size           = 0U;
  dma.direction             = DMA_DIR_MEMORY_TO_PERIPHERAL;
  dma.memory_base_addr      = (uint32_t)g_tx_buf;
  dma.memory_data_width     = DMA_MEMORY_DATA_WIDTH_BYTE;
  dma.memory_inc_enable     = TRUE;
  dma.peripheral_base_addr  = (uint32_t)&(TK75_TFT_UART->dt);
  dma.peripheral_data_width = DMA_PERIPHERAL_DATA_WIDTH_BYTE;
  dma.peripheral_inc_enable = FALSE;
  dma.loop_mode_enable      = FALSE;
  dma.priority              = DMA_PRIORITY_MEDIUM;
  dma_init(DMA1_CHANNEL4, &dma);
  dma_flexible_config(DMA1, DMA1MUX_CHANNEL4, DMAMUX_DMAREQ_ID_UART7_TX);
  dma_interrupt_enable(DMA1_CHANNEL4, DMA_FDT_INT, TRUE);
  nvic_irq_enable(DMA1_Channel4_IRQn, 4U, 0U);

  dma_reset(DMA1_CHANNEL5);
  dma_default_para_init(&dma);
  dma.buffer_size           = UART7_RX_RING_SIZE;
  dma.direction             = DMA_DIR_PERIPHERAL_TO_MEMORY;
  dma.memory_base_addr      = (uint32_t)g_rx_ring;
  dma.memory_data_width     = DMA_MEMORY_DATA_WIDTH_BYTE;
  dma.memory_inc_enable     = TRUE;
  dma.peripheral_base_addr  = (uint32_t)&(TK75_TFT_UART->dt);
  dma.peripheral_data_width = DMA_PERIPHERAL_DATA_WIDTH_BYTE;
  dma.peripheral_inc_enable = FALSE;
  dma.loop_mode_enable      = TRUE;
  dma.priority              = DMA_PRIORITY_MEDIUM;
  dma_init(DMA1_CHANNEL5, &dma);
  dma_flexible_config(DMA1, DMA1MUX_CHANNEL5, DMAMUX_DMAREQ_ID_UART7_RX);
  dma_channel_enable(DMA1_CHANNEL5, TRUE);
}

void uart7_tft_set_bus_power(bool enabled)
{
  if (!enabled) {
    dma_channel_enable(DMA1_CHANNEL4, FALSE);
    dma_channel_enable(DMA1_CHANNEL5, FALSE);
    usart_dma_transmitter_enable(TK75_TFT_UART, FALSE);
    usart_dma_receiver_enable(TK75_TFT_UART, FALSE);
    usart_enable(TK75_TFT_UART, FALSE);
    g_tx_busy = false;

    /* Drive TX/RX LOW with no pull-up so U24 & ST7735S discharge cleanly to 0V (no ESD back-powering) */
    gpio_init_type gpio;
    gpio_default_para_init(&gpio);
    gpio.gpio_pins           = TK75_TFT_TX_PIN | TK75_TFT_RX_PIN;
    gpio.gpio_mode           = GPIO_MODE_OUTPUT;
    gpio.gpio_out_type       = GPIO_OUTPUT_PUSH_PULL;
    gpio.gpio_pull           = GPIO_PULL_NONE;
    gpio.gpio_drive_strength = GPIO_DRIVE_STRENGTH_MODERATE;
    gpio_bits_reset(TK75_TFT_GPIO_PORT, TK75_TFT_TX_PIN | TK75_TFT_RX_PIN);
    gpio_init(TK75_TFT_GPIO_PORT, &gpio);
    return;
  }

  memset(g_rx_ring, 0, sizeof(g_rx_ring));
  g_rx_tail = 0U;
  g_tx_busy = false;
  uart7_tft_rx_init(&g_rx_parser);

  init_uart7_gpio();
  crm_periph_clock_enable(CRM_UART7_PERIPH_CLOCK, TRUE);

  usart_reset(TK75_TFT_UART);
  usart_init(TK75_TFT_UART, TK75_TFT_BAUD, USART_DATA_8BITS, USART_STOP_1_BIT);
  usart_parity_selection_config(TK75_TFT_UART, USART_PARITY_NONE);
  usart_hardware_flow_control_set(TK75_TFT_UART, USART_HARDWARE_FLOW_NONE);
  usart_transmitter_enable(TK75_TFT_UART, TRUE);
  usart_receiver_enable(TK75_TFT_UART, TRUE);

  init_uart7_dma();

  usart_dma_transmitter_enable(TK75_TFT_UART, TRUE);
  usart_dma_receiver_enable(TK75_TFT_UART, TRUE);
  usart_enable(TK75_TFT_UART, TRUE);
}

void uart7_tft_hw_init(void)
{
  uart7_tft_set_bus_power(true);
  board_set_tft_power(true);
}

bool uart7_tft_tx_busy(void)
{
  return g_tx_busy;
}

bool uart7_tft_send_frame(uart7_tx_frame_t const *frame)
{
  if (g_tx_busy) {
    return false;
  }

  uint16_t len = uart7_tft_encode_frame(frame, g_tx_buf, (uint16_t)sizeof(g_tx_buf));
  if (len == 0U) {
    return false;
  }

  g_tx_busy = true;
  dma_channel_enable(DMA1_CHANNEL4, FALSE);
  DMA1_CHANNEL4->maddr = (uint32_t)g_tx_buf;
  dma_data_number_set(DMA1_CHANNEL4, len);
  dma_channel_enable(DMA1_CHANNEL4, TRUE);
  return true;
}

bool uart7_tft_poll_rx(uart7_rx_frame_t *out_frame)
{
  if (out_frame == NULL) {
    return false;
  }

  uint16_t rem  = (uint16_t)dma_data_number_get(DMA1_CHANNEL5);
  uint16_t head = (uint16_t)((UART7_RX_RING_SIZE - rem) % UART7_RX_RING_SIZE);

  while (g_rx_tail != head) {
    uint8_t b = g_rx_ring[g_rx_tail];
    g_rx_tail = (uint16_t)((g_rx_tail + 1U) % UART7_RX_RING_SIZE);
    if (uart7_tft_rx_feed(&g_rx_parser, b, out_frame)) {
      return true;
    }
  }
  return false;
}

void DMA1_Channel4_IRQHandler(void)
{
  if (dma_flag_get(DMA1_FDT4_FLAG) != RESET) {
    dma_flag_clear(DMA1_FDT4_FLAG);
    dma_channel_enable(DMA1_CHANNEL4, FALSE);
    g_tx_busy = false;
  }
}

#endif /* AT32F405RCT7 */
