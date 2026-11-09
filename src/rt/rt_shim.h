// Typed shims: C++ functions callable from lifted x64 (Microsoft x64 ABI) code and vice versa.
#pragma once
#include "x64rt.h"
#include <type_traits>
#include <utility>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#ifdef _WIN32
#include <malloc.h>
static inline void* rt_amalloc(size_t al, size_t n) { return _aligned_malloc(n, al); }
static inline void rt_afree(void* p) { _aligned_free(p); }
#else
static inline void* rt_amalloc(size_t al, size_t n) { return aligned_alloc(al, n); }
static inline void rt_afree(void* p) { free(p); }
#endif

typedef void (*ShimThunk)(Ctx*, void*);
extern thread_local uint64_t t_guest_sp;   // guest rsp at the moment a shim was entered (nested guest calls go below it)

// register a thunk; returns the magic guest address that lifted code can call
void* rt_make_shim(ShimThunk t, void* user, const char* name);

namespace rtabi {
template <class T> T readArg(Ctx* c, int i) {
  static const int regs[4] = {1, 2, 8, 9};
  if constexpr (std::is_floating_point_v<T>) {
    if (i < 4) { if constexpr (sizeof(T) == 4) return c->x[i].f[0]; else return c->x[i].df[0]; }
    uint64_t v = L64(c->r[4] + 8 + 8 * i); T t; memcpy(&t, &v, sizeof(T)); return t;
  } else {
    uint64_t v = i < 4 ? c->r[regs[i]] : L64(c->r[4] + 8 + 8 * i);
    if constexpr (std::is_pointer_v<T>) return (T)(uintptr_t)v;
    else if constexpr (std::is_same_v<T, bool>) return (v & 0xff) != 0;
    else return (T)v;
  }
}
template <class T> void writeRet(Ctx* c, T v) {
  if constexpr (std::is_floating_point_v<T>) {
    c->x[0] = V128{}; if constexpr (sizeof(T) == 4) c->x[0].f[0] = v; else c->x[0].df[0] = v;
  } else if constexpr (std::is_pointer_v<T>) c->r[0] = (uint64_t)(uintptr_t)v;
  else if constexpr (std::is_signed_v<T>) c->r[0] = (uint64_t)(int64_t)v;
  else c->r[0] = (uint64_t)v;
}
template <class R, class... A, size_t... I>
R invoke(R (*f)(A...), Ctx* c, std::index_sequence<I...>) { return f(readArg<A>(c, (int)I)...); }
}  // namespace rtabi

template <class R, class... A> void* makeShim(R (*f)(A...), const char* name) {
  ShimThunk t = [](Ctx* c, void* u) {
    struct SpScope { uint64_t saved; SpScope(uint64_t sp) : saved(t_guest_sp) { t_guest_sp = sp; } ~SpScope() { t_guest_sp = saved; } } spScope(c->r[4]);   // nested host->guest calls go below this shim's frame; restored on exit (also when a guest exception unwinds)
    R (*fn)(A...) = (R (*)(A...))u;
    if constexpr (std::is_void_v<R>) rtabi::invoke(fn, c, std::index_sequence_for<A...>{});
    else rtabi::writeRet<R>(c, rtabi::invoke(fn, c, std::index_sequence_for<A...>{}));
    c->r[4] += 8;   // emulate `ret`
  };
  return rt_make_shim(t, (void*)f, name);
}

// ---- host -> guest calls ----
void rt_guest_call_raw(Ctx* c, uint64_t target);   // runs lifted function `target` with the given register state
namespace rtabi {
template <class T> void setArg(Ctx* c, int i, T v, uint64_t* stackArgs) {
  static const int regs[4] = {1, 2, 8, 9};
  if constexpr (std::is_floating_point_v<T>) {
    if (i < 4) { c->x[i] = V128{}; if constexpr (sizeof(T) == 4) c->x[i].f[0] = v; else c->x[i].df[0] = v; }
    else { uint64_t b = 0; memcpy(&b, &v, sizeof(T)); stackArgs[i - 4] = b; }
  } else {
    uint64_t b;
    if constexpr (std::is_pointer_v<T>) b = (uint64_t)(uintptr_t)v;
    else if constexpr (std::is_signed_v<T>) b = (uint64_t)(int64_t)v;
    else b = (uint64_t)v;
    if (i < 4) c->r[regs[i]] = b; else stackArgs[i - 4] = b;
  }
}
}
// guest stack top for a nested call (below the current shim's frame, or the thread's guest stack base)
uint64_t rt_guest_call_sp();
template <class R, class... A> R rt_guest_call(uint64_t va, A... a) {
  Ctx c; memset(&c, 0, sizeof c);
  uint64_t sp = (rt_guest_call_sp() - 0x200) & ~15ull;
  uint64_t stackArgs[16] = {0};
  int i = 0;
  (rtabi::setArg<A>(&c, i++, a, stackArgs), ...);
  // frame: [sp] = return address slot, [sp+8..+0x27] shadow, [sp+0x28..] stack args
  sp -= 0x60;
  for (int k = 0; k < 12; k++) S64(sp + 0x28 + 8 * k, stackArgs[k]);
  sp -= 8; S64(sp, 0);
  c.r[4] = sp;
  rt_guest_call_raw(&c, va);
  if constexpr (std::is_void_v<R>) return;
  else if constexpr (std::is_floating_point_v<R>) { if constexpr (sizeof(R) == 4) return c.x[0].f[0]; else return c.x[0].df[0]; }
  else if constexpr (std::is_pointer_v<R>) return (R)(uintptr_t)c.r[0];
  else return (R)c.r[0];
}

template <class F> struct GuestFn;
template <class R, class... A> struct GuestFn<R(A...)> {
  uint64_t va = 0;
  R operator()(A... a) const { return rt_guest_call<R, A...>(va, a...); }
};
