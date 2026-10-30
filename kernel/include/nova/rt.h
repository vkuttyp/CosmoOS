/* SPDX-License-Identifier: GPL-2.0-only WITH Classpath-exception-2.0 */
/*
 * Nova runtime: core interface (freestanding).
 *
 * Generated C includes only this header. It uses only C11 freestanding headers and defines the
 * checked operations whose semantics are specified in docs/type-system.md §6-7 and mirrored by
 * Nova.Compiler.Semantics.IntegerSemantics. Platform services (output, exit, abort) are declared
 * at the bottom and supplied by a profile runtime, e.g. runtime/hosted/nova_rt_hosted.c.
 *
 * Define NV_NO_BUILTIN_OVERFLOW to use the portable overflow checks instead of compiler builtins.
 * The tests exercise both.
 */
#ifndef NOVA_RT_H
#define NOVA_RT_H

#include <float.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Floats (type-system.md §35.6): IEEE binary32/binary64, each operation rounded once to its type. Only programs
   that use floats define NV_FLOATS (the generated main.c, and the float formatter); the others need not meet these
   checks, which a kernel built with -mgeneral-regs-only does not (x86-64 then has no SSE: FLT_EVAL_METHOD != 0). */
#if defined(NV_FLOATS)
#if FLT_RADIX != 2 || FLT_MANT_DIG != 24 || DBL_MANT_DIG != 53
#error "Nova needs IEEE 754 binary32 float and binary64 double"
#endif
#if defined(FLT_EVAL_METHOD) && FLT_EVAL_METHOD != 0
#error "Nova needs FLT_EVAL_METHOD == 0: float and double evaluated in their own precision"
#endif
#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#elif defined(_MSC_VER)
#pragma fp_contract(off)
#endif
/* gcc ignores the standard pragma, but does not contract under -std=c11; novac passes -ffp-contract=off too. */
#endif

#if defined(__GNUC__) || defined(__clang__)
#define NV_UNUSED __attribute__((unused))
#define NV_COLD __attribute__((cold))
#else
#define NV_UNUSED
#define NV_COLD
#endif

/*
 * The most-aligned fundamental type: C11's max_align_t, which MSVC's <stddef.h> lacks in C mode; there
 * `double` is the strictest fundamental alignment (8; docs/c-backend.md §1.1).
 */
#if defined(_MSC_VER) && !defined(__clang__)
typedef double nv_max_align_t;
#else
typedef max_align_t nv_max_align_t;
#endif

#if !defined(NV_NO_BUILTIN_OVERFLOW) && (defined(__clang__) || (defined(__GNUC__) && __GNUC__ >= 5))
#define NV_HAVE_BUILTIN_OVERFLOW 1
#endif

/* ---- Panics ---------------------------------------------------------------------------- */

typedef enum nv_panic_kind {
    NV_PANIC_OVERFLOW = 1,
    NV_PANIC_DIVIDE_BY_ZERO = 2,
    NV_PANIC_SHIFT = 3,
    NV_PANIC_UNREACHABLE = 4,
    NV_PANIC_INDEX = 5,
    NV_PANIC_SLICE = 6,
    NV_PANIC_UNWRAP = 7,
    NV_PANIC_SCRATCH = 8,
    NV_PANIC_UNWRAP_NULL = 9,
    NV_PANIC_STR_BOUNDARY = 10,
    NV_PANIC_FFI_ENUM = 11,
    NV_PANIC_LENGTH = 12,
    NV_PANIC_FLOAT_RANGE = 13
} nv_panic_kind;

/* A Nova source location; `file` points to a string in the generated program's file table. */
typedef struct nv_loc {
    const char *file;
    uint32_t line;
    uint32_t column;
} nv_loc;

#define NV_LOC(file, line, column) ((nv_loc){(file), (line), (column)})

/* The checked helpers below take a pointer to a location constant (`nvL_<n>` in generated code) and read it only
   when the check fails, so a passing check costs nothing for its location (c-backend.md §6, M9d). */

/* Reports the panic and aborts; never returns (docs/ownership.md §0: panic = abort). Defined by the hosted
   runtime, or by the environment of a freestanding package (cosmoos/profiles.md §4.2). */
NV_COLD _Noreturn void nova_panic(nv_panic_kind kind, nv_loc loc);

/* The text of a panic kind ("integer overflow", ...), for any nova_panic; in the freestanding core too. */
const char *nova_panic_reason(nv_panic_kind kind);

/* ---- Checked arithmetic: + - * ---------------------------------------------------------- */

#if defined(NV_HAVE_BUILTIN_OVERFLOW)

#define NV_ARITH(T, S)                                                                    \
    static inline T nv_add_##S(T a, T b, const nv_loc *loc) {                                    \
        T r;                                                                              \
        if (__builtin_add_overflow(a, b, &r)) nova_panic(NV_PANIC_OVERFLOW, *loc);        \
        return r;                                                                         \
    }                                                                                     \
    static inline T nv_sub_##S(T a, T b, const nv_loc *loc) {                                    \
        T r;                                                                              \
        if (__builtin_sub_overflow(a, b, &r)) nova_panic(NV_PANIC_OVERFLOW, *loc);        \
        return r;                                                                         \
    }                                                                                     \
    static inline T nv_mul_##S(T a, T b, const nv_loc *loc) {                                    \
        T r;                                                                              \
        if (__builtin_mul_overflow(a, b, &r)) nova_panic(NV_PANIC_OVERFLOW, *loc);        \
        return r;                                                                         \
    }
#define NV_ARITH_SIGNED_NARROW(T, S, MIN, MAX) NV_ARITH(T, S)
#define NV_ARITH_UNSIGNED_NARROW(T, S, MAX) NV_ARITH(T, S)
#define NV_ARITH_I64 NV_ARITH(int64_t, i64)
#define NV_ARITH_U64 NV_ARITH(uint64_t, u64)

#else /* portable fallback */

/* i8/i16/i32: the exact result always fits in int64_t. */
#define NV_ARITH_SIGNED_NARROW(T, S, MIN, MAX)                                            \
    static inline T nv_narrow_##S(int64_t r, const nv_loc *loc) {                                \
        if (r < (MIN) || r > (MAX)) nova_panic(NV_PANIC_OVERFLOW, *loc);                   \
        return (T)r;                                                                      \
    }                                                                                     \
    static inline T nv_add_##S(T a, T b, const nv_loc *loc) { return nv_narrow_##S((int64_t)a + (int64_t)b, loc); } \
    static inline T nv_sub_##S(T a, T b, const nv_loc *loc) { return nv_narrow_##S((int64_t)a - (int64_t)b, loc); } \
    static inline T nv_mul_##S(T a, T b, const nv_loc *loc) { return nv_narrow_##S((int64_t)a * (int64_t)b, loc); }

/* u8/u16/u32: sums and products always fit in uint64_t; differences are checked first. */
#define NV_ARITH_UNSIGNED_NARROW(T, S, MAX)                                               \
    static inline T nv_narrow_##S(uint64_t r, const nv_loc *loc) {                               \
        if (r > (MAX)) nova_panic(NV_PANIC_OVERFLOW, *loc);                                \
        return (T)r;                                                                      \
    }                                                                                     \
    static inline T nv_add_##S(T a, T b, const nv_loc *loc) { return nv_narrow_##S((uint64_t)a + (uint64_t)b, loc); } \
    static inline T nv_sub_##S(T a, T b, const nv_loc *loc) {                                    \
        if (a < b) nova_panic(NV_PANIC_OVERFLOW, *loc);                                    \
        return (T)(a - b);                                                                \
    }                                                                                     \
    static inline T nv_mul_##S(T a, T b, const nv_loc *loc) { return nv_narrow_##S((uint64_t)a * (uint64_t)b, loc); }

