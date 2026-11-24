# GamaKay TK75 HE V2 — Decompiled TFT Co-Processor Protocol & Architecture

> **Source of Truth**: Decompiled directly from the stock AT32F405 firmware binary (`tk75hev2_iso_v506_plain.bin` / `app.c`) and the RongYuan/GamaKay host upgrade suite (`ry_classes_pretty.js` / `PROTOCOL.md`).

---

## 1. Hardware & Physical Interface

| Signal / Parameter | MCU Pin / Peripheral | Details |
|---|---|---|
| **UART Peripheral** | `UART7` (`0x40007800`) | 460,800 baud, 8 data bits, 1 stop bit, no parity (`8N1`) |
| **UART7 TX (MCU $\rightarrow$ TFT)** | `PF7` (MUX7) | Driven by `DMA1_Channel4` (memory-to-peripheral, non-circular) |
| **UART7 RX (TFT $\rightarrow$ MCU)** | `PF6` (MUX7) | Driven by `DMA1_Channel5` (peripheral-to-memory, 256-byte ring buffer) |
| **TFT Power Gate** | `PC12` (`GPIO_PINS_12`) | Active-High rail enable (`1` = TFT & Backlight powered, `0` = cut power) |
| **Display Geometry** | 128 $\times$ 128 RGB565 | 16-bit Big-Endian (`R5:G6:B5`), **column-major** pixel ordering (`x` outer, `y` inner), 32,768 bytes/frame |
| **Onboard TFT Storage** | Dedicated Co-Processor + SPI NOR Flash | Stores internal UI theme/fonts/icons + **6 user image/GIF slots** (`0..5`) |

---

## 2. UART7 Wire Framing

### 2.1 Main MCU $\rightarrow$ TFT (`TX`, `FUN_08007e8c`)
Every outbound frame starts with sync byte `0x55` (`'U'`) and ends with an 8-bit additive checksum over bytes `[1 .. payload_len + 3]`:

```text
Offset  Size   Field          Description
──────  ─────  ─────────────  ──────────────────────────────────────────────────
0       1 B    sync           0x55 ('U')
1..2    2 B    len_field      uint16 LE = payload_len + 2 (covers cmd + checksum)
3       1 B    cmd            Command ID (0x01..0x0C, 0x30..0x31, 0x3B..0x3F)
4..N    P B    payload        Command payload (0..64 bytes)
N+1     1 B    checksum       sum(bytes[1 .. 3 + payload_len]) & 0xFF
```
- **Total wire bytes**: `payload_len + 5` (e.g. a 0-byte payload has `len_field = 2` and `total_len = 5`: `[0x55, 0x02, 0x00, cmd, (cmd + 2) & 0xFF]`).
- **Flow Control & Retries (`FUN_08007d84`)**:
  - After transmitting a frame, the stock MCU sets `tx_waiting_ack = 1` (`0x20014620 + 0x26`) and waits for a `'D'` response frame from the TFT.
  - If no valid ACK arrives within 40 ticks (~40 ms), the MCU re-transmits the frame up to **5 times** before clearing the busy flag.

### 2.2 TFT $\rightarrow$ Main MCU (`RX`, `FUN_08013c60` & `FUN_08016654`)
Every inbound frame from the TFT starts with sync byte `0x44` (`'D'`):

```text
Offset  Size   Field          Description
──────  ─────  ─────────────  ──────────────────────────────────────────────────
0       1 B    sync           0x44 ('D')
1       1 B    expected_len   uint8 = status_and_payload_len + 1 (covers rx[1..expected_len])
2       1 B    cmd            Response ID (0x81..0x8C, 0xB0..0xBF)
3       1 B    status         0x00 = OK / ACK; non-zero = NAK / Error (triggers retry)
4..M    K B    payload        Response data bytes (expected_len - 3 bytes)
M+1     1 B    checksum       sum(bytes[1 .. expected_len]) & 0xFF
```
*(Note: In `uart7_tft.c`, `out_frame->payload[0]` corresponds to `rx[3]` (`status`), `out_frame->payload[1]` corresponds to `rx[4]`, etc., with `out_frame->payload_len = expected_len - 2`).*

---

## 3. MCU $\rightarrow$ TFT Commands (`0x01` – `0x3F`)

### 3.1 `cmd = 0x01` — Menu Navigation, Backlight & Lighting Sync (`FUN_08008110`)
- **`len_field`**: `0x0014` (`18` payload bytes, `23` total wire bytes).
- **When sent**: Whenever the user navigates the on-screen menu, changes RGB/Sidelight settings, toggles OS/Language, or steps TFT screen brightness.

