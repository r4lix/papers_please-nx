// Full (non-hybrid) runtime: PE loader, fake TEB/TLS, shim registry, host<->guest calls.
#include "rt_shim.h"
#include "rt_full.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <mutex>
#include <condition_variable>
#include <map>
#include <chrono>
#include <thread>

thread_local uint64_t t_rt_gs = 0;
thread_local uint64_t t_guest_sp = 0;
static thread_local uint64_t t_guest_top = 0;

// ---------------- guest threads (for the cooperative stop-the-world) ----------------
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
  if (getenv("RT_VERBOSE")) fprintf(stderr, "[rt] SuspendThread tid %u\n", tid);
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
  if (getenv("RT_VERBOSE")) fprintf(stderr, "[rt] GetThreadContext tid %u rsp %llx\n", tid, (unsigned long long)g->ctx->r[4]);
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

// ---------------- shim registry ----------------
struct ShimRec { ShimThunk t; void* user; const char* name; };
static const unsigned kMaxShims = 16384;
static ShimRec g_recs[kMaxShims];
static unsigned g_nrecs = 0;
alignas(64) static uint8_t g_magic[kMaxShims * 16];
static std::unordered_map<std::string, void*>& named() { static std::unordered_map<std::string, void*> m; return m; }

void* rt_make_shim(ShimThunk t, void* user, const char* name) {
  if (g_nrecs >= kMaxShims) { fprintf(stderr, "[rt] too many shims\n"); abort(); }
  unsigned i = g_nrecs++;
  g_recs[i] = {t, user, name};
  return &g_magic[i * 16];
}
static std::mutex g_cntm; static std::map<std::string, uint64_t> g_cnt;
void rt_dump_shim_counts() {
  std::vector<std::pair<uint64_t, std::string>> v; for (auto& kv : g_cnt) v.push_back({kv.second, kv.first});
  std::sort(v.rbegin(), v.rend());
  for (size_t i = 0; i < v.size() && i < 25; i++) fprintf(stderr, "  %8llu %s\n", (unsigned long long)v[i].first, v[i].second.c_str());
  g_cnt.clear();
}
bool rt_shim_call(Ctx* c, uint64_t target) {
  uint64_t off = target - (uint64_t)(uintptr_t)g_magic;
  if (off >= sizeof g_magic) return false;
  ShimRec& r = g_recs[off / 16];
  GThread* gt = t_gthread;
  if (gt) { gt->ctx = c; if (gt->suspendReq.load(std::memory_order_acquire)) safepoint(gt); }
  static const bool count = getenv("RT_COUNT") != nullptr;
  if (count) { std::lock_guard<std::mutex> l(g_cntm); g_cnt[r.name]++; }
  static const bool trace = getenv("RT_TRACE") != nullptr;
  if (trace) fprintf(stderr, "[shim] %s\n", r.name);
  r.t(c, r.user);
  if (gt && gt->suspendReq.load(std::memory_order_acquire)) { gt->ctx = c; safepoint(gt); }
  return true;
}
const char* rt_shim_name(uint64_t target) {
  uint64_t off = target - (uint64_t)(uintptr_t)g_magic;
  return off < sizeof g_magic ? g_recs[off / 16].name : nullptr;
}
void rt_register_named(const char* key, void* shim) { named()[key] = shim; }
void* rt_find_named(const std::string& key) { auto it = named().find(key); return it == named().end() ? nullptr : it->second; }

static void missingThunk(Ctx* c, void* u) {
  fprintf(stderr, "[rt] call to unimplemented import: %s (rsp=%llx)\n", (const char*)u, (unsigned long long)c->r[4]);
  fflush(stderr);
  abort();
}

// ---------------- image loader ----------------
#pragma pack(push, 1)
struct DosHdr { uint16_t magic; uint8_t pad[58]; int32_t lfanew; };
struct FileHdr { uint16_t machine, nsec; uint32_t ts, symptr, nsym; uint16_t optsz, chars; };
struct DataDir { uint32_t va, size; };
struct OptHdr64 {
  uint16_t magic; uint8_t ml, mnl; uint32_t szcode, szinit, szuninit, entry, basecode;
  uint64_t imagebase; uint32_t secalign, filealign; uint16_t maj, min, imaj, imin, smaj, smin; uint32_t win32ver, sizeofimage, sizeofheaders, checksum;
  uint16_t subsys, dllchars; uint64_t stkres, stkcom, heapres, heapcom; uint32_t loaderflags, nrva; DataDir dd[16];
};
struct SecHdr { char name[8]; uint32_t vsize, va, rawsize, rawptr, reloc, line; uint16_t nreloc, nline; uint32_t chars; };
struct ImpDesc { uint32_t oft, ts, fwd, name, ft; };
struct ExpDir { uint32_t chars, ts; uint16_t maj, min; uint32_t name, base, nfuncs, nnames, aof, aon, aoo; };
struct TlsDir { uint64_t start, end, index, callbacks; uint32_t zerofill, chars; };
#pragma pack(pop)