/* i64: pre-checks in the style of CERT INT32-C; no operation overflows before the check. */
#define NV_ARITH_I64                                                                      \
    static inline int64_t nv_add_i64(int64_t a, int64_t b, const nv_loc *loc) {                  \
        if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b))                 \
            nova_panic(NV_PANIC_OVERFLOW, *loc);                                           \
        return a + b;                                                                     \
    }                                                                                     \
    static inline int64_t nv_sub_i64(int64_t a, int64_t b, const nv_loc *loc) {                  \
        if ((b < 0 && a > INT64_MAX + b) || (b > 0 && a < INT64_MIN + b))                 \
            nova_panic(NV_PANIC_OVERFLOW, *loc);                                           \
        return a - b;                                                                     \
    }                                                                                     \
    static inline int64_t nv_mul_i64(int64_t a, int64_t b, const nv_loc *loc) {                  \
        if (a > 0) {                                                                      \
            if (b > 0 ? a > INT64_MAX / b : b < INT64_MIN / a)                            \
                nova_panic(NV_PANIC_OVERFLOW, *loc);                                       \
        } else if (a < 0) {                                                               \
            if (b > 0 ? a < INT64_MIN / b : (b < 0 && a < INT64_MAX / b))                 \
                nova_panic(NV_PANIC_OVERFLOW, *loc);                                       \
        }                                                                                 \
        return a * b;                                                                     \
    }

#define NV_ARITH_U64                                                                      \
    static inline uint64_t nv_add_u64(uint64_t a, uint64_t b, const nv_loc *loc) {               \
        if (a > UINT64_MAX - b) nova_panic(NV_PANIC_OVERFLOW, *loc);                       \
        return a + b;                                                                     \
    }                                                                                     \
    static inline uint64_t nv_sub_u64(uint64_t a, uint64_t b, const nv_loc *loc) {               \
        if (a < b) nova_panic(NV_PANIC_OVERFLOW, *loc);                                    \
        return a - b;                                                                     \
    }                                                                                     \
    static inline uint64_t nv_mul_u64(uint64_t a, uint64_t b, const nv_loc *loc) {               \
        if (a != 0 && b > UINT64_MAX / a) nova_panic(NV_PANIC_OVERFLOW, *loc);             \
        return a * b;                                                                     \
    }

#endif

NV_ARITH_SIGNED_NARROW(int8_t, i8, INT8_MIN, INT8_MAX)
NV_ARITH_SIGNED_NARROW(int16_t, i16, INT16_MIN, INT16_MAX)
NV_ARITH_SIGNED_NARROW(int32_t, i32, INT32_MIN, INT32_MAX)
NV_ARITH_I64
NV_ARITH_UNSIGNED_NARROW(uint8_t, u8, UINT8_MAX)
NV_ARITH_UNSIGNED_NARROW(uint16_t, u16, UINT16_MAX)
NV_ARITH_UNSIGNED_NARROW(uint32_t, u32, UINT32_MAX)
NV_ARITH_U64

/* ---- Division, remainder, negation ------------------------------------------------------ */

/* `/` truncates toward zero; `%` has the dividend's sign; MIN / -1 overflows; MIN % -1 == 0. */
#define NV_DIV_SIGNED(T, S, MIN)                                                          \
    static inline T nv_div_##S(T a, T b, const nv_loc *loc) {                                    \
        if (b == 0) nova_panic(NV_PANIC_DIVIDE_BY_ZERO, *loc);                             \
        if (a == (MIN) && b == -1) nova_panic(NV_PANIC_OVERFLOW, *loc);                    \
        return (T)(a / b);                                                                \
    }                                                                                     \
    static inline T nv_rem_##S(T a, T b, const nv_loc *loc) {                                    \
        if (b == 0) nova_panic(NV_PANIC_DIVIDE_BY_ZERO, *loc);                             \
        if (b == -1) return 0; /* avoids the undefined MIN % -1 in C */                   \
        return (T)(a % b);                                                                \
    }                                                                                     \
    static inline T nv_neg_##S(T a, const nv_loc *loc) {                                         \
        if (a == (MIN)) nova_panic(NV_PANIC_OVERFLOW, *loc);                               \
        return (T)(-a);                                                                   \
    }

#define NV_DIV_UNSIGNED(T, S)                                                             \
    static inline T nv_div_##S(T a, T b, const nv_loc *loc) {                                    \
        if (b == 0) nova_panic(NV_PANIC_DIVIDE_BY_ZERO, *loc);                             \
        return (T)(a / b);                                                                \
    }                                                                                     \
    static inline T nv_rem_##S(T a, T b, const nv_loc *loc) {                                    \
        if (b == 0) nova_panic(NV_PANIC_DIVIDE_BY_ZERO, *loc);                             \
        return (T)(a % b);                                                                \
    }

NV_DIV_SIGNED(int8_t, i8, INT8_MIN)
NV_DIV_SIGNED(int16_t, i16, INT16_MIN)
NV_DIV_SIGNED(int32_t, i32, INT32_MIN)
NV_DIV_SIGNED(int64_t, i64, INT64_MIN)
NV_DIV_UNSIGNED(uint8_t, u8)
NV_DIV_UNSIGNED(uint16_t, u16)
NV_DIV_UNSIGNED(uint32_t, u32)
NV_DIV_UNSIGNED(uint64_t, u64)

/* ---- Shifts (left operand unsigned in M1; '>>' is logical) ------------------------------- */

static inline uint64_t nv_shift_amount_u(uint64_t n, uint64_t bits, const nv_loc *loc) {
    if (n >= bits) nova_panic(NV_PANIC_SHIFT, *loc);
    return n;
}

static inline uint64_t nv_shift_amount_s(int64_t n, uint64_t bits, const nv_loc *loc) {
    if (n < 0 || (uint64_t)n >= bits) nova_panic(NV_PANIC_SHIFT, *loc);
    return (uint64_t)n;
}

/* `n` has been validated by nv_shift_amount_*; bits shifted out are discarded. */
#define NV_SHIFT(T, S)                                                                    \
    static inline T nv_shl_##S(T a, uint64_t n) { return (T)((uint64_t)a << n); }         \
    static inline T nv_shr_##S(T a, uint64_t n) { return (T)((uint64_t)a >> n); }

NV_SHIFT(uint8_t, u8)
NV_SHIFT(uint16_t, u16)
NV_SHIFT(uint32_t, u32)
NV_SHIFT(uint64_t, u64)

/* ---- Reinterpretation (truncate<iN>) ----------------------------------------------------- */

/* Converting an out-of-range value to a signed type is implementation-defined in C11
 * (6.3.1.3p3), so two's-complement reinterpretation is spelled out. */
static inline int8_t nv_as_i8(uint8_t u) { return u <= INT8_MAX ? (int8_t)u : (int8_t)(-(int)(UINT8_MAX - u) - 1); }
static inline int16_t nv_as_i16(uint16_t u) { return u <= INT16_MAX ? (int16_t)u : (int16_t)(-(int)(UINT16_MAX - u) - 1); }
static inline int32_t nv_as_i32(uint32_t u) { return u <= INT32_MAX ? (int32_t)u : (int32_t)(-(int64_t)(UINT32_MAX - u) - 1); }
static inline int64_t nv_as_i64(uint64_t u) { return u <= INT64_MAX ? (int64_t)u : -(int64_t)(UINT64_MAX - u) - 1; }

/* ---- Explicit overflow behaviour: wrapping, checked, saturating (type-system.md §6, M11) ----- */

