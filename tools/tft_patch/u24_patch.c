#include <stdint.h>

/* Verified YC3123-Q (x5_plain.bin LCD_V102, CPU VA = file_offset + 0x01000200) addresses */
#define FB_BASE_ADDR       0x0002A54CUL /* 128x128x2 BE RGB565 column-major framebuffer */
#define FB_DIRTY_ADDR      0x0002A183UL /* Set to 1 to trigger DMA flush in main loop */
#define RX_BUF_ADDR        0x0003254CUL /* [0]=0x55, [1..2]=le16(len), [3]=cmd, [4..]=payload */
#define RX_ERR_ADDR        0x0003360AUL /* Original unknown-cmd error flag (0x00033604 + 6) */
#define ACK_BUF_ADDR       0x000335B0UL /* TX response buffer ([0]=0x44, [1]=len, ..., [0x53]=sum) */
#define ACK_FLG_ADDR       0x0002000DUL /* TX trigger bit0 */
#define PREV_PAGE_ADDR     0x00020060UL /* Set to 0xFF to force stock UI redraw on resume */
#define PWM_TBL_ADDR       0x00020014UL /* 5 x uint32_t stock backlight PWM table */
#define FONT_6X12_ADDR     0x01016D3AUL /* ROM 6x12 ASCII font (0x20..0x7E, 12 B/glyph) */

/* Unused SRAM above stock BSS (stock BSS ends at 0x00035210, SP starts at 0x00070000) */
#define MANUAL_MAGIC_ADDR  0x00036000UL
#define MANUAL_MAGIC_VAL   0xCAFE5060UL

#define STOCK_SEND_ACK_VA  0x0101563DUL /* void stock_send_ack(uint8_t const *cmd_ptr) */
#define STOCK_SET_PWM_VA   0x0101127DUL /* void stock_set_pwm(uint32_t pwm) */
#define STOCK_MENU_UI_VA   0x01015375UL /* void render_menu_pages(void) */
#define STOCK_HOME_UI_VA   0x01012EDDUL /* void render_home_screen(void) */
#define STOCK_LCD_CMD_VA   0x010119C1UL /* void lcd_write_cmd(uint8_t cmd) */
#define STOCK_LCD_DAT_VA   0x010119A5UL /* void lcd_write_data(uint8_t dat) */
#define STOCK_SPI_DMA_VA   0x010105B5UL /* void spi_dma_write(uint32_t port, uint32_t buf, uint32_t len) */
#define SPI_CTRL_REG       0x000F8720UL
#define SPI_DMA_ARG0       0x000F891CUL

typedef void (*fn_void_t)(void);
typedef void (*fn_ack_t)(uint8_t const *);
typedef void (*fn_u32_t)(uint32_t);
typedef void (*fn_u8_t)(uint8_t);
typedef void (*fn_dma_t)(uint32_t, uint32_t, uint32_t);

void custom_cmd_hook(void);
void custom_ui_hook(void);

/* Fixed 8-byte entry table at 0x01055700:
 *   0x01055700: BL custom_cmd_hook (called from 0x01012A60)
 *   0x01055704: BL custom_ui_hook  (called from 0x01016CA2) */
__attribute__((naked, section(".text.entry"))) void _patch_entry(void)
{
  __asm__ volatile(
    "b custom_cmd_hook\n"
    "nop\n"
    "b custom_ui_hook\n"
    "nop\n"
  );
}

static inline void put_px(uint32_t x, uint32_t y, uint8_t hi, uint8_t lo)
{
  if (x < 128U && y < 128U) {
    volatile uint8_t *fb = (volatile uint8_t *)FB_BASE_ADDR;
    uint32_t off = (x << 8U) + (y << 1U);
    fb[off]      = hi;
    fb[off + 1U] = lo;
  }
}

static void fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint8_t hi, uint8_t lo)
{
  uint32_t x_end = x + w;
  uint32_t y_end = y + h;
  if (x_end > 128U) x_end = 128U;
  if (y_end > 128U) y_end = 128U;
  volatile uint8_t *fb = (volatile uint8_t *)FB_BASE_ADDR;
  for (uint32_t cx = x; cx < x_end; cx++) {
    volatile uint8_t *col = fb + (cx << 8U);
    for (uint32_t cy = y; cy < y_end; cy++) {
      col[cy << 1U]      = hi;
      col[(cy << 1U) + 1U] = lo;
    }
  }
}

