// Win32 / baselib shims for the lifted GameAssembly.dll (portable C++: runs on Windows test host and on Switch).
#include "rt_full.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <wchar.h>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <thread>
#include <chrono>
#include <atomic>
#include <functional>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#ifdef _WIN32
#include <io.h>
#include <direct.h>
#include <windows.h>
#else
#include <unistd.h>
#include <dirent.h>
#include <pthread.h>
#include <sched.h>
#endif

typedef uint16_t WCH;
typedef uint64_t HND;
static const HND INVALID_HANDLE = (HND)-1;

// ---------------- paths ----------------
std::string g_gameRoot = "game";        // where "C:\game\..." maps to
std::string g_saveRoot = "save";
extern std::string g_dataDir;        // where AppData maps to
static std::string u16to8(const WCH* w, int n = -1) {
  std::string r;
  for (int i = 0; (n < 0 ? w[i] : i < n); i++) {
    uint32_t c = w[i];
    if (c >= 0xD800 && c < 0xDC00 && (n < 0 || i + 1 < n) && w[i + 1] >= 0xDC00) { c = 0x10000 + ((c - 0xD800) << 10) + (w[i + 1] - 0xDC00); i++; }
    if (c < 0x80) r += (char)c;
    else if (c < 0x800) { r += (char)(0xC0 | (c >> 6)); r += (char)(0x80 | (c & 63)); }
    else if (c < 0x10000) { r += (char)(0xE0 | (c >> 12)); r += (char)(0x80 | ((c >> 6) & 63)); r += (char)(0x80 | (c & 63)); }
    else { r += (char)(0xF0 | (c >> 18)); r += (char)(0x80 | ((c >> 12) & 63)); r += (char)(0x80 | ((c >> 6) & 63)); r += (char)(0x80 | (c & 63)); }
  }
  return r;
}
static std::vector<WCH> u8to16(const char* s, size_t n) {
  std::vector<WCH> r;
  for (size_t i = 0; i < n;) {
    uint8_t c = (uint8_t)s[i]; uint32_t cp; int len;
    if (c < 0x80) { cp = c; len = 1; } else if ((c >> 5) == 6) { cp = c & 31; len = 2; } else if ((c >> 4) == 14) { cp = c & 15; len = 3; } else { cp = c & 7; len = 4; }
    for (int k = 1; k < len && i + k < n; k++) cp = (cp << 6) | ((uint8_t)s[i + k] & 63);
    i += len;
    if (cp >= 0x10000) { cp -= 0x10000; r.push_back((WCH)(0xD800 + (cp >> 10))); r.push_back((WCH)(0xDC00 + (cp & 0x3ff))); } else r.push_back((WCH)cp);
  }
  return r;
}
static std::string mapPath(const WCH* w) {
  std::string p = u16to8(w);
  for (auto& ch : p) if (ch == '\\') ch = '/';
  if (p.size() > 1 && p[1] == ':') {          // drive letter: C:/game/x -> <gameRoot>/x ; C:/save/x -> <saveRoot>/x
    std::string rest = p.substr(2);
    if (rest.rfind("/save/", 0) == 0) return g_saveRoot + rest.substr(5);
    if (rest.rfind("/game/sa/", 0) == 0 || rest == "/game/sa") return g_dataDir + rest.substr(8);   // StreamingAssets
    if (rest.rfind("/game/", 0) == 0) return g_gameRoot + rest.substr(5);
    return g_gameRoot + rest;
  }
  if (!p.empty() && p[0] != '/' && p.find(':') == std::string::npos) return g_gameRoot + "/" + p;   // relative
  return p;
}

// ---------------- last error ----------------
static thread_local uint32_t t_lastError = 0;
static const uint32_t ERR_FILE_NOT_FOUND = 2, ERR_PATH_NOT_FOUND = 3, ERR_ACCESS_DENIED = 5, ERR_INVALID_HANDLE = 6, ERR_NO_MORE_FILES = 18, ERR_ALREADY_EXISTS = 183, ERR_INVALID_PARAM = 87, ERR_INSUFFICIENT = 122;
static uint32_t errFromErrno() { switch (errno) { case ENOENT: return ERR_FILE_NOT_FOUND; case EEXIST: return ERR_ALREADY_EXISTS; case EACCES: return ERR_ACCESS_DENIED; default: return ERR_PATH_NOT_FOUND; } }

// ---------------- kernel objects ----------------
struct KObj { virtual ~KObj() {} int type = 0; };
struct KFile : KObj { FILE* f = nullptr; std::string path; bool wr = false; KFile() { type = 1; } ~KFile() { if (f) fclose(f); } };
struct KMap : KObj { std::string path; uint64_t size = 0; KMap() { type = 2; } };
struct KEvent : KObj { std::mutex m; std::condition_variable cv; bool sig = false, manual = false; KEvent() { type = 3; } };
struct KSem : KObj { std::mutex m; std::condition_variable cv; int64_t count = 0, max = 0x7fffffff; KSem() { type = 4; } };
struct KMutex : KObj { std::recursive_timed_mutex m; KMutex() { type = 5; } };
struct KThread : KObj { std::thread th; std::mutex m; std::condition_variable cv; bool done = false; uint32_t exitCode = 0; uint32_t tid = 0; bool started = true; KThread() { type = 6; } };
struct KFind : KObj {
  std::vector<std::string> names; size_t idx = 0; std::string dir;
  KFind() { type = 7; }
};
static std::mutex g_hm;
static std::map<HND, std::shared_ptr<KObj>> g_handles;
static std::atomic<uint64_t> g_nextHandle{0x400};
static HND newHandle(std::shared_ptr<KObj> o) { std::lock_guard<std::mutex> l(g_hm); HND h = g_nextHandle.fetch_add(4); g_handles[h] = o; return h; }
static std::shared_ptr<KObj> getObj(HND h) { std::lock_guard<std::mutex> l(g_hm); auto it = g_handles.find(h); return it == g_handles.end() ? nullptr : it->second; }
template <class T> static std::shared_ptr<T> getAs(HND h, int type) { auto o = getObj(h); return (o && o->type == type) ? std::static_pointer_cast<T>(o) : nullptr; }

// ---------------- memory ----------------
struct HdrSz { uint64_t size, magic; };
static std::mutex g_dbgm; static std::map<uint64_t, size_t> g_dbgAllocs; static bool g_dbgOn = getenv("RT_ALLOCLOG") != nullptr;
extern "C" void rt_dbg_find(uint64_t a) {
  std::lock_guard<std::mutex> l(g_dbgm);
  auto it = g_dbgAllocs.upper_bound(a); if (it != g_dbgAllocs.begin()) { --it; fprintf(stderr, "alloc below %llx: base %llx size %zx (end %llx)\n", (unsigned long long)a, (unsigned long long)it->first, it->second, (unsigned long long)(it->first + it->second)); } else fprintf(stderr, "none\n");
}
static std::atomic<uint64_t> g_heapLive{0}, g_vaTotal{0};
extern "C" void rt_alloc_stats(uint64_t* heap, uint64_t* va) { *heap = g_heapLive; *va = g_vaTotal; }
static void* hAlloc(size_t n, bool zero) {
  uint8_t* p = (uint8_t*)rt_amalloc(16, (n + 16 + 15) & ~15ull);
  if (!p) return nullptr;
  ((HdrSz*)p)->size = n; ((HdrSz*)p)->magic = 0x48454150; g_heapLive += n;
  if (g_dbgOn) { std::lock_guard<std::mutex> l(g_dbgm); g_dbgAllocs[(uint64_t)(uintptr_t)(p + 16)] = n; }
  if (zero) memset(p + 16, 0, n);
  return p + 16;
}
static void* HeapAlloc_(HND, uint32_t flags, uint64_t n) { return hAlloc(n, flags & 8); }
static int32_t HeapFree_(HND, uint32_t, void* p) { if (p) { g_heapLive -= ((HdrSz*)((uint8_t*)p - 16))->size; rt_afree((uint8_t*)p - 16); } return 1; }
static void* HeapReAlloc_(HND h, uint32_t flags, void* p, uint64_t n) {
  if (!p) return hAlloc(n, flags & 8);
  size_t old = ((HdrSz*)((uint8_t*)p - 16))->size;
  void* q = hAlloc(n, false);
  if (!q) return nullptr;
  memcpy(q, p, old < n ? old : n);
  if (n > old && (flags & 8)) memset((uint8_t*)q + old, 0, n - old);
  rt_afree((uint8_t*)p - 16);
  return q;
}
static uint64_t HeapSize_(HND, uint32_t, void* p) { return p ? ((HdrSz*)((uint8_t*)p - 16))->size : (uint64_t)-1; }
static HND GetProcessHeap_() { return 0x100; }
static int32_t HeapQueryInformation_(HND, int, void*, uint64_t, uint64_t*) { return 0; }
static void* GlobalAlloc_(uint32_t flags, uint64_t n) { return hAlloc(n, flags & 0x40); }
static void* GlobalFree_(void* p) { if (p) rt_afree((uint8_t*)p - 16); return nullptr; }