| Payload Byte | Wire Offset | Stock Source (`0x200146a3` / `0x2001216c`) | Description |
|---|---|---|---|
| `p[0..1]` | `4..5` | `*(uint16_t *)(tft + 0x29)` | **`menu_cursor`** (`u16 LE`, see Section 5 for full state table) |
| `p[2]` | `6` | `*(uint8_t *)(tft + 0x39)` | **`menu_trigger`** (`1` = load/switch picture slot or execute action; cleared by `0x81` ACK) |
| `p[3]` | `7` | `*(uint8_t *)(tft + 0x43)` | **`pic_slot`** (`0..5`, active custom image/GIF slot) |
| `p[4]` | `8` | `*(uint8_t *)(tft + 0x20)` | **`tft_brt_down_toggle`** (toggles `0` $\leftrightarrow$ `1` on CCW turn at cursor `0x0017` to step TFT LCD backlight down) |
| `p[5]` | `9` | `*(uint8_t *)(tft + 0x1f)` | **`tft_brt_up_toggle`** (toggles `0` $\leftrightarrow$ `1` on CW turn at cursor `0x0017` to step TFT LCD backlight up) |
| `p[6]` | `10` | `*(uint8_t *)(tft + 0xef)` | **`lang_dirty`** (`1` when `tft + 0x1e` language toggled; cleared by `0x81`/`0x82` ACK) |
| `p[7]` | `11` | `(cfg[4] >> 1) & 3` | **`os_mode`** (`0` = Windows, `1` = macOS) |
| `p[8]` | `12` | `cfg[8]` | **`rgb_effect`** (Stock effect ID: `1..12, 14..19`; `0` = Off) |
| `p[9]` | `13` | `cfg[0x62 + rgb_effect] & 0x0F` | **`rgb_color_idx`** (`0`=Red, `1`=Orange, `2`=Yellow, `3`=Green, `4`=Cyan, `5`=Blue, `6`=Purple, `7`=Pink, `8`=Rainbow/Dazzle) |
| `p[10]` | `14` | `cfg[0x42 + rgb_effect]` | **`rgb_brightness`** (`0..4`) |
| `p[11]` | `15` | `cfg[0x22 + rgb_effect]` | **`rgb_speed`** (`0..4`) |
| `p[12]` | `16` | `cfg[0x0d]` | **`side_effect`** (`0..5`: `0`=Off, `1`=Constant, `2`=Breathing, `3`=Neon, `4`=Wave, `5`=Ripple/Stream) |
| `p[13]` | `17` | `cfg[0x10] & 0x0F` | **`side_color_idx`** (`0..8`) |
| `p[14]` | `18` | `cfg[0x0f]` | **`side_brightness`** (`0..4`) |
| `p[15]` | `19` | `cfg[0x0e]` | **`side_speed`** (`0..3`) |
| `p[16]` | `20` | `*(uint8_t *)(tft + 0xf0)` | Reserved (`0`) |
| `p[17]` | `21` | `*(uint8_t *)(tft + 0xf1)` | Reserved (`0`) |

---

### 3.2 `cmd = 0x02` — Home-Screen Status Bar & Telemetry (`FUN_08008254`)
- **`len_field`**: `0x000E` (`12` payload bytes, `17` total wire bytes).
- **When sent**: On connection mode change, RF pairing/connect state change, battery/charging update, Caps/Num/Scroll/Win lock change, or USB `0x22` CPU telemetry update.

| Payload Byte | Wire Offset | Stock Source | Description |
|---|---|---|---|
| `p[0]` | `4` | `conn_mode` (`0x2000a498 + 0x1e`, mapped `>4 -> -2`) | **`conn_icon`**: `0` = BT1, `1` = BT2, `2` = BT3, `3` = 2.4GHz (`conn_mode=5`), `4` = USB Wired (`conn_mode=6`) |
| `p[1]` | `5` | `rf_status` (`+0x26`) | **`rf_status`**: `0` = USB/None, `1` = Connected, `2` = Pairing/Reconnecting, `3` = Disconnected/Sleep |
| `p[2]` | `6` | `charge_state` (`+0x5f`) | **`charge_state`**: `0` = Discharging (on battery), `1` = USB Powered / Full, `2` = Actively Charging |
| `p[3]` | `7` | `battery_pct` (`+0x4c`) | **`battery_pct`**: `0..100` (%) |
| `p[4]` | `8` | `(cfg[4] >> 1) & 3` | **`os_mode`**: `0` = Windows, `1` = macOS |
| `p[5]` | `9` | `cfg[0]` | **`active_profile`**: `0..3` |
| `p[6]` | `10` | `tft[0]` (`host_leds & 7`) | **`lock_leds`**: bit 0 = Num Lock, bit 1 = Caps Lock, bit 2 = Scroll Lock |
| `p[7]` | `11` | `tft[0x17]` (from USB `0x22` byte 18) | **`cpu_usage_pct`**: Host CPU usage (`0..100` %) |
| `p[8]` | `12` | `tft[0x18]` (from USB `0x22` byte 19) | **`cpu_temp_c`**: Host CPU temperature (`0..255` °C) |
| `p[9]` | `13` | `tft[0xef]` | **`lang_dirty`**: Language change flag |
| `p[10]` | `14` | `(cfg[4] >> 4) & 1` | **`led_power_save`**: `0` = Lighting enabled, `1` = Master LED off / power-save |
| `p[11]` | `15` | `cfg[4] & 1` | **`win_lock`**: `0` = Win key unlocked, `1` = Win key locked |

---

### 3.3 `cmd = 0x03` (14B) — Prepare Image/GIF Upload (`FUN_08016898` case `0xA5`)
- **`len_field`**: `0x000E` (`12` payload bytes, `17` total wire bytes).
- **When sent**: Forwarded from USB `0xA5` (`FEA_CMD_GETTFTLCDDATA`) before streaming image chunks.
- **Payload (`12` bytes)**:
  - `p[0]` = `0xA5`
  - `p[1]` = `frame_idx` (for static image: slot `0..5`; for GIF: `0`)
  - `p[2]` = `total_frames` (`1` for static image; `N` for GIF)
  - `p[3]` = `frame_delay` (frame delay in ms / 10, or `0` for static)
  - `p[4..5]` = `frame_bytes_le16` (`0x8000` = `32768` bytes for 128$\times$128 RGB565)
  - `p[6..7]` = `0x00, 0x00`
  - `p[8]` = `left` (`0`)
  - `p[9]` = `top` (`0`)
  - `p[10]` = `right` (`128`)
  - `p[11]` = `bottom` (`128`)
- **TFT Response**: `cmd = 0x83`, `status = 0x00` (`[0x44, 0x02, 0x83, 0x00, 0x85]`), sets `tft_upload_active = 1` (`tft + 0x4b = 1`).

---

### 3.4 `cmd = 0x03` (10B) — Extended Host Disk/RAM Telemetry (`FUN_08013950`)
- **`len_field`**: `0x000A` (`8` payload bytes, `13` total wire bytes).
- **When sent**: Triggered after USB `0x22` (`SET_OLEDOPTION` / `syncScreenSystemInfo`) sets `*0x2000a488 |= 0x04`.
- **Payload (`8` bytes)**:
  - `p[0..1]` = `tft[0x5f], tft[0x5e]` (Host Network/Disk metric)
  - `p[2..3]` = `uint16 LE` `tft[0x58..0x59]` (Host Disk Available GB)
  - `p[4..5]` = `uint16 LE` `tft[0x5a..0x5b]` (Host Disk Total GB)
  - `p[6..7]` = `uint16 LE` `tft[0x5c..0x5d]` (Host RAM Used / Total GB)
  *(Note: Differentiated from `cmd=0x03` Media Prep by `len_field == 0x000A` vs `0x000E` and `p[0] != 0xA5`).*

