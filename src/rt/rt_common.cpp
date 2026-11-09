#include <algorithm>
#include <string>
// x64rt common: function table lookup, dispatch, helpers
#include "x64rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <unordered_map>
#include <vector>

uint64_t g_delta = 0;
uint64_t g_gs = 0;

struct LiftEntry { uint64_t va; LiftFn fn; unsigned size; };
extern const LiftEntry g_lift_table[];
extern const unsigned g_lift_count;

struct ShimEntry { RtShimFn fn; const char* name; };
static std::unordered_map<uint64_t, ShimEntry> g_shims;   // runtime address -> shim
static std::vector<uint8_t> g_active;
static uint32_t* g_hash = nullptr;
static uint32_t g_hmask = 0;
static uint64_t g_trace = 0;

static inline uint32_t hh(uint64_t va) { return (uint32_t)((va * 0x9E3779B97F4A7C15ull) >> 40); }

void rt_init_table() {
  uint32_t n = 1;
  while (n < g_lift_count * 2) n <<= 1;
  g_hmask = n - 1;
  static uint32_t g_hash_arr[1 << 17];
  g_hash = g_hash_arr;
  g_active.assign(g_lift_count, 1);
  for (unsigned i = 0; i < g_lift_count; i++) {
    uint32_t h = hh(g_lift_table[i].va) & g_hmask;
    while (g_hash[h]) h = (h + 1) & g_hmask;
    g_hash[h] = i + 1;
  }
}

int rt_lookup(uint64_t va_pref) {
  uint32_t h = hh(va_pref) & g_hmask;
  while (uint32_t e = g_hash[h]) {
    if (g_lift_table[e - 1].va == va_pref) return (int)(e - 1);
    h = (h + 1) & g_hmask;
  }
  return -1;
}
unsigned rt_table_count() { return g_lift_count; }
uint64_t rt_table_va(unsigned i) { return g_lift_table[i].va; }
void rt_set_active(unsigned i, bool on) { g_active[i] = on; }
bool rt_is_active(unsigned i) { return g_active[i]; }
LiftFn rt_table_fn(unsigned i) { return g_lift_table[i].fn; }
unsigned rt_table_size(unsigned i) { return g_lift_table[i].size; }

void rt_register_shim(uint64_t addr, RtShimFn fn, const char* name) { g_shims[addr] = {fn, name}; }

#ifdef LIFT_HYBRID
extern "C" void rt_native_call(Ctx* c, uint64_t target);
#endif

int g_rt_tracing = 0;
static thread_local int t_cdepth = 0;
static thread_local uint64_t t_chain[64];
#ifdef RT_FULL
bool rt_shim_call(Ctx* c, uint64_t target);
const char* rt_shim_name(uint64_t target);
#endif
void rt_call(Ctx* c, uint64_t target) {
#ifdef RT_FULL
  if (rt_shim_call(c, target)) return;
#endif
#ifndef RT_FULL
  struct CG { CG() { t_cdepth++; } ~CG() { t_cdepth--; } } cg;
  if (t_cdepth > 3000 && t_cdepth < 3064) { t_chain[t_cdepth - 3001] = target - g_delta; }
  if (t_cdepth == 3064) { fprintf(stderr, "[rt] runaway recursion, last targets:"); for (int i = 0; i < 64; i++) fprintf(stderr, " %llx", (unsigned long long)t_chain[i]); fprintf(stderr, "\n"); abort(); }
  if (g_rt_tracing) fprintf(stderr, "[rt_call] target=%llx rcx=%llx rdx=%llx r8=%llx r9=%llx rax=%llx rsp=%llx a5=%llx a6=%llx\n", (unsigned long long)target, (unsigned long long)c->r[1], (unsigned long long)c->r[2], (unsigned long long)c->r[8], (unsigned long long)c->r[9], (unsigned long long)c->r[0], (unsigned long long)c->r[4], (unsigned long long)*(uint64_t*)(c->r[4]+0x28), (unsigned long long)*(uint64_t*)(c->r[4]+0x30));
#endif
  int i = rt_lookup(target - g_delta);
  if (i >= 0 && g_active[i]) { g_lift_table[i].fn(c); return; }
  auto it = g_shims.find(target);
  if (it != g_shims.end()) { it->second.fn(c); return; }
#ifdef LIFT_HYBRID
  { extern void rt_note_native(uint64_t); rt_note_native(target - g_delta); }
  if (getenv("LIFT_CHECKFREE") && target - g_delta == 0x180128d4cull) {
    extern int rt_heap_validate(uint64_t p);
    if (!rt_heap_validate(c->r[1])) { fprintf(stderr, "[rt] FREE OF INVALID PTR %llx size %llx rsp=%llx RA=%llx\n", (unsigned long long)c->r[1], (unsigned long long)c->r[2], (unsigned long long)c->r[4], (unsigned long long)L64(c->r[4])); }
  }
  rt_native_call(c, target);
  return;
#else
  fprintf(stderr, "[rt] call to unknown target %llx (va %llx, rsp %llx, RA va %llx)\n", (unsigned long long)target, (unsigned long long)(target - g_delta), (unsigned long long)c->r[4], (unsigned long long)(L64(c->r[4]) - g_delta));
  abort();
#endif
}

