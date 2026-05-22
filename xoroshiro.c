/*
 * xoroshiro.c — PHP Extension for Xoroshiro128+ RNG
 *
 * Implements the same algorithm as Minecraft's Xoroshiro128+ generator,
 * ported from the Rust source. Designed for PocketMine-MP 5 (PHP 8.x).
 *
 * Build:
 *   phpize && ./configure && make
 *
 * All 64-bit arithmetic uses unsigned 64-bit integers internally and
 * is exposed to PHP via strings (GMP) or split hi/lo pairs to avoid
 * PHP's signed 63-bit integer limitations on 32-bit platforms.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "php_xoroshiro.h"
#include "zend_exceptions.h"
#include "ext/spl/spl_exceptions.h"
#include "ext/standard/md5.h"

#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Internal RNG helpers                                                */
/* ------------------------------------------------------------------ */

static inline uint64_t rotl64(uint64_t x, int k) {
    return (x << k) | (x >> (64 - k));
}

static uint64_t mix_stafford_13(uint64_t z) {
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return z ^ (z >> 31);
}

/* Xoroshiro128+ state */
typedef struct {
    uint64_t lo;
    uint64_t hi;
    /* Stored Box-Muller spare Gaussian value */
    int      has_gaussian;
    double   next_gaussian;
} xoroshiro_state;

static void xoro_init_from_seed(xoroshiro_state *st, uint64_t seed) {
    uint64_t l = seed  ^ UINT64_C(0x6A09E667F3BCC909);
    uint64_t m = l + UINT64_C(0x9E3779B97F4A7C15);
    l = mix_stafford_13(l);
    m = mix_stafford_13(m);
    if ((l | m) == 0) {
        l = UINT64_C(0x9E3779B97F4A7C15);
        m = UINT64_C(0x6A09E667F3BCC909);
    }
    st->lo = l;
    st->hi = m;
    st->has_gaussian = 0;
    st->next_gaussian = 0.0;
}

static void xoro_init_unmixed(xoroshiro_state *st, uint64_t seed) {
    uint64_t l = seed ^ UINT64_C(0x6A09E667F3BCC909);
    uint64_t m = l + UINT64_C(0x9E3779B97F4A7C15);
    if ((l | m) == 0) {
        l = UINT64_C(0x9E3779B97F4A7C15);
        m = UINT64_C(0x6A09E667F3BCC909);
    }
    st->lo = l;
    st->hi = m;
    st->has_gaussian = 0;
    st->next_gaussian = 0.0;
}

static void xoro_init_lo_hi(xoroshiro_state *st, uint64_t lo, uint64_t hi) {
    if ((lo | hi) == 0) {
        lo = UINT64_C(0x9E3779B97F4A7C15);
        hi = UINT64_C(0x6A09E667F3BCC909);
    }
    st->lo = lo;
    st->hi = hi;
    st->has_gaussian = 0;
    st->next_gaussian = 0.0;
}

static uint64_t xoro_next_raw(xoroshiro_state *st) {
    uint64_t l = st->lo;
    uint64_t m = st->hi;
    uint64_t n = rotl64(l + m, 17) + l;
    m ^= l;
    st->lo = rotl64(l, 49) ^ m ^ (m << 21);
    st->hi = rotl64(m, 28);
    return n;
}

static int64_t xoro_next_i32(xoroshiro_state *st) {
    return (int32_t)(xoro_next_raw(st) & UINT64_C(0xFFFFFFFF));
}

static int32_t xoro_next_bounded_i32(xoroshiro_state *st, int32_t bound) {
    uint64_t l, m, n;
    uint64_t threshold = ((uint64_t)(uint32_t)(-bound)) % (uint32_t)bound;
    do {
        l = xoro_next_raw(st) & UINT64_C(0xFFFFFFFF);
        m = l * (uint64_t)(uint32_t)bound;
        n = m & UINT64_C(0xFFFFFFFF);
    } while (n < threshold);
    return (int32_t)(m >> 32);
}

