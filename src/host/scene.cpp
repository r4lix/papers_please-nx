// Builds the (tiny) scene the real Unity player would deserialize from level0 / sharedassets0.
#include "minu.h"
#include "minu_objs.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

extern std::string g_dataDir;

static void setRef(Il2CppObject* o, const char* field, Il2CppObject* v) {
  Il2CppClass* c = il2cpp_object_get_class(o);
  void* f = nullptr;
  for (; c && !f; c = il2cpp_class_get_parent(c)) f = il2cpp_class_get_field_from_name(c, field);
  if (!f) { printf("setRef: no field %s\n", field); return; }
  il2cpp_field_set_value(o, f, (void*)v);
}

static Il2CppObject* newListFor(Il2CppObject* host, const char* field) {
  Il2CppClass* hc = il2cpp_object_get_class(host);
  void* f = il2cpp_class_get_field_from_name(hc, field);
  Il2CppClass* lc = il2cpp_class_from_type(il2cpp_field_get_type(f));
  Il2CppObject* l = il2cpp_object_new(lc);
  Il2CppObject* exc = nullptr;
  il2cpp_runtime_invoke(il2cpp_class_get_method_from_name(lc, ".ctor", 0), l, nullptr, &exc);
  return l;
}
static void listAdd(Il2CppObject* list, Il2CppObject* item) {
  Il2CppClass* lc = il2cpp_object_get_class(list);
  MethodInfo* add = il2cpp_class_get_method_from_name(lc, "Add", 1);
  void* args[1] = {item};
  Il2CppObject* exc = nullptr;
  il2cpp_runtime_invoke(add, list, args, &exc);
}

static std::vector<std::string> quotedStrings(const std::string& blob) {
  std::vector<std::string> r;
  size_t i = 0;
  while ((i = blob.find('"', i)) != std::string::npos) {
    size_t j = blob.find('"', i + 1);
    if (j == std::string::npos) break;
    r.push_back(blob.substr(i + 1, j - i - 1));
    i = j + 1;
  }
  return r;
}

Il2CppObject* buildHostUnity() {
  Il2CppClass* hu = K("", "HostUnity");
  Il2CppObject* obj = il2cpp_object_new(hu);
  Il2CppObject* exc = nullptr;
  il2cpp_runtime_invoke(il2cpp_class_get_method_from_name(hu, ".ctor", 0), obj, nullptr, &exc);

  auto mkShader = [&](const char* n) { auto* s = new NShader(); s->name = n; return newManaged("UnityEngine", "Shader", s); };
  setRef(obj, "screenShader", mkShader("Hidden/Screen"));
  setRef(obj, "screenPausedShader", mkShader("Hidden/ScreenPaused"));
  setRef(obj, "quadShader", mkShader("Hidden/Quad"));

  // art asset
  {
    auto* t = new NText(); t->name = "Art";
    std::ifstream f(g_dataDir + "/art.dat", std::ios::binary);
    std::stringstream ss; ss << f.rdbuf(); t->bytes = ss.str();
    printf("[scene] Art.dat %zu bytes\n", t->bytes.size());
    setRef(obj, "artAsset", newManaged("UnityEngine", "TextAsset", t));
  }
  setRef(obj, "audioMixer", newManaged("UnityEngine.Audio", "AudioMixer", new NMixer()));

  // quads
  auto mkQuad = [&](const char* name, const char* shaderName) {
    auto* go = new NGameObject(); go->name = name;
    auto* r = new NRenderer(); r->name = name; r->go = go;
    r->material = new NMaterial(); r->material->name = std::string(name) + "Mat"; r->material->shader = shaderName;
    bindNative(newManaged("UnityEngine", "GameObject", go), go);
    Il2CppObject* m = newManaged("UnityEngine", "MeshRenderer", r);
    r->material->managed = newManaged("UnityEngine", "Material", r->material);
    return m;
  };
  setRef(obj, "screenQuad", mkQuad("ScreenQuad", "Hidden/Screen"));
  setRef(obj, "phantomCursor", mkQuad("PhantomCursor", "Hidden/Quad"));

  // pixel perfect camera (Unity.2D.PixelPerfect.dll)
  {
    Il2CppClass* c = K("UnityEngine.Experimental.Rendering.Universal", "PixelPerfectCamera");
    if (!c) c = K("UnityEngine.U2D", "PixelPerfectCamera");
    if (c) {
      Il2CppObject* p = il2cpp_object_new(c);
      il2cpp_runtime_invoke(il2cpp_class_get_method_from_name(c, ".ctor", 0), p, nullptr, &exc);
      auto* b = new NBehaviour(); b->name = "PixelPerfectCamera";
      bindNative(p, b);
      setRef(obj, "pixelPerfectCamera", p);
    }
  }

  // audio
  {
    std::ifstream f(g_dataDir + "/audio.json");
    std::stringstream ss; ss << f.rdbuf();
    std::string js = ss.str();
    size_t a = js.find("\"audioPaths\""), b = js.find("\"audioClips\"");
    auto paths = quotedStrings(js.substr(a + 12, b - a - 12));
    auto clips = quotedStrings(js.substr(b + 12));
    Il2CppObject* lp = newListFor(obj, "audioPaths");
    Il2CppObject* lc = newListFor(obj, "audioClips");
    for (auto& p : paths) listAdd(lp, (Il2CppObject*)S(p));
    for (auto& c : clips) {
      auto* nc = new NClip(); nc->name = c;
      listAdd(lc, newManaged("UnityEngine", "AudioClip", nc));
    }
    setRef(obj, "audioPaths", lp);
    setRef(obj, "audioClips", lc);
    printf("[scene] audio %zu paths %zu clips; list sizes %d %d items %p %p\n", paths.size(), clips.size(), *(int*)((char*)lp + 0x18), *(int*)((char*)lc + 0x18), *(void**)((char*)lp + 0x10), *(void**)((char*)lc + 0x10));
  }
  return obj;
}


