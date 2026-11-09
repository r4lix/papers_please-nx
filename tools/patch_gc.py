import re

# ---------------- rt_full.h ----------------
p = 'src/rt/rt_full.h'
s = open(p).read()
s = s.replace('void rt_thread_setup(size_t guestStackBytes);', '''uint32_t rt_alloc_tid();
void rt_thread_setup(size_t guestStackBytes, uint32_t tid = 0);
void rt_register_region(uint64_t base, size_t size);          // for VirtualQuery (guest stacks)
bool rt_region_lookup(uint64_t addr, uint64_t* base, uint64_t* size);
// cooperative stop-the-world (Boehm SuspendThread / GetThreadContext emulation)
uint32_t rt_suspend_thread(uint32_t tid);
uint32_t rt_resume_thread(uint32_t tid);
bool rt_thread_context(uint32_t tid, uint8_t* context);       // fills a Win64 CONTEXT
struct BlockedScope { BlockedScope(); ~BlockedScope(); };''', 1)
open(p, 'w').write(s)

# ---------------- rt_full.cpp ----------------
p = 'src/rt/rt_full.cpp'
s = open(p).read()
s = s.replace('#include <mutex>\n', '#include <mutex>\n#include <condition_variable>\n#include <map>\n#include <chrono>\n#include <thread>\n', 1)

# GThread registry + safepoints
s = s.replace('// ---------------- shim registry ----------------', '''// ---------------- guest threads (for the cooperative stop-the-world) ----------------
struct GThread {
  uint32_t tid = 0;
  std::atomic<bool> suspendReq{false}, parked{false}, blocked{false};
  Ctx* ctx = nullptr;
  std::mutex m; std::condition_variable cv;
};
static std::mutex g_gtm;
static std::map<uint32_t, GThread*> g_gthreads;
static thread_local GThread* t_gthread = nullptr;

static void safepoint(GThread* g) {
  std::unique_lock<std::mutex> l(g->m);
  g->parked = true;
  g->cv.notify_all();
  g->cv.wait(l, [&] { return !g->suspendReq.load(); });
  g->parked = false;
}
BlockedScope::BlockedScope() { if (t_gthread) t_gthread->blocked = true; }
BlockedScope::~BlockedScope() { if (t_gthread) t_gthread->blocked = false; }

uint32_t rt_suspend_thread(uint32_t tid) {
  GThread* g = nullptr;
  { std::lock_guard<std::mutex> l(g_gtm); auto it = g_gthreads.find(tid); if (it != g_gthreads.end()) g = it->second; }
  if (!g || g == t_gthread) return (uint32_t)-1;
  g->suspendReq = true;
  auto t0 = std::chrono::steady_clock::now();
  while (!(g->parked.load() || g->blocked.load())) {
    if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(10)) { g->suspendReq = false; return (uint32_t)-1; }
    std::this_thread::sleep_for(std::chrono::microseconds(50));
  }
  return 0;
}
uint32_t rt_resume_thread(uint32_t tid) {
  GThread* g = nullptr;
  { std::lock_guard<std::mutex> l(g_gtm); auto it = g_gthreads.find(tid); if (it != g_gthreads.end()) g = it->second; }
  if (!g) return (uint32_t)-1;
  { std::lock_guard<std::mutex> l(g->m); g->suspendReq = false; }
  g->cv.notify_all();
  return 1;
}
bool rt_thread_context(uint32_t tid, uint8_t* context) {
  GThread* g = nullptr;
  { std::lock_guard<std::mutex> l(g_gtm); auto it = g_gthreads.find(tid); if (it != g_gthreads.end()) g = it->second; }
  if (!g || !g->ctx) return false;
  static const int off[16] = {0x78, 0x80, 0x88, 0x90, 0x98, 0xA0, 0xA8, 0xB0, 0xB8, 0xC0, 0xC8, 0xD0, 0xD8, 0xE0, 0xE8, 0xF0};
  memset(context, 0, 0x4d0);
  *(uint32_t*)(context + 0x30) = 0x10000F;
  for (int i = 0; i < 16; i++) *(uint64_t*)(context + off[i]) = g->ctx->r[i];
  *(uint64_t*)(context + 0xF8) = L64(g->ctx->r[4]);   // Rip = return address of the shim call
  return true;
}

static std::mutex g_regm;
static std::map<uint64_t, size_t> g_regions;
void rt_register_region(uint64_t base, size_t size) { std::lock_guard<std::mutex> l(g_regm); g_regions[base] = size; }
bool rt_region_lookup(uint64_t addr, uint64_t* base, uint64_t* size) {
  std::lock_guard<std::mutex> l(g_regm);
  auto it = g_regions.upper_bound(addr);
  if (it == g_regions.begin()) return false;
  --it;
  if (addr >= it->first && addr < it->first + it->second) { *base = it->first; *size = it->second; return true; }
  return false;
}

// ---------------- shim registry ----------------''', 1)

s = s.replace('  ShimRec& r = g_recs[off / 16];\n', '  ShimRec& r = g_recs[off / 16];\n  GThread* gt = t_gthread;\n  if (gt) { gt->ctx = c; if (gt->suspendReq.load(std::memory_order_acquire)) safepoint(gt); }\n', 1)
s = s.replace('  r.t(c, r.user);\n  return true;', '  r.t(c, r.user);\n  if (gt && gt->suspendReq.load(std::memory_order_acquire)) { gt->ctx = c; safepoint(gt); }\n  return true;', 1)