static double xoro_next_f64(xoroshiro_state *st) {
    uint64_t bits = xoro_next_raw(st) >> 11; /* 53 bits */
    return (double)bits * 1.1102230246251565E-16;
}

static float xoro_next_f32(xoroshiro_state *st) {
    uint64_t bits = xoro_next_raw(st) >> 40; /* 24 bits */
    return (float)bits * 5.9604645E-8f;
}

static double xoro_next_gaussian(xoroshiro_state *st) {
    if (st->has_gaussian) {
        st->has_gaussian = 0;
        return st->next_gaussian;
    }
    double v1, v2, s;
    do {
        v1 = 2.0 * xoro_next_f64(st) - 1.0;
        v2 = 2.0 * xoro_next_f64(st) - 1.0;
        s = v1 * v1 + v2 * v2;
    } while (s >= 1.0 || s == 0.0);
    double multiplier = sqrt(-2.0 * log(s) / s);
    st->next_gaussian = v2 * multiplier;
    st->has_gaussian = 1;
    return v1 * multiplier;
}

/* hash_block_pos: same as Minecraft's BlockPos hash */
static int64_t hash_block_pos(int32_t x, int32_t y, int32_t z) {
    int64_t l = (int64_t)x * 3129871LL ^ (int64_t)z * 116129781LL ^ (int64_t)y;
    l = l * l * 42317861LL + l * 11LL;
    return l >> 16;
}

/* ------------------------------------------------------------------ */
/*  PHP object definition                                               */
/* ------------------------------------------------------------------ */

zend_class_entry *xoroshiro_ce;
static zend_object_handlers xoroshiro_handlers;

typedef struct {
    xoroshiro_state state;
    zend_object     std;
} xoroshiro_obj;

static inline xoroshiro_obj *xoroshiro_from_obj(zend_object *obj) {
    return (xoroshiro_obj *)((char *)obj - XtOffsetOf(xoroshiro_obj, std));
}

#define XOROSHIRO_OBJ(zv) xoroshiro_from_obj(Z_OBJ_P(zv))

static zend_object *xoroshiro_create_object(zend_class_entry *ce) {
    xoroshiro_obj *intern = zend_object_alloc(sizeof(xoroshiro_obj), ce);
    zend_object_std_init(&intern->std, ce);
    object_properties_init(&intern->std, ce);
    intern->std.handlers = &xoroshiro_handlers;
    return &intern->std;
}

/* ------------------------------------------------------------------ */
/*  Helper: PHP long <-> uint64 conversions                            */
/*                                                                      */
/*  PHP_INT_SIZE == 8 on 64-bit platforms (all modern PocketMine hosts) */
/*  We store uint64 in a zend_long (int64). Bit patterns are preserved; */
/*  large unsigned values appear negative in PHP – use sprintf/gmp for  */
/*  display if needed.                                                  */
/* ------------------------------------------------------------------ */

static inline uint64_t zl_to_u64(zend_long v) {
    return (uint64_t)(int64_t)v;
}

static inline zend_long u64_to_zl(uint64_t v) {
    return (zend_long)(int64_t)v;
}

/* ------------------------------------------------------------------ */
/*  PHP Methods                                                         */
/* ------------------------------------------------------------------ */

/* Xoroshiro::__construct(int $seed) */
PHP_METHOD(Xoroshiro, __construct) {
    zend_long seed;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(seed)
    ZEND_PARSE_PARAMETERS_END();

    xoroshiro_obj *intern = XOROSHIRO_OBJ(ZEND_THIS);
    xoro_init_from_seed(&intern->state, zl_to_u64(seed));
}

/* Xoroshiro::fromSeed(int $seed): static */
PHP_METHOD(Xoroshiro, fromSeed) {
    zend_long seed;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(seed)
    ZEND_PARSE_PARAMETERS_END();

    object_init_ex(return_value, xoroshiro_ce);
    xoroshiro_obj *intern = XOROSHIRO_OBJ(return_value);
    xoro_init_from_seed(&intern->state, zl_to_u64(seed));
}