/* nv_try_op_S(a, b, &r): true and the result in r, or false if it does not fit in the type. */
#if defined(NV_HAVE_BUILTIN_OVERFLOW)
#define NV_TRY(T, S)                                                                                  \
    static inline bool nv_try_add_##S(T a, T b, T *r) { return !__builtin_add_overflow(a, b, r); }    \
    static inline bool nv_try_sub_##S(T a, T b, T *r) { return !__builtin_sub_overflow(a, b, r); }    \
    static inline bool nv_try_mul_##S(T a, T b, T *r) { return !__builtin_mul_overflow(a, b, r); }
#define NV_TRY_SIGNED_NARROW(T, S, MIN, MAX) NV_TRY(T, S)
#define NV_TRY_UNSIGNED_NARROW(T, S, MAX) NV_TRY(T, S)
#define NV_TRY_I64 NV_TRY(int64_t, i64)
#define NV_TRY_U64 NV_TRY(uint64_t, u64)
#else /* portable fallback, as for the checked operators above */
#define NV_TRY_SIGNED_NARROW(T, S, MIN, MAX)                                                          \
    static inline bool nv_tryfit_##S(int64_t x, T *r) {                                               \
        if (x < (MIN) || x > (MAX)) return false;                                                     \
        *r = (T)x;                                                                                    \
        return true;                                                                                  \
    }                                                                                                 \
    static inline bool nv_try_add_##S(T a, T b, T *r) { return nv_tryfit_##S((int64_t)a + (int64_t)b, r); } \
    static inline bool nv_try_sub_##S(T a, T b, T *r) { return nv_tryfit_##S((int64_t)a - (int64_t)b, r); } \
    static inline bool nv_try_mul_##S(T a, T b, T *r) { return nv_tryfit_##S((int64_t)a * (int64_t)b, r); }
#define NV_TRY_UNSIGNED_NARROW(T, S, MAX)                                                             \
    static inline bool nv_tryfit_##S(uint64_t x, T *r) {                                              \
        if (x > (MAX)) return false;                                                                  \
        *r = (T)x;                                                                                    \
        return true;                                                                                  \
    }                                                                                                 \
    static inline bool nv_try_add_##S(T a, T b, T *r) { return nv_tryfit_##S((uint64_t)a + (uint64_t)b, r); } \
    static inline bool nv_try_sub_##S(T a, T b, T *r) {                                               \
        if (a < b) return false;                                                                      \
        *r = (T)(a - b);                                                                              \
        return true;                                                                                  \
    }                                                                                                 \
    static inline bool nv_try_mul_##S(T a, T b, T *r) { return nv_tryfit_##S((uint64_t)a * (uint64_t)b, r); }
#define NV_TRY_I64                                                                                    \
    static inline bool nv_try_add_i64(int64_t a, int64_t b, int64_t *r) {                             \
        if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b)) return false;               \
        *r = a + b;                                                                                   \
        return true;                                                                                  \
    }                                                                                                 \
    static inline bool nv_try_sub_i64(int64_t a, int64_t b, int64_t *r) {                             \
        if ((b < 0 && a > INT64_MAX + b) || (b > 0 && a < INT64_MIN + b)) return false;               \
        *r = a - b;                                                                                   \
        return true;                                                                                  \
    }                                                                                                 \
    static inline bool nv_try_mul_i64(int64_t a, int64_t b, int64_t *r) {                             \
        if (a > 0) {                                                                                  \
            if (b > 0 ? a > INT64_MAX / b : b < INT64_MIN / a) return false;                          \
        } else if (a < 0) {                                                                           \
            if (b > 0 ? a < INT64_MIN / b : (b < 0 && a < INT64_MAX / b)) return false;               \
        }                                                                                             \
        *r = a * b;                                                                                   \
        return true;                                                                                  \
    }
#define NV_TRY_U64                                                                                    \
    static inline bool nv_try_add_u64(uint64_t a, uint64_t b, uint64_t *r) {                          \
        if (a > UINT64_MAX - b) return false;                                                         \
        *r = a + b;                                                                                   \
        return true;                                                                                  \
    }                                                                                                 \
    static inline bool nv_try_sub_u64(uint64_t a, uint64_t b, uint64_t *r) {                          \
        if (a < b) return false;                                                                      \
        *r = a - b;                                                                                   \
        return true;                                                                                  \
    }                                                                                                 \
    static inline bool nv_try_mul_u64(uint64_t a, uint64_t b, uint64_t *r) {                          \
        if (a != 0 && b > UINT64_MAX / a) return false;                                               \
        *r = a * b;                                                                                   \
        return true;                                                                                  \
    }
#endif

/* Wrapping: unsigned arithmetic in a type no narrower than unsigned int (so nothing is promoted to int),
   truncated; a signed type wraps through its unsigned twin and converts back with nv_as_iN. */
#define NV_WRAP_UNSIGNED(T, S, W)                                                                     \
    static inline T nv_wrapping_add_##S(T a, T b) { return (T)((W)a + (W)b); }                        \
    static inline T nv_wrapping_sub_##S(T a, T b) { return (T)((W)a - (W)b); }                        \
    static inline T nv_wrapping_mul_##S(T a, T b) { return (T)((W)a * (W)b); }
#define NV_WRAP_SIGNED(T, S, U, US)                                                                   \
    static inline T nv_wrapping_add_##S(T a, T b) { return nv_as_##S(nv_wrapping_add_##US((U)a, (U)b)); } \
    static inline T nv_wrapping_sub_##S(T a, T b) { return nv_as_##S(nv_wrapping_sub_##US((U)a, (U)b)); } \
    static inline T nv_wrapping_mul_##S(T a, T b) { return nv_as_##S(nv_wrapping_mul_##US((U)a, (U)b)); }

/* Saturating: the exact result if it fits, else the bound the overflow went past. */
#define NV_SAT_UNSIGNED(T, S, MAX)                                                                    \
    static inline T nv_saturating_add_##S(T a, T b) { T r; return nv_try_add_##S(a, b, &r) ? r : (MAX); } \
    static inline T nv_saturating_sub_##S(T a, T b) { T r; return nv_try_sub_##S(a, b, &r) ? r : 0u; }   \
    static inline T nv_saturating_mul_##S(T a, T b) { T r; return nv_try_mul_##S(a, b, &r) ? r : (MAX); }
#define NV_SAT_SIGNED(T, S, MIN, MAX)                                                                 \
    static inline T nv_saturating_add_##S(T a, T b) { T r; return nv_try_add_##S(a, b, &r) ? r : b < 0 ? (MIN) : (MAX); } \
    static inline T nv_saturating_sub_##S(T a, T b) { T r; return nv_try_sub_##S(a, b, &r) ? r : b < 0 ? (MAX) : (MIN); } \
    static inline T nv_saturating_mul_##S(T a, T b) { T r; return nv_try_mul_##S(a, b, &r) ? r : (a < 0) != (b < 0) ? (MIN) : (MAX); }

