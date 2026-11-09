// minu: the tiny Unity-shaped native layer the real GameAssembly.dll talks to (icalls + native objects).
#pragma once
#include "il2cpp_api.h"
#include <string>
#include <vector>
#include <map>
#include <cstdint>

// ---- native objects ------------------------------------------------------------------------------
enum NType { N_None, N_Text, N_Shader, N_Material, N_Texture2D, N_RenderTexture, N_Mesh, N_Clip, N_Mixer, N_GameObject,
             N_Transform, N_Renderer, N_Camera, N_Behaviour, N_Cmd, N_Block, N_Source };

struct NObj {
  int id = 0;            // read by managed Object.GetInstanceID() at offset 8 (GetOffsetOfInstanceIDInCPlusPlusObject)
  int type = N_None;
  std::string name;
  Il2CppObject* managed = nullptr;
  virtual ~NObj() {}
};

// managed <-> native
NObj* nativeOf(void* managedObj);
void bindNative(Il2CppObject* managed, NObj* n);
template <class T> T* as(void* managedObj) { return static_cast<T*>(nativeOf(managedObj)); }

Il2CppClass* K(const char* ns, const char* name);
Il2CppObject* newManaged(const char* ns, const char* name, NObj* n = nullptr);
Il2CppString* S(const std::string& s);
std::string U8(Il2CppString* s);

void minuRegister(std::map<std::string, void*>& impl);


NObj* nativeById(int id);
void gfxReleaseNative(NObj* n);
void registerNative(NObj* n);

