// Full (no native DLL) host: loads GameAssembly.dll as data, runs the lifted code with our own Win32 shims.
// Portable: Windows test build and Switch.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>
#include <atomic>
#include <map>
#include <set>
#include <chrono>
#include <thread>
#include "rt_full.h"
#include "il2cpp_api.h"
#include "minu.h"
#include "minu_objs.h"
#include "gfx.h"
#ifdef __SWITCH__
#include <pthread.h>
#include <switch.h>
static void nxdbg(const char* m) { svcOutputDebugString(m, strlen(m)); }   // mirrored to the SD card by nroloader --log-sd builds
#endif
#ifdef __SWITCH__
#include <malloc.h>
#endif

extern std::string g_gameRoot, g_saveRoot;
extern std::string g_dataDir;
bool il2cpp_load_full();
void rt_eh_register();

static std::set<std::string> g_logged;
std::map<std::string, void*> g_impl;

static void stubThunk(Ctx* c, void* u) {
  const char* n = (const char*)u;
  if (g_logged.insert(n).second) printf("[icall-stub] %s  (%llx %llx %llx %llx)\n", n, (unsigned long long)c->r[1], (unsigned long long)c->r[2], (unsigned long long)c->r[8], (unsigned long long)c->r[9]);
  c->r[0] = 0; c->x[0] = V128{};
  c->r[4] += 8;
}

static void registerIcalls(const std::string& listFile) {
  minuRegister(g_impl);
  FILE* f = fopen(listFile.c_str(), "rb");
  if (!f) { printf("no icall list %s\n", listFile.c_str()); return; }
  char line[1024];
  while (fgets(line, sizeof line, f)) {
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    if (!n || !strstr(line, "::")) continue;
    auto it = g_impl.find(line);
    if (it != g_impl.end()) { il2cpp_add_internal_call(strdup(line), it->second); continue; }
    char* nm = strdup(line);
    il2cpp_add_internal_call(nm, rt_make_shim(stubThunk, nm, nm));
  }
  fclose(f);
  for (auto& kv : g_impl) il2cpp_add_internal_call(strdup(kv.first.c_str()), kv.second);
}

static void printExc(Il2CppObject* exc) {
  if (!exc) return;
  static char buf[4096], st[8192];
  il2cpp_format_exception(exc, buf, sizeof buf);
  printf("EXCEPTION: %s\n", buf);
  st[0] = 0; il2cpp_format_stack_trace(exc, st, sizeof st); printf("STACK:\n%s\n", st);
}

static bool readFile(const std::string& p, std::vector<uint8_t>& out) {
  FILE* f = fopen(p.c_str(), "rb"); if (!f) return false;
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  out.resize((size_t)n); size_t r = fread(out.data(), 1, (size_t)n, f); fclose(f);
  return r == (size_t)n;
}