NV_TRY_SIGNED_NARROW(int8_t, i8, INT8_MIN, INT8_MAX)
NV_TRY_SIGNED_NARROW(int16_t, i16, INT16_MIN, INT16_MAX)
NV_TRY_SIGNED_NARROW(int32_t, i32, INT32_MIN, INT32_MAX)
NV_TRY_I64
NV_TRY_UNSIGNED_NARROW(uint8_t, u8, UINT8_MAX)
NV_TRY_UNSIGNED_NARROW(uint16_t, u16, UINT16_MAX)
NV_TRY_UNSIGNED_NARROW(uint32_t, u32, UINT32_MAX)
NV_TRY_U64
NV_WRAP_UNSIGNED(uint8_t, u8, uint32_t)
NV_WRAP_UNSIGNED(uint16_t, u16, uint32_t)
NV_WRAP_UNSIGNED(uint32_t, u32, uint32_t)
NV_WRAP_UNSIGNED(uint64_t, u64, uint64_t)
NV_WRAP_SIGNED(int8_t, i8, uint8_t, u8)
NV_WRAP_SIGNED(int16_t, i16, uint16_t, u16)
NV_WRAP_SIGNED(int32_t, i32, uint32_t, u32)
NV_WRAP_SIGNED(int64_t, i64, uint64_t, u64)
NV_SAT_UNSIGNED(uint8_t, u8, UINT8_MAX)
NV_SAT_UNSIGNED(uint16_t, u16, UINT16_MAX)
NV_SAT_UNSIGNED(uint32_t, u32, UINT32_MAX)
NV_SAT_UNSIGNED(uint64_t, u64, UINT64_MAX)
NV_SAT_SIGNED(int8_t, i8, INT8_MIN, INT8_MAX)
NV_SAT_SIGNED(int16_t, i16, INT16_MIN, INT16_MAX)
NV_SAT_SIGNED(int32_t, i32, INT32_MIN, INT32_MAX)
NV_SAT_SIGNED(int64_t, i64, INT64_MIN, INT64_MAX)

#if defined(NV_FLOATS)
/* ---- Floats: representation and conversions (type-system.md §35.4–§35.6) ----------------------- */

/* A float from its bits and back; a union reinterprets without aliasing questions (C11 6.5.2.3). */
static inline float nv_f32_from_bits(uint32_t u) { union { uint32_t u; float f; } v; v.u = u; return v.f; }
static inline double nv_f64_from_bits(uint64_t u) { union { uint64_t u; double f; } v; v.u = u; return v.f; }
static inline uint32_t nv_f32_bits(float f) { union { uint32_t u; float f; } v; v.f = f; return v.u; }
static inline uint64_t nv_f64_bits(double f) { union { uint64_t u; double f; } v; v.f = f; return v.u; }

/* round<f32>(d): C leaves converting a finite double beyond FLT_MAX undefined (C11 6.3.1.5), so those values are
   rounded here: up to the midpoint 2^128 - 2^103 they round to FLT_MAX, from it on (ties to even) to infinity. */
static inline float nv_f64_to_f32(double d) {
    double limit = nv_f64_from_bits(0x47EFFFFFF0000000u); /* 2^128 - 2^103 */
    if (d > (double)FLT_MAX) return d >= limit ? nv_f32_from_bits(0x7F800000u) : FLT_MAX;
    if (d < -(double)FLT_MAX) return d <= -limit ? nv_f32_from_bits(0xFF800000u) : -FLT_MAX;
    return (float)d;
}

/* truncate<T>(f) and its variants for each integer type: `x` (an f32 promoted exactly, or an f64) converts only after
   it is known to lie strictly between MIN - 1 and MAX + 1, which C requires (C11 6.3.1.4). The bounds are exact
   doubles: MIN - 1 for 8 to 32 bits; for i64 MIN itself (no double lies between MIN - 1 and MIN); 2^N or 2^(N-1). */
#define NV_FTOI(T, S, IN_LOW, HIGH, MIN, MAX)                                                             static inline bool nv_ftoi_fits_##S(double x) { return (IN_LOW) && x < (HIGH); }                     static inline T nv_ftoi_##S(double x, const nv_loc *loc) {                                                if (!nv_ftoi_fits_##S(x)) nova_panic(NV_PANIC_FLOAT_RANGE, *loc);                                     return (T)x;                                                                                      }                                                                                                     static inline bool nv_try_ftoi_##S(double x, T *r) {                                                      if (!nv_ftoi_fits_##S(x)) return false;                                                               *r = (T)x;                                                                                            return true;                                                                                      }                                                                                                     static inline T nv_sat_ftoi_##S(double x) {                                                               if (x != x) return 0;                                                                                 if (x < (HIGH) && !(IN_LOW)) return (MIN);                                                            if (!(x < (HIGH))) return (MAX);                                                                      return (T)x;                                                                                      }
NV_FTOI(int8_t, i8, x > -129.0, 128.0, INT8_MIN, INT8_MAX)
NV_FTOI(int16_t, i16, x > -32769.0, 32768.0, INT16_MIN, INT16_MAX)
NV_FTOI(int32_t, i32, x > -2147483649.0, 2147483648.0, INT32_MIN, INT32_MAX)
NV_FTOI(int64_t, i64, x >= -9223372036854775808.0, 9223372036854775808.0, INT64_MIN, INT64_MAX)
NV_FTOI(uint8_t, u8, x > -1.0, 256.0, 0u, UINT8_MAX)
NV_FTOI(uint16_t, u16, x > -1.0, 65536.0, 0u, UINT16_MAX)
NV_FTOI(uint32_t, u32, x > -1.0, 4294967296.0, 0u, UINT32_MAX)
NV_FTOI(uint64_t, u64, x > -1.0, 18446744073709551616.0, 0u, UINT64_MAX)

/* Math methods (type-system.md §35.8), never a libm call: CosmoOS has no libm, and baseline x86-64 has no instruction
   for floor or ceil (roundsd needs SSE4.1). */
#if defined(_MSC_VER) && !defined(__clang__)
#include <math.h> /* MSVC's sqrt is in its C runtime, which every program links */
#endif
static inline double nv_sqrt_f64(double x) {
#if defined(__clang__) && defined(__has_builtin)
#if __has_builtin(__builtin_elementwise_sqrt)
    return __builtin_elementwise_sqrt(x); /* the instruction; never sets errno */
#else
    return __builtin_sqrt(x);
#endif
#elif defined(__GNUC__)
    return __builtin_sqrt(x); /* with -fno-math-errno (novac passes it), the instruction alone */
#else
    return sqrt(x);
#endif
}
static inline float nv_sqrt_f32(float x) {
#if defined(__clang__) && defined(__has_builtin)
#if __has_builtin(__builtin_elementwise_sqrt)
    return __builtin_elementwise_sqrt(x);
#else
    return __builtin_sqrtf(x);
#endif
#elif defined(__GNUC__)
    return __builtin_sqrtf(x);
#else
    return sqrtf(x);
#endif
}
static inline double nv_abs_f64(double x) { return nv_f64_from_bits(nv_f64_bits(x) & ~(UINT64_C(1) << 63)); }
static inline float nv_abs_f32(float x) { return nv_f32_from_bits(nv_f32_bits(x) & ~(UINT32_C(1) << 31)); }
/* The magnitude of m with the sign of s. */
static inline double nv_with_sign_f64(double m, double s) {
    return nv_f64_from_bits((nv_f64_bits(m) & ~(UINT64_C(1) << 63)) | (nv_f64_bits(s) & (UINT64_C(1) << 63)));
}
static inline float nv_with_sign_f32(float m, float s) {
    return nv_f32_from_bits((nv_f32_bits(m) & ~(UINT32_C(1) << 31)) | (nv_f32_bits(s) & (UINT32_C(1) << 31)));
}
/* floor and ceil: a value with |x| >= 2^52 (2^23), an infinity or NaN is already integral; any other converts to an
   integer exactly, toward zero, and one step corrects it. A zero result takes x's sign, as IEEE says. */