---

### 3.5 `cmd = 0x04` — Stream Image/GIF Data Chunk (`FUN_08016898` case `0x25`)
- **`len_field`**: `0x0042` (`64` payload bytes, `69` total wire bytes).
- **Payload (`64` bytes)**:
  - `p[0]` = `slot_or_cmd` (`0x25` in stock USB passthrough, or slot `0..5`)
  - `p[1]` = `frame_idx` (`0..total_frames-1`, or slot index `0..5` when `total_frames == 1`)
  - `p[2]` = `total_frames`
  - `p[3]` = `frame_delay`
  - `p[4..5]` = `chunk_index` (`uint16 LE`, `0 .. 585` for 32,768 B in 56 B slices)
  - `p[6]` = `valid_slice_bytes` (`56` for chunks `0..584`, `8` for chunk `585`)
  - `p[7]` = `0x00` (stock USB checksum byte, ignored by TFT)
  - `p[8..63]` = `56` bytes of **column-major** Big-Endian RGB565 pixel data
- **TFT Response**: `cmd = 0x84`, `status = 0x00`.

---

### 3.6 `cmd = 0x05` — Finalize Image Upload (`FUN_080138a4`)
- **`len_field`**: `0x0002` (`0` payload bytes, `5` total wire bytes: `55 02 00 05 07`).
- **When sent**: Sent when the 8-second image upload watchdog (`tft + 0x4c >= 8000` in `FUN_08007d84`) expires after `0x83`/`0x04` streaming finishes.

---

### 3.7 `cmd = 0x06` & `cmd = 0x07` — Erase All Custom Images in TFT Flash (`FUN_080137e0` & `FUN_08007ffc`)
- **`cmd = 0x06` (`len_field = 0x0002`)**: Wire bytes `55 02 00 06 08`. Starts full erase of all 6 image/GIF slots in the TFT's onboard SPI flash and sets `erase_in_progress = 1` (`tft + 0x3e = 1`).
- **`cmd = 0x07` (`len_field = 0x0002`)**: Wire bytes `55 02 00 07 09`. Sent every 1,000 ticks (~1 s) while `erase_in_progress == 1` to poll completion until the TFT replies with `cmd = 0x87` (`status = 0x00`), which clears `tft + 0x3e = 0` and emits USB vendor event `05 2C 00` (`ScreenClearDone`).

---

### 3.8 `cmd = 0x08` — Query TFT Firmware & Flash Version (`FUN_08013bac`)
- **`len_field`**: `0x0002` (`0` payload bytes, `5` total wire bytes: `55 02 00 08 0A`).
- **When sent**: Triggered by USB `0xAD` (`FEA_CMD_GETOLED_VERSION`) setting `*(0x2000a488 + 2) |= 0x02`.
- **TFT Response (`cmd = 0x88`, `FUN_08012e80` lines 14613–14656)**:
  - `rx[4..5]` = `oled_fw_version` (`uint16 LE`, e.g. `102` = v1.02)
  - `rx[6..7]` = `spi_flash_version` (`uint16 LE`, `0` if `rx[8] == 0`)
  - `rx[8]` = `has_spi_flash` (`0` or `1`)
  - `rx[9]` = `screen_type_flag`
  - `rx[10..19]` = 10-byte screen capability descriptor

---

### 3.9 `cmd = 0x09` — Sync Wall-Clock RTC (`FUN_08013594`)
- **`len_field`**: `0x0009` (`7` payload bytes, `12` total wire bytes).
- **Payload (`7` bytes)**:
  - `p[0..1]` = `year` (`uint16 LE`, e.g. `2026` $\rightarrow$ `0xEA, 0x07`)
  - `p[2]` = `month` (`1..12`)
  - `p[3]` = `day` (`1..31`)
  - `p[4]` = `hour` (`0..23`)
  - `p[5]` = `minute` (`0..59`)
  - `p[6]` = `second` (`0..59`)
- **TFT Response**: `cmd = 0x89`, `status = 0x00`.

---

### 3.10 `cmd = 0x0A`, `0x0B`, `0x0C` — Raw TFT SPI Flash UI Asset Upgrade (`FUN_08013728`, `FUN_08016898`, `FUN_0801367c`)
Used by `upgradeFlash()` in the vendor tool (`USB 0xB2` / `0x32`) to overwrite the TFT's internal UI icons, fonts, and menu graphics stored in its external SPI NOR flash:
1. **`cmd = 0x0A` (`len_field = 0x0004`)**: `p[0..1]` = `total_56B_chunks` (`uint16 LE`). TFT erases flash and replies `cmd = 0x8A`, `status = 0x00` (`tft + 0xe9 = 1`).
2. **`cmd = 0x0B` (`len_field = 0x0042`)**: Streams 64-byte blocks (`p[0..7]` header + `56` bytes raw flash data per packet). TFT replies `cmd = 0x8B`, `status = 0x00`.
3. **`cmd = 0x0C` (`len_field = 0x0002`)**: Finalizes raw SPI flash asset write after 1,000 ms idle (`FUN_08007d84` lines 2926–2939).

---

