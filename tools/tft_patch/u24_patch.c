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
#define U9_STAGE_BUF_ADDR  0x00036100UL /* 256 B aligned U9 staging buffer */
#define U9_GIF_DELAY_CACHE 0x00036200UL /* 31 x uint16_t cached per-frame delays (ms) */
#define U9_GIF_CACHE_MAGIC 0x00036240UL
#define U9_GIF_CACHE_VAL   0x47494632UL /* "GIF2" */

/* Safe U9 SPI NOR region strictly dedicated to custom animated GIFs (1.0 MB = 32 x 32 KB blocks).
 * Isolated from stock assets (<=0x404000), U24 config (0x100000), Doom WAD (0x500000..0x677FFF),
 * and stock tail (0x7D1000). */
#define U9_GIF_MIN_ADDR    0x006C0000UL
#define U9_GIF_MAX_ADDR    0x007C0000UL
#define U9_GIF_HDR_ADDR    0x006C0000UL
#define U9_GIF_DATA_ADDR   0x006C8000UL
#define U9_GIF_FRAME_BYTES 0x00008000UL
#define U9_GIF_MAX_FRAMES  31U

#define STOCK_SEND_ACK_VA  0x0101563DUL /* void stock_send_ack(uint8_t const *cmd_ptr) */
#define STOCK_SET_PWM_VA   0x0101127DUL /* void stock_set_pwm(uint32_t pwm) */
#define STOCK_MENU_UI_VA   0x01015375UL /* void render_menu_pages(void) */
#define STOCK_HOME_UI_VA   0x01012EDDUL /* void render_home_screen(void) */
#define STOCK_LCD_CMD_VA   0x010119C1UL /* void lcd_write_cmd(uint8_t cmd) */
#define STOCK_LCD_DAT_VA   0x010119A5UL /* void lcd_write_data(uint8_t dat) */
#define STOCK_SPI_DMA_VA   0x010105B5UL /* void spi_dma_write(uint32_t port, uint32_t buf, uint32_t len) */
#define STOCK_U9_ERASE32K  0x01010A05UL /* void u9_erase_32k(uint32_t addr) */
#define STOCK_U9_PAGE_PROG 0x01010AE5UL /* void u9_page_prog(uint8_t const *src, uint32_t addr, uint32_t len) */
#define STOCK_U9_DMA_READ  0x01010B39UL /* void u9_dma_read(uint8_t *dst, uint32_t addr, uint32_t len) */
#define SPI_CTRL_REG       0x000F8720UL
#define SPI_DMA_ARG0       0x000F891CUL

typedef void (*fn_void_t)(void);
typedef void (*fn_ack_t)(uint8_t const *);
typedef void (*fn_u32_t)(uint32_t);
typedef void (*fn_u8_t)(uint8_t);
typedef void (*fn_dma_t)(uint32_t, uint32_t, uint32_t);
typedef void (*fn_u9_prog_t)(uint8_t const *, uint32_t, uint32_t);
typedef void (*fn_u9_read_t)(uint8_t *, uint32_t, uint32_t);

typedef struct {
  uint8_t  magic[4];    /* "TKGF" */
  uint8_t  version;     /* 1=fixed delay_ms, 2=per-frame frame_delays_ms[31] */
  uint8_t  frame_count; /* 1..31 */
  uint16_t delay_ms;    /* default frame interval in ms */
  uint32_t data_crc32;
  uint32_t reserved;
  char     name[16];
  uint16_t frame_delays_ms[U9_GIF_MAX_FRAMES]; /* v2: exact per-frame hold ms (33..10000) */
} u9_gif_hdr_t;

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

static void send_reply_frame(uint8_t ack_cmd, uint8_t const *payload, uint8_t len)
{
  volatile uint8_t *buf = (volatile uint8_t *)ACK_BUF_ADDR;
  if (len > 80U) {
    len = 80U;
  }
  buf[0] = 0x44U;
  buf[1] = (uint8_t)(len + 2U); /* cmd + payload + checksum slot */
  buf[2] = ack_cmd;
  for (uint32_t i = 0U; i < len; i++) {
    buf[3U + i] = payload[i];
  }
  uint8_t sum = 0U;
  for (uint32_t i = 1U; i <= (uint32_t)(len + 2U); i++) {
    sum = (uint8_t)(sum + buf[i]);
  }
  buf[0x53U] = sum;
  *(volatile uint8_t *)ACK_FLG_ADDR |= 1U;
}

