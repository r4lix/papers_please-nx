// SSE / AVX helpers (portable lane code) for lifted x86-64
#pragma once
#include "x64rt.h"

static inline V128 vzero() { V128 v; v.q[0] = 0; v.q[1] = 0; return v; }
static inline V128 ld_x(unsigned sz, uint64_t a) { V128 v = vzero(); memcpy(&v, (void*)a, sz); return v; }
static inline void st_x(unsigned sz, uint64_t a, const V128& v) { memcpy((void*)a, &v, sz); }
static inline V128 x_from_u32(uint32_t v) { V128 r = vzero(); r.d[0] = v; return r; }
static inline V128 x_from_u64(uint64_t v) { V128 r = vzero(); r.q[0] = v; return r; }

#define SSE_SCALAR_D(name, expr) static inline V128 name(V128 a, V128 b) { V128 r = a; r.df[0] = (expr); return r; }
#define SSE_SCALAR_F(name, expr) static inline V128 name(V128 a, V128 b) { V128 r = a; r.f[0] = (expr); return r; }
SSE_SCALAR_D(sse_addsd, a.df[0] + b.df[0]) SSE_SCALAR_D(sse_subsd, a.df[0] - b.df[0]) SSE_SCALAR_D(sse_mulsd, a.df[0] * b.df[0]) SSE_SCALAR_D(sse_divsd, a.df[0] / b.df[0])
SSE_SCALAR_D(sse_minsd, (a.df[0] < b.df[0]) ? a.df[0] : b.df[0]) SSE_SCALAR_D(sse_maxsd, (a.df[0] > b.df[0]) ? a.df[0] : b.df[0]) SSE_SCALAR_D(sse_sqrtsd, sqrt(b.df[0]))
SSE_SCALAR_F(sse_addss, a.f[0] + b.f[0]) SSE_SCALAR_F(sse_subss, a.f[0] - b.f[0]) SSE_SCALAR_F(sse_mulss, a.f[0] * b.f[0]) SSE_SCALAR_F(sse_divss, a.f[0] / b.f[0])
SSE_SCALAR_F(sse_minss, (a.f[0] < b.f[0]) ? a.f[0] : b.f[0]) SSE_SCALAR_F(sse_maxss, (a.f[0] > b.f[0]) ? a.f[0] : b.f[0]) SSE_SCALAR_F(sse_sqrtss, sqrtf(b.f[0]))
#define SSE_PACKED_D(name, expr) static inline V128 name(V128 a, V128 b) { V128 r; for (int i = 0; i < 2; i++) { double x = a.df[i], y = b.df[i]; r.df[i] = (expr); } return r; }
#define SSE_PACKED_F(name, expr) static inline V128 name(V128 a, V128 b) { V128 r; for (int i = 0; i < 4; i++) { float x = a.f[i], y = b.f[i]; r.f[i] = (expr); } return r; }
SSE_PACKED_D(sse_addpd, x + y) SSE_PACKED_D(sse_subpd, x - y) SSE_PACKED_D(sse_mulpd, x * y) SSE_PACKED_D(sse_divpd, x / y) SSE_PACKED_D(sse_minpd, x < y ? x : y) SSE_PACKED_D(sse_maxpd, x > y ? x : y)
SSE_PACKED_F(sse_addps, x + y) SSE_PACKED_F(sse_subps, x - y) SSE_PACKED_F(sse_mulps, x * y) SSE_PACKED_F(sse_divps, x / y) SSE_PACKED_F(sse_minps, x < y ? x : y) SSE_PACKED_F(sse_maxps, x > y ? x : y)
static inline V128 sse_and(V128 a, V128 b) { V128 r; r.q[0] = a.q[0] & b.q[0]; r.q[1] = a.q[1] & b.q[1]; return r; }
static inline V128 sse_andn(V128 a, V128 b) { V128 r; r.q[0] = ~a.q[0] & b.q[0]; r.q[1] = ~a.q[1] & b.q[1]; return r; }
static inline V128 sse_or(V128 a, V128 b) { V128 r; r.q[0] = a.q[0] | b.q[0]; r.q[1] = a.q[1] | b.q[1]; return r; }
static inline V128 sse_xor(V128 a, V128 b) { V128 r; r.q[0] = a.q[0] ^ b.q[0]; r.q[1] = a.q[1] ^ b.q[1]; return r; }
static inline bool cmp_pred_d(int p, double a, double b) {
  bool un = (a != a) || (b != b);
  switch (p & 7) { case 0: return a == b; case 1: return a < b; case 2: return a <= b; case 3: return un; case 4: return !(a == b); case 5: return !(a < b); case 6: return !(a <= b); default: return !un; }
}
static inline V128 sse_cmpsd(V128 a, V128 b, int p) { V128 r = a; r.q[0] = cmp_pred_d(p, a.df[0], b.df[0]) ? ~0ull : 0; return r; }
static inline V128 sse_cmpss(V128 a, V128 b, int p) { V128 r = a; r.d[0] = cmp_pred_d(p, a.f[0], b.f[0]) ? ~0u : 0; return r; }
static inline V128 sse_cvtdq2pd(V128 b) { V128 r; r.df[0] = b.sd[0]; r.df[1] = b.sd[1]; return r; }
static inline V128 sse_cvtdq2ps(V128 b) { V128 r; for (int i = 0; i < 4; i++) r.f[i] = (float)b.sd[i]; return r; }
static inline V128 sse_cvtps2pd(V128 b) { V128 r; r.df[0] = b.f[0]; r.df[1] = b.f[1]; return r; }
static inline V128 sse_cvtpd2ps(V128 b) { V128 r = vzero(); r.f[0] = (float)b.df[0]; r.f[1] = (float)b.df[1]; return r; }
static inline V128 sse_cvttpd2dq(V128 b) { V128 r = vzero(); r.sd[0] = cvtt_d2i32(b.df[0]); r.sd[1] = cvtt_d2i32(b.df[1]); return r; }
static inline V128 sse_cvtpd2dq(V128 b) { V128 r = vzero(); r.sd[0] = cvt_d2i32(b.df[0]); r.sd[1] = cvt_d2i32(b.df[1]); return r; }
static inline V128 sse_cvttps2dq(V128 b) { V128 r; for (int i = 0; i < 4; i++) r.sd[i] = cvtt_f2i32(b.f[i]); return r; }
static inline V128 sse_shufps(V128 a, V128 b, int i) { V128 r; r.f[0] = a.f[i & 3]; r.f[1] = a.f[(i >> 2) & 3]; r.f[2] = b.f[(i >> 4) & 3]; r.f[3] = b.f[(i >> 6) & 3]; return r; }
static inline V128 sse_shufpd(V128 a, V128 b, int i) { V128 r; r.df[0] = a.df[i & 1]; r.df[1] = b.df[(i >> 1) & 1]; return r; }
static inline V128 sse_pshufd(V128 b, int i) { V128 r; for (int k = 0; k < 4; k++) r.d[k] = b.d[(i >> (2 * k)) & 3]; return r; }
static inline V128 sse_pshuflw(V128 b, int i) { V128 r = b; for (int k = 0; k < 4; k++) r.w[k] = b.w[(i >> (2 * k)) & 3]; return r; }
static inline V128 sse_pshufhw(V128 b, int i) { V128 r = b; for (int k = 0; k < 4; k++) r.w[4 + k] = b.w[4 + ((i >> (2 * k)) & 3)]; return r; }
static inline V128 sse_unpcklps(V128 a, V128 b) { V128 r; r.d[0] = a.d[0]; r.d[1] = b.d[0]; r.d[2] = a.d[1]; r.d[3] = b.d[1]; return r; }
static inline V128 sse_unpckhps(V128 a, V128 b) { V128 r; r.d[0] = a.d[2]; r.d[1] = b.d[2]; r.d[2] = a.d[3]; r.d[3] = b.d[3]; return r; }
static inline V128 sse_unpcklpd(V128 a, V128 b) { V128 r; r.q[0] = a.q[0]; r.q[1] = b.q[0]; return r; }
static inline V128 sse_unpckhpd(V128 a, V128 b) { V128 r; r.q[0] = a.q[1]; r.q[1] = b.q[1]; return r; }
static inline V128 sse_punpcklbw(V128 a, V128 b) { V128 r; for (int i = 0; i < 8; i++) { r.b[2 * i] = a.b[i]; r.b[2 * i + 1] = b.b[i]; } return r; }
static inline V128 sse_punpcklwd(V128 a, V128 b) { V128 r; for (int i = 0; i < 4; i++) { r.w[2 * i] = a.w[i]; r.w[2 * i + 1] = b.w[i]; } return r; }
static inline V128 sse_punpckhwd(V128 a, V128 b) { V128 r; for (int i = 0; i < 4; i++) { r.w[2 * i] = a.w[4 + i]; r.w[2 * i + 1] = b.w[4 + i]; } return r; }
static inline V128 sse_punpckldq(V128 a, V128 b) { V128 r; r.d[0] = a.d[0]; r.d[1] = b.d[0]; r.d[2] = a.d[1]; r.d[3] = b.d[1]; return r; }
static inline V128 sse_punpcklqdq(V128 a, V128 b) { V128 r; r.q[0] = a.q[0]; r.q[1] = b.q[0]; return r; }
static inline V128 sse_psrldq(V128 a, int n) { V128 r = vzero(); if (n > 15) return r; for (int i = 0; i + n < 16; i++) r.b[i] = a.b[i + n]; return r; }
static inline V128 sse_pslldq(V128 a, int n) { V128 r = vzero(); if (n > 15) return r; for (int i = n; i < 16; i++) r.b[i] = a.b[i - n]; return r; }
static inline V128 sse_psrlq(V128 a, uint64_t n) { V128 r = vzero(); if (n < 64) { r.q[0] = a.q[0] >> n; r.q[1] = a.q[1] >> n; } return r; }
static inline V128 sse_psllq(V128 a, uint64_t n) { V128 r = vzero(); if (n < 64) { r.q[0] = a.q[0] << n; r.q[1] = a.q[1] << n; } return r; }
static inline V128 sse_psrld(V128 a, uint64_t n) { V128 r = vzero(); if (n < 32) for (int i = 0; i < 4; i++) r.d[i] = a.d[i] >> n; return r; }
static inline V128 sse_pslld(V128 a, uint64_t n) { V128 r = vzero(); if (n < 32) for (int i = 0; i < 4; i++) r.d[i] = a.d[i] << n; return r; }
static inline V128 sse_psrad(V128 a, uint64_t n) { V128 r; if (n > 31) n = 31; for (int i = 0; i < 4; i++) r.sd[i] = a.sd[i] >> n; return r; }
static inline V128 sse_paddq(V128 a, V128 b) { V128 r; r.q[0] = a.q[0] + b.q[0]; r.q[1] = a.q[1] + b.q[1]; return r; }
static inline V128 sse_psubq(V128 a, V128 b) { V128 r; r.q[0] = a.q[0] - b.q[0]; r.q[1] = a.q[1] - b.q[1]; return r; }
static inline V128 sse_paddd(V128 a, V128 b) { V128 r; for (int i = 0; i < 4; i++) r.d[i] = a.d[i] + b.d[i]; return r; }
static inline V128 sse_psubd(V128 a, V128 b) { V128 r; for (int i = 0; i < 4; i++) r.d[i] = a.d[i] - b.d[i]; return r; }
static inline V128 sse_pcmpeqb(V128 a, V128 b) { V128 r; for (int i = 0; i < 16; i++) r.b[i] = a.b[i] == b.b[i] ? 0xff : 0; return r; }
static inline V128 sse_pcmpeqw(V128 a, V128 b) { V128 r; for (int i = 0; i < 8; i++) r.w[i] = a.w[i] == b.w[i] ? 0xffff : 0; return r; }
static inline V128 sse_pcmpeqd(V128 a, V128 b) { V128 r; for (int i = 0; i < 4; i++) r.d[i] = a.d[i] == b.d[i] ? 0xffffffffu : 0; return r; }
static inline uint32_t sse_pmovmskb(V128 a) { uint32_t m = 0; for (int i = 0; i < 16; i++) m |= (uint32_t)(a.b[i] >> 7) << i; return m; }
static inline uint32_t sse_movmskps(V128 a) { uint32_t m = 0; for (int i = 0; i < 4; i++) m |= (a.d[i] >> 31) << i; return m; }
static inline uint32_t sse_movmskpd(V128 a) { return (uint32_t)((a.q[0] >> 63) | ((a.q[1] >> 63) << 1)); }
static inline V128 sse_movlhps(V128 a, V128 b) { V128 r = a; r.q[1] = b.q[0]; return r; }
static inline V128 sse_movhlps(V128 a, V128 b) { V128 r = a; r.q[0] = b.q[1]; return r; }
static inline V128 sse_pmovsxdq(V128 b) { V128 r; r.sq[0] = b.sd[0]; r.sq[1] = b.sd[1]; return r; }
static inline V128 fma_sd(V128 d, double a, double b, double c, bool neg_prod, bool neg_add) { V128 r = d; r.df[0] = fma(neg_prod ? -a : a, b, neg_add ? -c : c); return r; }
static inline V128 fma_ss(V128 d, float a, float b, float c, bool neg_prod, bool neg_add) { V128 r = d; r.f[0] = fmaf(neg_prod ? -a : a, b, neg_add ? -c : c); return r; }

