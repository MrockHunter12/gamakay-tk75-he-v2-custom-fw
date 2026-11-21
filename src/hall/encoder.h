#ifndef ENCODER_H
#define ENCODER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  int8_t delta;        /* Accumulated detent clicks (+1 CW, -1 CCW) */
  bool   btn_pressed;  /* Debounced encoder push switch state */
} encoder_events_t;

typedef struct {
  uint8_t  prev_ab;
  uint8_t  seq;
  int8_t   accum_steps;
  uint8_t  btn_debounce;
  bool     btn_state;
} encoder_state_t;

/** Initialize quadrature & button decoder state (HW-free). */
void encoder_state_init(encoder_state_t *st, bool a_high, bool b_high);

/** Step quadrature + active-LOW button state machine (call at >= 1 kHz). Returns +1/-1 detent or 0. */
int8_t encoder_state_step(encoder_state_t *st, uint8_t ab_bits, bool btn_active);

#ifdef AT32F405RCT7
/** Configure PC11 (A), PC10 (B), PC12 (Push, pull-up) and sample initial state. */
void encoder_hw_init(void);

/** Sample PC11/PC10/PC12 at 1 kHz from SysTick ISR with A/B deglitch + push debounce. */
void encoder_hw_sample_isr(void);

/** Drain pending ISR-decoded rotation detents and debounced push state. */
encoder_events_t encoder_hw_poll(void);

/** Return diagnostic encoder bitmask: bit0=A, bit1=B, bit2=btn_low, bit3=btn_state. */
uint8_t encoder_hw_diag_bits(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ENCODER_H */
