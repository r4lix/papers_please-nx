// x64rt: runtime support for statically lifted x86-64 code (see tools/lift/lift.py).
// Guest memory == host memory (identity mapped); guest registers live in C++ locals and are spilled to Ctx at calls.
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>

struct V128 {
  union { uint8_t b[16]; int8_t sb[16]; uint16_t w[8]; int16_t sw[8]; uint32_t d[4]; int32_t sd[4]; uint64_t q[2]; int64_t sq[2]; float f[4]; double df[2]; };
};
struct Ctx {
  uint64_t r[16];
  V128 x[16];
  uint64_t aux;   // bridge: original return address
};

struct GuestResume { uint64_t entry_rsp, rip; uint64_t r[16]; V128 x[16]; };   // thrown to resume execution in a lifted function after a catch

extern uint64_t g_delta;   // loaded image base - preferred base
extern uint64_t g_gs;      // guest TEB address
typedef void (*LiftFn)(Ctx*);
typedef void (*RtShimFn)(Ctx*);
void rt_init_table();
int rt_lookup(uint64_t va_pref);
unsigned rt_table_count();
uint64_t rt_table_va(unsigned i);
void rt_set_active(unsigned i, bool on);
bool rt_is_active(unsigned i);
LiftFn rt_table_fn(unsigned i);
unsigned rt_table_size(unsigned i);
void rt_register_shim(uint64_t addr, RtShimFn fn, const char* name);
void rt_cpuid(uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d);
uint64_t rt_rdtsc();
bool rt_cmpxchg16(uint64_t addr, uint64_t* rax, uint64_t* rdx, uint64_t rbx, uint64_t rcx);

// dispatch: call guest code at runtime address `target` (lifted function, import shim, or native fallback)
void rt_call(Ctx* c, uint64_t target);
[[noreturn]] void rt_unimpl(uint64_t addr, const char* what);
[[noreturn]] void rt_fastfail(Ctx* c, uint64_t addr);
[[noreturn]] void rt_badjump(uint64_t addr, uint64_t target);
void rt_trace_enter(uint64_t va);

#if defined(__x86_64__) && !defined(RT_FULL)
static inline uint64_t rt_gs() { uint64_t v; __asm__ volatile("mov %%gs:0x30, %0" : "=r"(v)); return v; }
#else
extern thread_local uint64_t t_rt_gs;
static inline uint64_t rt_gs() { return t_rt_gs; }
#endif
#define RT_BARRIER() __asm__ volatile("" ::: "memory")
#define A(x) ((uint64_t)(x) + g_delta)

static inline uint8_t  L8(uint64_t a)  { return *(volatile uint8_t*)a; }
static inline uint16_t L16(uint64_t a) { uint16_t v; memcpy(&v, (void*)a, 2); return v; }
static inline uint32_t L32(uint64_t a) { uint32_t v; memcpy(&v, (void*)a, 4); return v; }
static inline uint64_t L64(uint64_t a) { uint64_t v; memcpy(&v, (void*)a, 8); return v; }
static inline V128 L128(uint64_t a) { V128 v; memcpy(&v, (void*)a, 16); return v; }
static inline void S8(uint64_t a, uint8_t v)   { *(volatile uint8_t*)a = v; }
static inline void S16(uint64_t a, uint16_t v) { memcpy((void*)a, &v, 2); }
static inline void S32(uint64_t a, uint32_t v) { memcpy((void*)a, &v, 4); }
static inline void S64(uint64_t a, uint64_t v) { memcpy((void*)a, &v, 8); }
static inline void S128(uint64_t a, V128 v) { memcpy((void*)a, &v, 16); }

static inline uint8_t PARITY(uint32_t v) { v &= 0xff; v ^= v >> 4; v ^= v >> 2; v ^= v >> 1; return (~v) & 1; }

// ---- flag helpers (computed by generated code through macros) ----
#define MSB8(v)  ((uint8_t)(v) >> 7)
#define MSB16(v) ((uint16_t)(v) >> 15)
#define MSB32(v) ((uint32_t)(v) >> 31)
#define MSB64(v) ((uint64_t)(v) >> 63)

// float/double conversions with x86 semantics
static inline int32_t cvtt_d2i32(double d) { if (d != d || d >= 2147483648.0 || d < -2147483648.0) return INT32_MIN; return (int32_t)d; }
static inline int64_t cvtt_d2i64(double d) { if (d != d || d >= 9223372036854775808.0 || d < -9223372036854775808.0) return INT64_MIN; return (int64_t)d; }
static inline int32_t cvtt_f2i32(float d) { return cvtt_d2i32(d); }
static inline int64_t cvtt_f2i64(float d) { return cvtt_d2i64(d); }
static inline int32_t cvt_d2i32(double d) { double r = nearbyint(d); return cvtt_d2i32(r); }
static inline int64_t cvt_d2i64(double d) { double r = nearbyint(d); return cvtt_d2i64(r); }

static inline int32_t cvt_f2i32(float d) { return cvt_d2i32(d); }
static inline int64_t cvt_f2i64(float d) { return cvt_d2i64(d); }
static inline uint64_t bsr64(uint64_t v) { return 63 - __builtin_clzll(v); }
static inline uint64_t bsf64(uint64_t v) { return __builtin_ctzll(v); }

// 128-bit unsigned division helpers for div/idiv with rdx:rax
static inline void div64(uint64_t* rax, uint64_t* rdx, uint64_t d) {
  unsigned __int128 n = ((unsigned __int128)*rdx << 64) | *rax;
  *rax = (uint64_t)(n / d); *rdx = (uint64_t)(n % d);
}
static inline void idiv64(uint64_t* rax, uint64_t* rdx, int64_t d) {
  __int128 n = (__int128)(((unsigned __int128)*rdx << 64) | *rax);
  *rax = (uint64_t)(int64_t)(n / d); *rdx = (uint64_t)(int64_t)(n % d);
}
