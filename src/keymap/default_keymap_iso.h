#ifndef DEFAULT_KEYMAP_ISO_H
#define DEFAULT_KEYMAP_ISO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* QMK / VIA Standard Keycode Definitions */
#define KC_NO        0x0000U
#define KC_TRNS      0x0001U
#define KC_A         0x0004U
#define KC_B         0x0005U
#define KC_C         0x0006U
#define KC_D         0x0007U
#define KC_E         0x0008U
#define KC_F         0x0009U
#define KC_G         0x000AU
#define KC_H         0x000BU
#define KC_I         0x000CU
#define KC_J         0x000DU
#define KC_K         0x000EU
#define KC_L         0x000FU
#define KC_M         0x0010U
#define KC_N         0x0011U
#define KC_O         0x0012U
#define KC_P         0x0013U
#define KC_Q         0x0014U
#define KC_R         0x0015U
#define KC_S         0x0016U
#define KC_T         0x0017U
#define KC_U         0x0018U
#define KC_V         0x0019U
#define KC_W         0x001AU
#define KC_X         0x001BU
#define KC_Y         0x001CU
#define KC_Z         0x001DU
#define KC_1         0x001EU
#define KC_2         0x001FU
#define KC_3         0x0020U
#define KC_4         0x0021U
#define KC_5         0x0022U
#define KC_6         0x0023U
#define KC_7         0x0024U
#define KC_8         0x0025U
#define KC_9         0x0026U
#define KC_0         0x0027U
#define KC_ENT       0x0028U
#define KC_ESC       0x0029U
#define KC_BSPC      0x002AU
#define KC_TAB       0x002BU
#define KC_SPC       0x002CU
#define KC_MINS      0x002DU
#define KC_EQL       0x002EU
#define KC_LBRC      0x002FU
#define KC_RBRC      0x0030U
#define KC_BSLS      0x0031U
#define KC_NUHS      0x0032U /* ISO # / ~ */
#define KC_SCLN      0x0033U
#define KC_QUOT      0x0034U
#define KC_GRV       0x0035U
#define KC_COMM      0x0036U
#define KC_DOT       0x0037U
#define KC_SLSH      0x0038U
#define KC_CAPS      0x0039U
#define KC_F1        0x003AU
#define KC_F2        0x003BU
#define KC_F3        0x003CU
#define KC_F4        0x003DU
#define KC_F5        0x003EU
#define KC_F6        0x003FU
#define KC_F7        0x0040U
#define KC_F8        0x0041U
#define KC_F9        0x0042U
#define KC_F10       0x0043U
#define KC_F11       0x0044U
#define KC_F12       0x0045U
#define KC_PSCR      0x0046U
#define KC_HOME      0x004AU
#define KC_PGUP      0x004BU
#define KC_DEL       0x004CU
#define KC_END       0x004DU
#define KC_PGDN      0x004EU
#define KC_RGHT      0x004FU
#define KC_LEFT      0x0050U
#define KC_DOWN      0x0051U
#define KC_UP        0x0052U
#define KC_NUBS      0x0064U /* ISO < > | */

/* Consumer & System keycodes in QMK space */
#define KC_PWR       0x00A5U
#define KC_SLEP      0x00A6U
#define KC_WAKE      0x00A7U
#define KC_MUTE      0x00A8U
#define KC_VOLU      0x00A9U
#define KC_VOLD      0x00AAU
#define KC_MNXT      0x00ABU
#define KC_MPRV      0x00ACU
#define KC_MSTP      0x00ADU
#define KC_MPLY      0x00AEU
#define KC_BRID      0x00BDU
#define KC_BRIU      0x00BEU

/* Modifiers */
#define KC_LCTL      0x00E0U
#define KC_LSFT      0x00E1U
#define KC_LALT      0x00E2U
#define KC_LGUI      0x00E3U
#define KC_RCTL      0x00E4U
#define KC_RSFT      0x00E5U
#define KC_RALT      0x00E6U
#define KC_RGUI      0x00E7U

/* QMK Layer & Special Keycode Ranges */
#define QK_TO_BASE   0x5200U
#define QK_MO_BASE   0x5220U
#define QK_TG_BASE   0x5240U
#define TO(l)        (uint16_t)(QK_TO_BASE | ((l) & 0x1FU))
#define MO(l)        (uint16_t)(QK_MO_BASE | ((l) & 0x1FU))
#define TG(l)        (uint16_t)(QK_TG_BASE | ((l) & 0x1FU))

#define QK_MACRO_BASE 0x7700U
#define MACRO(n)      (uint16_t)(QK_MACRO_BASE | ((n) & 0x0FU))

#define RGB_TOG      0x7820U
#define RGB_MOD      0x7821U
#define RGB_RMOD     0x7822U
#define RGB_VAI      0x7827U
#define RGB_VAD      0x7828U
#define RGB_SPI      0x7829U
#define RGB_SPD      0x782AU
#define QK_BOOT      0x7C00U

/* Custom TK75 VIA User Keycodes (USER00..USER13 = 0x7E00..0x7E0D) */
#define QK_TK75_USER_BASE   0x7E00U
#define KC_TFT_NEXT         0x7E00U /* Cycle TFT HUD page */
#define KC_SOCD_CYCLE       0x7E01U /* Cycle SOCD mode (Off/Last/Neutral/First) */
#define KC_RT_TOG           0x7E02U /* Toggle Rapid Trigger */
#define KC_ACT_UP           0x7E03U /* Actuation +0.10 mm (deeper) */
#define KC_ACT_DN           0x7E04U /* Actuation -0.10 mm (faster) */
#define KC_WIN_LOCK         0x7E05U /* Toggle Win-key lock */
#define KC_TFT_TOG          0x7E06U /* Toggle TFT display power */
#define KC_ENC_MODE_NEXT    0x7E07U /* Cycle encoder knob mode */
#define KC_BT1              0x7E08U /* Select Bluetooth host slot 1 */
#define KC_BT2              0x7E09U /* Select Bluetooth host slot 2 */
#define KC_BT3              0x7E0AU /* Select Bluetooth host slot 3 */
#define KC_RF_24G           0x7E0BU /* Select 2.4 GHz wireless mode */
#define KC_RF_PAIR          0x7E0CU /* Enter BLE / 2.4 GHz pairing mode */
#define KC_MAC_TOG          0x7E0DU /* Toggle Windows/Linux vs macOS mode */
#define KC_TFT_PREV         0x7E0EU /* TFT HUD/Menu Previous */
#define KC_TFT_OK           0x7E0FU /* TFT HUD/Menu Enter / Confirm */
#define KC_MACRO_SPD_DN     0x7E10U /* Macro playback speed slower (more delay) */
#define KC_MACRO_SPD_UP     0x7E11U /* Macro playback speed faster (less delay) */
#define QK_TK75_USER_MAX    0x7E11U

/** Populate default 4-layer 6x16 ISO keymap (verified against tk75hev2_iso_v506_plain). */
void keymap_copy_default_layers(uint16_t dst[4][6][16]);

/** Populate default 4-layer [CCW, CW] encoder bindings. */
void keymap_copy_default_encoders(uint16_t dst[4][2]);

#ifdef __cplusplus
}
#endif

#endif /* DEFAULT_KEYMAP_ISO_H */