### 3.11 `cmd = 0x30` & `cmd = 0x31` — TFT Display Sleep & Wake (`FUN_08013aec` & `FUN_08013a2c`)
- **`cmd = 0x30` (`len_field = 0x0002`)**: Wire bytes `55 02 00 30 32`. Commands the TFT co-processor to turn off display rendering / enter sleep.
- **`cmd = 0x31` (`len_field = 0x0002`)**: Wire bytes `55 02 00 31 33`. Commands the TFT co-processor to wake up and resume display rendering.
- **How stock firmware triggers Sleep/Wake (`FUN_080159d8` lines 18716–18744)**:
  - Stock keycode `0x0413` (`case 0x13, sub 4`) sets `cfg[0xe7] = 1` (TFT disabled) $\rightarrow$ sends `cmd = 0x30` (`*(0x2000a488 + 1) |= 0x02`).
  - Stock keycode `0x0513` (`case 0x13, sub 5`) sets `cfg[0xe7] = 0` (TFT enabled) $\rightarrow$ sends `cmd = 0x31` (`*(0x2000a488 + 1) |= 0x04`).

---

### 3.12 `cmd = 0x3B .. 0x3F` — TFT Co-Processor Firmware Bootloader (`FUN_08013318` – `FUN_08013518`)
Used by `upgradeOled()` (`USB 0x30, 0xB0, 0x31, 0xB1`) to reflash the TFT co-processor's own MCU firmware (sliced from offset `0x10000` = `65536` of an upgrade file):
1. **`cmd = 0x3F` (`len_field = 0x0002`)**: Enter TFT bootloader (`USB 0x30` with magic `55 AA 55 AA`). TFT replies `cmd = 0xBF`, `status = 0x00`.
2. **`cmd = 0x3E` (`len_field = 0x0002`)**: Poll TFT bootloader status (`USB 0xB0`). TFT replies `cmd = 0xBE`, `status = 0x00`.
3. **`cmd = 0x3D` (`len_field = 0x0004`)**: Start firmware write (`USB 0x31`), `p[0..1]` = `num_64B_blocks` (`uint16 LE`). TFT replies `cmd = 0xBD`, `status = 0x00`.
4. **`cmd = 0x3C` (`len_field = 0x0042`)**: Write 64 bytes of TFT firmware (`p[0..63]`). TFT replies `cmd = 0xBC`, `status = 0x00`.
5. **`cmd = 0x3B` (`len_field = 0x0006`)**: Verify firmware checksum (`USB 0xB1`), `p[0..3]` = `sum32_le` (`uint32 LE` byte sum of firmware image). TFT replies `cmd = 0xBB`, `status = 0x00` or `0x55` (`rx[6..7]` echoed).

---

## 4. TFT $\rightarrow$ MCU Responses (`0x81` – `0xBF`, `FUN_08012e80`)

| RX Cmd | Triggered By | `rx` Wire Offsets & Meaning |
|---|---|---|
| **`0x81`** | `cmd = 0x01` (or TFT boot/heartbeat) | Clears `menu_trigger` (`tft + 0x39 = 0`) and `lang_dirty` (`tft + 0xef = 0`). When `rx[3] == 0`:<br>`rx[4..5]` = `menu_cursor` (`u16 LE`)<br>`rx[6]` = `pic_slot` (`0..5`)<br>`rx[7..12]` = `pic_exists[0..5]` (`1` if slot `i` has an image/GIF, `0` if empty)<br>`rx[13]` = `return_cursor` (`0` = Home, `2` = PicView)<br>`rx[14]` = `return_pic_slot` (`0..5`) |
| **`0x82`** | `cmd = 0x02` | Clears `lang_dirty` (`tft + 0xef = 0`) and updates `rx[4..14]` identically to `0x81`. |
| **`0x83`** | `cmd = 0x03` (Media Prep) | `rx[3] == 0` $\rightarrow$ sets USB reply `[0xA5, 0x01]` and enters image streaming mode (`tft + 0x4b = 1`). |
| **`0x84`** | `cmd = 0x04` (Media Chunk) | `rx[3] == 0` $\rightarrow$ chunk accepted; `rx[3] != 0` $\rightarrow$ sets `rx_nak = 1` for retry. |
| **`0x85` / `0x8C`** | `cmd = 0x05` / `0x0C` | Syncs `rx[4..14]` cursor, active slot, and `pic_exists[0..5]` after media/flash upload completes. |
| **`0x86`** | `cmd = 0x06` | Erase command acknowledged. |
| **`0x87`** | `cmd = 0x07` | Flash erase finished (`rx[3] == 0` $\rightarrow$ clears `tft + 0x3e = 0`, sends USB event `05 2C 00`). |
| **`0x88`** | `cmd = 0x08` | Returns TFT firmware version (`rx[4..5]`), SPI flash version (`rx[6..7]`), flash flag (`rx[8]`), mode (`rx[9]`), and descriptor (`rx[10..19]`). |
| **`0x89`** | `cmd = 0x09` | RTC clock sync acknowledged. |
| **`0x8A`** | `cmd = 0x0A` | Raw SPI flash upgrade ready (`rx[3] == 0` $\rightarrow$ sets USB reply `[0xB2, 0x01]`, `tft + 0xe9 = 1`). |
| **`0xB0` / `0xB1`** | `cmd = 0x30` / `0x31` | Sleep / Wake acknowledged. |
| **`0xBB..0xBF`** | `cmd = 0x3B..0x3F` | TFT Bootloader ACKs (`0xBF`=Entered Boot, `0xBE`=Boot Ready, `0xBD`=Start OK, `0xBC`=Chunk OK, `0xBB`=Checksum Verified). |

---

## 5. Complete Decompiled Menu State Machine (`FUN_080159d8`)

In stock firmware, menu navigation uses 3 actions stored in `*(uint8_t *)(0x200146a3 + 0x3d)`:
- **`act = 1` (PREV / CCW / Left)**: Keycode `0x010313`
- **`act = 2` (NEXT / CW / Right)**: Keycode `0x020313`
- **`act = 3` (OK / Press)**: Keycode `0x030313`
- **Quick Picture Slot Cycle**: Keycode `0x000013` (cycles `0x0000` Home $\rightarrow$ populated slots `0..5` in `0x0002` PicView $\rightarrow$ `0x0000` Home).
- **15-Second Menu Timeout**: `tft + 0x31` increments every 1 ms while `tft + 0x35 == 1`; at `15000` ms it restores `menu_cursor = return_cursor` (`0x0000` or `0x0002`) and `pic_slot = return_pic_slot`.

