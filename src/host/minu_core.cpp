// GameObject / Component / Object / AudioSource / Material / Shader icalls
#include "minu.h"
#include "minu_objs.h"
#include <cstdio>
#include <cstring>
#include <unordered_map>

#define ICALL(ret, name, ...) static ret name(__VA_ARGS__)
#ifdef RT_FULL
#include "rt_shim.h"
#define REG(str, fn) impl[str] = makeShim(&fn, str)
#else
#define REG(str, fn) impl[str] = (void*)&fn
#endif

static NObj* N(void* m) { return nativeOf(m); }

// ---- GameObject --------------------------------------------------------------------------------------------
ICALL(void, GO_Create, Il2CppObject* self, Il2CppString* name) {
  auto* g = new NGameObject(); g->name = U8(name);
  bindNative(self, g);
}
static Il2CppObject* transformOf(NGameObject* g) {
  static std::unordered_map<NGameObject*, Il2CppObject*> m;
  auto it = m.find(g);
  if (it != m.end()) return it->second;
  auto* t = new NTransform(); t->go = g; t->name = g->name;
  Il2CppObject* o = newManaged("UnityEngine", "Transform", t);
  m[g] = o;
  return o;
}
ICALL(Il2CppObject*, GO_AddComponentWithType, Il2CppObject* self, void* systemType) {
  Il2CppClass* c = il2cpp_class_from_system_type(systemType);
  Il2CppObject* o = il2cpp_object_new(c);
  std::string cn = il2cpp_class_get_name(c);
  NGameObject* g = (NGameObject*)N(self);
  if (cn == "AudioSource") { auto* s = new NSource(); s->go = g; s->name = g ? g->name : ""; bindNative(o, s); audioRegister(s); }
  else { auto* b = new NBehaviour(); b->go = g; b->name = cn; bindNative(o, b); }
  return o;
}
ICALL(void, GO_SetActive, Il2CppObject* self, bool v) { if (auto* g = (NGameObject*)N(self)) g->active = v; }
ICALL(void, GO_SendMessage, Il2CppObject*, Il2CppString*, Il2CppObject*, int) {}

// ---- Component ---------------------------------------------------------------------------------------------
static NGameObject* goOf(NObj* n) {
  if (!n) return nullptr;
  switch (n->type) {
    case N_Renderer: return ((NRenderer*)n)->go;
    case N_Transform: return ((NTransform*)n)->go;
    case N_Behaviour: return ((NBehaviour*)n)->go;
    case N_Source: return ((NSource*)n)->go;
    case N_GameObject: return (NGameObject*)n;
  }
  return nullptr;
}
ICALL(Il2CppObject*, Comp_gameObject, Il2CppObject* self) {
  NGameObject* g = goOf(N(self));
  if (!g) return nullptr;
  if (!g->managed) newManaged("UnityEngine", "GameObject", g);
  return g->managed;
}
ICALL(Il2CppObject*, Comp_transform, Il2CppObject* self) {
  NGameObject* g = goOf(N(self));
  if (!g) return nullptr;
  return transformOf(g);
}
ICALL(Il2CppObject*, Comp_GetComponent, Il2CppObject*, Il2CppString*) { return nullptr; }
ICALL(Il2CppObject*, Comp_GetComponentFastPath, Il2CppObject*, void*, void*) { return nullptr; }

// ---- Object ------------------------------------------------------------------------------------------------
ICALL(Il2CppString*, Obj_GetName, Il2CppObject* o) { NObj* n = N(o); return S(n ? n->name : ""); }
ICALL(void, Obj_Destroy, Il2CppObject* o, float) { gfxReleaseNative(N(o)); }
ICALL(void, Obj_DestroyImmediate, Il2CppObject* o, bool) { gfxReleaseNative(N(o)); }
ICALL(void, Obj_DontDestroyOnLoad, Il2CppObject*) {}
ICALL(Il2CppString*, Obj_ToString, Il2CppObject* o) { NObj* n = N(o); return S(n ? n->name : "null"); }
ICALL(void, Obj_set_hideFlags, Il2CppObject*, int) {}
ICALL(int, Obj_OffsetOfInstanceID) { return 8; }

// ---- MonoBehaviour -----------------------------------------------------------------------------------------
ICALL(bool, MB_IsObjectMonoBehaviour, Il2CppObject*) { return true; }
ICALL(bool, MB_get_useGUILayout, Il2CppObject*) { return false; }
ICALL(void, MB_set_useGUILayout, Il2CppObject*, bool) {}

