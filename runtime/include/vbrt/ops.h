// Instruction semantics used by the recompiled code (flag computation,
// multiply/divide, floating point, Nintendo extensions).
#pragma once

#include "cpu.h"
#include "memory.h"

#include <cmath>
#include <cstring>

namespace vb {

inline uint32_t zs_flags(uint32_t r) {
    return (r == 0 ? PSW_Z : 0) | ((r >> 31) ? PSW_S : 0);
}

inline uint32_t f_add(Cpu* c, uint32_t a, uint32_t b) {
    const uint32_t r = a + b;
    uint32_t f = zs_flags(r);
    if ((~(a ^ b) & (a ^ r)) >> 31) f |= PSW_OV;
    if (r < a) f |= PSW_CY;
    c->psw = (c->psw & ~0xFu) | f;
    return r;
}

// a - b (SUB/CMP compute reg2 - reg1)
inline uint32_t f_sub(Cpu* c, uint32_t a, uint32_t b) {
    const uint32_t r = a - b;
    uint32_t f = zs_flags(r);
    if (((a ^ b) & (a ^ r)) >> 31) f |= PSW_OV;
    if (a < b) f |= PSW_CY;
    c->psw = (c->psw & ~0xFu) | f;
    return r;
}

inline uint32_t f_shl(Cpu* c, uint32_t a, uint32_t n) {
    uint32_t r = a, cy = 0;
    if (n) {
        cy = (a >> (32 - n)) & 1;
        r = a << n;
    }
    c->psw = (c->psw & ~0xFu) | zs_flags(r) | (cy ? PSW_CY : 0);
    return r;
}

inline uint32_t f_shr(Cpu* c, uint32_t a, uint32_t n) {
    uint32_t r = a, cy = 0;
    if (n) {
        cy = (a >> (n - 1)) & 1;
        r = a >> n;
    }
    c->psw = (c->psw & ~0xFu) | zs_flags(r) | (cy ? PSW_CY : 0);
    return r;
}

inline uint32_t f_sar(Cpu* c, uint32_t a, uint32_t n) {
    uint32_t r = a, cy = 0;
    if (n) {
        cy = ((uint32_t)((int32_t)a >> (n - 1))) & 1;
        r = (uint32_t)((int32_t)a >> n);
    }
    c->psw = (c->psw & ~0xFu) | zs_flags(r) | (cy ? PSW_CY : 0);
    return r;
}

// OR/AND/XOR/NOT and immediates: Z, S, OV=0, CY unchanged.
inline uint32_t f_logic(Cpu* c, uint32_t r) {
    c->psw = (c->psw & ~(PSW_Z | PSW_S | PSW_OV)) | zs_flags(r);
    return r;
}

inline bool cond(uint32_t psw, int cc) {
    const bool z = psw & PSW_Z, s = psw & PSW_S, ov = psw & PSW_OV, cy = psw & PSW_CY;
    bool r;
    switch (cc & 7) {
    case 0: r = ov; break;
    case 1: r = cy; break;
    case 2: r = z; break;
    case 3: r = cy || z; break;
    case 4: r = s; break;
    case 5: r = true; break;
    case 6: r = s != ov; break;
    default: r = (s != ov) || z; break;
    }
    return (cc & 8) ? !r : r;
}

inline void set_reg(Cpu* c, int reg, uint32_t v) {
    if (reg) c->r[reg] = v;
}

inline void op_mul(Cpu* c, int r1, int r2) {
    const int64_t p = (int64_t)(int32_t)c->r[r2] * (int64_t)(int32_t)c->r[r1];
    const uint32_t lo = (uint32_t)p;
    c->psw = (c->psw & ~(PSW_Z | PSW_S | PSW_OV)) | zs_flags(lo) | (p != (int64_t)(int32_t)lo ? PSW_OV : 0);
    c->r[30] = (uint32_t)((uint64_t)p >> 32);
    set_reg(c, r2, lo);
}

inline void op_mulu(Cpu* c, int r1, int r2) {
    const uint64_t p = (uint64_t)c->r[r2] * (uint64_t)c->r[r1];
    const uint32_t lo = (uint32_t)p, hi = (uint32_t)(p >> 32);
    c->psw = (c->psw & ~(PSW_Z | PSW_S | PSW_OV)) | zs_flags(lo) | (hi ? PSW_OV : 0);
    c->r[30] = hi;
    set_reg(c, r2, lo);
}

void zero_division(Cpu* c);

inline void op_div(Cpu* c, int r1, int r2) {
    const int32_t a = (int32_t)c->r[r2], b = (int32_t)c->r[r1];
    if (b == 0) {
        zero_division(c);
        return;
    }
    uint32_t q, rem, ov = 0;
    if (a == INT32_MIN && b == -1) {
        q = 0x80000000u;
        rem = 0;
        ov = PSW_OV;
    } else {
        q = (uint32_t)(a / b);
        rem = (uint32_t)(a % b);
    }
    c->psw = (c->psw & ~(PSW_Z | PSW_S | PSW_OV)) | zs_flags(q) | ov;
    c->r[30] = rem;
    set_reg(c, r2, q);
}

inline void op_divu(Cpu* c, int r1, int r2) {
    const uint32_t a = c->r[r2], b = c->r[r1];
    if (b == 0) {
        zero_division(c);
        return;
    }
    const uint32_t q = a / b;
    c->psw = (c->psw & ~(PSW_Z | PSW_S | PSW_OV)) | zs_flags(q);
    c->r[30] = a % b;
    set_reg(c, r2, q);
}

// CAXI: compare-and-exchange interlocked.
inline void op_caxi(Cpu* c, int r1, int r2, uint32_t disp) {
    const uint32_t addr = c->r[r1] + disp;
    const uint32_t tmp = mem_read32(addr);
    f_sub(c, c->r[r2], tmp);
    mem_write32(addr, (c->psw & PSW_Z) ? c->r[30] : tmp);
    set_reg(c, r2, tmp);
}

inline float as_float(uint32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

inline uint32_t as_bits(float f) {
    uint32_t v;
    std::memcpy(&v, &f, 4);
    return v;
}

inline void float_result(Cpu* c, int r2, float f) {
    uint32_t fl = 0;
    if (f == 0.0f) fl |= PSW_Z;
    if (f < 0.0f) fl |= PSW_S | PSW_CY;
    c->psw = (c->psw & ~0xFu) | fl;
    set_reg(c, r2, as_bits(f));
}

inline void op_cmpf(Cpu* c, int r1, int r2) {
    const float a = as_float(c->r[r2]), b = as_float(c->r[r1]);
    uint32_t fl = 0;
    if (a == b) fl |= PSW_Z;
    if (a < b) fl |= PSW_S | PSW_CY;
    c->psw = (c->psw & ~0xFu) | fl;
}

inline void op_addf(Cpu* c, int r1, int r2) { float_result(c, r2, as_float(c->r[r2]) + as_float(c->r[r1])); }
inline void op_subf(Cpu* c, int r1, int r2) { float_result(c, r2, as_float(c->r[r2]) - as_float(c->r[r1])); }
inline void op_mulf(Cpu* c, int r1, int r2) { float_result(c, r2, as_float(c->r[r2]) * as_float(c->r[r1])); }
inline void op_divf(Cpu* c, int r1, int r2) { float_result(c, r2, as_float(c->r[r2]) / as_float(c->r[r1])); }
inline void op_cvt_ws(Cpu* c, int r1, int r2) { float_result(c, r2, (float)(int32_t)c->r[r1]); }

inline uint32_t float_to_int(float f, bool truncate) {
    if (std::isnan(f) || f >= 2147483648.0f || f < -2147483648.0f) return 0x80000000u;
    return (uint32_t)(int32_t)(truncate ? std::trunc(f) : std::nearbyint(f));
}

inline void op_cvt_sw(Cpu* c, int r1, int r2) {
    const uint32_t v = float_to_int(as_float(c->r[r1]), false);
    c->psw = (c->psw & ~(PSW_Z | PSW_S | PSW_OV)) | zs_flags(v);
    set_reg(c, r2, v);
}

inline void op_trnc_sw(Cpu* c, int r1, int r2) {
    const uint32_t v = float_to_int(as_float(c->r[r1]), true);
    c->psw = (c->psw & ~(PSW_Z | PSW_S | PSW_OV)) | zs_flags(v);
    set_reg(c, r2, v);
}

inline void op_mpyhw(Cpu* c, int r1, int r2) {
    const int32_t m = (int32_t)(c->r[r1] << 15) >> 15; // 17-bit signed multiplier
    set_reg(c, r2, (uint32_t)((int32_t)c->r[r2] * m));
}

inline uint32_t bit_reverse(uint32_t v) {
    v = ((v >> 1) & 0x55555555u) | ((v & 0x55555555u) << 1);
    v = ((v >> 2) & 0x33333333u) | ((v & 0x33333333u) << 2);
    v = ((v >> 4) & 0x0F0F0F0Fu) | ((v & 0x0F0F0F0Fu) << 4);
    v = ((v >> 8) & 0x00FF00FFu) | ((v & 0x00FF00FFu) << 8);
    return (v >> 16) | (v << 16);
}

} // namespace vb