| Cursor (`u16`) | Screen / Menu Node | `act = 1` (PREV / CCW) | `act = 2` (NEXT / CW) | `act = 3` (OK / Press) |
|---|---|---|---|---|
| **`0x0000`** | **Home Screen** (Clock, Battery, Mode, Locks) | Last populated slot in `0x0002` (or `0x0000` if none) | First populated slot in `0x0002` (or `0x0001` if none) | Enter Menu (`0x0001`, sets `timeout_active = 1`) |
| **`0x0002`** | **Picture/GIF View** (Full-screen 128$\times$128 slot `0..5`) | Previous populated slot (or `0x0000` Home if at start) | Next populated slot (or `0x0001` Menu if at end) | Enter Menu (`0x0001`) |
| **`0x0001`** | **Top Menu 1: Picture Select** | Return to `0x0002` (last slot) or `0x0000` | `0x0003` (Sidelight Root) | `0x0011` (Picture Submenu) |
| **`0x0011`** | **Picture Submenu** | `0x0001` | `0x0001` | Load slot & exit (`trigger = 1`) |
| **`0x0003`** | **Top Menu 2: Sidelight** | `0x0001` (Picture) | `0x0004` (Main RGB) | `0x0013` (Side Effect) |
| **`0x0013`** | Sidelight $\rightarrow$ Effect | `0x0053` (Back) | `0x0023` (Color) | `0x0113` (Edit Side FX) |
| **`0x0023`** | Sidelight $\rightarrow$ Color | `0x0013` (Effect) | `0x0033` (Brightness) | `0x0123` (Edit Side Color) |
| **`0x0033`** | Sidelight $\rightarrow$ Brightness | `0x0023` (Color) | `0x0043` (Speed) | `0x0133` (Edit Side Brightness) |
| **`0x0043`** | Sidelight $\rightarrow$ Speed | `0x0033` (Brightness) | `0x0053` (Back) | `0x0143` (Edit Side Speed) |
| **`0x0053`** | Sidelight $\rightarrow$ Back | `0x0043` (Speed) | `0x0013` (Effect) | `0x0003` (Sidelight Root) |
| **`0x0113`** | **Edit Side Effect** | Prev in `[0, 1, 2, 3, 4, 5]` | Next in `[0, 1, 2, 3, 4, 5]` | `0x0013` |
| **`0x0123`** | **Edit Side Color** | `(color - 1) % 9` (skip if FX==3) | `(color + 1) % 9` (skip if FX==3) | `0x0023` |
| **`0x0133`** | **Edit Side Brightness** | Clamp `bright - 1` (`0..4`) | Clamp `bright + 1` (`0..4`) | `0x0033` |
| **`0x0143`** | **Edit Side Speed** | Clamp `speed - 1` (`0..3`) | Clamp `speed + 1` (`0..3`) | `0x0043` |
| **`0x0004`** | **Top Menu 3: Main RGB** | `0x0003` (Sidelight) | `0x0005` (OS Mode) | `0x0014` (RGB Effect) |
| **`0x0014`** | Main RGB $\rightarrow$ Effect | `0x0054` (Back) | `0x0024` (Color) | `0x0114` (Edit RGB FX) |
| **`0x0024`** | Main RGB $\rightarrow$ Color | `0x0014` (Effect) | `0x0034` (Brightness) | `0x0124` (Edit RGB Color) |
| **`0x0034`** | Main RGB $\rightarrow$ Brightness | `0x0024` (Color) | `0x0044` (Speed) | `0x0134` (Edit RGB Brightness) |
| **`0x0044`** | Main RGB $\rightarrow$ Speed | `0x0034` (Brightness) | `0x0054` (Back) | `0x0144` (Edit RGB Speed) |
| **`0x0054`** | Main RGB $\rightarrow$ Back | `0x0044` (Speed) | `0x0014` (Effect) | `0x0004` (Main RGB Root) |
| **`0x0114`** | **Edit RGB Effect** | Prev in 18-FX table `[1..11, 19, 12, 14..18]` | Next in 18-FX table `[1..11, 19, 12, 14..18]` | `0x0014` |
| **`0x0124`** | **Edit RGB Color** | `(color - 1) % 9` | `(color + 1) % 9` | `0x0024` |
| **`0x0134`** | **Edit RGB Brightness** | Clamp `bright - 1` (`0..4`) | Clamp `bright + 1` (`0..4`) | `0x0034` |
| **`0x0144`** | **Edit RGB Speed** | Clamp `speed + 1` (`0..4`, inverted) | Clamp `speed - 1` (`0..4`, inverted) | `0x0044` |
| **`0x0005`** | **Top Menu 4: OS Mode** | `0x0004` (Main RGB) | `0x0006` (Language) | `0x0015` (Edit OS Mode) |
| **`0x0015`** | **Edit OS Mode** | Toggle Win (`0`) $\leftrightarrow$ Mac (`1`) | Toggle Win (`0`) $\leftrightarrow$ Mac (`1`) | `0x0005` |
| **`0x0006`** | **Top Menu 5: Language** | `0x0005` (OS Mode) | `0x0007` (TFT Brightness) | `0x0016` (Edit Language) |
| **`0x0016`** | **Edit Language** | Toggle `tft + 0x1e` (`0` $\leftrightarrow$ `1`), `lang_dirty = 1` | Toggle `tft + 0x1e` (`0` $\leftrightarrow$ `1`), `lang_dirty = 1` | `0x0006` |
| **`0x0007`** | **Top Menu 6: TFT Brightness** | `0x0006` (Language) | Exit to `return_cursor` (`0x0000` or `0x0002`) | `0x0017` (Edit TFT Brightness) |
| **`0x0017`** | **Edit TFT Brightness** | Toggle `tft + 0x20` (`p[4] ^= 1`, step down) | Toggle `tft + 0x1f` (`p[5] ^= 1`, step up) | `0x0007` |