static void u9_read_chunked(void *dst, uint32_t addr, uint32_t len)
{
  uint8_t *out = (uint8_t *)dst;
  fn_u9_read_t u9_read = (fn_u9_read_t)STOCK_U9_DMA_READ;
  while (len > 0U) {
    uint32_t chunk = (len > 4096U) ? 4096U : len;
    u9_read(out, addr, chunk);
    out  += chunk;
    addr += chunk;
    len  -= chunk;
  }
}

static uint32_t u9_compute_crc32(uint32_t addr, uint32_t len)
{
  uint8_t *buf = (uint8_t *)U9_STAGE_BUF_ADDR;
  uint32_t crc = 0xFFFFFFFFUL;
  while (len > 0U) {
    uint32_t chunk = (len > 256U) ? 256U : len;
    u9_read_chunked(buf, addr, chunk);
    for (uint32_t i = 0U; i < chunk; i++) {
      crc ^= (uint32_t)buf[i];
      for (uint32_t b = 0U; b < 8U; b++) {
        crc = (crc & 1U) ? ((crc >> 1U) ^ 0xEDB88320UL) : (crc >> 1U);
      }
    }
    addr += chunk;
    len  -= chunk;
  }
  return crc ^ 0xFFFFFFFFUL;
}

static void handle_u9_flash_cmd(volatile uint8_t const *p, uint16_t plen)
{
  if (plen < 4U) {
    uint8_t err = 0x01U;
    send_reply_frame(0xA8U, &err, 1U);
    return;
  }
  uint8_t  op   = p[0];
  uint32_t addr = (uint32_t)p[1] | ((uint32_t)p[2] << 8U) | ((uint32_t)p[3] << 16U);

  /* Strict hardware safety guard: erase/program strictly inside GIF region [0x6C0000..0x7C0000) */
  if (op == 0U) {
    if (addr < U9_GIF_MIN_ADDR || (addr + 0x8000UL) > U9_GIF_MAX_ADDR || (addr & 0x7FFFUL) != 0U) {
      uint8_t err = 0x02U;
      send_reply_frame(0xA8U, &err, 1U);
      return;
    }
    ((fn_u32_t)STOCK_U9_ERASE32K)(addr);
    uint8_t ok = 0x00U;
    send_reply_frame(0xA8U, &ok, 1U);
    return;
  }

  if (op == 1U && plen > 4U) {
    uint32_t n = (uint32_t)(plen - 4U);
    if (n > 88U || addr < U9_GIF_MIN_ADDR || (addr + n) > U9_GIF_MAX_ADDR ||
        ((addr & 0xFFU) + n) > 256U) {
      uint8_t err = 0x03U;
      send_reply_frame(0xA8U, &err, 1U);
      return;
    }
    uint8_t *stage = (uint8_t *)U9_STAGE_BUF_ADDR;
    for (uint32_t i = 0U; i < n; i++) {
      stage[i] = p[4U + i];
    }
    ((fn_u9_prog_t)STOCK_U9_PAGE_PROG)(stage, addr, n);
    uint8_t ok = 0x00U;
    send_reply_frame(0xA8U, &ok, 1U);
    return;
  }

  if (op == 2U && plen >= 5U) {
    uint32_t n = p[4];
    if (n > 16U || addr < U9_GIF_MIN_ADDR || (addr + n) > U9_GIF_MAX_ADDR) {
      uint8_t err = 0x04U;
      send_reply_frame(0xA8U, &err, 1U);
      return;
    }
    uint8_t resp[17];
    resp[0] = 0x00U;
    uint8_t *stage = (uint8_t *)U9_STAGE_BUF_ADDR;
    u9_read_chunked(stage, addr, n);
    for (uint32_t i = 0U; i < n; i++) {
      resp[1U + i] = stage[i];
    }
    send_reply_frame(0xA8U, resp, (uint8_t)(1U + n));
    return;
  }

  if (op == 3U && plen >= 8U) {
    uint32_t len = (uint32_t)p[4] | ((uint32_t)p[5] << 8U) |
                   ((uint32_t)p[6] << 16U) | ((uint32_t)p[7] << 24U);
    if (addr < U9_GIF_MIN_ADDR || addr >= U9_GIF_MAX_ADDR || len > (U9_GIF_MAX_ADDR - addr)) {
      uint8_t err = 0x05U;
      send_reply_frame(0xA8U, &err, 1U);
      return;
    }
    uint32_t crc = u9_compute_crc32(addr, len);
    uint8_t resp[5];
    resp[0] = 0x00U;
    resp[1] = (uint8_t)(crc & 0xFFU);
    resp[2] = (uint8_t)((crc >> 8U) & 0xFFU);
    resp[3] = (uint8_t)((crc >> 16U) & 0xFFU);
    resp[4] = (uint8_t)((crc >> 24U) & 0xFFU);
    send_reply_frame(0xA8U, resp, 5U);
    return;
  }

  uint8_t err = 0xFFU;
  send_reply_frame(0xA8U, &err, 1U);
}

