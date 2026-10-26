// Argument adapter for NVN entry points (nvn.h): AAPCS with the VFP variant, as the game's 32-bit code calls them.
// Core arguments take r0-r3 then the stack (a 64-bit one an even/odd pair, else an 8-byte aligned stack slot, and once
// one argument went to the stack every later core argument does too); float arguments take s0-s15 in order.
#include "nvn.h"

void nvn_args(Cpu *c, const char *sig, NvnArgs *a) {
    uint32_t ncrn = 0, nsaa = c->r[13], ns = 0;
    int nx = 0, nf = 0;
    if (!sig) {  // every argument 32-bit
        for (int i = 0; i < 16; i++) a->x[i] = i < 4 ? c->r[i] : LD32(nsaa + 4u * (uint32_t)(i - 4));
        return;
    }
    for (const char *p = sig; *p && nx < 16; p++) {
        switch (*p) {
        case 'l':
            ncrn = (ncrn + 1) & ~1u;
            if (ncrn <= 2) {
                a->x[nx++] = ((uint64_t)c->r[ncrn + 1] << 32) | c->r[ncrn];
                ncrn += 2;
            } else {
                ncrn = 4;
                nsaa = (nsaa + 7) & ~7u;
                a->x[nx++] = LD64(nsaa);
                nsaa += 8;
            }
            break;
        case 'f':
            if (ns < 16) a->f[nf++] = c->v.f[ns++];
            else { a->f[nf++] = LDF(nsaa); nsaa += 4; }
            break;
        default:
            if (ncrn < 4) a->x[nx++] = c->r[ncrn++];
            else { a->x[nx++] = LD32(nsaa); nsaa += 4; }
            break;
        }
        if (nf >= 8) nf = 7;
    }
    for (int i = nx; i < 16; i++) a->x[i] = 0;
}