---

## 6. Stock Main RGB Effect ID Table (`0x2000a0e8`)

The TFT screen's internal UI has 18 pre-rendered effect names/icons for Main RGB (`cmd = 0x01`, `p[8]`). Decompressing the stock Keil `.data` section (`0x0802070c` $\rightarrow$ `0x20000000`) revealed the exact 18-byte lookup table at `0x2000a0e8`:

| Index (`0..17`) | Stock Effect ID (`p[8]`) | Effect Name |
|---|---|---|
| `0` | `1` | Constant (Static) |
| `1` | `2` | Breathing |
| `2` | `3` | Neon |
| `3` | `4` | Wave |
| `4` | `5` | Ripple |
| `5` | `6` | Raindrop |
| `6` | `7` | Snake |
| `7` | `8` | Reactive (Press Action) |
| `8` | `9` | Converge |
| `9` | `10` | Sine Wave |
| `10` | `11` | Kaleidoscope |
| `11` | `19` | Reactive Off (Press Action Off) |
| `12` | `12` | Line Wave |
| `13` | `14` | Laser |
| `14` | `15` | Circle Wave |
| `15` | `16` | Rainbow (Dazzling) |
| `16` | `17` | Rain Down |
| `17` | `18` | Meteor |

---

## 7. Decompiled TFT Co-Processor (`U24` = YC3123-Q / `RY5060`) & `U9` SPI Flash Map

Recovered and decompiled from official GamaKay `TK75_DE_TMR_6.3.zip` (`ID3591_RY5088_JK_TK75-V6_UK_V404H_LCD_V102_FLASH_V100_UP`):
- `resources/x5` $\rightarrow$ `x5_plain.bin` (`349,260 B`): Full `YC3123-Q` (ARM Cortex-M0, 512 KB internal XIP flash at `0x01000000`) image:
  - `0x01000000 .. 0x01002FFF` (~12 KB): Bootloader (`SP = 0x00030000`, `Reset = 0x010002CD`).
  - `0x01010000 .. 0x0101000F`: 16-byte app magic `"RY5060RY5060\0\0\0\0"`.
  - `0x01010010 .. 0x01016FFF` (~28 KB): Application code (`SP = 0x00070000`, `Reset = 0x010102DD`).
  - `0x01017000 .. 0x0105544B` (~250 KB): **All menu UI icons, digits, battery/mode glyphs, and EN/CN labels (RGB565 big-endian) embedded directly in internal flash** — NOT in `U9`!
- `resources/x7` $\rightarrow$ `x7_plain.bin` (`36,812 B`): Entire `FLASH_V100` image written to `U9 @ 0x000000..0x008FCB` (just a 45$\times$409 factory test banner + 2-byte version `01 00` at `0x008FCA`).

### 7.1 Hardware Peripherals on `U24` (`YC3123-Q`)
- **Display**: 128$\times$128 **ST7735S** SPI TFT (`0x3A=0x05` RGB565, `0x36=0x88` MADCTL, `0x21` INVON, window `X=2..129`, `Y=3..130`), flushed via DMA from a 32 KB (`0x8000 B`) RAM framebuffer at `0x0002A54C` (`FUN_010169b8`).
- **UART to Main MCU**: 912,600 bps (`DAT_01016148 = 0x000DECD8`, 48 MHz peripheral clock).
- **External RTC (`U23`)**: I2C BM8563/PCF8563 (regs `0x02..0x08`, set via `cmd = 0x09` `FUN_01011098`, polled by `FUN_01012934`).
- **External SPI NOR Flash (`U9` = Zbit `ZB25VQ64`, 8 MB)**:

| `U9` Flash Address | Size | Contents |
|---|---|---|
| `0x000000 .. 0x008FCB` | 36,812 B | `x7_plain.bin` (`FLASH_V100`): factory test banner + 2-byte version at `0x008FCA` (`01 00` $\rightarrow$ `0x0100`). Written by `cmd = 0x0A..0x0C` (`FUN_010109c8`). |
| `0x100000 .. 0x1000FF` | 256 B | Persistent TFT config page (magic `0xAB 0x57` at bytes `0xFE..0xFF`, `FUN_01012594`/`FUN_0101275c`): 6$\times$7-byte slot headers (42 B), active slot, page, year (`0x07E6` default), language (`0`=EN, `1`=CN), backlight level (`0..4`, default `4`). |
| `0x101000 .. 0x7CFFFF` | ~7 MB | **User Slot 5 (GIF animation frames)**, 4 KB-aligned per frame (`FUN_01010cbc` / `FUN_01012488`). |
| `0x7D0000 .. 0x7FFFFF` | 192 KB | **User Slots 0..4 (5 static 128$\times$128 RGB565 images)**, `0x8000` (32 KB) each (`0x7D0000 + slot * 0x8000`). |

### 7.2 Complete `YC3123-Q` UART App Command Table (`FUN_01012810`)
Every valid `cmd` byte accepted by stock app firmware (`0x55 len_lo len_hi cmd payload... sum8`):

