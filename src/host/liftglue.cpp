// Hybrid lift test glue (x64 Windows): patch lifted functions into the loaded native GameAssembly.dll.
#ifdef LIFT_HYBRID
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <initializer_list>
#include "x64rt.h"
bool rt_hybrid_patch(unsigned idx, uint64_t native_va, uint64_t fn_size);
void rt_hybrid_init(uint64_t image_base, uint64_t image_size);

extern "C" void rt_native_call(Ctx* c, uint64_t target);
static uint64_t g_realRaise = 0;
static void shimRaiseException(Ctx* c) {
  uint32_t code = (uint32_t)c->r[1];
  if (code == 0x406D1388u) { c->r[4] += 8; return; }   // MS_VC_EXCEPTION: SetThreadName, always swallowed
  fprintf(stderr, "[rt] RaiseException(%x) passed to native (unwinding through lifted frames is unsupported)\n", code);
  rt_native_call(c, g_realRaise);
}
void liftHookExit();
void liftInstall() {
  liftHookExit();
  { extern void liftHookThreads(); liftHookThreads(); }
  for (const char* mod : {"kernel32.dll", "kernelbase.dll"}) {
    uint64_t a = (uint64_t)GetProcAddress(GetModuleHandleA(mod), "RaiseException");
    if (a) { g_realRaise = a; rt_register_shim(a, shimRaiseException, "RaiseException"); }
  }
  HMODULE m = GetModuleHandleA("GameAssembly.dll");
  uint64_t base = (uint64_t)m;
  g_delta = base - 0x180000000ull;
  rt_init_table();
  rt_hybrid_init(base, 0xfe3000);
  unsigned n = rt_table_count();
  unsigned lo = 0, hi = n;
  if (const char* r = getenv("LIFT_RANGE")) sscanf(r, "%u:%u", &lo, &hi);
  if (hi > n) hi = n;
  static bool skip[200000];
  if (const char* sk = getenv("LIFT_SKIP")) { const char* q = sk; while (*q) { unsigned v = strtoul(q, (char**)&q, 10); if (v < 200000) skip[v] = true; if (*q == ',') q++; else break; } }
  static bool only[200000]; bool haveOnly = false;
  if (const char* of = getenv("LIFT_ONLY")) { FILE* f = fopen(of, "r"); if (f) { haveOnly = true; unsigned v; while (fscanf(f, "%u,", &v) == 1) if (v < 200000) only[v] = true; fclose(f); } }
  unsigned patched = 0;
  for (unsigned i = 0; i < n; i++) {
    bool eh = (rt_table_size(i) >> 31) & 1;
    bool want = i >= lo && i < hi && !skip[i] && (!haveOnly || only[i]) && (!eh || !getenv("LIFT_NOEH"));
    if (want && rt_hybrid_patch(i, rt_table_va(i) + g_delta, rt_table_size(i))) { patched++; }
    else rt_set_active(i, false);
  }
  extern void* rt_dbg_active_ptr(); fprintf(stderr, "[lift] active ptr %p\n", rt_dbg_active_ptr());
  fprintf(stderr, "[lift] hybrid: %u of %u functions patched (range %u:%u), delta %llx\n", patched, n, lo, hi, (unsigned long long)g_delta);
}
#endif

#ifdef LIFT_HYBRID
// debugging: report who calls ExitProcess / TerminateProcess
static void WINAPI exitHook(UINT code) {
  { extern void rt_dump_native(); rt_dump_native(); }
  void* bt[16]; USHORT n = RtlCaptureStackBackTrace(0, 16, bt, nullptr);
  fprintf(stderr, "[exit] ExitProcess(%u) from:", code);
  uint64_t base = (uint64_t)GetModuleHandleA("GameAssembly.dll");
  for (int i = 0; i < n; i++) fprintf(stderr, " %llx(ga+%llx)", (unsigned long long)bt[i], (unsigned long long)((uint64_t)bt[i] - base));
  fprintf(stderr, "\n"); fflush(stderr);
  TerminateProcess(GetCurrentProcess(), code);
}
void liftHookExit() {
  uint8_t* p = (uint8_t*)GetProcAddress(GetModuleHandleA("kernelbase.dll"), "ExitProcess");
  if (!p) return;
  DWORD old; VirtualProtect(p, 16, PAGE_EXECUTE_READWRITE, &old);
  p[0] = 0xFF; p[1] = 0x25; *(uint32_t*)(p + 2) = 0; *(uint64_t*)(p + 6) = (uint64_t)&exitHook;
}
#endif

#ifdef LIFT_HYBRID
extern "C" void rt_hybrid_thread_init();
struct TArg { LPTHREAD_START_ROUTINE fn; void* p; };
static DWORD WINAPI threadTramp(void* a) {
  TArg t = *(TArg*)a; free(a);
  rt_hybrid_thread_init();
  return t.fn(t.p);
}
typedef HANDLE (WINAPI *CreateThread_t)(LPSECURITY_ATTRIBUTES, SIZE_T, LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD);
static CreateThread_t g_realCreateThread;
static HANDLE WINAPI hookCreateThread(LPSECURITY_ATTRIBUTES a, SIZE_T s, LPTHREAD_START_ROUTINE fn, LPVOID p, DWORD f, LPDWORD id) {
  TArg* t = (TArg*)malloc(sizeof(TArg)); t->fn = fn; t->p = p;
  return g_realCreateThread(a, s, threadTramp, t, f, id);
}
static void patchIat(HMODULE m, const char* name, void* hook) {
  if (!m) return;
  uint8_t* b = (uint8_t*)m;
  auto* nt = (IMAGE_NT_HEADERS*)(b + ((IMAGE_DOS_HEADER*)b)->e_lfanew);
  auto& dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  for (auto* d = (IMAGE_IMPORT_DESCRIPTOR*)(b + dd.VirtualAddress); d->Name; d++) {
    auto* thunk = (IMAGE_THUNK_DATA*)(b + d->FirstThunk);
    auto* orig = (IMAGE_THUNK_DATA*)(b + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
    for (; orig->u1.AddressOfData; orig++, thunk++) {
      if (orig->u1.Ordinal & IMAGE_ORDINAL_FLAG64) continue;
      auto* ibn = (IMAGE_IMPORT_BY_NAME*)(b + orig->u1.AddressOfData);
      if (!strcmp((char*)ibn->Name, name)) {
        DWORD old; VirtualProtect(&thunk->u1.Function, 8, PAGE_READWRITE, &old);
        thunk->u1.Function = (ULONGLONG)hook;
        VirtualProtect(&thunk->u1.Function, 8, old, &old);
        fprintf(stderr, "[lift] hooked %s in module %p\n", name, (void*)m);
      }
    }
  }
}
void liftHookThreads() {
  g_realCreateThread = (CreateThread_t)GetProcAddress(GetModuleHandleA("kernel32.dll"), "CreateThread");
  patchIat(GetModuleHandleA("GameAssembly.dll"), "CreateThread", (void*)hookCreateThread);
  patchIat(LoadLibraryA("baselib.dll"), "CreateThread", (void*)hookCreateThread);
}
#endif
