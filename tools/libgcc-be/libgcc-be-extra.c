/* C parts of libgcc needed by big-endian ARM targets (Samsung YP-S3).
 *
 * The ARM GNU toolchains only ship little-endian multilibs. The assembly
 * parts of libgcc (lib1funcs.S, ieee754-*.S, bpabi.S) are assembled from
 * the GCC sources by build.sh; these are the few C helpers that go with
 * them, written from scratch.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

typedef unsigned long long u64;
typedef long long s64;

u64 __udivmoddi4(u64 n, u64 d, u64 *rem)
{
    u64 q = 0;
    int shift = 0;

    if (d == 0) {
        if (rem)
            *rem = n;
        return ~0ULL;   /* undefined, don't trap */
    }
    while (!(d & (1ULL << 63)) && d <= n) {
        d <<= 1;
        shift++;
    }
    while (shift >= 0) {
        q <<= 1;
        if (n >= d) {
            n -= d;
            q |= 1;
        }
        d >>= 1;
        shift--;
    }
    if (rem)
        *rem = n;
    return q;
}

u64 __udivdi3(u64 n, u64 d)
{
    return __udivmoddi4(n, d, 0);
}

u64 __umoddi3(u64 n, u64 d)
{
    u64 r;
    __udivmoddi4(n, d, &r);
    return r;
}

s64 __divdi3(s64 a, s64 b)
{
    int neg = (a < 0) ^ (b < 0);
    u64 q = __udivmoddi4(a < 0 ? -(u64)a : (u64)a,
                         b < 0 ? -(u64)b : (u64)b, 0);
    return neg ? -(s64)q : (s64)q;
}

s64 __moddi3(s64 a, s64 b)
{
    u64 r;
    __udivmoddi4(a < 0 ? -(u64)a : (u64)a, b < 0 ? -(u64)b : (u64)b, &r);
    return a < 0 ? -(s64)r : (s64)r;
}

int __ctzdi2(u64 x)
{
    int n = 0;
    if (!(unsigned int)x) {
        n = 32;
        x >>= 32;
    }
    while (!(x & 1) && n < 64) {
        x >>= 1;
        n++;
    }
    return n;
}

int __clzdi2(u64 x)
{
    int n = 0;
    if (!(x >> 32)) {
        n = 32;
        x <<= 32;
    }
    while (!(x & (1ULL << 63)) && n < 64) {
        x <<= 1;
        n++;
    }
    return n;
}

/* double -> 64 bit integer, truncating, saturating like GCC's */
union dbl {
    double d;
    u64 u;
};

u64 __fixunsdfdi(double a)
{
    union dbl v = { .d = a };
    int exp = (int)((v.u >> 52) & 0x7ff) - 1023;
    u64 mant;

    if ((v.u >> 63) || exp < 0)
        return 0;
    if (exp >= 64)
        return ~0ULL;
    mant = (v.u & ((1ULL << 52) - 1)) | (1ULL << 52);
    return exp >= 52 ? mant << (exp - 52) : mant >> (52 - exp);
}

s64 __fixdfdi(double a)
{
    union dbl v = { .d = a };
    int neg = v.u >> 63;
    u64 m;

    v.u &= ~(1ULL << 63);
    if (((v.u >> 52) & 0x7ff) >= 1023 + 63)
        return neg ? (s64)(1ULL << 63) : (s64)((1ULL << 63) - 1);
    m = __fixunsdfdi(v.d);
    return neg ? -(s64)m : (s64)m;
}

/* run-time ABI names */
s64 __aeabi_d2lz(double a) __attribute__((alias("__fixdfdi")));
u64 __aeabi_d2ulz(double a) __attribute__((alias("__fixunsdfdi")));
