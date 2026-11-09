p = 'src/host/main_full.cpp'
s = open(p, encoding='utf-8', newline='').read().replace('\r\n', '\n')

s = s.replace('#include "gfx.h"\n', '#include "gfx.h"\n#ifdef __SWITCH__\n#include <pthread.h>\n#include <switch.h>\n#endif\n', 1)
s = s.replace('int main(int argc, char** argv) {\n  setvbuf(stdout, nullptr, _IONBF, 0); setvbuf(stderr, nullptr, _IONBF, 0);\n  std::string game = "game";      // folder containing GameAssembly.dll and il2cpp_data/\n  std::string iclist = "icall_names.txt";',
'''static int real_main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0); setvbuf(stderr, nullptr, _IONBF, 0);
  std::string root = ".";
#ifdef __SWITCH__
  root = "sdmc:/switch/papers";
  if (argc > 0 && argv && argv[0]) { std::string a = argv[0]; size_t sl = a.find_last_of('/'); if (sl != std::string::npos) root = a.substr(0, sl); }
#endif
  std::string game = root + "/game";      // folder containing GameAssembly.dll and il2cpp_data/
  std::string iclist = root + "/icall_names.txt";
  g_dataDir = root + "/data";''', 1)
s = s.replace('  g_saveRoot = "save";\n', '  g_saveRoot = root + "/save";\n', 1)
s = s.replace('  _Exit(0);\n}\n', '''  _Exit(0);
}

#ifdef __SWITCH__
struct MainArgs { int argc; char** argv; int rc; };
static void* mainThread(void* p) { auto* a = (MainArgs*)p; a->rc = real_main(a->argc, a->argv); return nullptr; }
int main(int argc, char** argv) {
  MainArgs a{argc, argv, 0};
  pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 64u << 20);
  pthread_t th; pthread_create(&th, &at, mainThread, &a); pthread_join(th, nullptr);
  return a.rc;
}
#else
int main(int argc, char** argv) { return real_main(argc, argv); }
#endif
''', 1)
open(p, 'w', encoding='utf-8', newline='').write(s)
print('ok', 'real_main' in s)