# thread setup with explicit tid
s = s.replace('static std::atomic<uint32_t> g_tid{1};\nvoid rt_thread_setup(size_t guestStack) {', 'static std::atomic<uint32_t> g_tid{1};\nuint32_t rt_alloc_tid() { return ++g_tid; }\nvoid rt_thread_setup(size_t guestStack, uint32_t tid) {\n  if (!tid) tid = rt_alloc_tid();', 1)
s = s.replace('  *(uint64_t*)(teb + 0x40) = g_tid++;             // ClientId.UniqueProcess/Thread pair (thread at 0x48)\n  *(uint64_t*)(teb + 0x48) = g_tid.load();', '  *(uint64_t*)(teb + 0x40) = 4242;                // ClientId.UniqueProcess\n  *(uint64_t*)(teb + 0x48) = tid;                 // ClientId.UniqueThread', 1)
s = s.replace('  t_rt_gs = (uint64_t)(uintptr_t)teb;', '  t_rt_gs = (uint64_t)(uintptr_t)teb;\n  rt_register_region((uint64_t)(uintptr_t)stk, guestStack);\n  { GThread* g = new GThread(); g->tid = tid; t_gthread = g; std::lock_guard<std::mutex> l(g_gtm); g_gthreads[tid] = g; }', 1)
open(p, 'w').write(s)

# ---------------- win32_shims.cpp ----------------
p = 'src/rt/win32_shims.cpp'
s = open(p).read()
# blocking sites
s = s.replace('static void EnterCriticalSection_(uint8_t* cs) { syncOf<SyncCS>(cs + 24)->m.lock(); }', 'static void EnterCriticalSection_(uint8_t* cs) { auto& m = syncOf<SyncCS>(cs + 24)->m; if (m.try_lock()) return; BlockedScope b; m.lock(); }', 1)
s = s.replace('static void AcquireSRWLockExclusive_(void* l) { syncOf<std::shared_mutex>(l)->lock(); }', 'static void AcquireSRWLockExclusive_(void* l) { auto* m = syncOf<std::shared_mutex>(l); if (m->try_lock()) return; BlockedScope b; m->lock(); }', 1)
s = s.replace('static void AcquireSRWLockShared_(void* l) { syncOf<std::shared_mutex>(l)->lock_shared(); }', 'static void AcquireSRWLockShared_(void* l) { auto* m = syncOf<std::shared_mutex>(l); if (m->try_lock_shared()) return; BlockedScope b; m->lock_shared(); }', 1)
s = s.replace('static int32_t SleepConditionVariableCS_(void* cv, uint8_t* cs, uint32_t ms) {\n', 'static int32_t SleepConditionVariableCS_(void* cv, uint8_t* cs, uint32_t ms) {\n  BlockedScope blk;\n', 1)
s = s.replace('static uint32_t waitMany(uint32_t n, const HND* hs, bool all, uint32_t ms) {\n', 'static uint32_t waitMany(uint32_t n, const HND* hs, bool all, uint32_t ms) {\n  BlockedScope blk;\n', 1)
s = s.replace('static void Sleep_(uint32_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }', 'static void Sleep_(uint32_t ms) { BlockedScope b; std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }', 1)
s = s.replace('static void BL_SemAcquire_(uint64_t h) { auto s = getAs<KSem>(h, 4); if (!s) return; std::unique_lock', 'static void BL_SemAcquire_(uint64_t h) { auto s = getAs<KSem>(h, 4); if (!s) return; BlockedScope b; std::unique_lock', 1)
# threads: tid assigned up front and used for the guest thread
s = s.replace('struct ThreadStart { uint64_t fn, param; std::shared_ptr<KThread> kt; };', 'struct ThreadStart { uint64_t fn, param; std::shared_ptr<KThread> kt; uint32_t tid; };', 1)
s = s.replace('  rt_thread_setup(4u << 20);', '  rt_thread_setup(4u << 20, ts->tid);', 1)
s = s.replace('  kt->tid = g_nextTid++;\n', '  kt->tid = rt_alloc_tid();\n', 1)
s = s.replace('  auto* ts = new ThreadStart{fn, param, kt};', '  auto* ts = new ThreadStart{fn, param, kt, kt->tid};', 1)
# suspend/resume/context
s = s.replace('static uint32_t SuspendThread_(HND h) { static int n = 0; if (n++ < 5) { auto t = getAs<KThread>(h, 6); fprintf(stderr, "[rt] SuspendThread(%llx) tid=%u cur=%u\\n", (unsigned long long)h, t ? t->tid : 0, rt_thread_id()); } return (uint32_t)-1; }', 'static uint32_t SuspendThread_(HND h) { auto t = getAs<KThread>(h, 6); return t ? rt_suspend_thread(t->tid) : (uint32_t)-1; }', 1)
s = s.replace('static uint32_t ResumeThread_(HND) { return 1; }', 'static uint32_t ResumeThread_(HND h) { auto t = getAs<KThread>(h, 6); return t ? rt_resume_thread(t->tid) : 1; }', 1)
s = s.replace('static int32_t GetThreadContext_(HND, void*) { return 0; }', 'static int32_t GetThreadContext_(HND h, uint8_t* ctx) { auto t = getAs<KThread>(h, 6); return t && rt_thread_context(t->tid, ctx) ? 1 : 0; }', 1)
# env: no GC_DONT_GC any more
s = s.replace('{{"GC_DONT_GC", "1"}}', '{}', 1)
# VirtualQuery: guest stacks
s = s.replace('  regs.push_back({g_img.base, g_img.size});\n  bool hit = false;', '  regs.push_back({g_img.base, g_img.size});\n  { uint64_t rb, rs; if (rt_region_lookup(a, &rb, &rs)) regs.push_back({rb, rs}); }\n  bool hit = false;', 1)
open(p, 'w').write(s)
print('patched')
