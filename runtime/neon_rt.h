// Advanced SIMD (NEON) helpers for recompiled code: the operations the lifter (tools/neon.py) lowers to calls.
#pragma once
#include "cpu.h"

// ARMv8 AES on q registers given as their first d register number
void neon_aese(Cpu *c, int d, int m);
void neon_aesd(Cpu *c, int d, int m);
void neon_aesmc(Cpu *c, int d, int m);
void neon_aesimc(Cpu *c, int d, int m);