static inline double nv_floor_f64(double x) {
    if (!(x > -4503599627370496.0 && x < 4503599627370496.0)) return x;
    double t = (double)(int64_t)x;
    if (t > x) t -= 1.0;
    return t == 0.0 ? nv_with_sign_f64(t, x) : t;
}
static inline double nv_ceil_f64(double x) {
    if (!(x > -4503599627370496.0 && x < 4503599627370496.0)) return x;
    double t = (double)(int64_t)x;
    if (t < x) t += 1.0;
    return t == 0.0 ? nv_with_sign_f64(t, x) : t;
}
static inline float nv_floor_f32(float x) {
    if (!(x > -8388608.0f && x < 8388608.0f)) return x;
    float t = (float)(int32_t)x;
    if (t > x) t -= 1.0f;
    return t == 0.0f ? nv_with_sign_f32(t, x) : t;
}
static inline float nv_ceil_f32(float x) {
    if (!(x > -8388608.0f && x < 8388608.0f)) return x;
    float t = (float)(int32_t)x;
    if (t < x) t += 1.0f;
    return t == 0.0f ? nv_with_sign_f32(t, x) : t;
}
/* IEEE 754-2019 minimum and maximum: a NaN if either operand is one, and -0 below +0. */
static inline double nv_min_f64(double a, double b) {
    if (a != a || b != b) return a + b;
    if (a == b) return (nv_f64_bits(a) >> 63) != 0 ? a : b;
    return a < b ? a : b;
}
static inline double nv_max_f64(double a, double b) {
    if (a != a || b != b) return a + b;
    if (a == b) return (nv_f64_bits(a) >> 63) != 0 ? b : a;
    return a > b ? a : b;
}
static inline float nv_min_f32(float a, float b) {
    if (a != a || b != b) return a + b;
    if (a == b) return (nv_f32_bits(a) >> 31) != 0 ? a : b;
    return a < b ? a : b;
}
static inline float nv_max_f32(float a, float b) {
    if (a != a || b != b) return a + b;
    if (a == b) return (nv_f32_bits(a) >> 31) != 0 ? b : a;
    return a > b ? a : b;
}
#endif /* NV_FLOATS */

/* ---- Bounds checks (type-system.md §16.3) ------------------------------------------------- */

static inline void nv_check_index(uint64_t index, uint64_t length, const nv_loc *loc) {
    if (index >= length) nova_panic(NV_PANIC_INDEX, *loc);
}

/* ---- CPU and target queries (type-system.md §29.3) ----------------------------------------- */

/* A hint inside a busy-wait loop: no effect on the program's meaning. */
static inline void nv_spin_hint(void) {
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
    __builtin_ia32_pause();
#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
    __asm__ __volatile__("yield");
#endif
}

/* The architecture the generated C is compiled for: one generated file serves every LP64 target. */
static inline bool nv_target_is_x86_64(void) {
#if defined(__x86_64__) || defined(_M_X64)
    return true;
#else
    return false;
#endif
}

static inline bool nv_target_is_aarch64(void) {
#if defined(__aarch64__) || defined(_M_ARM64)
    return true;
#else
    return false;
#endif
}

/* A constant index that its preceding check rejects: hides the constant from -Warray-bounds (c-backend.md §5). */
static inline uint64_t nv_opaque_index(uint64_t index) { return index; }

static inline void nv_check_range(uint64_t low, uint64_t high, uint64_t length, const nv_loc *loc) {
    if (low > high || high > length) nova_panic(NV_PANIC_SLICE, *loc);
}

/* ---- Bulk slice operations (type-system.md §16.5) ------------------------------------------ */

/*
 * Byte copies, moves and fills. The builtins may lower to calls to memcpy, memmove and memset, which every profile provides
 * (cosmoos/profiles.md §4.4); MSVC builds are hosted, so <string.h> is there.
 */
#if defined(__GNUC__) || defined(__clang__)
#define nova_mem_copy __builtin_memcpy
#define nova_mem_move __builtin_memmove
#define nova_mem_set __builtin_memset
#else
#include <string.h>
#define nova_mem_copy memcpy
#define nova_mem_move memmove
#define nova_mem_set memset
#endif

/* `d.copyFrom(s)`: the lengths must be equal. */
static inline void nv_check_length(uint64_t source, uint64_t destination, const nv_loc *loc) {
    if (source != destination) nova_panic(NV_PANIC_LENGTH, *loc);
}

/* `count` elements of `size` bytes, possibly overlapping. A slice from raw parts may be null when empty. */
static inline void nv_slice_copy(void *destination, const void *source, uint64_t count, size_t size) {
    if (count != 0u) nova_mem_move(destination, source, (size_t)count * size);
}

/* `d.fill(v)` for a one-byte element. */
static inline void nv_slice_fill_byte(void *destination, uint8_t value, uint64_t count) {
    if (count != 0u) nova_mem_set(destination, value, (size_t)count);
}

/* ---- String views (type-system.md §22.2) ------------------------------------------------- */

/* `StringView`: UTF-8 bytes, laid out like a `Slice<u8>`. */
typedef struct nv_str { const uint8_t *ptr; uint64_t len; } nv_str;

/* True if `index` is not inside a UTF-8 multi-byte sequence (the end counts as a boundary). */
static inline bool nv_str_boundary(nv_str text, uint64_t index) {
    return index >= text.len || (text.ptr[index] & 0xC0u) != 0x80u;
}

/* `s.appendNumber(n)` (type-system.md §23.2): the decimal text of n, written at the end of buf, which has room for
   the longest (20 bytes: "-9223372036854775808", "18446744073709551615"); the view is of buf. */
#define NV_DIGITS 20
static inline nv_str nv_format_u64(uint64_t value, uint8_t *buf) {
    uint8_t *p = buf + NV_DIGITS;
    do {
        *--p = (uint8_t)('0' + value % 10u);
        value /= 10u;
    } while (value != 0u);
    return (nv_str){ .ptr = p, .len = (uint64_t)(buf + NV_DIGITS - p) };
}
static inline nv_str nv_format_i64(int64_t value, uint8_t *buf) {
    /* The magnitude in unsigned arithmetic, so INT64_MIN negates without overflow. */
    uint64_t magnitude = value < 0 ? 0u - (uint64_t)value : (uint64_t)value;
    nv_str digits = nv_format_u64(magnitude, buf);
    if (value >= 0) return digits;
    buf[NV_DIGITS - digits.len - 1u] = (uint8_t)'-';
    return (nv_str){ .ptr = buf + NV_DIGITS - digits.len - 1u, .len = digits.len + 1u };
}

#if defined(NV_FLOATS)
/* Console.write and String.appendNumber of a float (type-system.md §35.7): the shortest decimal that reads back as the
   same value, laid out as ECMAScript's Number::toString, written into buf (NV_FLOAT_CHARS bytes); the view is of buf.
   runtime/hosted/nova_rt_float.c, linked only into programs that print floats. */
#define NV_FLOAT_CHARS 32
nv_str nova_format_f64(double value, uint8_t *buf);
nv_str nova_format_f32(float value, uint8_t *buf);
#endif

/* The range check of `s[lo..hi]`, then both bounds on character boundaries. */
static inline void nv_check_str_range(nv_str text, uint64_t low, uint64_t high, const nv_loc *loc) {
    nv_check_range(low, high, text.len, loc);
    if (!nv_str_boundary(text, low) || !nv_str_boundary(text, high)) nova_panic(NV_PANIC_STR_BOUNDARY, *loc);
}

static inline bool nova_str_eq(nv_str a, nv_str b) {
    if (a.len != b.len) return false;
    for (uint64_t i = 0; i < a.len; i++) {
        if (a.ptr[i] != b.ptr[i]) return false;
    }
    return true;
}

/* ---- Platform services (supplied by the profile runtime) -------------------------------- */