/* Xoroshiro::fromSeedUnmixed(int $seed): static */
PHP_METHOD(Xoroshiro, fromSeedUnmixed) {
    zend_long seed;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(seed)
    ZEND_PARSE_PARAMETERS_END();

    object_init_ex(return_value, xoroshiro_ce);
    xoroshiro_obj *intern = XOROSHIRO_OBJ(return_value);
    xoro_init_unmixed(&intern->state, zl_to_u64(seed));
}

/* Xoroshiro::fromLoHi(int $lo, int $hi): static */
PHP_METHOD(Xoroshiro, fromLoHi) {
    zend_long lo, hi;
    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_LONG(lo)
        Z_PARAM_LONG(hi)
    ZEND_PARSE_PARAMETERS_END();

    object_init_ex(return_value, xoroshiro_ce);
    xoroshiro_obj *intern = XOROSHIRO_OBJ(return_value);
    xoro_init_lo_hi(&intern->state, zl_to_u64(lo), zl_to_u64(hi));
}

/* nextI32(): int  — full 32-bit signed */
PHP_METHOD(Xoroshiro, nextI32) {
    ZEND_PARSE_PARAMETERS_NONE();
    xoroshiro_obj *intern = XOROSHIRO_OBJ(ZEND_THIS);
    RETURN_LONG((zend_long)(int32_t)(xoro_next_raw(&intern->state) & UINT64_C(0xFFFFFFFF)));
}

/* nextBoundedI32(int $bound): int  — [0, bound) */
PHP_METHOD(Xoroshiro, nextBoundedI32) {
    zend_long bound;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(bound)
    ZEND_PARSE_PARAMETERS_END();

    if (bound <= 0) {
        zend_throw_exception(spl_ce_InvalidArgumentException,
            "Bound must be a positive integer", 0);
        RETURN_THROWS();
    }

    xoroshiro_obj *intern = XOROSHIRO_OBJ(ZEND_THIS);
    RETURN_LONG((zend_long)xoro_next_bounded_i32(&intern->state, (int32_t)bound));
}

/* nextInbetweenI32(int $min, int $max): int  — [min, max] */
PHP_METHOD(Xoroshiro, nextInbetweenI32) {
    zend_long min, max;
    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_LONG(min)
        Z_PARAM_LONG(max)
    ZEND_PARSE_PARAMETERS_END();

    xoroshiro_obj *intern = XOROSHIRO_OBJ(ZEND_THIS);
    int32_t range = (int32_t)(max - min) + 1;
    int32_t val   = xoro_next_bounded_i32(&intern->state, range);
    RETURN_LONG((zend_long)(min + val));
}

/* nextI64(): int  — full 64-bit (signed interpretation) */
PHP_METHOD(Xoroshiro, nextI64) {
    ZEND_PARSE_PARAMETERS_NONE();
    xoroshiro_obj *intern = XOROSHIRO_OBJ(ZEND_THIS);
    RETURN_LONG(u64_to_zl(xoro_next_raw(&intern->state)));
}

/* nextBool(): bool */
PHP_METHOD(Xoroshiro, nextBool) {
    ZEND_PARSE_PARAMETERS_NONE();
    xoroshiro_obj *intern = XOROSHIRO_OBJ(ZEND_THIS);
    RETURN_BOOL((xoro_next_raw(&intern->state) & 1) != 0);
}

/* nextF32(): float */
PHP_METHOD(Xoroshiro, nextF32) {
    ZEND_PARSE_PARAMETERS_NONE();
    xoroshiro_obj *intern = XOROSHIRO_OBJ(ZEND_THIS);
    RETURN_DOUBLE((double)xoro_next_f32(&intern->state));
}

/* nextF64(): float */
PHP_METHOD(Xoroshiro, nextF64) {
    ZEND_PARSE_PARAMETERS_NONE();
    xoroshiro_obj *intern = XOROSHIRO_OBJ(ZEND_THIS);
    RETURN_DOUBLE(xoro_next_f64(&intern->state));
}