static void draw_glyph(uint32_t x, uint32_t y, uint8_t ch, uint8_t fg_hi, uint8_t fg_lo,
                       uint8_t bg_hi, uint8_t bg_lo, uint32_t scale, uint32_t transp)
{
  if (ch < 0x20U || ch > 0x7EU) {
    ch = 0x20U;
  }
  uint8_t const *glyph = (uint8_t const *)FONT_6X12_ADDR + ((uint32_t)(ch - 0x20U) * 12U);
  for (uint32_t col = 0U; col < 6U; col++) {
    uint16_t bits = ((uint16_t)glyph[col * 2U] << 8U) | (uint16_t)glyph[col * 2U + 1U];
    for (uint32_t row = 0U; row < 12U; row++) {
      uint32_t on = (bits & (0x8000U >> row)) != 0U;
      if (!on && transp) {
        continue;
      }
      uint8_t hi = on ? fg_hi : bg_hi;
      uint8_t lo = on ? fg_lo : bg_lo;
      if (scale == 1U) {
        put_px(x + col, y + row, hi, lo);
      } else {
        fill_rect(x + col * scale, y + row * scale, scale, scale, hi, lo);
      }
    }
  }
}

static void flush_window(uint8_t r0, uint8_t r1, uint8_t c0, uint8_t c1)
{
  fn_u8_t lcd_cmd = (fn_u8_t)STOCK_LCD_CMD_VA;
  fn_u8_t lcd_dat = (fn_u8_t)STOCK_LCD_DAT_VA;
  lcd_cmd(0x2BU);
  lcd_dat(0U);
  lcd_dat(r0);
  lcd_dat(0U);
  lcd_dat(r1);
  lcd_cmd(0x2AU);
  lcd_dat(0U);
  lcd_dat(c0);
  lcd_dat(0U);
  lcd_dat(c1);
  lcd_cmd(0x2CU);
  *(volatile uint8_t *)(SPI_CTRL_REG + 0x1EU) = 0x3FU;
  ((fn_dma_t)STOCK_SPI_DMA_VA)(SPI_DMA_ARG0, FB_BASE_ADDR, 0x8000U);
}

static void send_patch_ver(void)
{
  volatile uint8_t *buf = (volatile uint8_t *)ACK_BUF_ADDR;
  buf[0] = 0x44U;
  buf[1] = 5U;     /* len: cmd + status + major + minor + checksum slot */
  buf[2] = 0xA5U;  /* 0x25 | 0x80 */
  buf[3] = 0x00U;  /* status OK */
  buf[4] = 0x01U;  /* patch major = 1 */
  buf[5] = 0x02U;  /* patch minor = 2 (v1.2: full-GRAM edge clear + non-flushing PWM) */
  uint8_t sum = 0U;
  for (uint32_t i = 1U; i <= 5U; i++) {
    sum = (uint8_t)(sum + buf[i]);
  }
  buf[0x53U] = sum;
  *(volatile uint8_t *)ACK_FLG_ADDR |= 1U;
}

void custom_ui_hook(void)
{
  if (*(volatile uint32_t *)MANUAL_MAGIC_ADDR != MANUAL_MAGIC_VAL) {
    ((fn_void_t)STOCK_MENU_UI_VA)();
    ((fn_void_t)STOCK_HOME_UI_VA)();
  }
}