// ---- Renderer / Material / Shader --------------------------------------------------------------------------
ICALL(Il2CppObject*, Rend_GetMaterial, Il2CppObject* self) {
  auto* r = (NRenderer*)N(self);
  return r && r->material ? r->material->managed : nullptr;
}
ICALL(void, Rend_set_enabled, Il2CppObject* self, bool v) { if (auto* r = (NRenderer*)N(self)) r->enabled = v; }
ICALL(void, Mat_CreateWithShader, Il2CppObject* self, Il2CppObject* shader) {
  auto* m = new NMaterial(); m->name = "Material"; if (shader) m->shader = N(shader)->name; bindNative(self, m);
}
ICALL(void, Mat_CreateWithMaterial, Il2CppObject* self, Il2CppObject* src) {
  auto* m = new NMaterial(); if (auto* s = (NMaterial*)N(src)) *m = *s; m->managed = nullptr; bindNative(self, m);
}
ICALL(void, Mat_CreateWithString, Il2CppObject* self) { auto* m = new NMaterial(); bindNative(self, m); }
ICALL(Il2CppObject*, Mat_GetTextureImpl, Il2CppObject* self, int id) {
  auto* m = (NMaterial*)N(self);
  auto it = m->tex.find(id);
  return it == m->tex.end() || !it->second ? nullptr : it->second->managed;
}
ICALL(void, Mat_SetTextureImpl, Il2CppObject* self, int id, Il2CppObject* tex) {
  auto* m = (NMaterial*)N(self); m->tex[id] = tex ? N(tex) : nullptr;
}
static std::unordered_map<std::string, int> g_propIds;
ICALL(int, Shader_PropertyToID, Il2CppString* s) {
  std::string n = U8(s);
  auto it = g_propIds.find(n);
  if (it != g_propIds.end()) return it->second;
  int id = 0x1000 + (int)g_propIds.size();
  g_propIds[n] = id;
  return id;
}
ICALL(int, Shader_TagToID, Il2CppString*) { return 1; }

// ---- AudioSource / mixer -----------------------------------------------------------------------------------
struct ALock { ALock() { audioLock(true); } ~ALock() { audioLock(false); } };
ICALL(void, Src_set_clip, Il2CppObject* self, Il2CppObject* clip) { auto* s = (NSource*)N(self); audioSetClip(s, clip ? (NClip*)N(clip) : nullptr); }
ICALL(void, Src_PlayHelper, Il2CppObject* self, uint64_t delay) { auto* s = (NSource*)N(self); (void)delay; audioPlay(s); }
ICALL(void, Src_Stop, Il2CppObject* self, bool) { auto* s = (NSource*)N(self); audioStop(s); }
ICALL(void, Src_Pause, Il2CppObject* self) { ALock l; ((NSource*)N(self))->paused = true; }
ICALL(void, Src_UnPause, Il2CppObject* self) { ALock l; ((NSource*)N(self))->paused = false; }
ICALL(bool, Src_isPlaying, Il2CppObject* self) { ALock l; auto* s = (NSource*)N(self); return s->playing && !s->paused; }
ICALL(bool, Src_get_loop, Il2CppObject* self) { return ((NSource*)N(self))->loop; }
ICALL(void, Src_set_loop, Il2CppObject* self, bool v) { ALock l; ((NSource*)N(self))->loop = v; }
ICALL(void, Src_set_volume, Il2CppObject* self, float v) { ALock l; ((NSource*)N(self))->volume = v; }
ICALL(float, Src_get_time, Il2CppObject* self) { ALock l; auto* s = (NSource*)N(self); return (float)(s->pos / (s->clip ? s->clip->freq : 44100)); }
ICALL(int, Src_get_timeSamples, Il2CppObject* self) { ALock l; return (int)((NSource*)N(self))->pos; }
ICALL(Il2CppObject*, Src_get_group, Il2CppObject* self) { return nullptr; }
ICALL(void, Src_set_group, Il2CppObject* self, Il2CppObject* g) { ALock l; auto* s = (NSource*)N(self); s->group = g ? N(g)->name : ""; }
ICALL(Il2CppArray*, Mix_FindMatchingGroups, Il2CppObject*, Il2CppString* q) {
  Il2CppClass* c = K("UnityEngine.Audio", "AudioMixerGroup");
  Il2CppArray* a = il2cpp_array_new(c, 1);
  auto* g = new NGroup(); g->name = U8(q); if (g->name.empty()) g->name = "Master";
  ((Il2CppObject**)((char*)a + 0x20))[0] = newManaged("UnityEngine.Audio", "AudioMixerGroup", g);
  return a;
}
ICALL(bool, Mix_SetFloat, Il2CppObject*, Il2CppString* n, float v) { audioSetMixerFloat(U8(n), v); return true; }