/* nextGaussian(): float */
PHP_METHOD(Xoroshiro, nextGaussian) {
    ZEND_PARSE_PARAMETERS_NONE();
    xoroshiro_obj *intern = XOROSHIRO_OBJ(ZEND_THIS);
    RETURN_DOUBLE(xoro_next_gaussian(&intern->state));
}

/* nextTriangular(float $mode, float $spread): float */
PHP_METHOD(Xoroshiro, nextTriangular) {
    double mode, spread;
    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_DOUBLE(mode)
        Z_PARAM_DOUBLE(spread)
    ZEND_PARSE_PARAMETERS_END();

    xoroshiro_obj *intern = XOROSHIRO_OBJ(ZEND_THIS);
    double v = xoro_next_f64(&intern->state) - xoro_next_f64(&intern->state);
    RETURN_DOUBLE(mode + spread * v);
}

/* split(): static  — creates an independent child generator */
PHP_METHOD(Xoroshiro, split) {
    ZEND_PARSE_PARAMETERS_NONE();
    xoroshiro_obj *intern = XOROSHIRO_OBJ(ZEND_THIS);
    uint64_t new_lo = xoro_next_raw(&intern->state);
    uint64_t new_hi = xoro_next_raw(&intern->state);

    object_init_ex(return_value, xoroshiro_ce);
    xoroshiro_obj *child = XOROSHIRO_OBJ(return_value);
    xoro_init_lo_hi(&child->state, new_lo, new_hi);
}

/* nextSplitter(): XoroshiroSplitter */
PHP_METHOD(Xoroshiro, nextSplitter);   /* forward; defined after splitter class */

/* getState(): array{lo: int, hi: int}  — expose raw state for serialisation */
PHP_METHOD(Xoroshiro, getState) {
    ZEND_PARSE_PARAMETERS_NONE();
    xoroshiro_obj *intern = XOROSHIRO_OBJ(ZEND_THIS);
    array_init(return_value);
    add_assoc_long(return_value, "lo", u64_to_zl(intern->state.lo));
    add_assoc_long(return_value, "hi", u64_to_zl(intern->state.hi));
}

/* ------------------------------------------------------------------ */
/*  XoroshiroSplitter object                                            */
/* ------------------------------------------------------------------ */

zend_class_entry *xoroshiro_splitter_ce;
static zend_object_handlers xoroshiro_splitter_handlers;

typedef struct {
    uint64_t    lo;
    uint64_t    hi;
    zend_object std;
} xoroshiro_splitter_obj;

static inline xoroshiro_splitter_obj *splitter_from_obj(zend_object *obj) {
    return (xoroshiro_splitter_obj *)((char *)obj - XtOffsetOf(xoroshiro_splitter_obj, std));
}

#define SPLITTER_OBJ(zv) splitter_from_obj(Z_OBJ_P(zv))

static zend_object *xoroshiro_splitter_create_object(zend_class_entry *ce) {
    xoroshiro_splitter_obj *intern = zend_object_alloc(sizeof(xoroshiro_splitter_obj), ce);
    zend_object_std_init(&intern->std, ce);
    object_properties_init(&intern->std, ce);
    intern->std.handlers = &xoroshiro_splitter_handlers;
    return &intern->std;
}

/* Xoroshiro::nextSplitter() — now that splitter class is declared */
PHP_METHOD(Xoroshiro, nextSplitter) {
    ZEND_PARSE_PARAMETERS_NONE();
    xoroshiro_obj *intern = XOROSHIRO_OBJ(ZEND_THIS);
    uint64_t slo = xoro_next_raw(&intern->state);
    uint64_t shi = xoro_next_raw(&intern->state);

    object_init_ex(return_value, xoroshiro_splitter_ce);
    xoroshiro_splitter_obj *s = SPLITTER_OBJ(return_value);
    s->lo = slo;
    s->hi = shi;
}