RtImage g_img;

static std::string lower(const char* s) { std::string r = s; for (auto& ch : r) ch = (char)tolower((unsigned char)ch); return r; }

bool rt_load_image(const void* file, size_t fsize, const char* verbose_name) {
  const uint8_t* f = (const uint8_t*)file;
  auto* dos = (const DosHdr*)f;
  if (fsize < 0x200 || dos->magic != 0x5A4D) { fprintf(stderr, "[rt] not a PE file\n"); return false; }
  const uint8_t* nt = f + dos->lfanew;
  const FileHdr* fh = (const FileHdr*)(nt + 4);
  const OptHdr64* oh = (const OptHdr64*)(nt + 4 + sizeof(FileHdr));
  const SecHdr* secs = (const SecHdr*)((const uint8_t*)oh + fh->optsz);
  size_t sz = oh->sizeofimage;
  uint8_t* mem = (uint8_t*)rt_amalloc(65536, (sz + 65535) & ~65535ull);
  if (!mem) { fprintf(stderr, "[rt] cannot allocate %zu bytes for the image\n", sz); return false; }
  memset(mem, 0, (sz + 65535) & ~65535ull);
  memcpy(mem, f, oh->sizeofheaders);
  for (int i = 0; i < fh->nsec; i++) {
    const SecHdr& s = secs[i];
    size_t n = std::min<size_t>(s.rawsize, s.vsize ? s.vsize : s.rawsize);
    if (s.rawptr + n > fsize) n = fsize > s.rawptr ? fsize - s.rawptr : 0;
    memcpy(mem + s.va, f + s.rawptr, n);
  }
  g_img.mem = mem; g_img.size = sz; g_img.pref = oh->imagebase; g_img.base = (uint64_t)(uintptr_t)mem;
  g_img.delta = g_img.base - g_img.pref;
  g_img.entry = g_img.base + oh->entry;
  g_delta = g_img.delta;
  // relocations
  DataDir rd = oh->dd[5];
  size_t nrel = 0;
  for (uint32_t off = 0; off < rd.size;) {
    uint32_t page = *(uint32_t*)(mem + rd.va + off), bsz = *(uint32_t*)(mem + rd.va + off + 4);
    if (!bsz) break;
    for (uint32_t k = 8; k < bsz; k += 2) {
      uint16_t e = *(uint16_t*)(mem + rd.va + off + k);
      if ((e >> 12) == 10) { *(uint64_t*)(mem + page + (e & 0xfff)) += g_img.delta; nrel++; }
    }
    off += bsz;
  }
  // imports
  DataDir id = oh->dd[1];
  size_t nimp = 0, nmiss = 0;
  for (const ImpDesc* d = (const ImpDesc*)(mem + id.va); d->name; d++) {
    std::string dll = lower((const char*)(mem + d->name));
    if (dll.size() > 4 && dll.substr(dll.size() - 4) == ".dll") dll = dll.substr(0, dll.size() - 4);
    uint64_t* ft = (uint64_t*)(mem + d->ft);
    const uint64_t* oft = (const uint64_t*)(mem + (d->oft ? d->oft : d->ft));
    for (; *oft; oft++, ft++) {
      std::string nm = (*oft >> 63) ? "ord" + std::to_string(*oft & 0xffff) : std::string((const char*)(mem + (*oft & 0x7fffffff) + 2));
      std::string key = dll + "!" + lower(nm.c_str());
      void* sh = rt_find_named(key);
      if (!sh) {
        // some imports are matched by name regardless of the dll (api sets / forwarders)
        sh = rt_find_named("*!" + lower(nm.c_str()));
      }
      if (!sh && (*oft >> 63) && (dll == "ws2_32" || dll == "oleaut32" || dll == "ole32" || dll == "iphlpapi")) sh = rt_find_named("*!zero");   // ordinal imports of unused subsystems
      if (!sh) {
        char* keep = strdup((dll + "!" + nm).c_str());
        sh = rt_make_shim(missingThunk, keep, keep);
        nmiss++;
        if (getenv("RT_VERBOSE")) fprintf(stderr, "[rt] missing import %s\n", keep);
      }
      *ft = (uint64_t)(uintptr_t)sh;
      nimp++;
    }
  }
  // TLS
  DataDir td = oh->dd[9];
  if (td.va) {
    const TlsDir* t = (const TlsDir*)(mem + td.va);
    g_img.tlsStart = t->start; g_img.tlsEnd = t->end;   // already relocated (they have DIR64 relocs)
    g_img.tlsIndexVar = t->index; g_img.tlsZero = t->zerofill;
    *(uint32_t*)(uintptr_t)g_img.tlsIndexVar = 0;
  }
  // exports
  DataDir ed = oh->dd[0];
  if (ed.va) {
    const ExpDir* e = (const ExpDir*)(mem + ed.va);
    const uint32_t* names = (const uint32_t*)(mem + e->aon);
    const uint16_t* ords = (const uint16_t*)(mem + e->aoo);
    const uint32_t* funcs = (const uint32_t*)(mem + e->aof);
    for (uint32_t i = 0; i < e->nnames; i++) g_img.exports[(const char*)(mem + names[i])] = g_img.base + funcs[ords[i]];
  }
  fprintf(stderr, "[rt] image %s loaded at %llx (delta %llx), %zu relocs, %zu imports (%zu unimplemented)\n", verbose_name, (unsigned long long)g_img.base, (unsigned long long)g_img.delta, nrel, nimp, nmiss);
  return true;
}

