// AArch32 interpreter for guest code that was not recompiled ahead of time: Forge plugins (runtime/forge.c), loaded
// at run time from the mods, and game functions whose code cheats changed (runtime/cheats.c, dispatch_interpret).
// It works on the same Cpu state as the recompiled code and follows the lifter
// (tools/lifter.py, tools/neon.py) instruction by instruction, so both run guest code the same way.
#pragma once
#include "cpu.h"

// Execute the instruction w at pc and return the address of the next one. *link is set for a branch with link
// (BL / BLX: r14 already holds the return address).
uint32_t interp_exec(Cpu *c, uint32_t pc, uint32_t w, int *link);

// Run the function at c->pc until it returns to its caller (c->r[14] on entry). Code inside [lo, hi) of a
// registered range is interpreted; calls out of it (to the game, the SDK, host functions) go through rt_dispatch.
void interp_run(Cpu *c);
void interp_add_range(uint32_t lo, uint32_t hi);  // (one touching or overlapping a registered range extends it)
int interp_owns(uint32_t pc);