/* XoroshiroSplitter::splitString(string $seed): Xoroshiro */
PHP_METHOD(XoroshiroSplitter, splitString) {
    zend_string *seed;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(seed)
    ZEND_PARSE_PARAMETERS_END();

    xoroshiro_splitter_obj *intern = SPLITTER_OBJ(ZEND_THIS);

    /* MD5 of the string — 16 bytes */
    PHP_MD5_CTX ctx;
    unsigned char digest[16];
    PHP_MD5Init(&ctx);
    PHP_MD5Update(&ctx, (const unsigned char *)ZSTR_VAL(seed), ZSTR_LEN(seed));
    PHP_MD5Final(digest, &ctx);

    uint64_t l, m;
    memcpy(&l, digest,     8);
    memcpy(&m, digest + 8, 8);
    /* MD5 bytes are little-endian on x86; Minecraft reads big-endian */
    /* swap bytes to match Java's big-endian ByteBuffer.getLong() */
    l = __builtin_bswap64(l);
    m = __builtin_bswap64(m);

    object_init_ex(return_value, xoroshiro_ce);
    xoroshiro_obj *child = XOROSHIRO_OBJ(return_value);
    xoro_init_lo_hi(&child->state, l ^ intern->lo, m ^ intern->hi);
}

/* XoroshiroSplitter::splitU64(int $seed): Xoroshiro */
PHP_METHOD(XoroshiroSplitter, splitU64) {
    zend_long seed;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(seed)
    ZEND_PARSE_PARAMETERS_END();

    xoroshiro_splitter_obj *intern = SPLITTER_OBJ(ZEND_THIS);
    uint64_t s = zl_to_u64(seed);

    object_init_ex(return_value, xoroshiro_ce);
    xoroshiro_obj *child = XOROSHIRO_OBJ(return_value);
    xoro_init_lo_hi(&child->state, s ^ intern->lo, s ^ intern->hi);
}

/* XoroshiroSplitter::splitPos(int $x, int $y, int $z): Xoroshiro */
PHP_METHOD(XoroshiroSplitter, splitPos) {
    zend_long x, y, z;
    ZEND_PARSE_PARAMETERS_START(3, 3)
        Z_PARAM_LONG(x)
        Z_PARAM_LONG(y)
        Z_PARAM_LONG(z)
    ZEND_PARSE_PARAMETERS_END();

    xoroshiro_splitter_obj *intern = SPLITTER_OBJ(ZEND_THIS);
    uint64_t l = (uint64_t)hash_block_pos((int32_t)x, (int32_t)y, (int32_t)z);
    uint64_t m = l ^ intern->lo;

    object_init_ex(return_value, xoroshiro_ce);
    xoroshiro_obj *child = XOROSHIRO_OBJ(return_value);
    xoro_init_lo_hi(&child->state, m, intern->hi);
}

/* XoroshiroSplitter::fromLoHi(int $lo, int $hi, int $xorLo, int $xorHi): Xoroshiro
   Mirrors split_string's from_lo_and_hi */
PHP_METHOD(XoroshiroSplitter, fromLoHi) {
    zend_long lo, hi;
    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_LONG(lo)
        Z_PARAM_LONG(hi)
    ZEND_PARSE_PARAMETERS_END();

    xoroshiro_splitter_obj *intern = SPLITTER_OBJ(ZEND_THIS);

    object_init_ex(return_value, xoroshiro_ce);
    xoroshiro_obj *child = XOROSHIRO_OBJ(return_value);
    xoro_init_lo_hi(&child->state,
        zl_to_u64(lo) ^ intern->lo,
        zl_to_u64(hi) ^ intern->hi);
}

/* ------------------------------------------------------------------ */
/*  Free functions                                                      */
/* ------------------------------------------------------------------ */

/* xoroshiro_mix_stafford13(int $value): int */
PHP_FUNCTION(xoroshiro_mix_stafford13) {
    zend_long v;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(v)
    ZEND_PARSE_PARAMETERS_END();
    RETURN_LONG(u64_to_zl(mix_stafford_13(zl_to_u64(v))));
}