// pcmpistri (implicit length string compare, index result); returns ecx, flags in *fl as C|Z<<1|S<<2|O<<3
static inline uint32_t sse_pcmpistri(V128 a, V128 b, int imm, uint32_t* fl) {
  bool word = imm & 1, sgn = imm & 2; int n = word ? 8 : 16;
  auto el = [&](const V128& v, int i) -> int { return word ? (sgn ? (int)v.sw[i] : (int)v.w[i]) : (sgn ? (int)v.sb[i] : (int)v.b[i]); };
  int la = n, lb = n;
  for (int i = 0; i < n; i++) if (el(a, i) == 0) { la = i; break; }
  for (int i = 0; i < n; i++) if (el(b, i) == 0) { lb = i; break; }
  uint32_t r1 = 0;
  int agg = (imm >> 2) & 3;
  for (int j = 0; j < n; j++) {
    bool res = false;
    bool v2 = j < lb;
    if (agg == 0) { if (v2) for (int i = 0; i < la; i++) if (el(a, i) == el(b, j)) { res = true; break; } }
    else if (agg == 1) { if (v2) for (int k = 0; 2 * k + 1 < la; k++) if (el(a, 2 * k) <= el(b, j) && el(b, j) <= el(a, 2 * k + 1)) { res = true; break; } }
    else if (agg == 2) { bool v1 = j < la; res = (v1 && v2) ? el(a, j) == el(b, j) : (!v1 && !v2); }
    else { res = true; for (int i = 0; i < la; i++) { int p = j + i; if (p >= n || p >= lb || el(a, i) != el(b, p)) { res = false; break; } } }
    if (res) r1 |= 1u << j;
  }
  uint32_t full = (1u << n) - 1, r2 = r1;
  int pol = (imm >> 4) & 3;
  if (pol == 1) r2 = ~r1 & full;
  else if (pol == 3) { r2 = 0; for (int j = 0; j < n; j++) { bool v2 = j < lb; bool bit = (r1 >> j) & 1; if (v2 ? !bit : bit) r2 |= 1u << j; } }
  uint32_t idx = n;
  if (r2) idx = (imm & 0x40) ? 31 - __builtin_clz(r2) : __builtin_ctz(r2);
  *fl = (r2 != 0) | ((lb < n) << 1) | ((la < n) << 2) | ((r2 & 1) << 3);
  return idx;
}
