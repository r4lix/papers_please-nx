#include "il2cpp_api.h"
#include <string>
#ifdef RT_FULL
#include "rt_full.h"
#define DEF(R, N, A) GuestFn<R A> N;
IL2CPP_FUNCS(DEF)
#undef DEF
bool il2cpp_load_full() {
#define LOAD(R, N, A) N.va = rt_export(#N); if (!N.va) { printf("missing export %s\n", #N); return false; }
  IL2CPP_FUNCS(LOAD)
#undef LOAD
  return true;
}
#else
#define DEF(R, N, A) R(*N) A = nullptr;
IL2CPP_FUNCS(DEF)
#undef DEF

bool il2cpp_load(const char* gameDir) {
  std::string g = gameDir;
  SetDllDirectoryA(g.c_str());
  HMODULE m = LoadLibraryA((g + "\\GameAssembly.dll").c_str());
  if (!m) { printf("LoadLibrary GameAssembly.dll failed (%lu)\n", GetLastError()); return false; }
#define LOAD(R, N, A) N = (R(*) A)GetProcAddress(m, #N); if (!N) { printf("missing %s\n", #N); return false; }
  IL2CPP_FUNCS(LOAD)
#undef LOAD
  return true;
}
#endif