uint64_t rt_export(const char* name) {
  auto it = g_img.exports.find(name);
  return it == g_img.exports.end() ? 0 : it->second;
}

// ---------------- threads: fake TEB + TLS + guest stack ----------------
static std::atomic<uint32_t> g_tid{1};
uint32_t rt_alloc_tid() { return ++g_tid; }
void rt_thread_setup(size_t guestStack, uint32_t tid) {
  if (!tid) tid = rt_alloc_tid();
  uint8_t* stk = (uint8_t*)rt_amalloc(4096, guestStack + 65536);   // 64 KB of slack above the stack top: Boehm may scan a little past StackBase
  memset(stk + guestStack, 0, 65536);
  memset(stk, 0, 4096);
  uint8_t* teb = (uint8_t*)rt_amalloc(4096, 0x2000);
  memset(teb, 0, 0x2000);
  uint8_t* peb = (uint8_t*)calloc(1, 0x400);
  *(uint64_t*)(peb + 0x10) = g_img.base;
  uint64_t* tlsArr = (uint64_t*)calloc(64, 8);
  size_t tsz = (size_t)(g_img.tlsEnd - g_img.tlsStart) + g_img.tlsZero;
  if (tsz) {
    uint8_t* blk = (uint8_t*)calloc(1, tsz + 64);
    memcpy(blk, (void*)(uintptr_t)g_img.tlsStart, (size_t)(g_img.tlsEnd - g_img.tlsStart));
    tlsArr[0] = (uint64_t)(uintptr_t)blk;
  }
  uint64_t top = ((uint64_t)(uintptr_t)stk + guestStack - 64) & ~15ull;
  *(uint64_t*)(teb + 0x08) = top + 64;            // StackBase
  *(uint64_t*)(teb + 0x10) = (uint64_t)(uintptr_t)stk;   // StackLimit
  *(uint64_t*)(teb + 0x30) = (uint64_t)(uintptr_t)teb;
  *(uint64_t*)(teb + 0x40) = 4242;                // ClientId.UniqueProcess
  *(uint64_t*)(teb + 0x48) = tid;                 // ClientId.UniqueThread
  *(uint64_t*)(teb + 0x58) = (uint64_t)(uintptr_t)tlsArr;
  *(uint64_t*)(teb + 0x60) = (uint64_t)(uintptr_t)peb;
  t_rt_gs = (uint64_t)(uintptr_t)teb;
  if (getenv("RT_VERBOSE")) fprintf(stderr, "[rt] thread %u: stack %llx..%llx top %llx teb %llx\n", tid, (unsigned long long)(uintptr_t)stk, (unsigned long long)(uintptr_t)stk + guestStack, (unsigned long long)top, (unsigned long long)(uintptr_t)teb);
  rt_register_region((uint64_t)(uintptr_t)stk, guestStack);
  { GThread* g = new GThread(); g->tid = tid; t_gthread = g; std::lock_guard<std::mutex> l(g_gtm); g_gthreads[tid] = g; }
  t_guest_top = top;
  t_guest_sp = top;
}
uint64_t rt_guest_call_sp() { return t_guest_sp ? t_guest_sp : t_guest_top; }
uint32_t rt_thread_id() { return (uint32_t)*(uint64_t*)(t_rt_gs + 0x48); }

void rt_guest_call_raw(Ctx* c, uint64_t target) { rt_call(c, target); }

bool rt_run_dllmain() {
  int r = rt_guest_call<int>(g_img.entry, (void*)(uintptr_t)g_img.base, 1u, (void*)nullptr);
  fprintf(stderr, "[rt] DllMain(DLL_PROCESS_ATTACH) returned %d\n", r);
  return r != 0;
}