static uint16_t clamp_gif_delay(uint16_t dms)
{
  if (dms < 33U) return 100U;
  if (dms > 10000U) return 10000U;
  return dms;
}

static void handle_gif_play_cmd(volatile uint8_t const *p, uint16_t plen)
{
  if (plen < 1U) {
    uint8_t err = 0x01U;
    send_reply_frame(0xA9U, &err, 1U);
    return;
  }
  uint8_t op = p[0];

  /* op=0: Query U9 "TKGF" v1/v2 GIF header -> [0x00, valid, frame_count, d0_lo, d0_hi] */
  if (op == 0U) {
    u9_gif_hdr_t hdr;
    u9_read_chunked(&hdr, U9_GIF_HDR_ADDR, (uint32_t)sizeof(hdr));
    uint8_t valid = (hdr.magic[0] == 'T' && hdr.magic[1] == 'K' &&
                     hdr.magic[2] == 'G' && hdr.magic[3] == 'F' &&
                     (hdr.version == 1U || hdr.version == 2U) &&
                     hdr.frame_count >= 1U && hdr.frame_count <= U9_GIF_MAX_FRAMES) ? 1U : 0U;
    uint16_t def_d = clamp_gif_delay(hdr.delay_ms);
    volatile uint16_t *cache = (volatile uint16_t *)U9_GIF_DELAY_CACHE;
    if (valid) {
      for (uint32_t i = 0U; i < U9_GIF_MAX_FRAMES; i++) {
        cache[i] = (hdr.version == 2U) ? clamp_gif_delay(hdr.frame_delays_ms[i]) : def_d;
      }
      *(volatile uint32_t *)U9_GIF_CACHE_MAGIC = U9_GIF_CACHE_VAL;
    } else {
      *(volatile uint32_t *)U9_GIF_CACHE_MAGIC = 0U;
    }
    uint16_t d0 = valid ? cache[0] : def_d;
    uint8_t resp[5];
    resp[0] = 0x00U;
    resp[1] = valid;
    resp[2] = valid ? hdr.frame_count : 0U;
    resp[3] = (uint8_t)(d0 & 0xFFU);
    resp[4] = (uint8_t)((d0 >> 8U) & 0xFFU);
    send_reply_frame(0xA9U, resp, 5U);
    return;
  }

  /* op=1: DMA-read frame p[1] from U9 into framebuffer & reply [0x00, idx, delay_lo, delay_hi] */
  if (op == 1U && plen >= 2U) {
    uint8_t idx = p[1];
    uint8_t resp[4];
    if (idx >= U9_GIF_MAX_FRAMES) {
      resp[0] = 0x02U;
      resp[1] = idx;
      send_reply_frame(0xA9U, resp, 2U);
      return;
    }
    uint32_t faddr = U9_GIF_DATA_ADDR + ((uint32_t)idx * U9_GIF_FRAME_BYTES);
    u9_read_chunked((void *)FB_BASE_ADDR, faddr, U9_GIF_FRAME_BYTES);
    *(volatile uint8_t *)FB_DIRTY_ADDR = 1U;
    uint16_t dms = 100U;
    if (*(volatile uint32_t *)U9_GIF_CACHE_MAGIC == U9_GIF_CACHE_VAL) {
      volatile uint16_t const *cache = (volatile uint16_t const *)U9_GIF_DELAY_CACHE;
      dms = clamp_gif_delay(cache[idx]);
    }
    resp[0] = 0x00U;
    resp[1] = idx;
    resp[2] = (uint8_t)(dms & 0xFFU);
    resp[3] = (uint8_t)((dms >> 8U) & 0xFFU);
    send_reply_frame(0xA9U, resp, 4U);
    return;
  }

  uint8_t err = 0xFFU;
  send_reply_frame(0xA9U, &err, 1U);
}

static void send_patch_ver(void)
{
  uint8_t ver[3];
  ver[0] = 0x00U;
  ver[1] = 0x01U;
  ver[2] = 0x04U; /* v1.4: U9 GIF flasher (0x28) + per-frame delay GIF player (0x29) */
  send_reply_frame(0xA5U, ver, 3U);
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

  if (cmd == 0x28U) {
    handle_u9_flash_cmd(p, plen);
    return;
  }

  if (cmd == 0x29U) {
    handle_gif_play_cmd(p, plen);
    return;
  }

  /* Unknown command: preserve original stock behavior */
  *(volatile uint8_t *)RX_ERR_ADDR = 1U;
}
