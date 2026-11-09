#include "minu.h"
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------------------------------------
// managed/native glue
// ---------------------------------------------------------------------------------------------------------
static const size_t kCachedPtrOff = 0x10;

static std::map<int, NObj*> g_byId;
static int g_nextId = 1000;
NObj* nativeById(int id) { auto it = g_byId.find(id); return it == g_byId.end() ? nullptr : it->second; }
void registerNative(NObj* n) { if (n && !n->id) { n->id = g_nextId += 2; g_byId[n->id] = n; } }
NObj* nativeOf(void* m) {
  if (!m) return nullptr;
  return *(NObj**)((char*)m + kCachedPtrOff);
}
void bindNative(Il2CppObject* m, NObj* n) {
  *(NObj**)((char*)m + kCachedPtrOff) = n;
  if (n) { n->managed = m; il2cpp_gchandle_new(m, 0); registerNative(n); }
}

static std::map<std::string, Il2CppClass*> g_kcache;
Il2CppClass* K(const char* ns, const char* name) {
  std::string key = std::string(ns) + "." + name;
  auto it = g_kcache.find(key);
  if (it != g_kcache.end()) return it->second;
  size_t n = 0;
  void** as = il2cpp_domain_get_assemblies(il2cpp_domain_get(), &n);
  for (size_t i = 0; i < n; i++) {
    Il2CppClass* c = il2cpp_class_from_name(il2cpp_assembly_get_image(as[i]), ns, name);
    if (c) { g_kcache[key] = c; return c; }
  }
  printf("K: class %s not found\n", key.c_str());
  g_kcache[key] = nullptr;
  return nullptr;
}

Il2CppObject* newManaged(const char* ns, const char* name, NObj* n) {
  Il2CppClass* c = K(ns, name);
  if (!c) return nullptr;
  Il2CppObject* o = il2cpp_object_new(c);
  if (n) bindNative(o, n);
  return o;
}

Il2CppString* S(const std::string& s) { return il2cpp_string_new(s.c_str()); }

std::string U8(Il2CppString* s) {
  if (!s) return "";
  std::string r;
  for (int i = 0; i < s->length; i++) {
    uint32_t c = s->chars[i];
    if (c >= 0xD800 && c < 0xDC00 && i + 1 < s->length) { c = 0x10000 + ((c - 0xD800) << 10) + (s->chars[++i] - 0xDC00); }
    if (c < 0x80) r += (char)c;
    else if (c < 0x800) { r += (char)(0xC0 | (c >> 6)); r += (char)(0x80 | (c & 63)); }
    else if (c < 0x10000) { r += (char)(0xE0 | (c >> 12)); r += (char)(0x80 | ((c >> 6) & 63)); r += (char)(0x80 | (c & 63)); }
    else { r += (char)(0xF0 | (c >> 18)); r += (char)(0x80 | ((c >> 12) & 63)); r += (char)(0x80 | ((c >> 6) & 63)); r += (char)(0x80 | (c & 63)); }
  }
  return r;
}

// ---------------------------------------------------------------------------------------------------------
// global state
// ---------------------------------------------------------------------------------------------------------
struct Resolution { int32_t width, height, refreshRate; };
static int g_screenW = 1280, g_screenH = 720;
int g_screenW_pub = 1280, g_screenH_pub = 720;
static bool g_fullscreen = false;
static int g_targetFps = -1;
std::string g_dataDir = "local/data";

#define ICALL(ret, name, ...) static ret name(__VA_ARGS__)
#ifdef RT_FULL
#include "rt_shim.h"
#define REG(str, fn) impl[str] = makeShim(&fn, str)
#else
#define REG(str, fn) impl[str] = (void*)&fn
#endif

// ---- Application ----------------------------------------------------------------------------------------
ICALL(void, App_SetStackTraceLogType, int, int) {}
ICALL(Il2CppString*, App_persistentDataPath) { return S("C:\\save"); }
ICALL(Il2CppString*, App_streamingAssetsPath) {
#ifdef RT_FULL
  return S("C:\\game\\sa");
#else
  return S(g_dataDir);
#endif
}
ICALL(int, App_platform) { return 7; }          // RuntimePlatform.WindowsPlayer
ICALL(int, App_systemLanguage) { return 10; }   // English
ICALL(int, App_get_targetFrameRate) { return g_targetFps; }
ICALL(void, App_set_targetFrameRate, int v) { g_targetFps = v; }
bool g_appQuit = false;   // the main loop ends after this frame (exit() from inside the guest ran static destructors on a side thread: crash on the console)
ICALL(void, App_Quit, int) { printf("[app] Quit\n"); g_appQuit = true; }
ICALL(void, App_OpenURL, Il2CppString* s) { printf("[app] OpenURL %s\n", U8(s).c_str()); }

// ---- Screen ----------------------------------------------------------------------------------------------
ICALL(int, Scr_width) { return g_screenW; }
ICALL(int, Scr_height) { return g_screenH; }
ICALL(float, Scr_dpi) { return 96.f; }
ICALL(bool, Scr_get_fullScreen) { return g_fullscreen; }
ICALL(void, Scr_set_fullScreen, bool v) { g_fullscreen = v; }
ICALL(void, Scr_SetResolution, int w, int h, int mode, int rr) { g_screenW = g_screenW_pub = w; g_screenH = g_screenH_pub = h; g_fullscreen = mode != 3; (void)rr; }
ICALL(void, Scr_currentResolution, Resolution* r) { r->width = 1920; r->height = 1080; r->refreshRate = 60; }
ICALL(Il2CppArray*, Scr_resolutions) {
  Il2CppClass* c = K("UnityEngine", "Resolution");
  Il2CppArray* a = il2cpp_array_new(c, 1);
  Resolution* d = (Resolution*)((char*)a + 0x20);
  d[0] = {1920, 1080, 60};
  return a;
}
ICALL(void, Scr_SetOrientationEnabled, int, bool) {}

