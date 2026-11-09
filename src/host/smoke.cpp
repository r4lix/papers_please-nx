// Smoke test: load the real GameAssembly.dll in our own process and drive il2cpp's embedding API.
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>

typedef void* (*fn_domain_get)();
typedef void* (*fn_init)(const char*);
typedef void (*fn_set_data_dir)(const char*);
typedef void** (*fn_domain_get_assemblies)(void*, size_t*);
typedef void* (*fn_assembly_get_image)(void*);
typedef const char* (*fn_image_get_name)(void*);
typedef size_t (*fn_image_get_class_count)(void*);
typedef void* (*fn_class_from_name)(void*, const char*, const char*);
typedef const char* (*fn_class_get_name)(void*);
typedef void* (*fn_thread_attach)(void*);

static HMODULE g;
template <class T> static T sym(const char* n) {
  T p = (T)GetProcAddress(g, n);
  if (!p) { printf("missing export %s\n", n); exit(2); }
  return p;
}

int main(int argc, char** argv) {
  std::string game = argc > 1 ? argv[1] : "C:\\Program Files (x86)\\Steam\\steamapps\\common\\PapersPlease";
  SetDllDirectoryA(game.c_str());
  g = LoadLibraryA((game + "\\GameAssembly.dll").c_str());
  printf("LoadLibrary -> %p (err %lu)\n", (void*)g, GetLastError());
  if (!g) return 1;
  auto set_data_dir = sym<fn_set_data_dir>("il2cpp_set_data_dir");
  auto init = sym<fn_init>("il2cpp_init");
  auto domain_get_assemblies = sym<fn_domain_get_assemblies>("il2cpp_domain_get_assemblies");
  auto domain_get = sym<fn_domain_get>("il2cpp_domain_get");
  auto asm_image = sym<fn_assembly_get_image>("il2cpp_assembly_get_image");
  auto image_name = sym<fn_image_get_name>("il2cpp_image_get_name");
  auto image_ccount = sym<fn_image_get_class_count>("il2cpp_image_get_class_count");
  auto class_from_name = sym<fn_class_from_name>("il2cpp_class_from_name");
  set_data_dir((game + "\\PapersPlease_Data\\il2cpp_data").c_str());
  void* dom = init("IL2CPP Root Domain");
  printf("il2cpp_init -> %p\n", dom);
  size_t n = 0;
  void** as = domain_get_assemblies(domain_get(), &n);
  printf("%zu assemblies\n", n);
  for (size_t i = 0; i < n; i++) {
    void* img = asm_image(as[i]);
    printf("  %s classes=%zu\n", image_name(img), image_ccount(img));
  }
  void* ac = nullptr;
  for (size_t i = 0; i < n; i++) {
    void* img = asm_image(as[i]);
    if (strstr(image_name(img), "Assembly-CSharp.dll")) ac = img;
  }
  void* hu = class_from_name(ac, "", "HostUnity");
  printf("HostUnity class %p\n", hu);
  return 0;
}