void custom_cmd_hook(void)
{
  volatile uint8_t const *rx = (volatile uint8_t const *)RX_BUF_ADDR;
  uint16_t wire_len = (uint16_t)rx[1] | ((uint16_t)rx[2] << 8U);
  uint16_t plen     = (wire_len >= 2U) ? (uint16_t)(wire_len - 2U) : 0U;
  uint8_t  cmd      = rx[3];
  volatile uint8_t const *p = &rx[4];

  if (cmd == 0x20U && plen >= 1U) {
    if (p[0] != 0U) {
      *(volatile uint32_t *)MANUAL_MAGIC_ADDR = MANUAL_MAGIC_VAL;
      /* Clear full 132x132 ST7735S GRAM edges to COL_BG (0x0862) so zero snow ever appears */
      fill_rect(0U, 0U, 128U, 128U, 0x08U, 0x62U);
      flush_window(0U, 127U, 2U, 129U);
      flush_window(4U, 131U, 2U, 129U);
    } else {
      *(volatile uint32_t *)MANUAL_MAGIC_ADDR = 0U;
      *(volatile uint32_t *)PREV_PAGE_ADDR    = 0xFFU;
    }
    ((fn_ack_t)STOCK_SEND_ACK_VA)((uint8_t const *)&rx[3]);
    return;
  }

  if (cmd == 0x21U && plen >= 7U) {
    fill_rect(p[0], p[1], p[2], p[3], p[4], p[5]);
    if (p[6] != 0U) {
      *(volatile uint8_t *)FB_DIRTY_ADDR = 1U;
    }
    ((fn_ack_t)STOCK_SEND_ACK_VA)((uint8_t const *)&rx[3]);
    return;
  }

  if (cmd == 0x22U && plen >= 5U) {
    uint32_t x     = p[0];
    uint32_t y     = p[1];
    uint32_t w     = p[2];
    uint32_t h     = p[3];
    uint32_t flags = p[4];
    uint32_t avail = (uint32_t)(plen - 5U) >> 1U;
    uint32_t idx   = 0U;
    if ((flags & 1U) == 0U) {
      /* Row-major */
      for (uint32_t dy = 0U; dy < h && idx < avail; dy++) {
        for (uint32_t dx = 0U; dx < w && idx < avail; dx++) {
          put_px(x + dx, y + dy, p[5U + idx * 2U], p[6U + idx * 2U]);
          idx++;
        }
      }
    } else {
      /* Column-major */
      for (uint32_t dx = 0U; dx < w && idx < avail; dx++) {
        for (uint32_t dy = 0U; dy < h && idx < avail; dy++) {
          put_px(x + dx, y + dy, p[5U + idx * 2U], p[6U + idx * 2U]);
          idx++;
        }
      }
    }
    if ((flags & 2U) != 0U) {
      *(volatile uint8_t *)FB_DIRTY_ADDR = 1U;
    }
    ((fn_ack_t)STOCK_SEND_ACK_VA)((uint8_t const *)&rx[3]);
    return;
  }

  if (cmd == 0x23U && plen >= 8U) {
    uint32_t x      = p[0];
    uint32_t y      = p[1];
    uint8_t  fg_hi  = p[2];
    uint8_t  fg_lo  = p[3];
    uint8_t  bg_hi  = p[4];
    uint8_t  bg_lo  = p[5];
    uint32_t flags  = p[6];
    uint32_t scale  = flags & 3U;
    if (scale == 0U) scale = 1U;
    if (scale > 3U)  scale = 3U;
    uint32_t transp = (flags & 0x40U) != 0U;
    uint32_t step   = 6U * scale;
    for (uint32_t i = 7U; i < plen; i++) {
      uint8_t ch = p[i];
      if (ch == 0U) break;
      draw_glyph(x, y, ch, fg_hi, fg_lo, bg_hi, bg_lo, scale, transp);
      x += step;
      if (x >= 128U) break;
    }
    if ((flags & 0x80U) != 0U) {
      *(volatile uint8_t *)FB_DIRTY_ADDR = 1U;
    }
    ((fn_ack_t)STOCK_SEND_ACK_VA)((uint8_t const *)&rx[3]);
    return;
  }

  if (cmd == 0x24U && plen >= 1U) {
    if (plen >= 2U && p[1] == 1U && p[0] <= 100U) {
      /* Smooth 0..100% mapped safely to factory PWM range 0..30000 (no spurious LCD flush) */
      ((fn_u32_t)STOCK_SET_PWM_VA)((uint32_t)p[0] * 300U);
    } else if (p[0] <= 4U) {
      uint32_t const *pwm_tbl = (uint32_t const *)PWM_TBL_ADDR;
      ((fn_u32_t)STOCK_SET_PWM_VA)(pwm_tbl[p[0]]);
    } else if (p[0] == 0xFFU) {
      *(volatile uint8_t *)FB_DIRTY_ADDR = 1U;
    }
    ((fn_ack_t)STOCK_SEND_ACK_VA)((uint8_t const *)&rx[3]);
    return;
  }

  if (cmd == 0x25U) {
    send_patch_ver();
    return;
  }

  if (cmd == 0x26U && plen >= 2U) {
    uint32_t flags = p[0];
    uint32_t count = p[1];
    uint32_t off   = 2U;
    for (uint32_t i = 0U; i < count && (off + 6U) <= plen; i++, off += 6U) {
      fill_rect(p[off], p[off + 1U], p[off + 2U], p[off + 3U], p[off + 4U], p[off + 5U]);
    }
    if ((flags & 1U) != 0U) {
      *(volatile uint8_t *)FB_DIRTY_ADDR = 1U;
    }
    ((fn_ack_t)STOCK_SEND_ACK_VA)((uint8_t const *)&rx[3]);
    return;
  }

  /* Unknown command: preserve original stock behavior */
  *(volatile uint8_t *)RX_ERR_ADDR = 1U;
}