// ---- SystemInfo / Quality / misc -------------------------------------------------------------------------
ICALL(int, Sys_GetDeviceType) { return 2; }       // Desktop
ICALL(Il2CppString*, Sys_GetDeviceModel) { return S("PC"); }
ICALL(int, Q_activeColorSpace) { return 1; }      // Linear? Gamma=0 Linear=1 (set below by data)
ICALL(int, Q_get_vSyncCount) { return 0; }
ICALL(void, Q_set_vSyncCount, int) {}
ICALL(void, Q_set_maxQueuedFrames, int) {}
ICALL(void, Cur_set_visible, bool) {}
ICALL(void, Cur_SetCursor, void*, void*, int) {}
ICALL(void, Input_simulateMouse, bool) {}
int gfxTouchCount();
ICALL(int, Input_touchCount) { return gfxTouchCount(); }
ICALL(void, Dbg_Log, int type, int opt, Il2CppString* msg, void* ctx) { (void)opt; (void)ctx; printf("[unity-log %d] %s\n", type, U8(msg).c_str()); }
ICALL(void, Dbg_LogException, Il2CppObject* e, void* ctx) { (void)ctx; char b[2048]; il2cpp_format_exception(e, b, sizeof b); printf("[unity-exception] %s\n", b); }

void minuRegisterCore(std::map<std::string, void*>& impl);
void minuRegisterGfx(std::map<std::string, void*>& impl);
void minuRegister(std::map<std::string, void*>& impl) {
  minuRegisterCore(impl);
  minuRegisterGfx(impl);
  REG("UnityEngine.Application::SetStackTraceLogType(UnityEngine.LogType,UnityEngine.StackTraceLogType)", App_SetStackTraceLogType);
  REG("UnityEngine.Application::get_persistentDataPath()", App_persistentDataPath);
  REG("UnityEngine.Application::get_streamingAssetsPath()", App_streamingAssetsPath);
  REG("UnityEngine.Application::get_platform()", App_platform);
  REG("UnityEngine.Application::get_systemLanguage()", App_systemLanguage);
  REG("UnityEngine.Application::get_targetFrameRate()", App_get_targetFrameRate);
  REG("UnityEngine.Application::set_targetFrameRate(System.Int32)", App_set_targetFrameRate);
  REG("UnityEngine.Application::Quit(System.Int32)", App_Quit);
  REG("UnityEngine.Application::OpenURL(System.String)", App_OpenURL);
  REG("UnityEngine.Screen::get_width()", Scr_width);
  REG("UnityEngine.Screen::get_height()", Scr_height);
  REG("UnityEngine.Screen::get_dpi()", Scr_dpi);
  REG("UnityEngine.Screen::get_fullScreen()", Scr_get_fullScreen);
  REG("UnityEngine.Screen::set_fullScreen(System.Boolean)", Scr_set_fullScreen);
  REG("UnityEngine.Screen::SetResolution(System.Int32,System.Int32,UnityEngine.FullScreenMode,System.Int32)", Scr_SetResolution);
  REG("UnityEngine.Screen::get_currentResolution_Injected(UnityEngine.Resolution&)", Scr_currentResolution);
  REG("UnityEngine.Screen::get_resolutions()", Scr_resolutions);
  REG("UnityEngine.Screen::SetOrientationEnabled(UnityEngine.EnabledOrientation,System.Boolean)", Scr_SetOrientationEnabled);
  REG("UnityEngine.SystemInfo::GetDeviceType()", Sys_GetDeviceType);
  REG("UnityEngine.SystemInfo::GetDeviceModel()", Sys_GetDeviceModel);
  REG("UnityEngine.QualitySettings::get_activeColorSpace()", Q_activeColorSpace);
  REG("UnityEngine.QualitySettings::get_vSyncCount()", Q_get_vSyncCount);
  REG("UnityEngine.QualitySettings::set_vSyncCount(System.Int32)", Q_set_vSyncCount);
  REG("UnityEngine.QualitySettings::set_maxQueuedFrames(System.Int32)", Q_set_maxQueuedFrames);
  REG("UnityEngine.Cursor::set_visible(System.Boolean)", Cur_set_visible);
  REG("UnityEngine.Cursor::SetCursor_Injected(UnityEngine.Texture2D,UnityEngine.Vector2&,UnityEngine.CursorMode)", Cur_SetCursor);
  REG("UnityEngine.Input::set_simulateMouseWithTouches(System.Boolean)", Input_simulateMouse);
  REG("UnityEngine.Input::get_touchCount()", Input_touchCount);
  REG("UnityEngine.DebugLogHandler::Internal_Log(UnityEngine.LogType,UnityEngine.LogOption,System.String,UnityEngine.Object)", Dbg_Log);
  REG("UnityEngine.DebugLogHandler::Internal_LogException(System.Exception,UnityEngine.Object)", Dbg_LogException);
}



