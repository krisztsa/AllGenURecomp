// Controller state the game reads (runtime/hle/input.c), set by the window loop.
#pragma once
#include <stdint.h>

// Switch button bits (NpadButton): A 0, B 1, X 2, Y 3, LStick 4, RStick 5, L 6, R 7, ZL 8, ZR 9, Plus 10, Minus 11,
// Left 12, Up 13, Right 14, Down 15. Sticks: -32767..32767, y up.
enum { BTN_A, BTN_B, BTN_X, BTN_Y, BTN_LSTICK, BTN_RSTICK, BTN_L, BTN_R, BTN_ZL, BTN_ZR, BTN_PLUS, BTN_MINUS,
       BTN_LEFT, BTN_UP, BTN_RIGHT, BTN_DOWN };
void input_set(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry);
void input_get(uint64_t *buttons, int32_t sticks[4]);  // the controller state the game sees
extern volatile uint64_t g_a_pressed_ns;  // when A was last pressed (plat_now_ns; 0: never)