| `cmd` | App Handler | Function |
|---|---|---|
| `0x01` | `FUN_01011664` | Menu/control state + edge-triggered brightness (`p[4]` down, `p[5]` up, `0..4`) + language toggle + RGB/Side/Vol. |
| `0x02` | `FUN_0101172c` | Live status (mode, LEDs, battery, `p[7..8]` CPU/RAM % shown on page `0x0011`, `p[9]` lang toggle, `p[10]` OS, `p[11]` winlock). |
| `0x03` | `FUN_010115bc` | Start user image/GIF upload (`slot, frame_idx, total_frames, delay, frame_bytes_le16, 0, 0, x0, y0, x1, y1`). |
| `0x04` | `FUN_01011364` | User image/GIF 56-byte data chunk (`frame_idx, 0, 0, pkt_idx_le16, chunk_len, 0, data[56]`). |
| `0x05` | `FUN_0101151c` | Commit user image/GIF metadata to `U9 @ 0x100000` and switch to page `0x0002`. |
| `0x06` | `FUN_0101061c` | Erase all user images/GIFs (`0x100000..0x7FFFFF`, 224$\times$32 KB blocks with 0..100% progress bar). |
| `0x07` | `FUN_01015490` | Poll erase busy (`status = 0x7F` while erasing, `0x00` idle). |
| `0x08` | `FUN_010155d4` | Get version (`oled_ver = 0x0102`, `flash_ver = U9@0x8FCA`, `0x0001`, + 12 B ID). |
| `0x09` | `FUN_01011098` | Set external RTC `U23` (`year_le16, mon, day, hour, min, sec`). |
| `0x0A` | `FUN_01011328` | Start `U9 @ 0x000000` system flash write (`total_56B_pkts_le16`). |
| `0x0B` | `FUN_01011184` | `U9 @ 0x000000` system flash 56-byte chunk write. |
| `0x0C` | `FUN_0101130c` | Commit `U9 @ 0x000000` write and reload `flash_ver`. |
| `0x30` / `0x31` | `FUN_0101543c` | Screen sleep (blank) / wake. |
| `0x3E` | `FUN_0101543c` | Ping / ACK (no reset). |
| `0x3F` | `FUN_010154e8` | **Enter Bootloader**: writes `0xAA55AA55` to internal flash `0x0107F000`, turns off backlight, resets to `0x01000000`. |

### 7.3 Open / Unsigned `YC3123-Q` Bootloader DFU Protocol (`0x01000648`)
The bootloader has **no cryptographic signature or encryption check** — only a 32-bit byte sum and the 12-byte magic `"RY5060RY5060"`:
- **`0x3D`**: Start app flash (`p[0..1]` = `chunk_count` of 64-byte blocks); erases `0x01010000` (`11 * 32 KB = 352 KB`).
- **`0x3C`**: Write 64 raw bytes to app flash, read back to verify, accumulate unsigned 32-bit byte sum.
- **`0x3B`**: Commit (`p[0..3]` = expected LE32 byte sum); verifies sum + `"RY5060RY5060"` header $\rightarrow$ erases `0x0107F000` flag $\rightarrow$ reboots into custom app!
- **`0x08` / `0x3E` / `0x3F`**: Bootloader version / ping / ACK.
- **Full zero-soldering rollback**: Reflashing `x5_plain.bin[0x10000:]` over UART7 restores 100% stock TFT firmware and menus.

### 7.4 Live Hardware Verification on Connected TK75 HE V2 (`cmd = 0x08`)
- Raw response (`len = 19`): `00 01 01 ff ff 01 00 52 59 35 30 36 30 52 59 35 30 36 30`
- `status = 0x00`, `oled_ver = 0x0101` (`LCD_V101`), `flash_ver = 0xFFFF` (`U9 @ 0x8FCA` blank from factory), `proto = 0x0001`.
- 12-byte ID: **`RY5060RY5060`** — **100% exact match** with `x5_plain.bin @ 0x10000`.

---

## 8. Hybrid Custom `U24` Patch (`0x20..0x25`, Live 128×128 Framebuffer Control)

Appended at file offset `0x55500` (`CPU VA = 0x01055700`, `XIP_BASE = 0x01000200`) on stock `LCD_V102` (`backups/stock_u24_lcd_v102_x5.bin`), built by `tools/tft_patch/build_patched_u24.py` and flashed via `tools/tk75_tft_flash.py` (AT32 VIA Ext `ch=8, val_id=7` raw UART7 bridge).

- Stock init, UART7 `912,600` bps, ST7735S DMA flush, watchdog, and `cmd = 0x3F` bootloader entry remain **100% stock**.
- Hook 1 (`VA 0x01012A60`, unknown-cmd branch) $\rightarrow$ `BL 0x01055700` (`custom_cmd_hook`).
- Hook 2 (`VA 0x01016CA2`, main loop UI calls) $\rightarrow$ `BL 0x01055704` (`custom_ui_hook`: skips stock menu/home renderers only when `*(u32 *)0x00036000 == 0xCAFE5060`).

| `cmd` | Payload | Action |
|---|---|---|
| `0x20` | `[mode]` (`0`=stock, `1`=manual) | Toggle manual framebuffer mode (restores stock UI redraw on `0`). |
| `0x21` | `[x, y, w, h, rgb565_hi, rgb565_lo, flush]` | Fill rectangle directly in 32 KB RAM framebuffer (`0x0002A54C`). |
| `0x22` | `[x, y, w, h, flags, px_hi, px_lo, ...]` | Blit raw BE RGB565 pixels (up to 44 px/pkt; `bit0`: col-major, `bit1`: flush). |
| `0x23` | `[x, y, fg_hi, fg_lo, bg_hi, bg_lo, flags, ascii...]` | Draw ASCII text using ROM 6×12 font (`0x01016D3A`); `bits 0..1` = scale `1..3`, `bit6` = transparent bg, `bit7` = flush. |
| `0x24` | `[level]` (`0..4` or `0xFF`=flush only) | Set PWM backlight level (`PWM_TBL[level]`) and flush framebuffer. |
| `0x25` | `[]` | Query custom patch version (`'D' 0x05 0xA5 0x00 major minor sum`). |
| *(Special)* | `13` | User Picture (Custom Per-Key RGB) |
| *(Special)* | `20` / `22` | Music Visualizer (Audio Reactive) |
| *(Special)* | `21` | Screen Color Sync |

---

## 9. Custom `U24` Hybrid Patch v1.2 (`tools/tft_patch/`)

To enable full tear-free RAM framebuffer rendering without touching stock `U24` startup, ST7735S init, watchdog, or `cmd=0x3F` bootloader DFU, [tools/tft_patch/u24_patch.c](../tools/tft_patch/u24_patch.c) appends a `~1.3 KB` Cortex-M0 payload at `VA 0x01055700` (`file 0x55500`) on top of stock `LCD_V102` (`backups/stock_u24_lcd_v102_x5.bin`) with two verified Thumb `BL` hooks plus a 2-byte `RASET` alignment fix:

| Hook / Patch Site | File Offset | CPU VA (`file + 0x01000200`) | Purpose |
|---|---|---|---|
| Unknown-cmd handler | `0x12860` | `0x01012A60` | `BL 0x01055700` (`custom_cmd_hook`) — handles `0x20..0x26`, preserves stock error flag otherwise |
| Stock UI main loop | `0x16AA2` | `0x01016CA2` | `BL 0x01055704` (`custom_ui_hook`) — skips stock menu/home redraw when `*(u32*)0x00036000 == 0xCAFE5060` |
| ST7735S flush `RASET` | `0x169C6`, `0x169D2` | `0x01016BC6`, `0x01016BD2` | Shifts `RASET` from `3..130` → `1..128` (`0x01..0x80`) to eliminate edge offset noise bars |

### Custom Commands (`0x20..0x26`, ACK = `cmd | 0x80`)

| Cmd | Payload | Behavior |
|---|---|---|
| `0x20` | `[mode]` | `1`: enable manual RAM framebuffer (`0xCAFE5060` @ `0x00036000`) & clear full `132x132` ST7735S GRAM edges to `0x0862`; `0`: restore stock UI (`PREV_PAGE = 0xFF`) |
| `0x21` | `[x, y, w, h, rgb565_hi, rgb565_lo, flush]` | Fill rectangle directly in `0x0002A54C` RAM framebuffer (zero flash wear); sets `FB_DIRTY` if `flush != 0` |
| `0x22` | `[x, y, w, h, flags, px...]` | Blit raw big-endian RGB565 pixels (bit0: column-major, bit1: flush) |
| `0x23` | `[x, y, fg_hi, fg_lo, bg_hi, bg_lo, flags, ascii...]` | Draw ASCII string using stock ROM 6×12 font (`0x01016D3A`); `flags[1:0]` = scale `1..3`, bit6 = transparent bg, bit7 = flush |
| `0x24` | `[pct, 1]` / `[0..4]` / `[0xFF]` | `[pct, 1]`: smooth `0..100%` backlight PWM (`pct * 300`, max `30000`, no LCD flush); `[0..4]`: stock PWM table; `[0xFF]`: flush framebuffer only |
| `0x25` | *(empty)* | Returns patch version reply `cmd=0xA5`, payload `[status=0x00, major=0x01, minor=0x04]` |
| `0x26` | `[flags, count, (x, y, w, h, hi, lo) * count]` | Atomic multi-rectangle fill batch (up to 15 rects/packet); bit0 of `flags` triggers `FB_DIRTY` |
| `0x28` | `[op, addr_le24, ...]` | Safe `U9` (`ZB25VQ64`) GIF flasher strictly locked to `0x006C0000..0x007BFFFF` (reply `0xA8`): `op=0` erase 32 KB block; `op=1` page-program `1..88 B` (no 256 B page crossing); `op=2` read `1..16 B`; `op=3` compute IEEE CRC32 over `[addr, addr+len_le32)` |
| `0x29` | `[op, (frame_idx)]` | `U9` Custom Animated GIF player (reply `0xA9`): `op=0` reads `TKGF` v1/v2 header @ `0x006C0000`, caches per-frame delays in SRAM (`0x00036200`), returns `[0x00, valid, frame_count, d0_lo, d0_hi]`; `op=1` DMA-reads frame `frame_idx` (`0x6C8000 + idx*0x8000`, 32 KB) into `FB_BASE_ADDR`, sets `FB_DIRTY=1`, and returns `[0x00, idx, delay_lo, delay_hi]` |

### `U9` `TKGF` v2 Custom Animated GIF Format (`0x006C0000 .. 0x007BFFFF`)

- **Block 0 (`0x006C0000`, 94-byte header)**:
  - `[0..3]`: Magic `"TKGF"`
  - `[4]`: Version (`1` = uniform `delay_ms`; `2` = per-frame `frame_delays_ms[31]`)
  - `[5]`: `frame_count` (`1..31`)
  - `[6..7]`: `delay_ms` LE16 default/frame-0 interval (`33..10000 ms`)
  - `[8..11]`: `data_crc32` LE32 IEEE CRC32 over `frame_count * 32768` bytes
  - `[12..15]`: Reserved (`0`)
  - `[16..31]`: NUL-padded ASCII label (up to 15 chars)
  - `[32..93]`: `frame_delays_ms[31]` LE16 per-frame hold durations (`33..10000 ms`, v2)
- **Blocks 1..31 (`0x006C8000 + idx * 0x8000`)**: Raw 32,768-byte `128×128` column-major big-endian RGB565 frames matching `FB_BASE_ADDR` (`0x0002A54C`) directly for zero-conversion DMA playback.

### Host & Firmware Integration

- **VIA Raw UART7 Bridge**: `ch=8, val_id=7` (`op=0` pause/resume HUD, `op=1` stage chunk, `op=2` inline send, `op=3` stage+send, GET returns last RX frame).
- **Flasher CLI**: `.venv/bin/python tools/tk75_tft_flash.py --info | --flash <bin> [--dry-run]`
- **Custom GIF CLI**: `.venv/bin/python tools/tk75_tft_gif.py upload <file.gif> [--preview 6] | info | preview | set-mode <mode> | clear`
- **Live Draw CLI**: `.venv/bin/python tools/tk75_tft_draw.py demo | text | fill | image | brightness | stock`
- **Onboard Multi-Theme HUD**: [src/tft/pixel_hud.c](../src/tft/pixel_hud.c) auto-probes `0x25` every `100 ms` at boot, activates automatically on `0xA5` reply, renders all 7 pages across 3 persistent skins (`PIXEL`, `STOCK+`, `TERM`) plus 6 idle screensavers (`CLOCK`, `EYES`, `CAT`, `MATRIX`, `DVD`, `GIF`), and falls back to stock `hud_engine` after `900 ms` if running unmodified stock `U24`.