#include "minu_objs.h"
ICALL(Il2CppArray*, TA_get_bytes, Il2CppObject* self) {
  auto* t = (NText*)N(self);
  Il2CppArray* a = il2cpp_array_new(K("System", "Byte"), t->bytes.size());
  memcpy((char*)a + 0x20, t->bytes.data(), t->bytes.size());
  return a;
}


// ---- Camera / Transform / Display -------------------------------------------------------------------------------
extern int g_screenW_pub, g_screenH_pub;
static Il2CppObject* g_mainCam = nullptr;
static Il2CppObject* mainCam() {
  if (!g_mainCam) {
    auto* g = new NGameObject(); g->name = "Main Camera";
    Il2CppObject* go = newManaged("UnityEngine", "GameObject", g); (void)go;
    auto* c = new NCamera(); c->go = g; c->name = "Main Camera";
    g_mainCam = newManaged("UnityEngine", "Camera", c);
  }
  return g_mainCam;
}
ICALL(Il2CppObject*, Cam_main) { return mainCam(); }
ICALL(int, Cam_count) { return 1; }
ICALL(int, Cam_all, Il2CppArray* a) { ((Il2CppObject**)((char*)a + 0x20))[0] = mainCam(); return 1; }
ICALL(void, Cam_void, Il2CppObject*) {}
ICALL(float, Cam_aspect, Il2CppObject*) { return (float)g_screenW_pub / g_screenH_pub; }
ICALL(int, Cam_clearFlags, Il2CppObject*) { return 2; }
ICALL(int, Cam_mask, Il2CppObject*) { return -1; }
ICALL(float, Cam_far, Il2CppObject*) { return 1000.f; }
ICALL(float, Cam_near, Il2CppObject*) { return 0.3f; }
ICALL(float, Cam_ortho, Il2CppObject* s) { return ((NCamera*)N(s))->ortho; }
ICALL(void, Cam_set_ortho, Il2CppObject* s, float v) { ((NCamera*)N(s))->ortho = v; }
ICALL(bool, Cam_false, Il2CppObject*) { return false; }
ICALL(void, Cam_set_bool, Il2CppObject*, bool) {}
ICALL(Il2CppObject*, Cam_null, Il2CppObject*) { return nullptr; }
ICALL(int, Cam_zero, Il2CppObject*) { return 0; }
ICALL(void, Cam_pixelRect, Il2CppObject*, float* r) { r[0] = 0; r[1] = 0; r[2] = (float)g_screenW_pub; r[3] = (float)g_screenH_pub; }
ICALL(void, Cam_set_rect, Il2CppObject*, float*) {}
ICALL(void, Cam_set_mat, Il2CppObject*, float*) {}
ICALL(void, Cam_ScreenToWorld, Il2CppObject* s, float* v, int, float* o) { o[0] = v[0] / g_screenW_pub - 0.5f; o[1] = v[1] / g_screenH_pub - 0.5f; o[2] = 0; }  // world == quad-local (screen quad spans -0.5..0.5)
ICALL(void, Cam_ScreenPointToRay, Il2CppObject*, float* v, int, float* o) { memset(o, 0, 24); o[0] = v[0]; o[1] = v[1]; o[5] = 1; }
ICALL(void, Disp_ext, void*, int* w, int* h) { *w = g_screenW_pub; *h = g_screenH_pub; }
ICALL(int, Disp_rel, int, int, int* x, int* y) { *x = 0; *y = 0; return 0; }
ICALL(int, Tr_childCount, Il2CppObject*) { return 0; }
ICALL(Il2CppObject*, Tr_child, Il2CppObject*, int) { return nullptr; }
ICALL(void, Tr_matrix, Il2CppObject*, float* m) { memset(m, 0, 64); m[0] = m[5] = m[10] = m[15] = 1; }
ICALL(void, Tr_get_pos, Il2CppObject* s, float* p) { auto* t = (NTransform*)N(s); p[0] = t->pos[0]; p[1] = t->pos[1]; p[2] = t->pos[2]; }
ICALL(void, Tr_set_pos, Il2CppObject* s, float* p) { auto* t = (NTransform*)N(s); t->pos[0] = p[0]; t->pos[1] = p[1]; t->pos[2] = p[2]; }
ICALL(void, Tr_get_rot, Il2CppObject*, float* q) { q[0] = q[1] = q[2] = 0; q[3] = 1; }
ICALL(void, Tr_set_scale, Il2CppObject*, float*) {}