#ifdef _WIN32
static void rt_setenv(const char* k, const char* v) { _putenv_s(k, v); }
#else
static void rt_setenv(const char* k, const char* v) { setenv(k, v, 1); }
#endif
#ifdef _WIN32
#include <windows.h>
#include <map>
static std::atomic<bool> g_profGo(false);
static void startProfiler() {
  if (!getenv("RT_PROF")) return;
  if (!getenv("RT_PROF_FROM")) g_profGo = true;   // RT_PROF_FROM=<frame>: start sampling at that frame (skips startup/menus)
  HANDLE th = nullptr;
  DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &th, 0, FALSE, DUPLICATE_SAME_ACCESS);
  std::thread([th]() {
    FILE* o = fopen(getenv("RT_PROF"), "w");
    uint64_t exeBase = (uint64_t)GetModuleHandleA(nullptr);
    int n = 0;
    for (;;) {
      Sleep(2);
      if (!g_profGo) { Sleep(20); continue; }
      if (SuspendThread(th) == (DWORD)-1) break;
      CONTEXT cx; memset(&cx, 0, sizeof cx); cx.ContextFlags = CONTEXT_FULL;
      if (GetThreadContext(th, &cx)) {
        fprintf(o, "S");
        uint64_t rip = cx.Rip;
        for (int d = 0; d < 600; d++) {
          fprintf(o, " %llx", (unsigned long long)(rip >= exeBase && rip < exeBase + 0x10000000 ? rip - exeBase : 0));
          DWORD64 ib = 0; PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(cx.Rip, &ib, nullptr);
          if (!rf) break;
          PVOID hd = nullptr; DWORD64 ef = 0; KNONVOLATILE_CONTEXT_POINTERS np; memset(&np, 0, sizeof np);
          RtlVirtualUnwind(UNW_FLAG_NHANDLER, ib, cx.Rip, rf, &cx, &hd, &ef, &np);
          if (!cx.Rip) break;
          rip = cx.Rip;
        }
        fprintf(o, "\n"); n++;
      }
      ResumeThread(th);
      if (n % 100 == 0) fflush(o);
    }
  }).detach();
}
#else
static void startProfiler() {}
static std::atomic<bool> g_profGo(false);
#endif
static double nowMs() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
static double g_t0 = 0, g_tl = 0;
#define TSTAGE(name) do { double t_ = nowMs(); printf("[time] %-28s +%8.0f ms (total %8.0f ms)\n", name, t_ - g_tl, t_ - g_t0); g_tl = t_; } while (0)
static int real_main(int argc, char** argv) {
  g_t0 = g_tl = nowMs();
  startProfiler();
  setvbuf(stdout, nullptr, _IONBF, 0); setvbuf(stderr, nullptr, _IONBF, 0);
  std::string root = ".";
#ifdef __SWITCH__
  root = "sdmc:/switch/papers";
  if (argc > 0 && argv && argv[0] && strncmp(argv[0], "sdmc:/", 6) == 0) { std::string a = argv[0]; size_t sl = a.find_last_of('/'); if (sl != std::string::npos) root = a.substr(0, sl); }
#endif
#ifdef __SWITCH__
  { std::string lg = root + "/papers.log"; { FILE* t = fopen(lg.c_str(), "w"); if (t) fclose(t); } freopen(lg.c_str(), "a", stdout); freopen(lg.c_str(), "a", stderr); }
  setvbuf(stdout, nullptr, _IONBF, 0); setvbuf(stderr, nullptr, _IONBF, 0);
#endif
  { FILE* vf = fopen((root + "/verbose").c_str(), "rb"); if (vf) { fclose(vf); rt_setenv("RT_VERBOSE", "1"); } }
  std::string game = root + "/game";      // folder containing GameAssembly.dll and il2cpp_data/
  std::string iclist = root + "/icall_names.txt";
  g_dataDir = root + "/data";
  struct Click { int f; float x, y; }; std::vector<Click> clicks; std::vector<std::pair<int, std::string>> shotAt;
  int maxFrames = 0; const char* shot = nullptr; bool hidden = false;
  g_saveRoot = root + "/save";
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--frames") && i + 1 < argc) maxFrames = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
    else if (!strcmp(argv[i], "--hidden")) hidden = true;
    else if (!strcmp(argv[i], "--game") && i + 1 < argc) game = argv[++i];
    else if (!strcmp(argv[i], "--data") && i + 1 < argc) g_dataDir = argv[++i];
    else if (!strcmp(argv[i], "--save") && i + 1 < argc) g_saveRoot = argv[++i];
    else if (!strcmp(argv[i], "--icalls") && i + 1 < argc) iclist = argv[++i];
    else if (!strcmp(argv[i], "--click") && i + 1 < argc) { int f; float x, y; if (sscanf(argv[++i], "%d:%f:%f", &f, &x, &y) == 3) clicks.push_back({f, x, y}); }
    else if (!strcmp(argv[i], "--shotat") && i + 1 < argc) { char* a = argv[++i]; char* c = strchr(a, ':'); if (c) { *c = 0; shotAt.push_back({atoi(a), std::string(c + 1)}); } }
  }
  TSTAGE("args");
  if (hidden) {
#ifdef _WIN32
    _putenv("SDL_AUDIODRIVER=dummy");
#else
    setenv("SDL_AUDIODRIVER", "dummy", 1);
#endif
  }
  if (!gfxInit(1280, 720, !hidden)) return 1;
  TSTAGE("gfxInit");
  gfxSplash();
  TSTAGE("splash");
  g_gameRoot = game;
  rt_win32_register();
  rt_eh_register();
  std::vector<uint8_t> dll;
  if (!readFile(game + "/GameAssembly.dll", dll)) { printf("cannot read %s/GameAssembly.dll\n", game.c_str()); return 1; }
  if (!rt_load_image(dll.data(), dll.size(), "GameAssembly.dll")) return 1;
  TSTAGE("read+load image");
  dll.clear(); dll.shrink_to_fit();
  rt_init_table();
  { extern void fastpathInstall(); fastpathInstall(); }
  TSTAGE("init_table");
  rt_thread_setup(16u << 20);
  if (!rt_run_dllmain()) return 2;
  TSTAGE("dllmain");
  if (!il2cpp_load_full()) return 1;
  TSTAGE("il2cpp_load_full");
  il2cpp_set_data_dir("C:\\game\\il2cpp_data");
  registerIcalls(iclist);
  TSTAGE("registerIcalls");
  void* dom = il2cpp_init("IL2CPP Root Domain");
  printf("init -> %p\n", dom);
  TSTAGE("il2cpp_init");
  il2cpp_gc_disable();   // enabled again once the scene is built and rooted
  audioInit();
  TSTAGE("audioInit");
  Il2CppObject* obj = buildHostUnity();
  il2cpp_gchandle_new(obj, 0);
  Il2CppClass* hu = il2cpp_object_get_class(obj);
  Il2CppObject* exc = nullptr;
  MethodInfo* start = il2cpp_class_get_method_from_name(hu, "Start", 0);
  il2cpp_runtime_invoke(start, obj, nullptr, &exc);
  if (exc) { printExc(exc); return 3; }
  TSTAGE("buildHostUnity+Start");
  il2cpp_gc_enable();
  il2cpp_gc_collect(0);
  TSTAGE("startup gc");
  { extern void fastpathReport(); fastpathReport(); }   // il2cpp init + scene build leave ~500 MB of garbage
  printf("[gc] after startup collection: used %.1f MB heap %.1f MB\n", (double)il2cpp_gc_get_used_size() / 1048576.0, (double)il2cpp_gc_get_heap_size() / 1048576.0);
  MethodInfo* update = il2cpp_class_get_method_from_name(hu, "Update", 0);
  void* tf = il2cpp_class_get_field_from_name(hu, "target");
  bool quit = false;
  for (int frame = 0; !quit && (maxFrames == 0 || frame < maxFrames); frame++) {
    {
      bool act = false, down = false; float mx = 0, my = 0;
      for (auto& c : clicks) { if (frame >= c.f - 5) { act = true; mx = c.x; my = c.y; } if (frame >= c.f && frame < c.f + 4) down = true; }
      gfxScriptMouse(act, mx, my, down);
    }
    { static int pf = getenv("RT_PROF_FROM") ? atoi(getenv("RT_PROF_FROM")) : -1; if (pf >= 0 && frame == pf) g_profGo = true; }
    { extern int g_hostFrame; g_hostFrame = frame + 1; }
    gfxPumpEvents(&quit);
    exc = nullptr;
    static double sUpd = 0, sPres = 0, sT0 = nowMs(); double tA = nowMs();
    il2cpp_runtime_invoke(update, obj, nullptr, &exc);
    if (exc) { printExc(exc); break; }
    double tB = nowMs();
    { extern bool g_appQuit; if (g_appQuit) quit = true; }
    Il2CppObject* target = nullptr; il2cpp_field_get_value(obj, tf, &target);
    gfxPresent(target);
    sUpd += tB - tA; sPres += nowMs() - tB;
    if (frame % 120 == 119) {
      extern long g_t2dCreated; extern double g_t2dBytes; static long lastTex = 0; static double lastTexB = 0;
      double el = nowMs() - sT0;
      printf("[fps] frame %d: %.1f fps (%.1f ms/frame: Update %.1f, present %.1f) tex +%ld (%.1f MB)\n", frame, 120000.0 / el, el / 120, sUpd / 120, sPres / 120, g_t2dCreated - lastTex, (g_t2dBytes - lastTexB) / 1048576.0);
      lastTex = g_t2dCreated; lastTexB = g_t2dBytes; sUpd = sPres = 0; sT0 = nowMs(); }
    if (frame < 5 || frame == 30 || frame == 60 || frame == 120 || frame == 300) { char nm[32]; snprintf(nm, sizeof nm, "frame %d done", frame); TSTAGE(nm); }
    if (frame % 300 == 0) printf("[gc] frame %d used %.1f MB heap %.1f MB disabled=%d\n", frame, (double)il2cpp_gc_get_used_size() / 1048576.0, (double)il2cpp_gc_get_heap_size() / 1048576.0, (int)il2cpp_gc_is_disabled());
#ifdef __SWITCH__
    if (frame % 600 == 0) { struct mallinfo mi = mallinfo(); printf("[mem] frame %d heap in use %.1f MB (arena %.1f MB)\n", frame, mi.uordblks / 1048576.0, mi.arena / 1048576.0); }
#endif
    { static int stress = getenv("RT_GCSTRESS") ? atoi(getenv("RT_GCSTRESS")) : 0; if (stress && frame % stress == 0 && frame) il2cpp_gc_collect(0); }
    if (hidden) std::this_thread::sleep_for(std::chrono::milliseconds(16));
    for (auto& sa : shotAt) if (sa.first == frame) { gfxScreenshot(target, sa.second.c_str()); printf("[host] shot %s\n", sa.second.c_str()); }
    if (shot && frame == maxFrames - 1) { gfxScreenshot(target, shot); printf("[host] screenshot %s\n", shot); }
  }
  printf("done\n");
  fflush(stdout); fflush(stderr);