/* xoroshiro_hash_block_pos(int $x, int $y, int $z): int */
PHP_FUNCTION(xoroshiro_hash_block_pos) {
    zend_long x, y, z;
    ZEND_PARSE_PARAMETERS_START(3, 3)
        Z_PARAM_LONG(x)
        Z_PARAM_LONG(y)
        Z_PARAM_LONG(z)
    ZEND_PARSE_PARAMETERS_END();
    RETURN_LONG((zend_long)hash_block_pos((int32_t)x, (int32_t)y, (int32_t)z));
}

/* ------------------------------------------------------------------ */
/*  arginfo (PHP 8 stub style)                                          */
/* ------------------------------------------------------------------ */

ZEND_BEGIN_ARG_INFO_EX(arginfo_construct, 0, 0, 1)
    ZEND_ARG_TYPE_INFO(0, seed, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_static_seed, 0, 1, Xoroshiro, 0)
    ZEND_ARG_TYPE_INFO(0, seed, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_static_lohi, 0, 2, Xoroshiro, 0)
    ZEND_ARG_TYPE_INFO(0, lo, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, hi, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_no_args_long, 0, 0, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_no_args_bool, 0, 0, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_no_args_double, 0, 0, IS_DOUBLE, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_bounded_i32, 0, 1, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, bound, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_inbetween_i32, 0, 2, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, min, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, max, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_triangular, 0, 2, IS_DOUBLE, 0)
    ZEND_ARG_TYPE_INFO(0, mode, IS_DOUBLE, 0)
    ZEND_ARG_TYPE_INFO(0, spread, IS_DOUBLE, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_split, 0, 0, Xoroshiro, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_next_splitter, 0, 0, XoroshiroSplitter, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_get_state, 0, 0, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

/* Splitter arginfos */
ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_split_string, 0, 1, Xoroshiro, 0)
    ZEND_ARG_TYPE_INFO(0, seed, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_split_u64, 0, 1, Xoroshiro, 0)
    ZEND_ARG_TYPE_INFO(0, seed, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_split_pos, 0, 3, Xoroshiro, 0)
    ZEND_ARG_TYPE_INFO(0, x, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, y, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, z, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_splitter_lohi, 0, 2, Xoroshiro, 0)
    ZEND_ARG_TYPE_INFO(0, lo, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, hi, IS_LONG, 0)
ZEND_END_ARG_INFO()

/* Free function arginfos */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_mix_stafford13, 0, 1, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, value, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_hash_block_pos, 0, 3, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, x, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, y, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, z, IS_LONG, 0)
ZEND_END_ARG_INFO()

/* ------------------------------------------------------------------ */
/*  Method tables                                                       */
/* ------------------------------------------------------------------ */

