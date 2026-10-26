// A32 parallel add / subtract (SIMD within a register), SEL and USAD8: rare in game code, so out of line.
#include "cpu.h"

// op = (bits 22:20 << 3) | bits 7:5 of the instruction: prefix 001 S, 010 Q, 011 SH, 101 U, 110 UQ, 111 UH;
// operation 000 ADD16, 001 ASX, 010 SAX, 011 SUB16, 100 ADD8, 111 SUB8.
uint32_t rt_parallel(Cpu *c, uint32_t op, uint32_t a, uint32_t b) {
    uint32_t prefix = op >> 3, kind = op & 7;
    int is_signed = prefix <= 3;
    int sat = prefix == 2 || prefix == 6, halve = prefix == 3 || prefix == 7, plain = prefix == 1 || prefix == 5;
    uint32_t r = 0, ge = 0;
    if (kind == 4 || kind == 7) {  // 8-bit lanes
        for (int i = 0; i < 4; i++) {
            int32_t x = is_signed ? (int8_t)(a >> (8 * i)) : (uint8_t)(a >> (8 * i));
            int32_t y = is_signed ? (int8_t)(b >> (8 * i)) : (uint8_t)(b >> (8 * i));
            int32_t s = kind == 4 ? x + y : x - y;
            if (plain) {
                if (is_signed ? s >= 0 : (kind == 4 ? s >= 0x100 : s >= 0)) ge |= 1u << i;
            }
            if (sat) {
                if (is_signed) s = s > 127 ? 127 : s < -128 ? -128 : s;
                else s = s > 255 ? 255 : s < 0 ? 0 : s;
            } else if (halve) {
                s >>= 1;
            }
            r |= ((uint32_t)s & 0xFF) << (8 * i);
        }
    } else {  // 16-bit lanes
        int32_t a0 = is_signed ? (int16_t)a : (uint16_t)a, a1 = is_signed ? (int16_t)(a >> 16) : (uint16_t)(a >> 16);
        int32_t b0 = is_signed ? (int16_t)b : (uint16_t)b, b1 = is_signed ? (int16_t)(b >> 16) : (uint16_t)(b >> 16);
        int32_t lo, hi;
        int lo_add, hi_add;
        switch (kind) {
        case 0: lo = a0 + b0; hi = a1 + b1; lo_add = 1; hi_add = 1; break;  // ADD16
        case 1: lo = a0 - b1; hi = a1 + b0; lo_add = 0; hi_add = 1; break;  // ASX
        case 2: lo = a0 + b1; hi = a1 - b0; lo_add = 1; hi_add = 0; break;  // SAX
        default: lo = a0 - b0; hi = a1 - b1; lo_add = 0; hi_add = 0; break; // SUB16
        }
        int32_t v[2] = {lo, hi};
        int add[2] = {lo_add, hi_add};
        for (int i = 0; i < 2; i++) {
            int32_t s = v[i];
            if (plain) {
                int set = is_signed ? s >= 0 : (add[i] ? s >= 0x10000 : s >= 0);
                if (set) ge |= 3u << (2 * i);
            }
            if (sat) {
                if (is_signed) s = s > 32767 ? 32767 : s < -32768 ? -32768 : s;
                else s = s > 65535 ? 65535 : s < 0 ? 0 : s;
            } else if (halve) {
                s >>= 1;
            }
            r |= ((uint32_t)s & 0xFFFF) << (16 * i);
        }
    }
    if (plain) c->ge = ge;
    return r;
}

uint32_t rt_sel(Cpu *c, uint32_t a, uint32_t b) {
    uint32_t r = 0;
    for (int i = 0; i < 4; i++) r |= ((c->ge >> i) & 1 ? a : b) & (0xFFu << (8 * i));
    return r;
}

#define CRC_ROW(p, n) (((n) & 1 ? (p) : 0) ^ ((n) >> 1))
#define CRC8(p, n) CRC_ROW(p, CRC_ROW(p, CRC_ROW(p, CRC_ROW(p, CRC_ROW(p, CRC_ROW(p, CRC_ROW(p, CRC_ROW(p, (uint32_t)(n)))))))))
#define CRC_T4(p, n) CRC8(p, n), CRC8(p, n + 1), CRC8(p, n + 2), CRC8(p, n + 3)
#define CRC_T16(p, n) CRC_T4(p, n), CRC_T4(p, n + 4), CRC_T4(p, n + 8), CRC_T4(p, n + 12)
#define CRC_T64(p, n) CRC_T16(p, n), CRC_T16(p, n + 16), CRC_T16(p, n + 32), CRC_T16(p, n + 48)
const uint32_t rt_crc32_tab[256] = {CRC_T64(0xEDB88320u, 0), CRC_T64(0xEDB88320u, 64), CRC_T64(0xEDB88320u, 128),
                                    CRC_T64(0xEDB88320u, 192)};
const uint32_t rt_crc32c_tab[256] = {CRC_T64(0x82F63B78u, 0), CRC_T64(0x82F63B78u, 64), CRC_T64(0x82F63B78u, 128),
                                     CRC_T64(0x82F63B78u, 192)};

uint32_t rt_usad8(uint32_t a, uint32_t b) {
    uint32_t s = 0;
    for (int i = 0; i < 4; i++) {
        int32_t d = (int32_t)((a >> (8 * i)) & 0xFF) - (int32_t)((b >> (8 * i)) & 0xFF);
        s += (uint32_t)(d < 0 ? -d : d);
    }
    return s;
}