static std::mutex g_vm;
static std::map<uint64_t, size_t> g_vallocs;   // base -> size
static void* VirtualAlloc_(void* addr, uint64_t size, uint32_t type, uint32_t prot) {
  std::lock_guard<std::mutex> l(g_vm);
  if (addr) {
    // commit inside an existing reservation
    for (auto& kv : g_vallocs) if ((uint64_t)(uintptr_t)addr >= kv.first && (uint64_t)(uintptr_t)addr < kv.first + kv.second) return addr;
    if (!(type & 0x2000)) { t_lastError = ERR_INVALID_PARAM; return nullptr; }
  }
  size_t sz = (size + 65535) & ~65535ull;
  void* p = rt_amalloc(65536, sz);
  if (!p) { t_lastError = 8; return nullptr; }
  memset(p, 0, sz);
  g_vallocs[(uint64_t)(uintptr_t)p] = sz; g_vaTotal += sz;
  if (getenv("RT_VERBOSE")) fprintf(stderr, "[rt] VirtualAlloc %p size %llx type %x\n", p, (unsigned long long)size, type);
  return p;
}
static int32_t VirtualFree_(void* addr, uint64_t size, uint32_t type) {
  std::lock_guard<std::mutex> l(g_vm);
  if (getenv("RT_VERBOSE")) fprintf(stderr, "[rt] VirtualFree %p size %llx type %x\n", addr, (unsigned long long)size, type);
  if (type & 0x8000) { auto it = g_vallocs.find((uint64_t)(uintptr_t)addr); if (it != g_vallocs.end()) { rt_afree(addr); g_vallocs.erase(it); } return 1; }
  if ((type & 0x4000) && addr && size) {
    // MEM_DECOMMIT: Windows hands back zero pages on the next commit and Boehm relies on it (it skips clearing remapped blocks).
    uint64_t a = (uint64_t)(uintptr_t)addr;
    for (auto& kv : g_vallocs) if (a >= kv.first && a + size <= kv.first + kv.second) { memset(addr, getenv("RT_POISON") ? 0xA5 : 0, size); break; }
  }
  return 1;
}
static uint64_t VirtualQuery_(void* addr, uint8_t* mbi, uint64_t len) {
  memset(mbi, 0, len < 48 ? len : 48);
  uint64_t a = (uint64_t)(uintptr_t)addr;
  uint64_t base = a & ~0xfffull, size = 0, allocBase = 0, next = 0x7FFFFFFF0000ull;
  uint32_t state = 0x10000, prot = 1, type = 0;
  std::vector<std::pair<uint64_t, uint64_t>> regs;
  { std::lock_guard<std::mutex> l(g_vm); for (auto& kv : g_vallocs) regs.push_back({kv.first, kv.second}); }
  regs.push_back({g_img.base, g_img.size});
  { uint64_t rb, rs; if (rt_region_lookup(a, &rb, &rs)) regs.push_back({rb, rs}); }
  bool hit = false;
  for (auto& r : regs) {
    if (a >= r.first && a < r.first + r.second) {
      // Windows semantics: BaseAddress = the queried page, RegionSize counts from there to the end of the region
      hit = true; allocBase = r.first; base = a & ~0xfffull; size = r.first + r.second - base; state = 0x1000; prot = 4;
      type = (r.first == g_img.base) ? 0x1000000 : 0x20000;
    } else if (r.first > a && r.first < next) next = r.first;
  }
  if (!hit) size = next > base ? next - base : 0x1000;
  if (getenv("RT_VQ") && hit) fprintf(stderr, "[vq] %llx -> base %llx size %llx type %x state %x\n", (unsigned long long)a, (unsigned long long)base, (unsigned long long)size, type, state);
  *(uint64_t*)(mbi + 0) = base; *(uint64_t*)(mbi + 8) = allocBase; *(uint32_t*)(mbi + 16) = allocBase ? 4 : 0;
  *(uint64_t*)(mbi + 24) = size; *(uint32_t*)(mbi + 32) = state; *(uint32_t*)(mbi + 36) = prot; *(uint32_t*)(mbi + 40) = type;
  return 48;
}

