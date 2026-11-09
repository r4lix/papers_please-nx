// papers-pc host: runs the real GameAssembly.dll with a tiny "Unity" around it.
#include <windows.h>
#include <process.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <set>
#include "il2cpp_api.h"
#include "minu.h"
#include "minu_objs.h"
#include "gfx.h"
#include "minu_objs.h"

static std::set<std::string> g_logged;
static std::vector<std::string> g_stubNames;
std::map<std::string, void*> g_impl;   // implemented icalls (filled by minu)

static void* makeStub(int id);
extern "C" uint64_t stub_log(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t id) {
  const std::string& n = g_stubNames[id];
  if (!g_logged.count(n)) { g_logged.insert(n); printf("[icall-stub] %s  (%llx %llx %llx %llx)\n", n.c_str(), (unsigned long long)a, (unsigned long long)b, (unsigned long long)c, (unsigned long long)d); fflush(stdout); }
  return 0;
}

static void* makeStub(int id) {
  static uint8_t* pool = nullptr; static size_t used = 0;
  if (!pool) pool = (uint8_t*)VirtualAlloc(nullptr, 1 << 20, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
  uint8_t* p = pool + used;
  uint8_t code[] = {
    0x48, 0x83, 0xEC, 0x38,                    // sub rsp, 0x38
    0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,        // mov rax, id
    0x48, 0x89, 0x44, 0x24, 0x20,              // mov [rsp+0x20], rax   (5th arg)
    0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,        // mov rax, stub_log
    0xFF, 0xD0,                                // call rax
    0x48, 0x83, 0xC4, 0x38,                    // add rsp, 0x38
    0x66, 0x0F, 0xEF, 0xC0,                    // pxor xmm0, xmm0
    0x31, 0xC0,                                // xor eax, eax
    0xC3 };
  *(uint64_t*)(code + 6) = (uint64_t)id;
  *(uint64_t*)(code + 22) = (uint64_t)&stub_log;
  memcpy(p, code, sizeof code);
  used += 64;
  return p;
}

static void registerIcalls(const char* listFile) {
  minuRegister(g_impl);
  FILE* f = fopen(listFile, "rb");
  if (!f) { printf("no icall list %s\n", listFile); return; }
  char line[1024];
  while (fgets(line, sizeof line, f)) {
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    if (!n) continue;
    if (!strstr(line, "::")) continue;
    auto it = g_impl.find(line);
    if (it != g_impl.end()) { il2cpp_add_internal_call(line, it->second); continue; }
    g_stubNames.push_back(line);
    il2cpp_add_internal_call(line, makeStub((int)g_stubNames.size() - 1));
  }
  fclose(f);
  // implemented icalls not in the list
  for (auto& kv : g_impl) il2cpp_add_internal_call(kv.first.c_str(), kv.second);
}

static void printExc(Il2CppObject* exc) {
  if (!exc) return;
  Il2CppString* s = nullptr;
  char buf[4096];
  uint32_t n = il2cpp_format_exception(exc, buf, sizeof buf);
  (void)s; (void)n;
  printf("EXCEPTION: %s\n", buf);
  char st[8192]; st[0]=0; il2cpp_format_stack_trace(exc, st, sizeof st); printf("STACK:\n%s\n", st);
}

static LONG WINAPI vehHandler(EXCEPTION_POINTERS* p) {
  if (p->ExceptionRecord->ExceptionCode == 0xC0000005) {
    HMODULE m = GetModuleHandleA("GameAssembly.dll");
    uint64_t rip = (uint64_t)p->ExceptionRecord->ExceptionAddress;
    printf("[AV] rip=%llx rva=%llx (va %llx) access=%llu addr=%llx\n", (unsigned long long)rip, (unsigned long long)(rip - (uint64_t)m), (unsigned long long)(rip - (uint64_t)m + 0x180000000ull), (unsigned long long)p->ExceptionRecord->ExceptionInformation[0], (unsigned long long)p->ExceptionRecord->ExceptionInformation[1]);
    CONTEXT* c = p->ContextRecord;
    printf("[AV] exe_rva=%llx\n", (unsigned long long)(rip - (uint64_t)GetModuleHandleA(nullptr)));
    printf("[AV] rax=%llx rbx=%llx rcx=%llx rdx=%llx r15=%llx r14=%llx r13=%llx rsi=%llx rdi=%llx\n", (unsigned long long)c->Rax, (unsigned long long)c->Rbx, (unsigned long long)c->Rcx, (unsigned long long)c->Rdx, (unsigned long long)c->R15, (unsigned long long)c->R14, (unsigned long long)c->R13, (unsigned long long)c->Rsi, (unsigned long long)c->Rdi);
    fflush(stdout);
  }
  if (p->ExceptionRecord->ExceptionCode == 0xE06D7363) {
    HMODULE m = GetModuleHandleA("GameAssembly.dll");
    void* fr[16]; USHORT n = RtlCaptureStackBackTrace(0, 16, fr, nullptr);
    printf("[throw] frames:");
    for (int i = 0; i < n; i++) printf(" %llx", (unsigned long long)((uint64_t)fr[i] - (uint64_t)m + 0x180000000ull));
    printf("\n"); fflush(stdout);
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

#ifdef LIFT_HYBRID
void liftInstall();
#endif
int main(int argc, char** argv) {
  AddVectoredExceptionHandler(1, vehHandler);
  {   // never touch the real Steam saves: sandbox every user-profile folder the game could resolve
    char cwd[MAX_PATH]; GetCurrentDirectoryA(MAX_PATH, cwd);
    std::string sb = std::string(cwd) + "\\local\\sandbox";
    CreateDirectoryA((std::string(cwd) + "\\local").c_str(), nullptr); CreateDirectoryA(sb.c_str(), nullptr);
    SetEnvironmentVariableA("APPDATA", sb.c_str()); SetEnvironmentVariableA("LOCALAPPDATA", sb.c_str());
    SetEnvironmentVariableA("USERPROFILE", sb.c_str()); SetEnvironmentVariableA("HOME", sb.c_str());
  }
  std::string game = "C:\\Program Files (x86)\\Steam\\steamapps\\common\\PapersPlease";
  if (!il2cpp_load(game.c_str())) return 1;
#ifdef LIFT_HYBRID
  if (getenv("LIFT")) liftInstall();
#endif
  il2cpp_set_data_dir((game + "\\PapersPlease_Data\\il2cpp_data").c_str());
  registerIcalls("local\\dump\\icall_names.txt");
  void* dom = il2cpp_init("IL2CPP Root Domain");
  printf("init -> %p\n", dom);
  il2cpp_gc_disable();
  size_t n = 0;
  void** as = il2cpp_domain_get_assemblies(il2cpp_domain_get(), &n);
  void* ac = nullptr;
  for (size_t i = 0; i < n; i++) {
    void* img = il2cpp_assembly_get_image(as[i]);
    if (strstr(il2cpp_image_get_name(img), "Assembly-CSharp.dll")) ac = img;
  }
  (void)ac;
  struct Click { int f; float x, y; }; std::vector<Click> clicks; std::vector<std::pair<int, std::string>> shotAt;
  int maxFrames = 0; const char* shot = nullptr; bool hidden = false;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--frames") && i + 1 < argc) maxFrames = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
    else if (!strcmp(argv[i], "--hidden")) hidden = true;
    else if (!strcmp(argv[i], "--click") && i + 1 < argc) { int f; float x, y; if (sscanf(argv[++i], "%d:%f:%f", &f, &x, &y) == 3) clicks.push_back({f, x, y}); }
    else if (!strcmp(argv[i], "--shotat") && i + 1 < argc) { char* a = argv[++i]; char* c = strchr(a, ':'); if (c) { *c = 0; shotAt.push_back({atoi(a), std::string(c + 1)}); } }
  }
  if (hidden) _putenv("SDL_AUDIODRIVER=dummy");
  if (!gfxInit(1280, 720, !hidden)) return 1;
  audioInit();
  Il2CppObject* obj = buildHostUnity();
  Il2CppClass* hu = il2cpp_object_get_class(obj);
  Il2CppObject* exc = nullptr;
  MethodInfo* start = il2cpp_class_get_method_from_name(hu, "Start", 0);
  il2cpp_runtime_invoke(start, obj, nullptr, &exc);
  if (exc) { printExc(exc); return 3; }
  MethodInfo* update = il2cpp_class_get_method_from_name(hu, "Update", 0);
  void* tf = il2cpp_class_get_field_from_name(hu, "target");
  bool quit = false;
  for (int frame = 0; !quit && (maxFrames == 0 || frame < maxFrames); frame++) {
    {
      bool act = false, down = false; float mx = 0, my = 0;
      for (auto& c : clicks) { if (frame >= c.f - 5) { act = true; mx = c.x; my = c.y; } if (frame >= c.f && frame < c.f + 4) down = true; }
      gfxScriptMouse(act, mx, my, down);
    }
    gfxPumpEvents(&quit);
    exc = nullptr;
    il2cpp_runtime_invoke(update, obj, nullptr, &exc);
    if (exc) { printExc(exc); break; }
    Il2CppObject* target = nullptr; il2cpp_field_get_value(obj, tf, &target);
    gfxPresent(target);
    if (hidden) Sleep(16);
    for (auto& sa : shotAt) if (sa.first == frame) { gfxScreenshot(target, sa.second.c_str()); printf("[host] shot %s\n", sa.second.c_str()); }
    if (shot && frame == maxFrames - 1) { gfxScreenshot(target, shot); printf("[host] screenshot %s\n", shot); }
  }
  printf("done\n");
  fflush(stdout); fflush(stderr);
  _exit(0);
}