static void regCam(std::map<std::string, void*>& impl) {
  REG("UnityEngine.Camera::get_main()", Cam_main);
  REG("UnityEngine.Camera::GetAllCamerasCount()", Cam_count);
  REG("UnityEngine.Camera::GetAllCamerasImpl(UnityEngine.Camera[])", Cam_all);
  REG("UnityEngine.Camera::ResetAspect()", Cam_void);
  REG("UnityEngine.Camera::ResetWorldToCameraMatrix()", Cam_void);
  REG("UnityEngine.Camera::get_aspect()", Cam_aspect);
  REG("UnityEngine.Camera::get_clearFlags()", Cam_clearFlags);
  REG("UnityEngine.Camera::get_cullingMask()", Cam_mask);
  REG("UnityEngine.Camera::get_eventMask()", Cam_mask);
  REG("UnityEngine.Camera::get_farClipPlane()", Cam_far);
  REG("UnityEngine.Camera::get_nearClipPlane()", Cam_near);
  REG("UnityEngine.Camera::get_orthographicSize()", Cam_ortho);
  REG("UnityEngine.Camera::set_orthographicSize(System.Single)", Cam_set_ortho);
  REG("UnityEngine.Camera::get_allowMSAA()", Cam_false);
  REG("UnityEngine.Camera::get_forceIntoRenderTexture()", Cam_false);
  REG("UnityEngine.Camera::set_forceIntoRenderTexture(System.Boolean)", Cam_set_bool);
  REG("UnityEngine.Camera::get_activeTexture()", Cam_null);
  REG("UnityEngine.Camera::get_targetTexture()", Cam_null);
  REG("UnityEngine.Camera::get_targetDisplay()", Cam_zero);
  REG("UnityEngine.Camera::get_pixelRect_Injected(UnityEngine.Rect&)", Cam_pixelRect);
  REG("UnityEngine.Camera::set_pixelRect_Injected(UnityEngine.Rect&)", Cam_set_rect);
  REG("UnityEngine.Camera::set_rect_Injected(UnityEngine.Rect&)", Cam_set_rect);
  REG("UnityEngine.Camera::set_worldToCameraMatrix_Injected(UnityEngine.Matrix4x4&)", Cam_set_mat);
  REG("UnityEngine.Camera::ScreenToWorldPoint_Injected(UnityEngine.Vector3&,UnityEngine.Camera/MonoOrStereoscopicEye,UnityEngine.Vector3&)", Cam_ScreenToWorld);
  REG("UnityEngine.Camera::ScreenPointToRay_Injected(UnityEngine.Vector2&,UnityEngine.Camera/MonoOrStereoscopicEye,UnityEngine.Ray&)", Cam_ScreenPointToRay);
  REG("UnityEngine.Display::GetSystemExtImpl(System.IntPtr,System.Int32&,System.Int32&)", Disp_ext);
  REG("UnityEngine.Display::RelativeMouseAtImpl(System.Int32,System.Int32,System.Int32&,System.Int32&)", Disp_rel);
  REG("UnityEngine.Transform::get_childCount()", Tr_childCount);
  REG("UnityEngine.Transform::GetChild(System.Int32)", Tr_child);
  REG("UnityEngine.Transform::get_localToWorldMatrix_Injected(UnityEngine.Matrix4x4&)", Tr_matrix);
  REG("UnityEngine.Transform::get_worldToLocalMatrix_Injected(UnityEngine.Matrix4x4&)", Tr_matrix);
  REG("UnityEngine.Transform::get_position_Injected(UnityEngine.Vector3&)", Tr_get_pos);
  REG("UnityEngine.Transform::set_position_Injected(UnityEngine.Vector3&)", Tr_set_pos);
  REG("UnityEngine.Transform::get_rotation_Injected(UnityEngine.Quaternion&)", Tr_get_rot);
  REG("UnityEngine.Transform::set_localScale_Injected(UnityEngine.Vector3&)", Tr_set_scale);
}
ICALL(bool, US_IsBlittable, void*) { return true; }
ICALL(void, US_Free, void* p, int) { (void)p; }
int g_hostFrame = 1;   // advanced once per rendered frame by the main loop (it used to tick on every call, which made the game's texture cache evict everything each frame)
ICALL(int, Time_frameCount) { return g_hostFrame; }
ICALL(void*, Prof_CreateMarker, Il2CppString*, uint16_t, int, int) { static char dummy[64]; return dummy; }