void nova_platform_write_stdout(const uint8_t *bytes, size_t length);
void nova_platform_write_stderr(const uint8_t *bytes, size_t length);
_Noreturn void nova_platform_exit(int32_t code);
_Noreturn void nova_platform_abort(void);
/* Heap memory; NULL on failure. Must be aligned for any Nova type (max_align_t). */
void *nova_platform_alloc(size_t size);
/* Resizes a block from nova_platform_alloc, keeping its first bytes; NULL with the block unchanged on failure. */
void *nova_platform_realloc(void *p, size_t size);
void nova_platform_free(void *p);
/* A configuration number from the environment; 0 if absent or not a number (hosted test builds). */
uint64_t nova_platform_config(const char *name);

/* ---- Heap (type-system.md §18; runtime/core/nova_rt_core.c) ------------------------------ */

/* Allocator types, as passed by generated code. */
#define NV_ALLOC_SYSTEM 0u
#define NV_ALLOC_FAILING 1u

/*
 * Allocation for Box and every later heap type. Every attempt is counted, and the attempt numbered
 * NOVA_FAIL_ALLOC_AT (1-based, all allocators) fails: the fault-injection hook for the M3 sweep.
 */
void *nova_alloc(size_t size, uint32_t allocator);
/* Resizes a block from nova_alloc: one attempt, the block unchanged (and still live) on failure. */
void *nova_realloc(void *p, size_t size, uint32_t allocator);
void nova_free(void *p, uint32_t allocator);
/* FailingAllocator.failAfter(n) / FailingAllocator.reset(). */
void nova_fail_after(uint64_t n);
void nova_fail_reset(void);

/* ---- Threads and mutexes (type-system.md §38; runtime/hosted/nova_rt_thread.c), and atomics (here) ---------- */

typedef struct nv_thread nv_thread;
/* Starts a thread running body(arg); NULL if it or its record could not be made. */
nv_thread *nova_thread_start(void *(*body)(void *), void *arg);
/* Waits for the thread to end, then frees its record. */
void nova_thread_join(nv_thread *t);
/* A platform mutex in allocated memory, or NULL. */
void *nova_mutex_create(void);
void nova_mutex_lock(void *m);
void nova_mutex_unlock(void *m);
void nova_mutex_destroy(void *m);
/* ---- Atomics (§38.3): sequentially consistent, and with an ordering (ADR 0021); compare-exchange returns the
 * previous value. Static inline (ADR 0026), so an operation is its instructions where the compiler inlines it, and
 * a freestanding build needs no runtime for them. An ordering is lib/core's code: 0 relaxed, 1 acquire, 2 release, 3 acquire-release, 4 sequentially
 * consistent. lib/core passes a load only 0, 1 or 4 and a store only 0, 2 or 4, so no ordering C11 leaves undefined
 * reaches a builtin. --------------------------------------------------------------------------------------------- */

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
static inline uint64_t nova_atomic_load_u64(uint64_t *p) { return (uint64_t)_InterlockedCompareExchange64((volatile __int64 *)p, 0, 0); }
static inline void nova_atomic_store_u64(uint64_t *p, uint64_t v) { (void)_InterlockedExchange64((volatile __int64 *)p, (__int64)v); }
static inline uint64_t nova_atomic_swap_u64(uint64_t *p, uint64_t v) { return (uint64_t)_InterlockedExchange64((volatile __int64 *)p, (__int64)v); }
static inline uint64_t nova_atomic_cas_u64(uint64_t *p, uint64_t expected, uint64_t desired) {
    return (uint64_t)_InterlockedCompareExchange64((volatile __int64 *)p, (__int64)desired, (__int64)expected);
}
static inline uint64_t nova_atomic_add_u64(uint64_t *p, uint64_t v) { return (uint64_t)_InterlockedExchangeAdd64((volatile __int64 *)p, (__int64)v); }
static inline uint32_t nova_atomic_load_u32(uint32_t *p) { return (uint32_t)_InterlockedCompareExchange((volatile long *)p, 0, 0); }
static inline void nova_atomic_store_u32(uint32_t *p, uint32_t v) { (void)_InterlockedExchange((volatile long *)p, (long)v); }
static inline uint32_t nova_atomic_swap_u32(uint32_t *p, uint32_t v) { return (uint32_t)_InterlockedExchange((volatile long *)p, (long)v); }
static inline uint32_t nova_atomic_cas_u32(uint32_t *p, uint32_t expected, uint32_t desired) {
    return (uint32_t)_InterlockedCompareExchange((volatile long *)p, (long)desired, (long)expected);
}
static inline uint32_t nova_atomic_add_u32(uint32_t *p, uint32_t v) { return (uint32_t)_InterlockedExchangeAdd((volatile long *)p, (long)v); }
/* Every _Interlocked intrinsic is a full barrier: a correct, stronger, implementation of each ordering. */
static inline uint64_t nova_atomic_load_ord_u64(uint64_t *p, int32_t o) { (void)o; return nova_atomic_load_u64(p); }
static inline void nova_atomic_store_ord_u64(uint64_t *p, uint64_t v, int32_t o) { (void)o; nova_atomic_store_u64(p, v); }
static inline uint64_t nova_atomic_swap_ord_u64(uint64_t *p, uint64_t v, int32_t o) { (void)o; return nova_atomic_swap_u64(p, v); }
static inline uint64_t nova_atomic_cas_ord_u64(uint64_t *p, uint64_t e, uint64_t d, int32_t o) { (void)o; return nova_atomic_cas_u64(p, e, d); }
static inline uint64_t nova_atomic_add_ord_u64(uint64_t *p, uint64_t v, int32_t o) { (void)o; return nova_atomic_add_u64(p, v); }
static inline uint32_t nova_atomic_load_ord_u32(uint32_t *p, int32_t o) { (void)o; return nova_atomic_load_u32(p); }
static inline void nova_atomic_store_ord_u32(uint32_t *p, uint32_t v, int32_t o) { (void)o; nova_atomic_store_u32(p, v); }
static inline uint32_t nova_atomic_swap_ord_u32(uint32_t *p, uint32_t v, int32_t o) { (void)o; return nova_atomic_swap_u32(p, v); }
static inline uint32_t nova_atomic_cas_ord_u32(uint32_t *p, uint32_t e, uint32_t d, int32_t o) { (void)o; return nova_atomic_cas_u32(p, e, d); }
static inline uint32_t nova_atomic_add_ord_u32(uint32_t *p, uint32_t v, int32_t o) { (void)o; return nova_atomic_add_u32(p, v); }
#else
static inline uint64_t nova_atomic_load_u64(uint64_t *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
static inline void nova_atomic_store_u64(uint64_t *p, uint64_t v) { __atomic_store_n(p, v, __ATOMIC_SEQ_CST); }
static inline uint64_t nova_atomic_swap_u64(uint64_t *p, uint64_t v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
static inline uint64_t nova_atomic_cas_u64(uint64_t *p, uint64_t expected, uint64_t desired) {
    (void)__atomic_compare_exchange_n(p, &expected, desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expected; /* the previous value: equal to the argument exactly when the exchange happened */
}
static inline uint64_t nova_atomic_add_u64(uint64_t *p, uint64_t v) { return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST); }
static inline uint32_t nova_atomic_load_u32(uint32_t *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
static inline void nova_atomic_store_u32(uint32_t *p, uint32_t v) { __atomic_store_n(p, v, __ATOMIC_SEQ_CST); }
static inline uint32_t nova_atomic_swap_u32(uint32_t *p, uint32_t v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
static inline uint32_t nova_atomic_cas_u32(uint32_t *p, uint32_t expected, uint32_t desired) {
    (void)__atomic_compare_exchange_n(p, &expected, desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expected;
}
static inline uint32_t nova_atomic_add_u32(uint32_t *p, uint32_t v) { return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST); }

/* A builtin's ordering must be a constant for the compiler to choose its instructions (a variable one is treated as
 * sequentially consistent), so each entry point switches. A compare-exchange fails with the ordering C11 allows for
 * its success ordering: acquire for acquire-release, relaxed for release. */
#define NV_LOAD_ORD(T, W)                                                                                            \
    static inline T nova_atomic_load_ord_##W(T *p, int32_t o) {                                                                    \
        switch (o) {                                                                                                 \
        case 0: return __atomic_load_n(p, __ATOMIC_RELAXED);                                                         \
        case 1: return __atomic_load_n(p, __ATOMIC_ACQUIRE);                                                         \
        default: return __atomic_load_n(p, __ATOMIC_SEQ_CST);                                                        \
        }                                                                                                            \
    }
#define NV_STORE_ORD(T, W)                                                                                           \
    static inline void nova_atomic_store_ord_##W(T *p, T v, int32_t o) {                                                           \
        switch (o) {                                                                                                 \
        case 0: __atomic_store_n(p, v, __ATOMIC_RELAXED); break;                                                     \
        case 2: __atomic_store_n(p, v, __ATOMIC_RELEASE); break;                                                     \
        default: __atomic_store_n(p, v, __ATOMIC_SEQ_CST); break;                                                    \
        }                                                                                                            \
    }
#define NV_RMW_ORD(T, W, NAME, CALL)                                                                                 \
    static inline T nova_atomic_##NAME##_ord_##W(T *p, T v, int32_t o) {                                                           \
        switch (o) {                                                                                                 \
        case 0: return CALL(p, v, __ATOMIC_RELAXED);                                                                 \
        case 1: return CALL(p, v, __ATOMIC_ACQUIRE);                                                                 \
        case 2: return CALL(p, v, __ATOMIC_RELEASE);                                                                 \
        case 3: return CALL(p, v, __ATOMIC_ACQ_REL);                                                                 \
        default: return CALL(p, v, __ATOMIC_SEQ_CST);                                                                \
        }                                                                                                            \
    }
#define NV_CAS_ORD(T, W)                                                                                             \
    static inline T nova_atomic_cas_ord_##W(T *p, T e, T d, int32_t o) {                                                           \
        switch (o) {                                                                                                 \
        case 0: (void)__atomic_compare_exchange_n(p, &e, d, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED); break;       \
        case 1: (void)__atomic_compare_exchange_n(p, &e, d, false, __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE); break;       \
        case 2: (void)__atomic_compare_exchange_n(p, &e, d, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED); break;       \
        case 3: (void)__atomic_compare_exchange_n(p, &e, d, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE); break;       \
        default: (void)__atomic_compare_exchange_n(p, &e, d, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); break;      \
        }                                                                                                            \
        return e;                                                                                                    \
    }