#ifdef __SWITCH__
  nxdbg("[papers-exit] loop ended, closing audio");
  { extern void audioShutdown(); audioShutdown(); }
  nxdbg("[papers-exit] audio closed, returning to main");
  return 0;   // main() leaves through libnx on the real main thread
#else
  _Exit(0);
#endif
}

#ifdef __SWITCH__
struct MainArgs { int argc; char** argv; int rc; };
static void* mainThread(void* p) { auto* a = (MainArgs*)p; a->rc = real_main(a->argc, a->argv); return nullptr; }
int main(int argc, char** argv) {
  MainArgs a{argc, argv, 0};
  // Installed as an NSP (nroloader, our own title id) libnx must end the process the way a kernel-started NSO does: with ISelfController::Exit,
  // otherwise Home says "The software was closed because an error occurred" on every quit. exit mode 1 = libnx sends Exit and waits for am to end us.
  // (Run from hbmenu the NRO must still return to its loader, so only do this under our own title id.)
  bool asTitle = false;
  { extern u32 __nx_applet_exit_mode; u64 pid = 0; if (R_SUCCEEDED(svcGetInfo(&pid, InfoType_ProgramId, CUR_PROCESS_HANDLE, 0)) && pid == 0x01005CB000100000ULL) { __nx_applet_exit_mode = 1; asTitle = true; } }
  pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 64u << 20);
  pthread_t th; pthread_create(&th, &at, mainThread, &a); pthread_join(th, nullptr);
  nxdbg("[papers-exit] main thread: leaving");
  fflush(stdout); fflush(stderr);
  // libnx's Exit path never returns to the loader (it would have committed the virtual SD / device save on exit): commit it ourselves
  if (asTitle) { int rc = fsdevCommitDevice("sdmc"); char b[64]; snprintf(b, sizeof b, "[papers-exit] commit sdmc = %d", rc); nxdbg(b); }
  _Exit(a.rc);   // no static destructors / atexit: libnx exit on the main thread
}
#else
int main(int argc, char** argv) { return real_main(argc, argv); }
#endif