static const zend_function_entry xoroshiro_methods[] = {
    PHP_ME(Xoroshiro, __construct,       arginfo_construct,       ZEND_ACC_PUBLIC)
    PHP_ME(Xoroshiro, fromSeed,          arginfo_static_seed,     ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(Xoroshiro, fromSeedUnmixed,   arginfo_static_seed,     ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(Xoroshiro, fromLoHi,          arginfo_static_lohi,     ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(Xoroshiro, nextI32,           arginfo_no_args_long,    ZEND_ACC_PUBLIC)
    PHP_ME(Xoroshiro, nextBoundedI32,    arginfo_bounded_i32,     ZEND_ACC_PUBLIC)
    PHP_ME(Xoroshiro, nextInbetweenI32,  arginfo_inbetween_i32,   ZEND_ACC_PUBLIC)
    PHP_ME(Xoroshiro, nextI64,           arginfo_no_args_long,    ZEND_ACC_PUBLIC)
    PHP_ME(Xoroshiro, nextBool,          arginfo_no_args_bool,    ZEND_ACC_PUBLIC)
    PHP_ME(Xoroshiro, nextF32,           arginfo_no_args_double,  ZEND_ACC_PUBLIC)
    PHP_ME(Xoroshiro, nextF64,           arginfo_no_args_double,  ZEND_ACC_PUBLIC)
    PHP_ME(Xoroshiro, nextGaussian,      arginfo_no_args_double,  ZEND_ACC_PUBLIC)
    PHP_ME(Xoroshiro, nextTriangular,    arginfo_triangular,      ZEND_ACC_PUBLIC)
    PHP_ME(Xoroshiro, split,             arginfo_split,           ZEND_ACC_PUBLIC)
    PHP_ME(Xoroshiro, nextSplitter,      arginfo_next_splitter,   ZEND_ACC_PUBLIC)
    PHP_ME(Xoroshiro, getState,          arginfo_get_state,       ZEND_ACC_PUBLIC)
    PHP_FE_END
};

static const zend_function_entry xoroshiro_splitter_methods[] = {
    PHP_ME(XoroshiroSplitter, splitString, arginfo_split_string,   ZEND_ACC_PUBLIC)
    PHP_ME(XoroshiroSplitter, splitU64,    arginfo_split_u64,      ZEND_ACC_PUBLIC)
    PHP_ME(XoroshiroSplitter, splitPos,    arginfo_split_pos,      ZEND_ACC_PUBLIC)
    PHP_ME(XoroshiroSplitter, fromLoHi,    arginfo_splitter_lohi,  ZEND_ACC_PUBLIC)
    PHP_FE_END
};

static const zend_function_entry xoroshiro_functions[] = {
    PHP_FE(xoroshiro_mix_stafford13, arginfo_mix_stafford13)
    PHP_FE(xoroshiro_hash_block_pos, arginfo_hash_block_pos)
    PHP_FE_END
};

/* ------------------------------------------------------------------ */
/*  Module lifecycle                                                    */
/* ------------------------------------------------------------------ */

PHP_MINIT_FUNCTION(xoroshiro) {
    /* Register Xoroshiro class */
    xoroshiro_ce = zend_register_internal_class(
        &(zend_class_entry){ .name = zend_string_init_interned("Xoroshiro", 9, 1) });
    xoroshiro_ce->create_object = xoroshiro_create_object;

    memcpy(&xoroshiro_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
    xoroshiro_handlers.offset = XtOffsetOf(xoroshiro_obj, std);
    xoroshiro_handlers.free_obj = zend_object_std_dtor; /* no extra resources */

    zend_register_functions(xoroshiro_ce, xoroshiro_methods, &xoroshiro_ce->function_table, EG(current_module)->type);

    /* Register XoroshiroSplitter class */
    xoroshiro_splitter_ce = zend_register_internal_class(
        &(zend_class_entry){ .name = zend_string_init_interned("XoroshiroSplitter", 17, 1) });
    xoroshiro_splitter_ce->create_object = xoroshiro_splitter_create_object;

    memcpy(&xoroshiro_splitter_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
    xoroshiro_splitter_handlers.offset = XtOffsetOf(xoroshiro_splitter_obj, std);
    xoroshiro_splitter_handlers.free_obj = zend_object_std_dtor;

    zend_register_functions(xoroshiro_splitter_ce, xoroshiro_splitter_methods, &xoroshiro_splitter_ce->function_table, EG(current_module)->type);

    return SUCCESS;
}

PHP_MINFO_FUNCTION(xoroshiro) {
    php_info_print_table_start();
    php_info_print_table_header(2, "xoroshiro support", "enabled");
    php_info_print_table_row(2, "Version", PHP_XOROSHIRO_VERSION);
    php_info_print_table_row(2, "Algorithm", "Xoroshiro128+");
    php_info_print_table_end();
}

zend_module_entry xoroshiro_module_entry = {
    STANDARD_MODULE_HEADER,
    "xoroshiro",
    xoroshiro_functions,
    PHP_MINIT(xoroshiro),
    NULL, /* MSHUTDOWN */
    NULL, /* RINIT */
    NULL, /* RSHUTDOWN */
    PHP_MINFO(xoroshiro),
    PHP_XOROSHIRO_VERSION,
    STANDARD_MODULE_PROPERTIES
};

#ifdef COMPILE_DL_XOROSHIRO
ZEND_GET_MODULE(xoroshiro)
#endif