void rt_unimpl(uint64_t addr, const char* what) {
  fprintf(stderr, "[rt] UNIMPLEMENTED insn at %llx: %s\n", (unsigned long long)addr, what);
  fflush(stderr);
  abort();
}
void rt_badjump(uint64_t addr, uint64_t target) {
  fprintf(stderr, "[rt] bad jump at %llx -> %llx\n", (unsigned long long)addr, (unsigned long long)target);
  fflush(stderr);
  abort();
}

void rt_cpuid(uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d) {
  uint32_t leaf = *a;
  *a = *b = *c = *d = 0;
  if (leaf == 0) { *a = 1; *b = 0x756e6547; *d = 0x49656e69; *c = 0x6c65746e; }   // "GenuineIntel"
  else if (leaf == 1) { *a = 0x000306a9; *d = (1u << 0) | (1u << 4) | (1u << 8) | (1u << 15) | (1u << 23) | (1u << 25) | (1u << 26); *c = 0; }
}
uint64_t rt_rdtsc() {
#if defined(__x86_64__)
  uint32_t lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); return ((uint64_t)hi << 32) | lo;
#else
  uint64_t v; __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v)); return v;
#endif
}

#include <atomic>
static std::atomic_flag g_cx16 = ATOMIC_FLAG_INIT;
bool rt_cmpxchg16(uint64_t addr, uint64_t* rax, uint64_t* rdx, uint64_t rbx, uint64_t rcx) {
  while (g_cx16.test_and_set(std::memory_order_acquire)) {}
  uint64_t* p = (uint64_t*)addr;
  bool ok = p[0] == *rax && p[1] == *rdx;
  if (ok) { p[0] = rbx; p[1] = rcx; } else { *rax = p[0]; *rdx = p[1]; }
  g_cx16.clear(std::memory_order_release);
  return ok;
}
void* rt_dbg_active_ptr() { return g_active.data(); }

#ifdef LIFT_HYBRID
#include <windows.h>
int rt_heap_validate(uint64_t p) { return HeapValidate(GetProcessHeap(), 0, (void*)p); }
#endif

void rt_fastfail(Ctx* c, uint64_t addr) {
  fprintf(stderr, "[rt] FASTFAIL code %llx at %llx rsp=%llx. stack return addresses (image-relative):\n", (unsigned long long)c->r[1], (unsigned long long)addr, (unsigned long long)c->r[4]);
  for (int i = 0; i < 96; i++) {
    uint64_t v = *(uint64_t*)(c->r[4] + 8 * i);
    uint64_t rel = v - g_delta;
    if (rel >= 0x180001000ull && rel < 0x180fe3000ull) fprintf(stderr, "  +%x: %llx\n", i * 8, (unsigned long long)rel);
  }
  { uint64_t rsp_f = c->r[4] + 0x40; uint64_t ck = *(uint64_t*)(0x180c969b8ull + g_delta); uint64_t st = *(uint64_t*)(rsp_f + 0xe0);
    fprintf(stderr, "  rsp_f=%llx cookie=%llx expect(cookie^rsp)=%llx stored=%llx\n", (unsigned long long)rsp_f, (unsigned long long)ck, (unsigned long long)(ck ^ rsp_f), (unsigned long long)st);
    for (int k = 0; k < 40; k++) fprintf(stderr, "  [rsp_f+%x]=%llx\n", k * 8 + 0x30, (unsigned long long)*(uint64_t*)(rsp_f + 0x30 + 8 * k)); }
  fflush(stderr);
  abort();
}

#ifdef LIFT_HYBRID
#include <map>
#include <mutex>
static std::map<uint64_t, unsigned> g_nat;
static std::mutex g_natm;
void rt_note_native(uint64_t va) { std::lock_guard<std::mutex> l(g_natm); g_nat[va]++; }
void rt_dump_native() {
  std::vector<std::pair<unsigned, uint64_t>> v;
  for (auto& kv : g_nat) v.push_back({kv.second, kv.first});
  std::sort(v.rbegin(), v.rend());
  fprintf(stderr, "[native fallbacks] %zu distinct targets, top:", v.size());
  for (size_t i = 0; i < v.size() && i < 25; i++) fprintf(stderr, " %llx x%u", (unsigned long long)v[i].second, v[i].first);
  fputc(10, stderr);
}
#endif
