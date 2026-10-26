// Run-time support functions of the SDK replaced by host code: integer division (the SDK's compiler-rt versions
// are unrolled loops entered by computed jumps) and the memory / string primitives every subsystem leans on.
#include <string.h>

#include "../hle.h"

// ---- 32-bit division (AEABI: quotient in r0, remainder in r1). Division by zero returns 0, as the SDK's
// __aeabi_idiv0 does.
static void udivmod32(Cpu *c) {
    uint32_t n = ARG(0), d = ARG(1);
    c->r[0] = d ? n / d : 0;
    c->r[1] = d ? n % d : 0;
}
static void sdivmod32(Cpu *c) {
    int32_t n = (int32_t)ARG(0), d = (int32_t)ARG(1);
    if (!d) { c->r[0] = c->r[1] = 0; return; }
    if (n == INT32_MIN && d == -1) { c->r[0] = (uint32_t)n; c->r[1] = 0; return; }
    c->r[0] = (uint32_t)(n / d);
    c->r[1] = (uint32_t)(n % d);
}
HLE_SDK("__aeabi_uidiv", __aeabi_uidiv) { udivmod32(c); }
HLE_SDK("__aeabi_uidivmod", __aeabi_uidivmod) { udivmod32(c); }
HLE_SDK("__udivsi3", __udivsi3) { udivmod32(c); }
HLE_SDK("__umodsi3", __umodsi3) { udivmod32(c); c->r[0] = c->r[1]; }
HLE_SDK("__aeabi_idiv", __aeabi_idiv) { sdivmod32(c); }
HLE_SDK("__aeabi_idivmod", __aeabi_idivmod) { sdivmod32(c); }
HLE_SDK("__divsi3", __divsi3) { sdivmod32(c); }
HLE_SDK("__modsi3", __modsi3) { sdivmod32(c); c->r[0] = c->r[1]; }

// ---- 64-bit division (n in r0:r1, d in r2:r3; AEABI: quotient r0:r1, remainder r2:r3)
static void udivmod64(Cpu *c) {
    uint64_t n = ARG64(0), d = ARG64(2), q = d ? n / d : 0, r = d ? n % d : 0;
    c->r[0] = (uint32_t)q; c->r[1] = (uint32_t)(q >> 32);
    c->r[2] = (uint32_t)r; c->r[3] = (uint32_t)(r >> 32);
}
static void sdivmod64(Cpu *c) {
    int64_t n = (int64_t)ARG64(0), d = (int64_t)ARG64(2), q, r;
    if (!d) q = r = 0;
    else if (n == INT64_MIN && d == -1) q = n, r = 0;
    else q = n / d, r = n % d;
    c->r[0] = (uint32_t)q; c->r[1] = (uint32_t)((uint64_t)q >> 32);
    c->r[2] = (uint32_t)r; c->r[3] = (uint32_t)((uint64_t)r >> 32);
}
HLE_SDK("__aeabi_uldivmod", __aeabi_uldivmod) { udivmod64(c); }
HLE_SDK("__aeabi_ldivmod", __aeabi_ldivmod) { sdivmod64(c); }
HLE_SDK("__udivdi3", __udivdi3) { udivmod64(c); }
HLE_SDK("__divdi3", __divdi3) { sdivmod64(c); }
HLE_SDK("__umoddi3", __umoddi3) { udivmod64(c); c->r[0] = c->r[2]; c->r[1] = c->r[3]; }
HLE_SDK("__moddi3", __moddi3) { sdivmod64(c); c->r[0] = c->r[2]; c->r[1] = c->r[3]; }
HLE_SDK("__udivmoddi4", __udivmoddi4) {  // (a, b, uint64_t *rem): rem pointer on the stack
    uint32_t remp = STK(0);
    udivmod64(c);
    if (remp) { ST32(remp, c->r[2]); ST32(remp + 4, c->r[3]); }
}

// ---- memory / strings (guest pointers; host memmove handles any overlap)
// (debug) MHGU_MEMCPY_WATCH=lo-hi (hex): log copies whose source lies in [lo, hi) (the first 200)
static void memcpy_watch(Cpu *c) {
    static uint32_t lo, hi;
    static int state = -1, n;
    if (state < 0) state = getenv("MHGU_MEMCPY_WATCH") && sscanf(getenv("MHGU_MEMCPY_WATCH"), "%x-%x", &lo, &hi) == 2;
    if (state && ARG(1) >= lo && ARG(1) < hi && n++ < 200)
        LOG("[memcpy] %x <- %x size %x lr %x\n", ARG(0), ARG(1), ARG(2), c->r[14]);
}
HLE_SDK("memcpy", memcpy) { memcpy_watch(c); memmove(ARGP(0), ARGP(1), ARG(2)); }
HLE_SDK("memmove", memmove) { memmove(ARGP(0), ARGP(1), ARG(2)); }
HLE_SDK("memset", memset) { memset(ARGP(0), (int)ARG(1), ARG(2)); }
HLE_SDK("memcmp", memcmp) { int r = memcmp(ARGP(0), ARGP(1), ARG(2)); RET(r < 0 ? -1 : r > 0); }
HLE_SDK("strlen", strlen) { RET(strlen(gstr(ARG(0)))); }
HLE_SDK("strcmp", strcmp) { int r = strcmp(gstr(ARG(0)), gstr(ARG(1))); RET(r < 0 ? -1 : r > 0); }
HLE_SDK("strncmp", strncmp) { int r = strncmp(gstr(ARG(0)), gstr(ARG(1)), ARG(2)); RET(r < 0 ? -1 : r > 0); }
HLE_SDK("strcpy", strcpy) { strcpy((char *)ARGP(0), gstr(ARG(1))); }
// AEABI forms: (dest, src, n), memset is (dest, n, value), memclr (dest, n); no return value
HLE_SDK("__aeabi_memcpy", __aeabi_memcpy) { memcpy_watch(c); memmove(ARGP(0), ARGP(1), ARG(2)); }
HLE_SDK("__aeabi_memcpy4", __aeabi_memcpy4) { memcpy_watch(c); memmove(ARGP(0), ARGP(1), ARG(2)); }
HLE_SDK("__aeabi_memcpy8", __aeabi_memcpy8) { memcpy_watch(c); memmove(ARGP(0), ARGP(1), ARG(2)); }
HLE_SDK("__aeabi_memmove", __aeabi_memmove) { memmove(ARGP(0), ARGP(1), ARG(2)); }
HLE_SDK("__aeabi_memmove4", __aeabi_memmove4) { memmove(ARGP(0), ARGP(1), ARG(2)); }
HLE_SDK("__aeabi_memmove8", __aeabi_memmove8) { memmove(ARGP(0), ARGP(1), ARG(2)); }
HLE_SDK("__aeabi_memset", __aeabi_memset) { memset(ARGP(0), (int)ARG(2), ARG(1)); }
HLE_SDK("__aeabi_memset4", __aeabi_memset4) { memset(ARGP(0), (int)ARG(2), ARG(1)); }
HLE_SDK("__aeabi_memset8", __aeabi_memset8) { memset(ARGP(0), (int)ARG(2), ARG(1)); }
HLE_SDK("__aeabi_memclr", __aeabi_memclr) { memset(ARGP(0), 0, ARG(1)); }
HLE_SDK("__aeabi_memclr4", __aeabi_memclr4) { memset(ARGP(0), 0, ARG(1)); }
HLE_SDK("__aeabi_memclr8", __aeabi_memclr8) { memset(ARGP(0), 0, ARG(1)); }