// ---------------- time ----------------
static uint64_t nowNs() { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec; }
static int32_t QueryPerformanceCounter_(int64_t* v) { *v = (int64_t)nowNs(); return 1; }
static int32_t QueryPerformanceFrequency_(int64_t* v) { *v = 1000000000ll; return 1; }
static void GetSystemTimeAsFileTime_(uint64_t* ft) { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); *ft = ((uint64_t)ts.tv_sec + 11644473600ull) * 10000000ull + ts.tv_nsec / 100; }
static void Sleep_(uint32_t ms) { BlockedScope b; std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
static uint32_t SleepEx_(uint32_t ms, int32_t) { Sleep_(ms); return 0; }
static int32_t SwitchToThread_() { std::this_thread::yield(); return 1; }
static int32_t GetSystemTimes_(uint64_t* idle, uint64_t* k, uint64_t* u) { if (idle) *idle = 0; if (k) *k = 0; if (u) *u = 0; return 1; }
static uint32_t GetTimeZoneInformation_(uint8_t* tz) { memset(tz, 0, 172); return 0; }
static uint32_t GetDynamicTimeZoneInformation_(uint8_t* tz) { memset(tz, 0, 432); return 0; }

// ---------------- sync ----------------
struct SyncCS { std::recursive_mutex m; };
static std::mutex g_syncInit;
template <class T> static T* syncOf(void* slot) {
  uint64_t* p = (uint64_t*)slot;
  T* o = (T*)(uintptr_t)__atomic_load_n(p, __ATOMIC_ACQUIRE);
  if (o) return o;
  std::lock_guard<std::mutex> l(g_syncInit);
  o = (T*)(uintptr_t)*p;
  if (!o) { o = new T(); __atomic_store_n(p, (uint64_t)(uintptr_t)o, __ATOMIC_RELEASE); }
  return o;
}
static void InitializeCriticalSection_(uint8_t* cs) { memset(cs, 0, 40); syncOf<SyncCS>(cs + 24); }
static int32_t InitializeCriticalSectionAndSpinCount_(uint8_t* cs, uint32_t) { InitializeCriticalSection_(cs); return 1; }
static void EnterCriticalSection_(uint8_t* cs) { auto& m = syncOf<SyncCS>(cs + 24)->m; if (m.try_lock()) return; BlockedScope b; m.lock(); }
static void LeaveCriticalSection_(uint8_t* cs) { syncOf<SyncCS>(cs + 24)->m.unlock(); }
static void DeleteCriticalSection_(uint8_t* cs) { }
static void InitializeSRWLock_(void* l) { *(uint64_t*)l = 0; }
static void AcquireSRWLockExclusive_(void* l) { auto* m = syncOf<std::shared_mutex>(l); if (m->try_lock()) return; BlockedScope b; m->lock(); }
static void ReleaseSRWLockExclusive_(void* l) { syncOf<std::shared_mutex>(l)->unlock(); }
static void AcquireSRWLockShared_(void* l) { auto* m = syncOf<std::shared_mutex>(l); if (m->try_lock_shared()) return; BlockedScope b; m->lock_shared(); }
static void ReleaseSRWLockShared_(void* l) { syncOf<std::shared_mutex>(l)->unlock_shared(); }
struct SyncCV { std::condition_variable_any cv; };
static void InitializeConditionVariable_(void* cv) { *(uint64_t*)cv = 0; }
static void WakeConditionVariable_(void* cv) { syncOf<SyncCV>(cv)->cv.notify_one(); }
static void WakeAllConditionVariable_(void* cv) { syncOf<SyncCV>(cv)->cv.notify_all(); }
static int32_t SleepConditionVariableCS_(void* cv, uint8_t* cs, uint32_t ms) {
  BlockedScope blk;
  SyncCV* c = syncOf<SyncCV>(cv); SyncCS* m = syncOf<SyncCS>(cs + 24);
  std::unique_lock<std::recursive_mutex> lk(m->m, std::adopt_lock);
  bool ok = true;
  if (ms == 0xFFFFFFFFu) c->cv.wait(lk); else ok = c->cv.wait_for(lk, std::chrono::milliseconds(ms)) == std::cv_status::no_timeout;
  lk.release();
  if (!ok) t_lastError = 1460;
  return ok;
}

static HND CreateEventW_(void*, int32_t manual, int32_t initial, const WCH*) { auto e = std::make_shared<KEvent>(); e->manual = manual; e->sig = initial; return newHandle(e); }
static int32_t SetEvent_(HND h) { auto e = getAs<KEvent>(h, 3); if (!e) return 0; { std::lock_guard<std::mutex> l(e->m); e->sig = true; } e->cv.notify_all(); return 1; }
static int32_t ResetEvent_(HND h) { auto e = getAs<KEvent>(h, 3); if (!e) return 0; std::lock_guard<std::mutex> l(e->m); e->sig = false; return 1; }
static HND CreateSemaphoreW_(void*, int32_t initial, int32_t max, const WCH*) { auto s = std::make_shared<KSem>(); s->count = initial; s->max = max; return newHandle(s); }
static int32_t ReleaseSemaphore_(HND h, int32_t n, int32_t* prev) {
  auto s = getAs<KSem>(h, 4); if (!s) return 0;
  { std::lock_guard<std::mutex> l(s->m); if (prev) *prev = (int32_t)s->count; s->count += n; }
  s->cv.notify_all(); return 1;
}
static HND CreateMutexW_(void*, int32_t owned, const WCH*) { auto m = std::make_shared<KMutex>(); HND h = newHandle(m); if (owned) m->m.lock(); return h; }
static int32_t ReleaseMutex_(HND h) { auto m = getAs<KMutex>(h, 5); if (!m) return 0; m->m.unlock(); return 1; }

static const uint32_t WAIT_TIMEOUT_ = 258, WAIT_FAILED_ = 0xFFFFFFFFu;
static bool tryWait(std::shared_ptr<KObj> o) {
  switch (o->type) {
    case 3: { auto e = std::static_pointer_cast<KEvent>(o); std::lock_guard<std::mutex> l(e->m); if (e->sig) { if (!e->manual) e->sig = false; return true; } return false; }
    case 4: { auto s = std::static_pointer_cast<KSem>(o); std::lock_guard<std::mutex> l(s->m); if (s->count > 0) { s->count--; return true; } return false; }
    case 5: { auto m = std::static_pointer_cast<KMutex>(o); return m->m.try_lock(); }
    case 6: { auto t = std::static_pointer_cast<KThread>(o); std::lock_guard<std::mutex> l(t->m); return t->done; }
    default: return true;
  }
}
static uint32_t waitMany(uint32_t n, const HND* hs, bool all, uint32_t ms) {
  BlockedScope blk;
  std::vector<std::shared_ptr<KObj>> os;
  for (uint32_t i = 0; i < n; i++) { auto o = getObj(hs[i]); if (!o) { t_lastError = ERR_INVALID_HANDLE; return WAIT_FAILED_; } os.push_back(o); }
  auto t0 = std::chrono::steady_clock::now();
  for (;;) {
    if (!all) { for (uint32_t i = 0; i < n; i++) if (tryWait(os[i])) return i; }
    else { bool ok = true; for (uint32_t i = 0; i < n && ok; i++) { auto o = os[i]; if (o->type == 3 && static_cast<KEvent*>(o.get())->manual) { std::lock_guard<std::mutex> l(static_cast<KEvent*>(o.get())->m); ok = static_cast<KEvent*>(o.get())->sig; } else if (!tryWait(o)) ok = false; } if (ok) return 0; }
    if (ms != 0xFFFFFFFFu && std::chrono::steady_clock::now() - t0 >= std::chrono::milliseconds(ms)) return WAIT_TIMEOUT_;
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
}
static uint32_t WaitForSingleObjectEx_(HND h, uint32_t ms, int32_t) {
  if (h == (HND)-1 || h == (HND)-2) return 0;
  return waitMany(1, &h, false, ms);
}
static uint32_t WaitForMultipleObjectsEx_(uint32_t n, const HND* hs, int32_t all, uint32_t ms, int32_t) { return waitMany(n, hs, all, ms); }
static int32_t CloseHandle_(HND h) { std::lock_guard<std::mutex> l(g_hm); return g_handles.erase(h) ? 1 : 0; }
static int32_t DuplicateHandle_(HND, HND src, HND, HND* dst, uint32_t, int32_t, uint32_t) {
  if (src == (HND)-2 || src == (HND)-1) { auto t = std::make_shared<KThread>(); t->tid = rt_thread_id(); t->done = false; *dst = newHandle(t); return 1; }
  auto o = getObj(src); if (!o) return 0; *dst = newHandle(o); return 1; }

// ---------------- threads ----------------
static std::atomic<uint32_t> g_nextTid{100};
struct ThreadStart { uint64_t fn, param; std::shared_ptr<KThread> kt; uint32_t tid; };
extern "C++" void rt_thread_entry(ThreadStart* ts);
static uint32_t RtThreadMain(ThreadStart* ts) {
  rt_thread_setup(4u << 20, ts->tid);
  uint32_t code = rt_guest_call<uint32_t>(ts->fn, (void*)(uintptr_t)ts->param);
  { std::lock_guard<std::mutex> l(ts->kt->m); ts->kt->done = true; ts->kt->exitCode = code; }
  ts->kt->cv.notify_all();
  delete ts;
  return code;
}
static HND CreateThread_(void*, uint64_t, uint64_t fn, uint64_t param, uint32_t flags, uint32_t* tid) {
  auto kt = std::make_shared<KThread>();
  kt->tid = rt_alloc_tid();
  if (tid) *tid = kt->tid;
  HND h = newHandle(kt);
  auto* ts = new ThreadStart{fn, param, kt, kt->tid};
#if defined(_WIN32)
  kt->th = std::thread([ts]() { RtThreadMain(ts); });
#else
  pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 8u << 20);
  static pthread_t tmp;
  struct Arg { ThreadStart* ts; };
  auto* a = new Arg{ts};
  pthread_create(&tmp, &at, [](void* p) -> void* { auto* a = (Arg*)p; RtThreadMain(a->ts); delete a; return nullptr; }, a);
  pthread_detach(tmp);
#endif
  (void)flags;
  if (kt->th.joinable()) kt->th.detach();
  return h;
}
static HND GetCurrentThread_() { return (HND)-2; }
static HND GetCurrentProcess_() { return (HND)-1; }
static uint32_t GetCurrentThreadId_() { return rt_thread_id(); }
static uint32_t GetCurrentProcessId_() { return 4242; }
static int32_t SetThreadPriority_(HND, int32_t) { return 1; }
static int32_t GetThreadPriority_(HND) { return 0; }
static int32_t GetExitCodeThread_(HND h, uint32_t* c) { auto t = getAs<KThread>(h, 6); if (!t) return 0; *c = t->done ? t->exitCode : 259; return 1; }
static int32_t GetExitCodeProcess_(HND, uint32_t* c) { *c = 259; return 1; }
static uint32_t SuspendThread_(HND h) { auto t = getAs<KThread>(h, 6); return t ? rt_suspend_thread(t->tid) : (uint32_t)-1; }
static uint32_t ResumeThread_(HND h) { auto t = getAs<KThread>(h, 6); return t ? rt_resume_thread(t->tid) : 1; }
static int32_t GetThreadContext_(HND h, uint8_t* ctx) { auto t = getAs<KThread>(h, 6); return t && rt_thread_context(t->tid, ctx) ? 1 : 0; }
static uint32_t QueueUserAPC_(uint64_t, HND, uint64_t) { return 0; }

// TLS slots
static std::atomic<uint32_t> g_tlsNext{0};
static thread_local uint64_t t_tls[1088];
static uint32_t TlsAlloc_() { uint32_t i = g_tlsNext++; return i < 1088 ? i : 0xFFFFFFFFu; }
static uint64_t TlsGetValue_(uint32_t i) { t_lastError = 0; return i < 1088 ? t_tls[i] : 0; }
static int32_t TlsSetValue_(uint32_t i, uint64_t v) { if (i >= 1088) return 0; t_tls[i] = v; return 1; }
static int32_t TlsFree_(uint32_t) { return 1; }

// ---------------- files ----------------
static HND CreateFileW_(const WCH* name, uint32_t access, uint32_t share, void*, uint32_t disp, uint32_t flags, HND) {
  std::string p = mapPath(name);
  if (getenv("RT_VERBOSE")) fprintf(stderr, "[rt] CreateFileW(%s) access=%x disp=%u\n", p.c_str(), access, disp);
  const char* mode;
  bool wr = access & 0x40000000;
  // dispositions: 1 CREATE_NEW 2 CREATE_ALWAYS 3 OPEN_EXISTING 4 OPEN_ALWAYS 5 TRUNCATE_EXISTING
  struct stat st; bool exists = stat(p.c_str(), &st) == 0;
  if (exists && (st.st_mode & S_IFDIR)) { auto f = std::make_shared<KFile>(); f->path = p; return newHandle(f); }
  if (disp == 1 && exists) { t_lastError = ERR_ALREADY_EXISTS; return INVALID_HANDLE; }
  if ((disp == 3 || disp == 5) && !exists) { t_lastError = ERR_FILE_NOT_FOUND; return INVALID_HANDLE; }
  if (disp == 2 || disp == 5) mode = "wb+"; else if (wr) mode = exists ? "rb+" : "wb+"; else mode = "rb";
  FILE* fp = fopen(p.c_str(), mode);
  if (!fp) { t_lastError = errFromErrno(); return INVALID_HANDLE; }
  if (disp == 4 && exists) t_lastError = ERR_ALREADY_EXISTS; else t_lastError = 0;
  auto f = std::make_shared<KFile>(); f->f = fp; f->path = p; f->wr = wr;
  return newHandle(f);
}
static int32_t ReadFile_(HND h, void* buf, uint32_t n, uint32_t* got, void*) {
  auto f = getAs<KFile>(h, 1); if (!f || !f->f) { t_lastError = ERR_INVALID_HANDLE; return 0; }
  size_t r = fread(buf, 1, n, f->f); if (got) *got = (uint32_t)r; return 1;
}
static int32_t WriteFile_(HND h, const void* buf, uint32_t n, uint32_t* wrote, void*) {
  if (h == 4 || h == 5 || h == 3) { fwrite(buf, 1, n, h == 3 ? stdout : stderr); if (wrote) *wrote = n; return 1; }
  auto f = getAs<KFile>(h, 1); if (!f || !f->f) { t_lastError = ERR_INVALID_HANDLE; return 0; }
  size_t r = fwrite(buf, 1, n, f->f); if (wrote) *wrote = (uint32_t)r; return r == n;
}
static int32_t FlushFileBuffers_(HND h) { auto f = getAs<KFile>(h, 1); if (f && f->f) fflush(f->f); return 1; }
static int32_t GetFileSizeEx_(HND h, int64_t* sz) {
  auto f = getAs<KFile>(h, 1); if (!f) return 0;
  if (f->f) { long cur = ftell(f->f); fseek(f->f, 0, SEEK_END); *sz = ftell(f->f); fseek(f->f, cur, SEEK_SET); return 1; }
  *sz = 0; return 1;
}
static int32_t SetFilePointerEx_(HND h, int64_t off, int64_t* newpos, uint32_t method) {
  auto f = getAs<KFile>(h, 1); if (!f || !f->f) return 0;
  fseek(f->f, (long)off, method == 0 ? SEEK_SET : method == 1 ? SEEK_CUR : SEEK_END);
  if (newpos) *newpos = ftell(f->f); return 1;
}
static uint32_t SetFilePointer_(HND h, int32_t off, int32_t* hi, uint32_t method) {
  auto f = getAs<KFile>(h, 1); if (!f || !f->f) return 0xFFFFFFFFu;
  int64_t o = off; if (hi) o |= (int64_t)*hi << 32;
  fseek(f->f, (long)o, method == 0 ? SEEK_SET : method == 1 ? SEEK_CUR : SEEK_END);
  return (uint32_t)ftell(f->f);
}
static int32_t SetEndOfFile_(HND h) {
  auto f = getAs<KFile>(h, 1); if (!f || !f->f) return 0;
  long pos = ftell(f->f); fflush(f->f);
#ifdef _WIN32
  _chsize_s(_fileno(f->f), pos);
#else
  ftruncate(fileno(f->f), pos);
#endif
  return 1;
}
static uint32_t GetFileType_(HND h) { return (h >= 3 && h <= 5) ? 2 : 1; }
static HND GetStdHandle_(uint32_t id) { return id == (uint32_t)-10 ? 3 : id == (uint32_t)-11 ? 4 : 5; }
static int32_t SetStdHandle_(uint32_t, HND) { return 1; }
static int32_t GetFileAttributesExW_(const WCH* name, uint32_t, uint8_t* d) {
  std::string p = mapPath(name); struct stat st;
  if (getenv("RT_VERBOSE")) fprintf(stderr, "[rt] GetFileAttributesExW(%s) -> %d\n", p.c_str(), stat(p.c_str(), &st) == 0);

  if (stat(p.c_str(), &st) != 0) { t_lastError = ERR_FILE_NOT_FOUND; return 0; }
  memset(d, 0, 36);
  *(uint32_t*)d = (st.st_mode & S_IFDIR) ? 0x10 : 0x80;
  uint64_t ft = ((uint64_t)st.st_mtime + 11644473600ull) * 10000000ull;
  memcpy(d + 4, &ft, 8); memcpy(d + 12, &ft, 8); memcpy(d + 20, &ft, 8);
  *(uint32_t*)(d + 28) = (uint32_t)((uint64_t)st.st_size >> 32); *(uint32_t*)(d + 32) = (uint32_t)st.st_size;
  return 1;
}
static int32_t DeleteFileW_(const WCH* n) { return remove(mapPath(n).c_str()) == 0; }
static int32_t RemoveDirectoryW_(const WCH* n) {
#ifdef _WIN32
  return _rmdir(mapPath(n).c_str()) == 0;
#else
  return rmdir(mapPath(n).c_str()) == 0;
#endif
}
static int32_t CreateDirectoryW_(const WCH* n, void*) {
  if (getenv("RT_VERBOSE")) fprintf(stderr, "[rt] CreateDirectoryW(%s)\n", mapPath(n).c_str());
#ifdef _WIN32
  int r = _mkdir(mapPath(n).c_str());
#else
  int r = mkdir(mapPath(n).c_str(), 0777);
#endif
  if (r != 0) { t_lastError = errFromErrno(); return 0; }
  return 1;
}
static int32_t MoveFileExW_(const WCH* a, const WCH* b, uint32_t flags) {
  std::string pa = mapPath(a), pb = mapPath(b);
  if (flags & 1) remove(pb.c_str());
  return rename(pa.c_str(), pb.c_str()) == 0;
}
static int32_t CopyFileW_(const WCH* a, const WCH* b, int32_t failIfExists) {
  std::string pa = mapPath(a), pb = mapPath(b); struct stat st;
  if (failIfExists && stat(pb.c_str(), &st) == 0) { t_lastError = ERR_ALREADY_EXISTS; return 0; }
  FILE* i = fopen(pa.c_str(), "rb"); if (!i) return 0; FILE* o = fopen(pb.c_str(), "wb"); if (!o) { fclose(i); return 0; }
  char buf[65536]; size_t n; while ((n = fread(buf, 1, sizeof buf, i)) > 0) fwrite(buf, 1, n, o);
  fclose(i); fclose(o); return 1;
}
static uint32_t GetFullPathNameW_(const WCH* in, uint32_t n, WCH* out, WCH** fp) {
  size_t len = 0; while (in[len]) len++;
  if (len + 1 > n) return (uint32_t)len + 1;
  memcpy(out, in, (len + 1) * 2); if (fp) *fp = out;
  return (uint32_t)len;
}
static uint32_t GetCurrentDirectoryW_(uint32_t n, WCH* out) { auto s = u8to16("C:\\game", 7); if (s.size() + 1 > n) return (uint32_t)s.size() + 1; memcpy(out, s.data(), s.size() * 2); out[s.size()] = 0; return (uint32_t)s.size(); }
static int32_t SetCurrentDirectoryW_(const WCH*) { return 1; }

// mappings
static HND CreateFileMappingW_(HND hf, void*, uint32_t, uint32_t hi, uint32_t lo, const WCH*) {
  auto f = getAs<KFile>(hf, 1); if (!f) { t_lastError = ERR_INVALID_HANDLE; return 0; }
  auto m = std::make_shared<KMap>(); m->path = f->path; m->size = ((uint64_t)hi << 32) | lo;
  return newHandle(m);
}
static HND OpenFileMappingW_(uint32_t, int32_t, const WCH*) { t_lastError = ERR_FILE_NOT_FOUND; return 0; }
static void* MapViewOfFile_(HND h, uint32_t, uint32_t, uint32_t, uint64_t) {
  auto m = getAs<KMap>(h, 2); if (!m) return nullptr;
  FILE* f = fopen(m->path.c_str(), "rb"); if (!f) return nullptr;
  fseek(f, 0, SEEK_END); size_t sz = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
  uint8_t* p = (uint8_t*)rt_amalloc(65536, (sz + 65535) & ~65535ull);
  memset(p, 0, (sz + 65535) & ~65535ull);
  fread(p, 1, sz, f); fclose(f);
  return p;
}
static int32_t UnmapViewOfFile_(void* p) { rt_afree(p); return 1; }

// directory enumeration
static HND FindFirstFileExW_(const WCH* pat, int32_t, uint8_t* data, int32_t, void*, uint32_t) {
  std::string p = mapPath(pat);
  if (getenv("RT_VERBOSE")) fprintf(stderr, "[rt] FindFirstFileExW(%s)\n", p.c_str());
 size_t sl = p.find_last_of('/'); std::string dir = sl == std::string::npos ? "." : p.substr(0, sl), mask = sl == std::string::npos ? p : p.substr(sl + 1);
  auto fh = std::make_shared<KFind>(); fh->dir = dir;
#ifndef _WIN32
  DIR* d = opendir(dir.c_str());
  if (!d) { t_lastError = ERR_PATH_NOT_FOUND; return INVALID_HANDLE; }
  while (dirent* e = readdir(d)) {
    std::string n = e->d_name;
    if (mask == "*" || mask == "*.*" || n == mask) fh->names.push_back(n);
    else if (mask.size() > 1 && mask[0] == '*' && n.size() >= mask.size() - 1 && n.compare(n.size() - (mask.size() - 1), mask.size() - 1, mask.substr(1)) == 0) fh->names.push_back(n);
  }
  closedir(d);
#endif
  if (fh->names.empty()) { t_lastError = ERR_FILE_NOT_FOUND; return INVALID_HANDLE; }
  auto fill = [&](const std::string& n) {
    memset(data, 0, 592); struct stat st; stat((dir + "/" + n).c_str(), &st);
    *(uint32_t*)data = (st.st_mode & S_IFDIR) ? 0x10 : 0x80;
    *(uint32_t*)(data + 28) = (uint32_t)((uint64_t)st.st_size >> 32); *(uint32_t*)(data + 32) = (uint32_t)st.st_size;
    auto w = u8to16(n.c_str(), n.size()); memcpy(data + 44, w.data(), std::min<size_t>(w.size(), 259) * 2);
  };
  fill(fh->names[0]); fh->idx = 1;
  return newHandle(fh);
}
static int32_t FindNextFileW_(HND h, uint8_t* data) {
  auto fh = getAs<KFind>(h, 7); if (!fh || fh->idx >= fh->names.size()) { t_lastError = ERR_NO_MORE_FILES; return 0; }
  const std::string& n = fh->names[fh->idx++];
  memset(data, 0, 592); struct stat st; stat((fh->dir + "/" + n).c_str(), &st);
  *(uint32_t*)data = (st.st_mode & S_IFDIR) ? 0x10 : 0x80;
  *(uint32_t*)(data + 32) = (uint32_t)st.st_size;
  auto w = u8to16(n.c_str(), n.size()); memcpy(data + 44, w.data(), std::min<size_t>(w.size(), 259) * 2);
  return 1;
}
static int32_t FindClose_(HND h) { return CloseHandle_(h); }

// ---------------- system info / misc ----------------
static void GetSystemInfo_(uint8_t* si) {
  memset(si, 0, 48);
  *(uint16_t*)si = 9; *(uint32_t*)(si + 4) = 4096; *(uint64_t*)(si + 8) = 0x10000; *(uint64_t*)(si + 16) = 0x7FFFFFFEFFFFull;
  *(uint64_t*)(si + 24) = 0xF; *(uint32_t*)(si + 32) = 4; *(uint32_t*)(si + 36) = 8664; *(uint32_t*)(si + 40) = 0x10000;
}
static int32_t GetVersionExW_(uint8_t* v) { *(uint32_t*)(v + 4) = 10; *(uint32_t*)(v + 8) = 0; *(uint32_t*)(v + 12) = 19045; *(uint32_t*)(v + 16) = 2; return 1; }
static const WCH* kCmdLine = nullptr;
static const WCH* GetCommandLineW_() { static std::vector<WCH> w; if (w.empty()) { w = u8to16("game.exe", 8); w.push_back(0); } return w.data(); }
static const char* GetCommandLineA_() { return "game.exe"; }
static uint64_t GetModuleHandleW_(const WCH* n) { return n ? 0x10000 : g_img.base; }
static uint32_t GetModuleFileNameW_(uint64_t, WCH* out, uint32_t n) { auto s = u8to16("C:\\game\\GameAssembly.dll", 24); if (s.size() + 1 > n) return 0; memcpy(out, s.data(), s.size() * 2); out[s.size()] = 0; return (uint32_t)s.size(); }
static uint32_t GetModuleFileNameA_(uint64_t, char* out, uint32_t n) { const char* s = "C:\\game\\GameAssembly.dll"; size_t l = strlen(s); if (l + 1 > n) return 0; memcpy(out, s, l + 1); return (uint32_t)l; }
static int32_t GetModuleHandleExW_(uint32_t, const void* addr, uint64_t* out) { if (out) *out = g_img.base; return 1; }
static uint64_t loadLib(const WCH* n) {
  std::string s = u16to8(n); for (auto& ch : s) ch = (char)tolower((unsigned char)ch);
  size_t sl = s.find_last_of("/\\"); if (sl != std::string::npos) s = s.substr(sl + 1);
  static const char* known[] = {"kernel32", "advapi32", "ole32", "user32", "shell32", "ws2_32", "oleaut32", "iphlpapi", "ntdll", "kernelbase", "ucrtbase", "msvcrt", "mswsock", "winhttp", "crypt32", "secur32", "ws2_32.dll"};
  if (s.rfind("api-ms-win", 0) == 0) return 0x10000;
  for (const char* k : known) if (s.rfind(k, 0) == 0) return 0x10000;
  if (getenv("RT_VERBOSE")) fprintf(stderr, "[rt] LoadLibrary(%s) -> not found\n", s.c_str());
  return 0;
}
static uint64_t LoadLibraryW_(const WCH* n) { return loadLib(n); }
static uint64_t LoadLibraryExW_(const WCH* n, HND, uint32_t) { return loadLib(n); }
static uint64_t GetProcAddress_(uint64_t, const char* name) {
  std::string low; for (const char* q = name; *q; q++) low += (char)tolower((unsigned char)*q);
  void* sh = rt_find_named("*!" + low);
  static const char* dlls[] = {"kernel32", "advapi32", "ole32", "user32", "shell32", "ws2_32", "oleaut32", "iphlpapi"};
  for (int i = 0; !sh && i < 8; i++) sh = rt_find_named(std::string(dlls[i]) + "!" + low);
  if (getenv("RT_VERBOSE")) fprintf(stderr, "[rt] GetProcAddress(%s) -> %p\n", name, sh);
  return (uint64_t)(uintptr_t)sh;
}
static uint32_t FlsAlloc_(uint64_t) { return TlsAlloc_(); }
static uint64_t FlsGetValue_(uint32_t i) { return TlsGetValue_(i); }
static int32_t FlsSetValue_(uint32_t i, uint64_t v) { return TlsSetValue_(i, v); }
static int32_t FlsFree_(uint32_t) { return 1; }
static int32_t InitializeCriticalSectionEx_(uint8_t* cs, uint32_t, uint32_t) { InitializeCriticalSection_(cs); return 1; }
static int32_t FreeLibrary_(uint64_t) { return 1; }
static std::map<std::string, std::string>& envMap() { static std::map<std::string, std::string> m = {}; return m; }
static uint32_t GetEnvironmentVariableW_(const WCH* name, WCH* out, uint32_t n) {
  auto it = envMap().find(u16to8(name));
  if (it == envMap().end()) { t_lastError = 203; return 0; }
  auto w = u8to16(it->second.c_str(), it->second.size());
  if (w.size() + 1 > n) return (uint32_t)w.size() + 1;
  memcpy(out, w.data(), w.size() * 2); out[w.size()] = 0; return (uint32_t)w.size();
}
static int32_t SetEnvironmentVariableW_(const WCH*, const WCH*) { return 1; }
static int32_t SetEnvironmentVariableA_(const char*, const char*) { return 1; }
static WCH* GetEnvironmentStringsW_() {
  std::vector<WCH> blk;
  for (auto& kv : envMap()) { auto w = u8to16((kv.first + "=" + kv.second).c_str(), kv.first.size() + 1 + kv.second.size()); blk.insert(blk.end(), w.begin(), w.end()); blk.push_back(0); }
  blk.push_back(0); blk.push_back(0);
  WCH* p = (WCH*)hAlloc(blk.size() * 2, true); memcpy(p, blk.data(), blk.size() * 2); return p;
}
static int32_t FreeEnvironmentStringsW_(WCH* p) { if (p) rt_afree((uint8_t*)p - 16); return 1; }
static void GetStartupInfoW_(uint8_t* si) { memset(si, 0, 104); *(uint32_t*)si = 104; }
static int32_t IsDebuggerPresent_() { return 0; }
static int32_t IsProcessorFeaturePresent_(uint32_t f) { return f == 10 || f == 13 ? 1 : 0; }
static uint64_t SetUnhandledExceptionFilter_(uint64_t) { return 0; }
static int32_t UnhandledExceptionFilter_(void*) { return 0; }
static uint64_t DecodePointer_(uint64_t p) { return p; }
static uint64_t EncodePointer_(uint64_t p) { return p; }
static void InitializeSListHead_(uint64_t* h) { h[0] = h[1] = 0; }
static uint64_t InterlockedFlushSList_(uint64_t* h) { uint64_t e = h[0]; h[0] = h[1] = 0; return e; }
static uint64_t RtlPcToFileHeader_(uint64_t pc, uint64_t* base) { if (pc >= g_img.base && pc < g_img.base + g_img.size) { *base = g_img.base; return g_img.base; } *base = 0; return 0; }
static uint16_t RtlCaptureStackBackTrace_(uint32_t, uint32_t, void*, uint32_t*) { return 0; }
static void RtlCaptureContext_(void* ctx) { memset(ctx, 0, 1232); }
static void* RtlLookupFunctionEntry_(uint64_t, uint64_t* base, void*) { *base = 0; return nullptr; }
static void RaiseException_(uint32_t code, uint32_t flags, uint32_t n, const uint64_t* args) {
  if (code == 0x406D1388u) return;   // SetThreadName
  fprintf(stderr, "[rt] RaiseException(%x) - exceptions are not supported yet\n", code);
  fflush(stderr); abort();
}
static void RtlUnwind_(void*, void*, void*, void*) { fprintf(stderr, "[rt] RtlUnwind: exceptions are not supported yet\n"); abort(); }
static void TerminateProcess_Void() {}
static int32_t TerminateProcess_(HND, uint32_t code) { fprintf(stderr, "[rt] TerminateProcess(%u)\n", code); fflush(stderr); _Exit((int)code); return 0; }
static void ExitProcess_(uint32_t code) { fprintf(stderr, "[rt] ExitProcess(%u)\n", code); fflush(stderr); _Exit((int)code); }
static uint32_t GetACP_() { return 65001; }
static uint32_t GetOEMCP_() { return 65001; }
static int32_t IsValidCodePage_(uint32_t) { return 1; }
static int32_t GetCPInfo_(uint32_t, uint8_t* i) { memset(i, 0, 18); *(uint32_t*)i = 4; i[4] = '?'; return 1; }
static uint32_t GetUserDefaultLCID_() { return 0x409; }
static uint32_t GetThreadLocale_() { return 0x409; }
static int32_t IsValidLocale_(uint32_t, uint32_t) { return 1; }
static int32_t EnumSystemLocalesW_(uint64_t, uint32_t) { return 1; }
static int32_t GetLocaleInfoW_(uint32_t, uint32_t type, WCH* out, int32_t n) {
  const char* s = (type & 0xFFFF) == 0x5c ? "en-US" : (type & 0xFFFF) == 0x1 ? "English" : "1";
  size_t l = strlen(s);
  if (n == 0) return (int32_t)l + 1;
  if ((size_t)n < l + 1) return 0;
  for (size_t i = 0; i <= l; i++) out[i] = (WCH)s[i];
  return (int32_t)l + 1;
}
static int32_t GetConsoleMode_(HND, uint32_t* m) { *m = 0; return 0; }
static uint32_t GetConsoleCP_() { return 65001; }
static int32_t WriteConsoleW_(HND, const WCH* b, uint32_t n, uint32_t* w, void*) { std::string s = u16to8(b, (int)n); fwrite(s.data(), 1, s.size(), stderr); if (w) *w = n; return 1; }
static int32_t ReadConsoleW_(HND, void*, uint32_t, uint32_t*, void*) { return 0; }
static uint32_t FormatMessageW_(uint32_t, const void*, uint32_t, uint32_t, WCH*, uint32_t, void*) { return 0; }
static int32_t GetFileInformationByHandleEx_(HND h, int32_t cls, uint8_t* buf, uint32_t n) {
  auto f = getAs<KFile>(h, 1); if (!f) { t_lastError = ERR_INVALID_HANDLE; return 0; }
  struct stat st; if (stat(f->path.c_str(), &st) != 0) return 0;
  if (cls == 1 && n >= 24) { memset(buf, 0, 24); *(int64_t*)(buf + 0) = st.st_size; *(int64_t*)(buf + 8) = st.st_size; *(uint32_t*)(buf + 16) = 1; buf[21] = (st.st_mode & S_IFDIR) ? 1 : 0; return 1; }
  if (cls == 0 && n >= 40) { memset(buf, 0, 40); uint64_t ft = ((uint64_t)st.st_mtime + 11644473600ull) * 10000000ull; memcpy(buf, &ft, 8); memcpy(buf + 8, &ft, 8); memcpy(buf + 16, &ft, 8); memcpy(buf + 24, &ft, 8); *(uint32_t*)(buf + 32) = (st.st_mode & S_IFDIR) ? 0x10 : 0x80; return 1; }
  t_lastError = ERR_INVALID_PARAM; return 0;
}
static uint32_t K32GetProcessImageFileNameW_(HND, WCH*, uint32_t) { return 0; }
static int32_t CreatePipe_(HND*, HND*, void*, uint32_t) { return 0; }
static int32_t GetOverlappedResult_(HND, void*, uint32_t*, int32_t) { return 0; }
static int32_t MultiByteToWideChar_(uint32_t, uint32_t, const char* s, int32_t n, WCH* out, int32_t cap) {
  if (n < 0) n = (int32_t)strlen(s) + 1;
  auto w = u8to16(s, (size_t)n);
  if (cap == 0) return (int32_t)w.size();
  if ((int32_t)w.size() > cap) { t_lastError = ERR_INSUFFICIENT; return 0; }
  memcpy(out, w.data(), w.size() * 2); return (int32_t)w.size();
}
static int32_t WideCharToMultiByte_(uint32_t, uint32_t, const WCH* w, int32_t n, char* out, int32_t cap, const char*, int32_t*) {
  bool nul = n < 0; if (n < 0) { n = 0; while (w[n]) n++; }
  std::string s = u16to8(w, n); if (nul) s.push_back(0);
  if (cap == 0) return (int32_t)s.size();
  if ((int32_t)s.size() > cap) { t_lastError = ERR_INSUFFICIENT; return 0; }
  memcpy(out, s.data(), s.size()); return (int32_t)s.size();
}
static int32_t CompareStringW_(uint32_t, uint32_t flags, const WCH* a, int32_t na, const WCH* b, int32_t nb) {
  if (na < 0) { na = 0; while (a[na]) na++; } if (nb < 0) { nb = 0; while (b[nb]) nb++; }
  int n = na < nb ? na : nb;
  for (int i = 0; i < n; i++) { uint32_t x = a[i], y = b[i]; if (flags & 1) { if (x >= 'A' && x <= 'Z') x += 32; if (y >= 'A' && y <= 'Z') y += 32; } if (x != y) return x < y ? 1 : 3; }
  return na == nb ? 2 : na < nb ? 1 : 3;
}
static int32_t LCMapStringW_(uint32_t, uint32_t flags, const WCH* s, int32_t n, WCH* out, int32_t cap) {
  if (n < 0) { n = 0; while (s[n]) n++; n++; }
  if (cap == 0) return n;
  if (n > cap) return 0;
  for (int i = 0; i < n; i++) { uint32_t c = s[i]; if ((flags & 0x100) && c >= 'A' && c <= 'Z') c += 32; if ((flags & 0x200) && c >= 'a' && c <= 'z') c -= 32; out[i] = (WCH)c; }
  return n;
}
static int32_t GetStringTypeW_(uint32_t, const WCH* s, int32_t n, uint16_t* out) {
  for (int i = 0; i < n; i++) { uint32_t c = s[i]; uint16_t t = 0; if (c >= 'A' && c <= 'Z') t = 0x101; else if (c >= 'a' && c <= 'z') t = 0x102; else if (c >= '0' && c <= '9') t = 4; else if (c == ' ') t = 0x40; else if (c < 32) t = 0x20; else if (c < 128) t = 0x10; else t = 0x100; out[i] = t; }
  return 1;
}
static int32_t CryptAcquireContextW_(uint64_t* h, const WCH*, const WCH*, uint32_t, uint32_t) { *h = 1; return 1; }
static int32_t CryptReleaseContext_(uint64_t, uint32_t) { return 1; }
static int32_t CryptGenRandom_(uint64_t, uint32_t n, uint8_t* b) { for (uint32_t i = 0; i < n; i++) b[i] = (uint8_t)rand(); return 1; }
static int32_t ret0_() { return 0; }
static int32_t ret2_() { return 2; }
static int32_t ret1_() { return 1; }

// SHGet*
static int32_t SHGetFolderPathW_(HND, int32_t, HND, uint32_t, WCH* out) { auto s = u8to16("C:\\save", 7); memcpy(out, s.data(), s.size() * 2); out[s.size()] = 0; return 0; }
static int32_t SHGetKnownFolderPath_(const void*, uint32_t, HND, WCH** out) { auto s = u8to16("C:\\save", 7); WCH* p = (WCH*)hAlloc((s.size() + 1) * 2, true); memcpy(p, s.data(), s.size() * 2); *out = p; return 0; }
static void* CoTaskMemAlloc_(uint64_t n) { return hAlloc(n, false); }
static void CoTaskMemFree_(void* p) { if (p) rt_afree((uint8_t*)p - 16); }
static int32_t MessageBoxA_(HND, const char* t, const char* c, uint32_t) { fprintf(stderr, "[MessageBox] %s: %s\n", c ? c : "", t ? t : ""); return 1; }

// baselib (Unity): semaphores + thread id
static uint64_t BL_SemCreate_() { auto s = std::make_shared<KSem>(); return newHandle(s); }
static void BL_SemFree_(uint64_t h) { CloseHandle_(h); }
static void BL_SemAcquire_(uint64_t h) { auto s = getAs<KSem>(h, 4); if (!s) return; BlockedScope b; std::unique_lock<std::mutex> l(s->m); s->cv.wait(l, [&] { return s->count > 0; }); s->count--; }
static void BL_SemRelease_(uint64_t h, uint32_t n) { auto s = getAs<KSem>(h, 4); if (!s) return; { std::lock_guard<std::mutex> l(s->m); s->count += n; } s->cv.notify_all(); }
static int64_t BL_ThreadId_() { return rt_thread_id(); }
static int32_t WSAStartup_(uint16_t, void*) { return 0; }
static int32_t WSACleanup_() { return 0; }
static int32_t WSAGetLastError_() { return 0; }

#define K(dll, name, fn) rt_register_named(dll "!" name, makeShim(fn, name))
void rt_win32_register() {
  K("kernel32", "heapalloc", HeapAlloc_); K("kernel32", "heapfree", HeapFree_); K("kernel32", "heaprealloc", HeapReAlloc_); K("kernel32", "heapsize", HeapSize_);
  K("kernel32", "getprocessheap", GetProcessHeap_); K("kernel32", "heapqueryinformation", HeapQueryInformation_);
  K("kernel32", "globalalloc", GlobalAlloc_); K("kernel32", "globalfree", GlobalFree_);
  K("kernel32", "virtualalloc", VirtualAlloc_); K("kernel32", "virtualfree", VirtualFree_); K("kernel32", "virtualquery", VirtualQuery_);
  K("kernel32", "queryperformancecounter", QueryPerformanceCounter_); K("kernel32", "queryperformancefrequency", QueryPerformanceFrequency_);
  K("kernel32", "getsystemtimeasfiletime", GetSystemTimeAsFileTime_); K("kernel32", "sleep", Sleep_); K("kernel32", "sleepex", SleepEx_); K("kernel32", "switchtothread", SwitchToThread_);
  K("kernel32", "getsystemtimes", GetSystemTimes_); K("kernel32", "gettimezoneinformation", GetTimeZoneInformation_); K("kernel32", "getdynamictimezoneinformation", GetDynamicTimeZoneInformation_);
  K("kernel32", "initializecriticalsection", InitializeCriticalSection_); K("kernel32", "initializecriticalsectionandspincount", InitializeCriticalSectionAndSpinCount_);
  K("kernel32", "entercriticalsection", EnterCriticalSection_); K("kernel32", "leavecriticalsection", LeaveCriticalSection_); K("kernel32", "deletecriticalsection", DeleteCriticalSection_);
  K("kernel32", "initializesrwlock", InitializeSRWLock_); K("kernel32", "acquiresrwlockexclusive", AcquireSRWLockExclusive_); K("kernel32", "releasesrwlockexclusive", ReleaseSRWLockExclusive_);
  K("kernel32", "acquiresrwlockshared", AcquireSRWLockShared_); K("kernel32", "releasesrwlockshared", ReleaseSRWLockShared_);
  K("kernel32", "initializeconditionvariable", InitializeConditionVariable_); K("kernel32", "wakeconditionvariable", WakeConditionVariable_); K("kernel32", "wakeallconditionvariable", WakeAllConditionVariable_);
  K("kernel32", "sleepconditionvariablecs", SleepConditionVariableCS_);
  K("kernel32", "createeventw", CreateEventW_); K("kernel32", "setevent", SetEvent_); K("kernel32", "resetevent", ResetEvent_);
  K("kernel32", "createsemaphorew", CreateSemaphoreW_); K("kernel32", "releasesemaphore", ReleaseSemaphore_); K("kernel32", "createmutexw", CreateMutexW_); K("kernel32", "releasemutex", ReleaseMutex_);
  K("kernel32", "waitforsingleobjectex", WaitForSingleObjectEx_); K("kernel32", "waitformultipleobjectsex", WaitForMultipleObjectsEx_); K("kernel32", "closehandle", CloseHandle_); K("kernel32", "duplicatehandle", DuplicateHandle_);
  K("kernel32", "createthread", CreateThread_); K("kernel32", "getcurrentthread", GetCurrentThread_); K("kernel32", "getcurrentprocess", GetCurrentProcess_); K("kernel32", "getcurrentthreadid", GetCurrentThreadId_);
  K("kernel32", "getcurrentprocessid", GetCurrentProcessId_); K("kernel32", "setthreadpriority", SetThreadPriority_); K("kernel32", "getthreadpriority", GetThreadPriority_);
  K("kernel32", "getexitcodethread", GetExitCodeThread_); K("kernel32", "getexitcodeprocess", GetExitCodeProcess_); K("kernel32", "suspendthread", SuspendThread_); K("kernel32", "resumethread", ResumeThread_);
  K("kernel32", "getthreadcontext", GetThreadContext_); K("kernel32", "queueuserapc", QueueUserAPC_);
  K("kernel32", "tlsalloc", TlsAlloc_); K("kernel32", "tlsgetvalue", TlsGetValue_); K("kernel32", "tlssetvalue", TlsSetValue_); K("kernel32", "tlsfree", TlsFree_);
  K("kernel32", "getlasterror", +[]() -> uint32_t { return t_lastError; }); K("kernel32", "setlasterror", +[](uint32_t e) { t_lastError = e; });
  K("kernel32", "createfilew", CreateFileW_); K("kernel32", "readfile", ReadFile_); K("kernel32", "writefile", WriteFile_); K("kernel32", "flushfilebuffers", FlushFileBuffers_);
  K("kernel32", "getfilesizeex", GetFileSizeEx_); K("kernel32", "setfilepointerex", SetFilePointerEx_); K("kernel32", "setfilepointer", SetFilePointer_); K("kernel32", "setendoffile", SetEndOfFile_);
  K("kernel32", "getfiletype", GetFileType_); K("kernel32", "getstdhandle", GetStdHandle_); K("kernel32", "setstdhandle", SetStdHandle_);
  K("kernel32", "getfileattributesexw", GetFileAttributesExW_); K("kernel32", "deletefilew", DeleteFileW_); K("kernel32", "removedirectoryw", RemoveDirectoryW_); K("kernel32", "createdirectoryw", CreateDirectoryW_);
  K("kernel32", "movefileexw", MoveFileExW_); K("kernel32", "copyfilew", CopyFileW_); K("kernel32", "getfullpathnamew", GetFullPathNameW_); K("kernel32", "getcurrentdirectoryw", GetCurrentDirectoryW_); K("kernel32", "setcurrentdirectoryw", SetCurrentDirectoryW_);
  K("kernel32", "createfilemappingw", CreateFileMappingW_); K("kernel32", "openfilemappingw", OpenFileMappingW_); K("kernel32", "mapviewoffile", MapViewOfFile_); K("kernel32", "unmapviewoffile", UnmapViewOfFile_);
  K("kernel32", "findfirstfileexw", FindFirstFileExW_); K("kernel32", "findnextfilew", FindNextFileW_); K("kernel32", "findclose", FindClose_);
  K("kernel32", "getsysteminfo", GetSystemInfo_); K("kernel32", "getnativesysteminfo", GetSystemInfo_); K("kernel32", "getversionexw", GetVersionExW_);
  K("kernel32", "getcommandlinew", GetCommandLineW_); K("kernel32", "getcommandlinea", GetCommandLineA_); K("kernel32", "getmodulehandlew", GetModuleHandleW_); K("kernel32", "getmodulefilenamew", GetModuleFileNameW_);
  K("kernel32", "getmodulefilenamea", GetModuleFileNameA_); K("kernel32", "getmodulehandleexw", GetModuleHandleExW_); K("kernel32", "loadlibraryw", LoadLibraryW_); K("kernel32", "loadlibraryexw", LoadLibraryExW_);
  K("kernel32", "getprocaddress", GetProcAddress_);
  rt_register_named("*!flsalloc", makeShim(FlsAlloc_, "FlsAlloc")); rt_register_named("*!flsgetvalue", makeShim(FlsGetValue_, "FlsGetValue")); rt_register_named("*!flssetvalue", makeShim(FlsSetValue_, "FlsSetValue")); rt_register_named("*!flsfree", makeShim(FlsFree_, "FlsFree"));
  rt_register_named("*!initializecriticalsectionex", makeShim(InitializeCriticalSectionEx_, "InitializeCriticalSectionEx")); rt_register_named("*!getsystemtimepreciseasfiletime", makeShim(GetSystemTimeAsFileTime_, "GetSystemTimePreciseAsFileTime")); K("kernel32", "freelibrary", FreeLibrary_);
  K("kernel32", "getenvironmentvariablew", GetEnvironmentVariableW_); K("kernel32", "setenvironmentvariablew", SetEnvironmentVariableW_); K("kernel32", "setenvironmentvariablea", SetEnvironmentVariableA_);
  K("kernel32", "getenvironmentstringsw", GetEnvironmentStringsW_); K("kernel32", "freeenvironmentstringsw", FreeEnvironmentStringsW_); K("kernel32", "getstartupinfow", GetStartupInfoW_);
  K("kernel32", "isdebuggerpresent", IsDebuggerPresent_); K("kernel32", "isprocessorfeaturepresent", IsProcessorFeaturePresent_);
  K("kernel32", "setunhandledexceptionfilter", SetUnhandledExceptionFilter_); K("kernel32", "unhandledexceptionfilter", UnhandledExceptionFilter_);
  K("kernel32", "decodepointer", DecodePointer_); K("kernel32", "encodepointer", EncodePointer_); K("kernel32", "initializeslisthead", InitializeSListHead_); K("kernel32", "interlockedflushslist", InterlockedFlushSList_);
  K("kernel32", "rtlpctofileheader", RtlPcToFileHeader_); K("kernel32", "rtlcapturestackbacktrace", RtlCaptureStackBackTrace_); K("kernel32", "rtlcapturecontext", RtlCaptureContext_);
  K("kernel32", "rtllookupfunctionentry", RtlLookupFunctionEntry_); K("kernel32", "raiseexception", RaiseException_); K("kernel32", "rtlunwind", RtlUnwind_);
  K("kernel32", "terminateprocess", TerminateProcess_); K("kernel32", "exitprocess", ExitProcess_);
  K("kernel32", "getacp", GetACP_); K("kernel32", "getoemcp", GetOEMCP_); K("kernel32", "isvalidcodepage", IsValidCodePage_); K("kernel32", "getcpinfo", GetCPInfo_);
  K("kernel32", "getuserdefaultlcid", GetUserDefaultLCID_); K("kernel32", "getthreadlocale", GetThreadLocale_); K("kernel32", "isvalidlocale", IsValidLocale_); K("kernel32", "enumsystemlocalesw", EnumSystemLocalesW_);
  K("kernel32", "getlocaleinfow", GetLocaleInfoW_); K("kernel32", "getconsolemode", GetConsoleMode_); K("kernel32", "getconsolecp", GetConsoleCP_); K("kernel32", "writeconsolew", WriteConsoleW_); K("kernel32", "readconsolew", ReadConsoleW_);
  K("kernel32", "formatmessagew", FormatMessageW_); K("kernel32", "getfileinformationbyhandleex", GetFileInformationByHandleEx_); K("kernel32", "k32getprocessimagefilenamew", K32GetProcessImageFileNameW_);
  K("kernel32", "createpipe", CreatePipe_); K("kernel32", "getoverlappedresult", GetOverlappedResult_);
  K("kernel32", "multibytetowidechar", MultiByteToWideChar_); K("kernel32", "widechartomultibyte", WideCharToMultiByte_); K("kernel32", "comparestringw", CompareStringW_); K("kernel32", "lcmapstringw", LCMapStringW_); K("kernel32", "getstringtypew", GetStringTypeW_);
  K("advapi32", "cryptacquirecontextw", CryptAcquireContextW_); K("advapi32", "cryptreleasecontext", CryptReleaseContext_); K("advapi32", "cryptgenrandom", CryptGenRandom_);
  K("shell32", "shgetfolderpathw", SHGetFolderPathW_); K("shell32", "shgetknownfolderpath", SHGetKnownFolderPath_);
  K("ole32", "cotaskmemalloc", CoTaskMemAlloc_); K("ole32", "cotaskmemfree", CoTaskMemFree_); K("ole32", "coinitializeex", +[](void*, uint32_t) -> int32_t { return 0; }); K("ole32", "couninitialize", +[]() {});
  K("ole32", "cocreatefreethreadedmarshaler", +[](void*, void**) -> int32_t { return (int32_t)0x80004005; }); K("ole32", "cogetcontexttoken", +[](uint64_t*) -> int32_t { return (int32_t)0x80004005; });
  K("user32", "messageboxa", MessageBoxA_);
  K("ws2_32", "wsastartup", WSAStartup_); K("ws2_32", "wsacleanup", WSACleanup_); K("ws2_32", "wsagetlasterror", WSAGetLastError_); K("ws2_32", "send", +[](uint64_t, const char*, int32_t, int32_t) -> int32_t { return -1; });
  K("baselib", "?baselib_systemsemaphore_create@il2cpp_baselib@@ya?auBaselib_SystemSemaphore_Handle@1@xz", BL_SemCreate_);
  rt_register_named("baselib!?baselib_systemsemaphore_create@il2cpp_baselib@@ya?aubaselib_systemsemaphore_handle@1@xz", makeShim(BL_SemCreate_, "BL_SemCreate"));
  rt_register_named("baselib!?baselib_systemsemaphore_free@il2cpp_baselib@@yaxubaselib_systemsemaphore_handle@1@@z", makeShim(BL_SemFree_, "BL_SemFree"));
  rt_register_named("baselib!?baselib_systemsemaphore_acquire@il2cpp_baselib@@yaxubaselib_systemsemaphore_handle@1@@z", makeShim(BL_SemAcquire_, "BL_SemAcquire"));
  rt_register_named("baselib!?baselib_systemsemaphore_release@il2cpp_baselib@@yaxubaselib_systemsemaphore_handle@1@i@z", makeShim(BL_SemRelease_, "BL_SemRelease"));
  rt_register_named("baselib!?baselib_thread_getcurrentthreadid@il2cpp_baselib@@ya_jxz", makeShim(BL_ThreadId_, "BL_ThreadId"));
  // everything else in iphlpapi/oleaut32 returns 0 when called
  rt_register_named("*!zero", makeShim(ret0_, "zero"));
  for (const char* rn : {"regopenkeyexw", "regopenkeyex", "regclosekey", "regqueryvalueexw", "regenumkeyexw", "reggetvaluew", "regqueryinfokeyw", "regenumvaluew", "regcreatekeyexw", "regsetvalueexw", "regdeletekeyw"}) rt_register_named((std::string("advapi32!") + rn).c_str(), makeShim(ret2_, rn));
  const char* zero[] = {"iphlpapi!getifentry", "iphlpapi!getadaptersaddresses", "iphlpapi!getnetworkparams", "oleaut32!safearrayunaccessdata", "oleaut32!safearraygetvartype", "oleaut32!safearrayaccessdata",
                        "oleaut32!safearraygetubound", "oleaut32!safearraygetdim", "oleaut32!safearraydestroy", "oleaut32!safearraycreate", "oleaut32!sysstringlen", "oleaut32!sysfreestring", "oleaut32!safearraygetlbound"};
  for (const char* z : zero) rt_register_named(z, makeShim(ret0_, z));
}