NV_LOAD_ORD(uint64_t, u64)
NV_STORE_ORD(uint64_t, u64)
NV_RMW_ORD(uint64_t, u64, swap, __atomic_exchange_n)
NV_RMW_ORD(uint64_t, u64, add, __atomic_fetch_add)
NV_CAS_ORD(uint64_t, u64)
NV_LOAD_ORD(uint32_t, u32)
NV_STORE_ORD(uint32_t, u32)
NV_RMW_ORD(uint32_t, u32, swap, __atomic_exchange_n)
NV_RMW_ORD(uint32_t, u32, add, __atomic_fetch_add)
NV_CAS_ORD(uint32_t, u32)
#undef NV_LOAD_ORD
#undef NV_STORE_ORD
#undef NV_RMW_ORD
#undef NV_CAS_ORD
#endif

/* ---- Vectors and strings (type-system.md §23; runtime/core/nova_rt_core.c) ---------------- */

/*
 * The buffer address of every empty vector: valid and aligned for any element type (elements are
 * at most max_align_t-aligned, asserted by the generated helpers), and never dereferenced.
 */
extern nv_max_align_t nova_vec_empty;

/*
 * Room for `extra` more elements of `size` bytes after the `len` in use. Returns the buffer to use:
 * `ptr` itself if it has room; otherwise a new one (capacity doubled, at least 4 and at least
 * len + extra) holding the old bytes, after the old one (if any) is freed, with *cap updated.
 * NULL, with nothing changed, if the size overflows or the allocation fails.
 */
void *nova_vec_grow(void *ptr, uint64_t len, uint64_t *cap, uint64_t extra, uint64_t size, uint32_t allocator);

/* ---- Stable vectors and slot maps (type-system.md §24) ------------------------------------ */

/* A StableVector's 32 chunks hold 8, 16, 32, ... elements: element i is in chunk log2(i + 8) - 3. */
#define NV_SVEC_CHUNKS 32u

/* Leading zero bits of a nonzero value: the builtin where GCC or clang has it, else a loop (MSVC, c-backend.md §1.1). */
static inline unsigned nv_clz64(uint64_t x) {
#if defined(__GNUC__) || defined(__clang__)
    return (unsigned)__builtin_clzll(x);
#else
    unsigned n = 0;
    while ((x & ((uint64_t)1 << 63)) == 0) {
        x <<= 1;
        n++;
    }
    return n;
#endif
}

/* The address of element `i` (i < length) of a stable vector with elements of `size` bytes. */
static inline void *nova_svec_at(void *const *chunks, uint64_t i, uint64_t size) {
    uint64_t j = i + 8u;
    unsigned k = 60u - nv_clz64(j);
    return (uint8_t *)chunks[k] + (j - ((uint64_t)8u << k)) * size;
}

/* The slot for element `len`, allocating its chunk if it is the chunk's first; NULL if full or out of memory. */
void *nova_svec_slot(void **chunks, uint64_t len, uint64_t size, uint32_t allocator);

/* Destroys nothing: frees every chunk of a stable vector. */
void nova_svec_free(void **chunks, uint32_t allocator);

/* `Handle<T>`: a slot index and the generation it was given (odd: a live value). */
typedef struct nv_handle { uint32_t index; uint32_t gen; } nv_handle;

static inline bool nova_handle_eq(nv_handle a, nv_handle b) { return a.index == b.index && a.gen == b.gen; }
/* ---- Arena (type-system.md §19.2; runtime/core/nova_rt_core.c) ---------------------------- */

/*
 * A bump allocator over a list of chunks from a free-standing allocator. Layout fixed by Nova's
 * TypeTable (five 8-byte fields: 40 bytes, alignment 8 on LP64).
 */
typedef struct nv_arena {
    void *chunks;         /* most recent chunk; each chunk starts with the previous chunk's address */
    uint8_t *cur;         /* next free byte in the current chunk */
    uint8_t *end;         /* end of the current chunk */
    uint64_t chunk_size;  /* bytes per chunk (larger allocations get their own) */
    uint64_t allocator;   /* NV_ALLOC_* */
} nv_arena;
#if UINTPTR_MAX == UINT64_MAX
_Static_assert(sizeof(nv_arena) == 40 && _Alignof(nv_arena) == 8, "nova/rt.h: nv_arena layout");
#endif

nv_arena nova_arena_create(uint32_t allocator, uint64_t chunk_size);
/* `size` bytes aligned to `align` (a power of two ≤ 16); NULL if no chunk can be obtained. */
void *nova_arena_alloc(nv_arena *a, uint64_t size, uint64_t align);
/* Releases every chunk; the arena stays usable. Drop = reset. */
void nova_arena_reset(nv_arena *a);
void nova_arena_drop(nv_arena *a);