ICALL(void*, NoAlloc_Extract, Il2CppObject* list) { return list ? *(void**)((char*)list + 0x10) : nullptr; }
#include <cmath>
ICALL(void, M4_Ortho, float l, float r, float b, float t, float zn, float zf, float* m) {
  memset(m, 0, 64);
  m[0] = 2.f / (r - l); m[5] = 2.f / (t - b); m[10] = -2.f / (zf - zn);
  m[12] = -(r + l) / (r - l); m[13] = -(t + b) / (t - b); m[14] = -(zf + zn) / (zf - zn); m[15] = 1.f;
}
ICALL(void, M4_TRS, float* p, float* q, float* s, float* m) {
  float x = q[0], y = q[1], z = q[2], w = q[3];
  float xx = x*x, yy = y*y, zz = z*z, xy = x*y, xz = x*z, yz = y*z, wx = w*x, wy = w*y, wz = w*z;
  m[0] = (1 - 2*(yy+zz)) * s[0]; m[1] = (2*(xy+wz)) * s[0]; m[2] = (2*(xz-wy)) * s[0]; m[3] = 0;
  m[4] = (2*(xy-wz)) * s[1]; m[5] = (1 - 2*(xx+zz)) * s[1]; m[6] = (2*(yz+wx)) * s[1]; m[7] = 0;
  m[8] = (2*(xz+wy)) * s[2]; m[9] = (2*(yz-wx)) * s[2]; m[10] = (1 - 2*(xx+yy)) * s[2]; m[11] = 0;
  m[12] = p[0]; m[13] = p[1]; m[14] = p[2]; m[15] = 1;
}
ICALL(float, Mathf_GammaToLinear, float v) { return v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f); }
void minuRegisterCore(std::map<std::string, void*>& impl) {
  REG("UnityEngine.Matrix4x4::Ortho_Injected(System.Single,System.Single,System.Single,System.Single,System.Single,System.Single,UnityEngine.Matrix4x4&)", M4_Ortho);
  REG("UnityEngine.Matrix4x4::TRS_Injected(UnityEngine.Vector3&,UnityEngine.Quaternion&,UnityEngine.Vector3&,UnityEngine.Matrix4x4&)", M4_TRS);
  REG("UnityEngine.Mathf::GammaToLinearSpace(System.Single)", Mathf_GammaToLinear);
  REG("UnityEngine.NoAllocHelpers::ExtractArrayFromList(System.Object)", NoAlloc_Extract);
  REG("Unity.Collections.LowLevel.Unsafe.UnsafeUtility::IsBlittable(System.Type)", US_IsBlittable);
  REG("Unity.Collections.LowLevel.Unsafe.UnsafeUtility::Free(System.Void*,Unity.Collections.Allocator)", US_Free);
  REG("UnityEngine.Time::get_frameCount()", Time_frameCount);
  REG("Unity.Profiling.LowLevel.Unsafe.ProfilerUnsafeUtility::CreateMarker(System.String,System.UInt16,Unity.Profiling.LowLevel.MarkerFlags,System.Int32)", Prof_CreateMarker);
  regCam(impl);
  REG("UnityEngine.TextAsset::get_bytes()", TA_get_bytes);
  REG("UnityEngine.GameObject::Internal_CreateGameObject(UnityEngine.GameObject,System.String)", GO_Create);
  REG("UnityEngine.GameObject::Internal_AddComponentWithType(System.Type)", GO_AddComponentWithType);
  REG("UnityEngine.GameObject::SetActive(System.Boolean)", GO_SetActive);
  REG("UnityEngine.GameObject::SendMessage(System.String,System.Object,UnityEngine.SendMessageOptions)", GO_SendMessage);
  REG("UnityEngine.Component::get_gameObject()", Comp_gameObject);
  REG("UnityEngine.Component::get_transform()", Comp_transform);
  REG("UnityEngine.Component::GetComponent(System.String)", Comp_GetComponent);
  REG("UnityEngine.Component::GetComponentFastPath(System.Type,System.IntPtr)", Comp_GetComponentFastPath);
  REG("UnityEngine.Object::GetName(UnityEngine.Object)", Obj_GetName);
  REG("UnityEngine.Object::Destroy(UnityEngine.Object,System.Single)", Obj_Destroy);
  REG("UnityEngine.Object::DestroyImmediate(UnityEngine.Object,System.Boolean)", Obj_DestroyImmediate);
  REG("UnityEngine.Object::DontDestroyOnLoad(UnityEngine.Object)", Obj_DontDestroyOnLoad);
  REG("UnityEngine.Object::ToString(UnityEngine.Object)", Obj_ToString);
  REG("UnityEngine.Object::set_hideFlags(UnityEngine.HideFlags)", Obj_set_hideFlags);
  REG("UnityEngine.Object::GetOffsetOfInstanceIDInCPlusPlusObject()", Obj_OffsetOfInstanceID);
  REG("UnityEngine.MonoBehaviour::IsObjectMonoBehaviour(UnityEngine.Object)", MB_IsObjectMonoBehaviour);
  REG("UnityEngine.MonoBehaviour::get_useGUILayout()", MB_get_useGUILayout);
  REG("UnityEngine.MonoBehaviour::set_useGUILayout(System.Boolean)", MB_set_useGUILayout);
  REG("UnityEngine.Renderer::GetMaterial()", Rend_GetMaterial);
  REG("UnityEngine.Renderer::set_enabled(System.Boolean)", Rend_set_enabled);
  REG("UnityEngine.Material::CreateWithShader(UnityEngine.Material,UnityEngine.Shader)", Mat_CreateWithShader);
  REG("UnityEngine.Material::CreateWithMaterial(UnityEngine.Material,UnityEngine.Material)", Mat_CreateWithMaterial);
  REG("UnityEngine.Material::CreateWithString(UnityEngine.Material)", Mat_CreateWithString);
  REG("UnityEngine.Material::GetTextureImpl(System.Int32)", Mat_GetTextureImpl);
  REG("UnityEngine.Material::SetTextureImpl(System.Int32,UnityEngine.Texture)", Mat_SetTextureImpl);
  REG("UnityEngine.Shader::PropertyToID(System.String)", Shader_PropertyToID);
  REG("UnityEngine.Shader::TagToID(System.String)", Shader_TagToID);
  REG("UnityEngine.AudioSource::set_clip(UnityEngine.AudioClip)", Src_set_clip);
  REG("UnityEngine.AudioSource::PlayHelper(UnityEngine.AudioSource,System.UInt64)", Src_PlayHelper);
  REG("UnityEngine.AudioSource::Stop(System.Boolean)", Src_Stop);
  REG("UnityEngine.AudioSource::Pause()", Src_Pause);
  REG("UnityEngine.AudioSource::UnPause()", Src_UnPause);
  REG("UnityEngine.AudioSource::get_isPlaying()", Src_isPlaying);
  REG("UnityEngine.AudioSource::get_loop()", Src_get_loop);
  REG("UnityEngine.AudioSource::set_loop(System.Boolean)", Src_set_loop);
  REG("UnityEngine.AudioSource::set_volume(System.Single)", Src_set_volume);
  REG("UnityEngine.AudioSource::get_time()", Src_get_time);
  REG("UnityEngine.AudioSource::get_timeSamples()", Src_get_timeSamples);
  REG("UnityEngine.AudioSource::get_outputAudioMixerGroup()", Src_get_group);
  REG("UnityEngine.AudioSource::set_outputAudioMixerGroup(UnityEngine.Audio.AudioMixerGroup)", Src_set_group);
  REG("UnityEngine.Audio.AudioMixer::FindMatchingGroups(System.String)", Mix_FindMatchingGroups);
  REG("UnityEngine.Audio.AudioMixer::SetFloat(System.String,System.Single)", Mix_SetFloat);
}