/* ---- Fixed-buffer allocator and scratch frames (type-system.md §19.4-19.5) ---------------- */

/* Allocation from caller-provided bytes; nothing is freed. 24 bytes on LP64. */
typedef struct nv_fba {
    uint8_t *base;
    uint64_t capacity;
    uint64_t used;
} nv_fba;

/*
 * A scratch frame: the scratch stack's state when the frame was entered, which its drop restores.
 * The scratch stack itself (runtime-owned heap chunks) is global; v0 is single-threaded. 32 bytes.
 */
typedef struct nv_scratch {
    void *chunk;
    uint8_t *cur;
    uint8_t *end;
    uint64_t depth;
} nv_scratch;
#if UINTPTR_MAX == UINT64_MAX
_Static_assert(sizeof(nv_fba) == 24 && _Alignof(nv_fba) == 8, "nova/rt.h: nv_fba layout");
_Static_assert(sizeof(nv_scratch) == 32 && _Alignof(nv_scratch) == 8, "nova/rt.h: nv_scratch layout");
#endif

nv_fba nova_fba_over(uint8_t *base, uint64_t capacity);
/* Panics if a scratch frame is already active: nested frames come from nova_scratch_enter_child. */
nv_scratch nova_scratch_enter_root(nv_loc loc);
nv_scratch nova_scratch_enter_child(nv_scratch *parent);
void nova_scratch_drop(nv_scratch *frame);

/* ---- Pools (type-system.md §19.6) ------------------------------------------------------- */

/* Up to `capacity` slots of one size, allocated at the first take. 48 bytes on LP64. */
typedef struct nv_pool {
    uint8_t *slots;
    void *free;           /* free list through the slots; NULL with `used < capacity` means untouched slots */
    uint64_t capacity;
    uint64_t used;        /* slots handed out at least once */
    uint64_t slot_size;
    uint64_t allocator;
} nv_pool;
#if UINTPTR_MAX == UINT64_MAX
_Static_assert(sizeof(nv_pool) == 48 && _Alignof(nv_pool) == 8, "nova/rt.h: nv_pool layout");
#endif

nv_pool nova_pool_create(uint32_t allocator, uint64_t capacity);
/* A free slot of `size` bytes (the same for every call on a pool), aligned to 16; NULL if none. */
void *nova_pool_take(nv_pool *p, uint64_t size);
void nova_pool_release(nv_pool *p, void *slot);
void nova_pool_drop(nv_pool *p);

/* Allocation from any region (the generated helpers' single entry point); NULL on failure. */
#define NV_REGION_ARENA 0u
#define NV_REGION_FBA 1u
#define NV_REGION_SCRATCH 2u
void *nova_region_alloc(void *region, uint32_t kind, uint64_t size, uint64_t align);

/* Called by the generated `main`: configuration at start; the NOVA_ALLOC_REPORT line at the end. */
void nova_rt_start(void);
int32_t nova_rt_finish(int32_t code);

/* ---- Output and process control (runtime/core/nova_rt_core.c) --------------------------- */

void nova_write_i64(int64_t value);
void nova_write_u64(uint64_t value);
void nova_write_bool(bool value);
void nova_write_str(const char *text, size_t length);
void nova_write_view(nv_str text);
/* Console.writeBytes: raw bytes to standard output, in order with the other writes. */
void nova_write_bytes(const uint8_t *bytes, uint64_t length);
/* Console.writeError: text to standard error (standard output is flushed first). */
void nova_write_error(nv_str text);

/* ---- Program arguments, files and sockets (type-system.md §26; runtime/hosted/nova_rt_io.c) ---- */

/* The outcome of an I/O call: `error` is 0 on success (then `value` is the result), else the C library's errno. */
typedef struct nv_io_result { int64_t value; int32_t error; } nv_io_result;

/* IoError's variants, in declaration order (the checker declares the enum in this order). */
#define NV_IO_NOT_FOUND 0u
#define NV_IO_PERMISSION_DENIED 1u
#define NV_IO_ALREADY_EXISTS 2u
#define NV_IO_IS_DIRECTORY 3u
#define NV_IO_NOT_DIRECTORY 4u
#define NV_IO_INVALID_INPUT 5u
#define NV_IO_BROKEN_PIPE 6u
#define NV_IO_CONNECTION_REFUSED 7u
#define NV_IO_ADDRESS_IN_USE 8u
#define NV_IO_OUT_OF_MEMORY 9u
#define NV_IO_OTHER 10u

/* The IoError variant for an errno value (hosted), and its message (core: static text). */
uint32_t nova_io_kind(int32_t error);
nv_str nova_io_message(uint32_t kind);

/* Called by the generated `main` before anything else: keeps argv for Process.argument (never copied). */
void nova_rt_args(int32_t count, const uint8_t *const *arguments);
uint64_t nova_arg_count(void);
/* Argument `index` as a view; false if out of range or not valid UTF-8. */
bool nova_arg(uint64_t index, nv_str *out);
bool nova_utf8_valid(const uint8_t *bytes, uint64_t length);
/* Host services (type-system.md §26.7-§26.10, ADR 0028; lib/std/host.nova): a value >= 0, or a negated errno. The types
 * are written as novac writes lib/std's extern declarations, which repeat them. */
int64_t nova_host_env(uint8_t const *name, uint64_t length, uint8_t const **value, uint64_t *value_length);
int64_t nova_host_path_kind(uint8_t const *path, uint64_t length);
int64_t nova_host_dir_create(uint8_t const *path, uint64_t length);
int64_t nova_host_dir_remove(uint8_t const *path, uint64_t length);
void *nova_host_dir_open(uint8_t const *path, uint64_t length, int32_t *error);
int64_t nova_host_dir_next(void *dir, uint8_t const **name, uint64_t *length);
void nova_host_dir_close(void *dir);
int64_t nova_host_temporary(uint8_t *buffer, uint64_t capacity);
int64_t nova_host_current(uint8_t *buffer, uint64_t capacity);
int64_t nova_host_spawn(void const *arguments, uint64_t count, uint8_t const *input, uint64_t input_length,
                        uint8_t const *output, uint64_t output_length, uint8_t const *errors, uint64_t errors_length);

/* Files: `mode` 0 opens for reading, 1 creates or truncates for writing; `value` is the descriptor. */
nv_io_result nova_file_open(nv_str path, uint32_t mode);
nv_io_result nova_file_remove(nv_str path);
/* Reads up to `length` bytes (0 at the end); retries interrupted calls. */
nv_io_result nova_fd_read(int32_t fd, uint8_t *buffer, uint64_t length);
/* Writes all `length` bytes, or fails. */
nv_io_result nova_fd_write(int32_t fd, const uint8_t *bytes, uint64_t length);
/* The drop glue of File, TcpListener and TcpStream: closes the descriptor, ignoring errors. */
void nova_fd_drop(int32_t *fd);

/* TCP over IPv4: `address` is dotted text ("127.0.0.1"); `port` 0 binds any free port. */
nv_io_result nova_tcp_listen(nv_str address, uint16_t port);
nv_io_result nova_tcp_port(int32_t fd);
nv_io_result nova_tcp_accept(int32_t fd);
nv_io_result nova_tcp_connect(nv_str address, uint16_t port);
nv_io_result nova_tcp_shutdown_write(int32_t fd);

/* Process.run: starts `program` (searched on PATH) with `arguments`, waits, and returns its exit status
   (128 + the signal number if a signal ended it). Standard output is flushed first. */
nv_io_result nova_process_run(nv_str program, const nv_str *arguments, uint64_t count);
void nova_write_newline(void);
_Noreturn void nova_exit(int32_t code);

#endif /* NOVA_RT_H */
