// Texture2D / RenderTexture / Mesh / CommandBuffer / MaterialPropertyBlock / Graphics icalls + tiny GL renderer.
#include "minu.h"
#include "minu_objs.h"
#include "gfx.h"
#include "splash_data.h"
#include <SDL.h>
#ifdef __SWITCH__
#include <switch.h>
#endif
#include <cstdio>
#include <cstring>
#include <cmath>
#include <unordered_map>
#include <algorithm>

#define ICALL(ret, name, ...) static ret name(__VA_ARGS__)
#ifdef RT_FULL
#include "rt_shim.h"
#define REG(str, fn) impl[str] = makeShim(&fn, str)
#else
#define REG(str, fn) impl[str] = (void*)&fn
#endif
static NObj* N(void* m) { return nativeOf(m); }

// ---- GL loader -----------------------------------------------------------------------------------------------
#define GLF(X) \
  X(void, glClearColor, float, float, float, float) X(void, glClear, unsigned) X(void, glViewport, int, int, int, int) \
  X(void, glEnable, unsigned) X(void, glDisable, unsigned) X(void, glBlendFunc, unsigned, unsigned) \
  X(void, glGenTextures, int, unsigned*) X(void, glBindTexture, unsigned, unsigned) X(void, glDeleteTextures, int, const unsigned*) \
  X(void, glTexImage2D, unsigned, int, int, int, int, int, unsigned, unsigned, const void*) \
  X(void, glTexParameteri, unsigned, unsigned, int) X(void, glPixelStorei, unsigned, int) \
  X(void, glActiveTexture, unsigned) X(void, glReadPixels, int, int, int, int, unsigned, unsigned, void*) \
  X(void, glDrawElements, unsigned, int, unsigned, const void*) X(void, glScissor, int, int, int, int) \
  X(unsigned, glCreateShader, unsigned) X(void, glShaderSource, unsigned, int, const char* const*, const int*) \
  X(void, glCompileShader, unsigned) X(void, glGetShaderiv, unsigned, unsigned, int*) X(void, glGetShaderInfoLog, unsigned, int, int*, char*) \
  X(unsigned, glCreateProgram) X(void, glAttachShader, unsigned, unsigned) X(void, glLinkProgram, unsigned) \
  X(void, glGetProgramiv, unsigned, unsigned, int*) X(void, glGetProgramInfoLog, unsigned, int, int*, char*) \
  X(void, glUseProgram, unsigned) X(int, glGetUniformLocation, unsigned, const char*) X(int, glGetAttribLocation, unsigned, const char*) \
  X(void, glUniformMatrix4fv, int, int, unsigned char, const float*) X(void, glUniform1i, int, int) X(void, glUniform4f, int, float, float, float, float) \
  X(void, glGenBuffers, int, unsigned*) X(void, glBindBuffer, unsigned, unsigned) X(void, glBufferData, unsigned, ptrdiff_t, const void*, unsigned) \
  X(void, glEnableVertexAttribArray, unsigned) X(void, glDisableVertexAttribArray, unsigned) \
  X(void, glVertexAttribPointer, unsigned, int, unsigned, unsigned char, int, const void*) \
  X(void, glGenFramebuffers, int, unsigned*) X(void, glBindFramebuffer, unsigned, unsigned) \
  X(void, glFramebufferTexture2D, unsigned, unsigned, unsigned, unsigned, int) X(unsigned, glCheckFramebufferStatus, unsigned) \
  X(void, glBlitFramebuffer, int, int, int, int, int, int, int, int, unsigned, unsigned)
#define X(R, N, ...) static R(*N)(__VA_ARGS__) = nullptr;
GLF(X)
#undef X
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_TEXTURE_2D 0x0DE1
#define GL_BLEND 0x0BE2
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_RGBA 0x1908
#define GL_BGRA 0x80E1
#define GL_UNSIGNED_BYTE 0x1401
#define GL_FLOAT 0x1406
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_REPEAT 0x2901
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_STREAM_DRAW 0x88E0
#define GL_FRAMEBUFFER 0x8D40
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_TRIANGLES 4
#define GL_UNSIGNED_SHORT 0x1403
#define GL_UNSIGNED_INT 0x1405
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_SCISSOR_TEST 0x0C11
#define GL_DEPTH_TEST 0x0B71
#define GL_CULL_FACE 0x0B44

// ---- native objects ---------------------------------------------------------------------------------------------
struct NTex : NObj { int w = 0, h = 0, fmt = 0; std::vector<uint8_t> px; unsigned gl = 0; bool dirty = true; bool dead = false; int filter = 1, wrap = 1; };
struct NTexture2D : NTex { NTexture2D() { type = N_Texture2D; } };
struct NRT : NTex { unsigned fbo = 0; int gw = 0, gh = 0; NRT() { type = N_RenderTexture; w = 256; h = 256; } };
struct VAttr { int attr, fmt, dim, stream; };
struct NSub { int topo, indexStart, indexCount, baseVertex; };
struct NMeshO : NObj {
  std::vector<VAttr> attrs; std::vector<uint8_t> vb, ib; int stride = 0; int indexFmt = 0; std::vector<NSub> subs;
  NMeshO() { type = N_Mesh; }
};
struct Op { int kind; int a = 0; float m[16]; NMeshO* mesh = nullptr; NMaterial* mat = nullptr; int sub = 0; std::map<int, NObj*> mpb; float col[4] = {0, 0, 0, 1}; NTex* rt = nullptr; };
struct NCmdO : NObj { std::vector<Op> ops; NCmdO() { type = N_Cmd; } };
struct NBlockO : NObj { std::map<int, NObj*> tex; NBlockO() { type = N_Block; } };


// ---- GL state ---------------------------------------------------------------------------------------------------
static SDL_Window* g_win = nullptr;
static SDL_GLContext g_ctx = nullptr;
static unsigned g_prog = 0, g_vbo = 0, g_ibo = 0, g_blitProg = 0;
static int g_uVP, g_uTex, g_aPos, g_aCol, g_aUV;
static int g_winW = 1280, g_winH = 720;
static int g_curW = 1280, g_curH = 720;
static bool g_curIsScreen = true;
static float g_view[16], g_proj[16];

static unsigned mkShader(unsigned type, const char* src) {
  unsigned s = glCreateShader(type);
  glShaderSource(s, 1, &src, nullptr); glCompileShader(s);
  int ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) { char b[1024]; glGetShaderInfoLog(s, 1024, nullptr, b); printf("[gl] shader error: %s\n", b); }
  return s;
}
static unsigned mkProg(const char* vs, const char* fs) {
  unsigned p = glCreateProgram();
  glAttachShader(p, mkShader(GL_VERTEX_SHADER, vs)); glAttachShader(p, mkShader(GL_FRAGMENT_SHADER, fs));
  glLinkProgram(p);
  int ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok) { char b[1024]; glGetProgramInfoLog(p, 1024, nullptr, b); printf("[gl] link error: %s\n", b); }
  return p;
}

bool gfxInit(int w, int h, bool visible) {
  g_winW = w; g_winH = h;
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) != 0) { printf("SDL_Init: %s\n", SDL_GetError()); return false; }
#ifdef GFX_ES
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
  g_win = SDL_CreateWindow("Papers, Please", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h, SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN);
#else
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
  g_win = SDL_CreateWindow("Papers, Please", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | (visible ? 0 : SDL_WINDOW_HIDDEN));
#endif
  if (!g_win) { printf("SDL_CreateWindow: %s\n", SDL_GetError()); return false; }
  g_ctx = SDL_GL_CreateContext(g_win);
  if (!g_ctx) { printf("SDL_GL_CreateContext: %s\n", SDL_GetError()); return false; }
  SDL_GL_SetSwapInterval(1);
#define X(R, N, ...) N = (R(*)(__VA_ARGS__))SDL_GL_GetProcAddress(#N); if (!N) printf("[gl] missing %s\n", #N);
  GLF(X)
#undef X
#ifdef GFX_ES
#define GLSLV ""
#define GLSLF "precision mediump float;\n"
#else
#define GLSLV "#version 120\n"
#define GLSLF "#version 120\n"
#endif
  const char* vs =
      GLSLV "attribute vec3 aPos; attribute vec4 aCol; attribute vec2 aUV; uniform mat4 uVP; varying vec4 vCol; varying vec2 vUV;\n"
      "void main(){ gl_Position = uVP * vec4(aPos,1.0); vCol = aCol; vUV = aUV; }\n";
  const char* fs =
      GLSLF "varying vec4 vCol; varying vec2 vUV; uniform sampler2D uTex;\n"
      "void main(){ gl_FragColor = texture2D(uTex, vUV) * vCol; }\n";
  g_prog = mkProg(vs, fs);
  g_uVP = glGetUniformLocation(g_prog, "uVP"); g_uTex = glGetUniformLocation(g_prog, "uTex");
  g_aPos = glGetAttribLocation(g_prog, "aPos"); g_aCol = glGetAttribLocation(g_prog, "aCol"); g_aUV = glGetAttribLocation(g_prog, "aUV");
  const char* bvs = GLSLV "attribute vec2 p; varying vec2 uv; void main(){ uv = p*0.5+0.5; gl_Position = vec4(p,0.0,1.0); }\n";
  const char* bfs = GLSLF "varying vec2 uv; uniform sampler2D t; void main(){ gl_FragColor = vec4(texture2D(t, uv).rgb, 1.0); }\n";
  g_blitProg = mkProg(bvs, bfs);
  glGenBuffers(1, &g_vbo); glGenBuffers(1, &g_ibo);
  printf("[gfx] GL ready\n");
  return true;
}

// Per-frame textures (2 x ~250 KB) are created and destroyed every frame: recycle their buffers, otherwise the newlib arena fragments and grows without bound.
static std::vector<std::vector<uint8_t>> g_pxPool;
static void takePx(std::vector<uint8_t>& dst, size_t n) {
  for (size_t i = 0; i < g_pxPool.size(); i++) if (g_pxPool[i].size() == n) { dst.swap(g_pxPool[i]); g_pxPool.erase(g_pxPool.begin() + i); std::fill(dst.begin(), dst.end(), 0); return; }
  dst.assign(n, 0);
}
static void givePx(std::vector<uint8_t>& src) {
  if (src.empty()) return;
  if (g_pxPool.size() >= 12) { std::vector<uint8_t>().swap(src); return; }
  g_pxPool.emplace_back(); g_pxPool.back().swap(src);
}
static void ensureGL(NTex* t) {
  if (!t->gl) { glGenTextures(1, &t->gl); t->dirty = true; }
}
static void uploadTex(NTex* t) {
  if (t->dead || !t->dirty || t->type == N_RenderTexture) return;
  ensureGL(t);
  glBindTexture(GL_TEXTURE_2D, t->gl);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  unsigned fmt = GL_RGBA;
  const void* data = t->px.empty() ? nullptr : t->px.data();
  static std::vector<uint8_t> tmp;
  if (t->fmt == 59 || t->fmt == 57) {
#ifdef GFX_ES
    tmp.assign(t->px.begin(), t->px.end());
    for (size_t i = 0; i + 3 < tmp.size(); i += 4) std::swap(tmp[i], tmp[i + 2]);
    data = tmp.data();
#else
    fmt = GL_BGRA;
#endif
  }
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, t->w, t->h, 0, fmt, GL_UNSIGNED_BYTE, data);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, t->filter ? GL_LINEAR : GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, t->filter ? GL_LINEAR : GL_NEAREST);
#ifdef GFX_ES
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);   // GLES2: NPOT textures must clamp
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
#else
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, t->wrap ? GL_CLAMP_TO_EDGE : GL_REPEAT);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, t->wrap ? GL_CLAMP_TO_EDGE : GL_REPEAT);
#endif
  t->dirty = false;
}
// Object.Destroy / DestroyImmediate: release the heavy payload (the small native shell stays valid for stale references)
void gfxReleaseNative(NObj* n) {
  if (!n) return;
  if (n->type == N_Texture2D) {
    auto* t = (NTex*)n;
    if (t->gl) { glDeleteTextures(1, &t->gl); t->gl = 0; }
    givePx(t->px);
    t->dirty = false; t->dead = true;
  } else if (n->type == N_Mesh) {
    auto* m = (NMeshO*)n;
    std::vector<uint8_t>().swap(m->vb); std::vector<uint8_t>().swap(m->ib);
  }
}
static void ensureRT(NRT* r) {
  if (r->fbo && r->gw == r->w && r->gh == r->h) return;
  ensureGL(r);
  glBindTexture(GL_TEXTURE_2D, r->gl);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, r->w, r->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, r->filter ? GL_LINEAR : GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, r->filter ? GL_LINEAR : GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  if (!r->fbo) glGenFramebuffers(1, &r->fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, r->fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, r->gl, 0);
  r->gw = r->w; r->gh = r->h;
}

// ---- command execution ---------------------------------------------------------------------------------------------
static void bindTarget(NTex* rt) {
  if (rt && rt->type == N_RenderTexture) {
    auto* r = (NRT*)rt; ensureRT(r);
    glBindFramebuffer(GL_FRAMEBUFFER, r->fbo); glViewport(0, 0, r->w, r->h); g_curW = r->w; g_curH = r->h; g_curIsScreen = false;
  } else {
    glBindFramebuffer(GL_FRAMEBUFFER, 0); glViewport(0, 0, g_winW, g_winH); g_curW = g_winW; g_curH = g_winH; g_curIsScreen = true;
  }
}
static void mulMat(const float* a, const float* b, float* o) {   // column-major o = a*b
  for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) {
    float s = 0; for (int k = 0; k < 4; k++) s += a[k * 4 + r] * b[c * 4 + k];
    o[c * 4 + r] = s;
  }
}
static int attribSize(int fmt) { static const int s[] = {4, 2, 1, 1, 2, 2, 1, 1, 2, 2, 4, 4}; return s[fmt]; }
static unsigned attribGLType(int fmt) {
  switch (fmt) { case 0: return GL_FLOAT; case 2: case 6: return GL_UNSIGNED_BYTE; case 4: case 8: return GL_UNSIGNED_SHORT; default: return GL_FLOAT; }
}
static void drawOp(const Op& op) {
  NMeshO* m = op.mesh; if (!m || op.sub >= (int)m->subs.size()) return;
  const NSub& sub = m->subs[op.sub];
  NObj* tex = nullptr;
  for (auto& kv : op.mpb) if (kv.second) { tex = kv.second; break; }
  if (!tex && op.mat) for (auto& kv : op.mat->tex) if (kv.second) { tex = kv.second; break; }
  glUseProgram(g_prog);
  float mv[16], vp[16], mvp[16];
  mulMat(g_view, op.m, mv); mulMat(g_proj, mv, mvp); (void)vp;
  glUniformMatrix4fv(g_uVP, 1, 0, mvp);
  glActiveTexture(0x84C0); glBindTexture(GL_TEXTURE_2D, 0);
  if (tex && (tex->type == N_Texture2D || tex->type == N_RenderTexture)) {
    auto* t = (NTex*)tex;
    if (t->type == N_Texture2D) uploadTex(t);
    glBindTexture(GL_TEXTURE_2D, t->gl);
  }
  glUniform1i(g_uTex, 0);
  glBindBuffer(GL_ARRAY_BUFFER, g_vbo); glBufferData(GL_ARRAY_BUFFER, m->vb.size(), m->vb.data(), GL_STREAM_DRAW);
  int off = 0;
  glDisableVertexAttribArray(g_aPos); glDisableVertexAttribArray(g_aCol); glDisableVertexAttribArray(g_aUV);
  for (auto& a : m->attrs) {
    int loc = a.attr == 0 ? g_aPos : a.attr == 3 ? g_aCol : a.attr == 4 ? g_aUV : -1;
    if (loc >= 0) {
      bool norm = a.fmt == 2 || a.fmt == 4;
      glEnableVertexAttribArray(loc);
      glVertexAttribPointer(loc, a.dim, attribGLType(a.fmt), norm, m->stride, (const void*)(intptr_t)(off + sub.baseVertex * m->stride));
    }
    off += attribSize(a.fmt) * a.dim;
  }
  bool u32 = m->indexFmt == 1; int es = u32 ? 4 : 2;
  std::vector<uint16_t> qi16; std::vector<uint32_t> qi32;
  const uint8_t* idx = m->ib.data() + sub.indexStart * es; int cnt = sub.indexCount;
  if (sub.topo == 2) {   // quads -> triangles
    int nq = cnt / 4;
    if (u32) { qi32.reserve(nq * 6); for (int q = 0; q < nq; q++) { const uint32_t* s = (const uint32_t*)idx + q * 4; uint32_t t[6] = {s[0], s[1], s[2], s[0], s[2], s[3]}; qi32.insert(qi32.end(), t, t + 6); } idx = (const uint8_t*)qi32.data(); }
    else { qi16.reserve(nq * 6); for (int q = 0; q < nq; q++) { const uint16_t* s = (const uint16_t*)idx + q * 4; uint16_t t[6] = {s[0], s[1], s[2], s[0], s[2], s[3]}; qi16.insert(qi16.end(), t, t + 6); } idx = (const uint8_t*)qi16.data(); }
    cnt = nq * 6;
  }
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_ibo); glBufferData(GL_ELEMENT_ARRAY_BUFFER, cnt * es, idx, GL_STREAM_DRAW);
  glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);
  glDrawElements(GL_TRIANGLES, cnt, u32 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT, nullptr);
}
static int g_execCount = 0;
static void execute(NCmdO* c) {
  if (g_execCount < 6 || (g_execCount % 60) == 0) {
    int k[8] = {0}; int draws = 0;
    for (auto& op : c->ops) { k[op.kind]++; }
    printf("[gfx] exec #%d ops=%zu (rt %d clear %d view %d proj %d draw %d)\n", g_execCount, c->ops.size(), k[1], k[2], k[3], k[4], k[5]); (void)draws;
    for (auto& op : c->ops) if (op.kind == 5 && g_execCount < 3) {
      printf("   draw mesh vb=%zu ib=%zu stride=%d subs=%zu sub=%d", op.mesh ? op.mesh->vb.size() : 0, op.mesh ? op.mesh->ib.size() : 0, op.mesh ? op.mesh->stride : 0, op.mesh ? op.mesh->subs.size() : 0, op.sub);
      if (op.mesh && op.sub < (int)op.mesh->subs.size()) printf(" topo=%d idx=%d..+%d", op.mesh->subs[op.sub].topo, op.mesh->subs[op.sub].indexStart, op.mesh->subs[op.sub].indexCount);
      NObj* t = nullptr; for (auto& kv : op.mpb) if (kv.second) t = kv.second; printf(" tex=%s", t ? (std::to_string(((NTex*)t)->w) + "x" + std::to_string(((NTex*)t)->h)).c_str() : "none"); printf("\n");
    }
  }
  if (g_execCount == 100) {
    for (auto& op : c->ops) if (op.kind == 5 && op.mesh && op.sub < (int)op.mesh->subs.size()) {
      const NSub& sb = op.mesh->subs[op.sub]; const uint16_t* ix = (const uint16_t*)op.mesh->ib.data() + sb.indexStart;
      float mnx = 1e9, mxx = -1e9, mny = 1e9, mxy = -1e9; float ca = 0;
      for (int i = 0; i < sb.indexCount; i++) { const float* v = (const float*)(op.mesh->vb.data() + (size_t)ix[i] * op.mesh->stride); mnx = fminf(mnx, v[0]); mxx = fmaxf(mxx, v[0]); mny = fminf(mny, v[1]); mxy = fmaxf(mxy, v[1]); ca = v[6]; }
      printf("   sub %d: x %g..%g y %g..%g alpha %g\n", op.sub, mnx, mxx, mny, mxy, ca);
    }
  }
  if (g_execCount == 0) {
    for (auto& op : c->ops) {
      if (op.kind == 3) printf("   view: %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g\n", op.m[0],op.m[1],op.m[2],op.m[3],op.m[4],op.m[5],op.m[6],op.m[7],op.m[8],op.m[9],op.m[10],op.m[11],op.m[12],op.m[13],op.m[14],op.m[15]);
      if (op.kind == 4) printf("   proj: %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g\n", op.m[0],op.m[1],op.m[2],op.m[3],op.m[4],op.m[5],op.m[6],op.m[7],op.m[8],op.m[9],op.m[10],op.m[11],op.m[12],op.m[13],op.m[14],op.m[15]);
      if (op.kind == 2) printf("   clear: %g %g %g %g\n", op.col[0], op.col[1], op.col[2], op.col[3]);
      if (op.kind == 5 && op.sub == 1) {
        const float* v = (const float*)op.mesh->vb.data();
        printf("   v0: pos %g %g %g col %g %g %g %g uv %g %g\n", v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8]);
        const float* w = v + 9; printf("   v1: pos %g %g %g col %g %g %g %g uv %g %g\n", w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8]);
        const float* mm = op.m; printf("   model: %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g\n", mm[0],mm[1],mm[2],mm[3],mm[4],mm[5],mm[6],mm[7],mm[8],mm[9],mm[10],mm[11],mm[12],mm[13],mm[14],mm[15]);
        NObj* t = nullptr; for (auto& kv : op.mpb) if (kv.second) t = kv.second;
        if (t) { auto* tx = (NTex*)t; int nz = 0; for (size_t i = 0; i < tx->px.size(); i++) if (tx->px[i]) nz++; printf("   tex fmt=%d bytes=%zu nonzero=%d first=%02x%02x%02x%02x\n", tx->fmt, tx->px.size(), nz, tx->px.size()>3?tx->px[0]:0, tx->px.size()>3?tx->px[1]:0, tx->px.size()>3?tx->px[2]:0, tx->px.size()>3?tx->px[3]:0); }
      }
    }
  }
  g_execCount++;
  for (auto& op : c->ops) {
    switch (op.kind) {
      case 1: bindTarget(op.rt); break;
      case 2: glClearColor(op.col[0], op.col[1], op.col[2], op.col[3]); glClear(GL_COLOR_BUFFER_BIT); break;
      case 3: memcpy(g_view, op.m, 64); break;
      case 4: memcpy(g_proj, op.m, 64); break;
      case 5: drawOp(op); break;
    }
  }
}

// blit a render texture to the window (letterboxed) and swap
void gfxPresent(void* managedRT) {
  NRT* r = (NRT*)nativeOf(managedRT);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  int ww, wh; SDL_GL_GetDrawableSize(g_win, &ww, &wh); g_winW = ww; g_winH = wh;
  glViewport(0, 0, ww, wh); glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
  if (r && r->gl) {
    float sx = (float)ww / r->w, sy = (float)wh / r->h, s = sx < sy ? sx : sy;
    int vw = (int)(r->w * s), vh = (int)(r->h * s);
    glViewport((ww - vw) / 2, (wh - vh) / 2, vw, vh);
    glUseProgram(g_blitProg); glActiveTexture(0x84C0); glBindTexture(GL_TEXTURE_2D, r->gl);
    glUniform1i(glGetUniformLocation(g_blitProg, "t"), 0);
    static unsigned qb = 0; if (!qb) glGenBuffers(1, &qb);
    float q[8] = {-1, -1, 1, -1, 1, 1, -1, 1}; unsigned short qi[6] = {0, 1, 2, 0, 2, 3};
    glBindBuffer(GL_ARRAY_BUFFER, qb); glBufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_STREAM_DRAW);
    int pl = glGetAttribLocation(g_blitProg, "p"); glEnableVertexAttribArray(pl); glVertexAttribPointer(pl, 2, GL_FLOAT, 0, 8, nullptr);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0); glDisable(GL_BLEND);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, qi);
  }
  SDL_GL_SwapWindow(g_win);
}

// Startup splash (the Vita shows its LiveArea startup image, the "ENTRY GRANTED" stamp on black, while it loads): drawn right after
// the GL context exists, before the multi-second guest startup; both buffers get it so it stays up with no frames being presented.
void gfxSplash() {
  if (!g_win || !g_blitProg) return;
  std::vector<uint8_t> px((size_t)kSplashW * kSplashH * 4);
  for (int y = 0; y < kSplashH; y++) for (int x = 0; x < kSplashW; x++) {   // GL rows start at the bottom
    const unsigned char* c = kSplashPal[kSplashPx[(size_t)y * kSplashW + x]]; memcpy(&px[((size_t)(kSplashH - 1 - y) * kSplashW + x) * 4], c, 4);
  }
  unsigned tex = 0; glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kSplashW, kSplashH, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F);
  int ww, wh; SDL_GL_GetDrawableSize(g_win, &ww, &wh);
  float sc = 2.0f * ww / 1280.f; int vw = (int)(kSplashW * sc), vh = (int)(kSplashH * sc);   // 2x of a 1280x720 screen: about the stamp size the Vita shows
  for (int b = 0; b < 2; b++) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0); glViewport(0, 0, ww, wh); glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
    glViewport((ww - vw) / 2, (wh - vh) / 2, vw, vh);
    glUseProgram(g_blitProg); glActiveTexture(0x84C0); glBindTexture(GL_TEXTURE_2D, tex);
    glUniform1i(glGetUniformLocation(g_blitProg, "t"), 0);
    unsigned qb = 0; glGenBuffers(1, &qb);
    float q[8] = {-1, -1, 1, -1, 1, 1, -1, 1}; unsigned short qi[6] = {0, 1, 2, 0, 2, 3};
    glBindBuffer(GL_ARRAY_BUFFER, qb); glBufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_STREAM_DRAW);
    int pl = glGetAttribLocation(g_blitProg, "p"); glEnableVertexAttribArray(pl); glVertexAttribPointer(pl, 2, GL_FLOAT, 0, 8, nullptr);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0); glDisable(GL_BLEND);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, qi);
    SDL_GL_SwapWindow(g_win);
  }
  glDeleteTextures(1, &tex);
}
bool gfxScreenshot(void* managedRT, const char* path) {
  NRT* r = (NRT*)nativeOf(managedRT);
  if (!r || !r->fbo) return false;
  std::vector<uint8_t> px(r->w * r->h * 4);
  glBindFramebuffer(GL_FRAMEBUFFER, r->fbo);
  glReadPixels(0, 0, r->w, r->h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
  // write TGA (bottom-up -> flip to top-down via descriptor bit)
  FILE* f = fopen(path, "wb"); if (!f) return false;
  uint8_t hdr[18] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, (uint8_t)(r->w & 255), (uint8_t)(r->w >> 8), (uint8_t)(r->h & 255), (uint8_t)(r->h >> 8), 32, 0};
  fwrite(hdr, 1, 18, f);
  for (int i = 0; i < r->w * r->h; i++) { uint8_t p[4] = {px[i * 4 + 2], px[i * 4 + 1], px[i * 4], px[i * 4 + 3]}; fwrite(p, 1, 4, f); }
  fclose(f);
  return true;
}

// ---- icalls ----------------------------------------------------------------------------------------------------------
long g_t2dCreated = 0; double g_t2dBytes = 0;
ICALL(bool, T2D_Create, Il2CppObject* self, int w, int h, int mips, int fmt, int flags, void* nativeTex) {
  (void)mips; (void)flags; (void)nativeTex;
  g_t2dCreated++; g_t2dBytes += (double)w * h * 4;
  { static int lg = getenv("RT_TEXLOG") ? 1 : 0; if (lg) printf("[tex] create %dx%d fmt %d\n", w, h, fmt); }
  auto* t = new NTexture2D(); t->w = w; t->h = h; t->fmt = fmt; takePx(t->px, (size_t)w * h * 4);
  bindNative(self, t); registerNative(t); return true;
}
ICALL(void, T2D_SetPixelDataArray, Il2CppObject* self, Il2CppArray* data, int mip, int elemSize, int dataArraySize, int srcStart) {
  auto* t = (NTexture2D*)N(self); (void)mip; (void)dataArraySize; (void)srcStart;
  if (t->dead) return;
  size_t bytes = (size_t)elemSize * data->max_length;
  if (srcStart) bytes = (size_t)elemSize * (data->max_length - srcStart);
  size_t want = (size_t)t->w * t->h * 4;
  if (t->px.size() != want) t->px.assign(want, 0);
  memcpy(t->px.data(), (char*)data + 0x20 + (size_t)srcStart * elemSize, bytes < want ? bytes : want);
  t->dirty = true;
}
ICALL(void, T2D_Apply, Il2CppObject* self, bool, bool) { ((NTexture2D*)N(self))->dirty = true; }
ICALL(bool, T_isReadable, Il2CppObject*) { return true; }
ICALL(int, T_GetDataWidth, Il2CppObject* self) { return ((NTex*)N(self))->w; }
ICALL(int, T_GetDataHeight, Il2CppObject* self) { return ((NTex*)N(self))->h; }
ICALL(void, T_set_filterMode, Il2CppObject* self, int m) { auto* t = (NTex*)N(self); t->filter = m != 0; t->dirty = true; }
ICALL(void, T_set_wrapMode, Il2CppObject* self, int m) { auto* t = (NTex*)N(self); t->wrap = m == 1 ? 0 : 1; t->dirty = true; }

ICALL(void, RT_Create, Il2CppObject* self) { auto* r = new NRT(); bindNative(self, r); registerNative(r); }
ICALL(int, RT_width, Il2CppObject* self) { return ((NRT*)N(self))->w; }
ICALL(int, RT_height, Il2CppObject* self) { return ((NRT*)N(self))->h; }
ICALL(void, RT_set_width, Il2CppObject* self, int v) { ((NRT*)N(self))->w = v; }
ICALL(void, RT_set_height, Il2CppObject* self, int v) { ((NRT*)N(self))->h = v; }
ICALL(void, RT_set_depth, Il2CppObject*, int) {}
ICALL(void, RT_set_graphicsFormat, Il2CppObject*, int) {}
ICALL(void, RT_SetSRGB, Il2CppObject*, bool) {}
ICALL(void, RT_SetDescriptor, Il2CppObject* self, int* d) { auto* r = (NRT*)N(self); r->w = d[0]; r->h = d[1]; }
ICALL(void, RT_GetDescriptor, Il2CppObject* self, int* d) { auto* r = (NRT*)N(self); memset(d, 0, 0x40); d[0] = r->w; d[1] = r->h; d[2] = 1; d[3] = 1; }
ICALL(void, RT_GetBuffer, Il2CppObject* self, void* rb) { (void)self; memset(rb, 0, 16); }

ICALL(void, Mesh_Create, Il2CppObject* self) { auto* m = new NMeshO(); bindNative(self, m); registerNative(m); }
ICALL(void, Mesh_MarkDynamic, Il2CppObject*) {}
ICALL(bool, Mesh_canAccess, Il2CppObject*) { return true; }
ICALL(int, Mesh_get_subMeshCount, Il2CppObject* self) { return (int)((NMeshO*)N(self))->subs.size(); }
ICALL(void, Mesh_set_subMeshCount, Il2CppObject* self, int n) { ((NMeshO*)N(self))->subs.resize(n); }
ICALL(void, Mesh_Upload, Il2CppObject*, bool) {}
ICALL(void, Mesh_SetVBParams, Il2CppObject* self, int vcount, Il2CppArray* attrs) {
  auto* m = (NMeshO*)N(self); m->attrs.clear(); m->stride = 0;
  int n = (int)attrs->max_length; const int* d = (const int*)((char*)attrs + 0x20);
  for (int i = 0; i < n; i++) { VAttr a = {d[i * 4], d[i * 4 + 1], d[i * 4 + 2], d[i * 4 + 3]}; m->attrs.push_back(a); m->stride += attribSize(a.fmt) * a.dim; }
  m->vb.resize((size_t)vcount * m->stride);
}
ICALL(void, Mesh_SetIBParams, Il2CppObject* self, int count, int fmt) { auto* m = (NMeshO*)N(self); m->indexFmt = fmt; m->ib.resize((size_t)count * (fmt ? 4 : 2)); }
ICALL(void, Mesh_SetVBData, Il2CppObject* self, int stream, Il2CppArray* data, int dataStart, int bufStart, int count, int elemSize, int flags) {
  auto* m = (NMeshO*)N(self); (void)stream; (void)flags;
  size_t off = (size_t)bufStart * elemSize, n = (size_t)count * elemSize;
  if (off + n > m->vb.size()) m->vb.resize(off + n);
  memcpy(m->vb.data() + off, (char*)data + 0x20 + (size_t)dataStart * elemSize, n);
}
ICALL(void, Mesh_SetIBData, Il2CppObject* self, Il2CppArray* data, int dataStart, int bufStart, int count, int elemSize, int flags) {
  auto* m = (NMeshO*)N(self); (void)flags;
  size_t off = (size_t)bufStart * elemSize, n = (size_t)count * elemSize;
  if (off + n > m->ib.size()) m->ib.resize(off + n);
  memcpy(m->ib.data() + off, (char*)data + 0x20 + (size_t)dataStart * elemSize, n);
}
ICALL(void, Mesh_SetSubmeshes, Il2CppObject* self, Il2CppArray* descs, int start, int count, int flags) {
  auto* m = (NMeshO*)N(self); (void)flags;
  if ((int)m->subs.size() < count) m->subs.resize(count);
  const char* d = (const char*)descs + 0x20;
  for (int i = 0; i < count; i++) {
    const char* e = d + (size_t)(start + i) * 0x30;
    m->subs[i] = {*(int*)(e + 0x18), *(int*)(e + 0x1C), *(int*)(e + 0x20), *(int*)(e + 0x24)};
  }
}

ICALL(void*, Cmd_Init) { auto* c = new NCmdO(); registerNative(c); return c; }
ICALL(void, Cmd_Release, Il2CppObject*) {}
ICALL(void, Cmd_Clear, Il2CppObject* self) { ((NCmdO*)N(self))->ops.clear(); }
ICALL(void, Cmd_ClearRT, Il2CppObject* self, bool depth, bool color, float* col, float d) {
  (void)depth; (void)color; (void)d; Op o; o.kind = 2; memcpy(o.col, col, 16); ((NCmdO*)N(self))->ops.push_back(o);
}
ICALL(void, Cmd_SetView, Il2CppObject* self, float* m) { Op o; o.kind = 3; memcpy(o.m, m, 64); ((NCmdO*)N(self))->ops.push_back(o); }
ICALL(void, Cmd_SetProj, Il2CppObject* self, float* m) { Op o; o.kind = 4; memcpy(o.m, m, 64); ((NCmdO*)N(self))->ops.push_back(o); }
ICALL(void, Cmd_SetRT, Il2CppObject* self, int* rti, int, int, int, int) {
  // RenderTargetIdentifier: { type, nameID, instanceID, ... }
  Op o; o.kind = 1; NObj* t = rti[2] ? nativeById(rti[2]) : nullptr; o.rt = (NTex*)t; ((NCmdO*)N(self))->ops.push_back(o);
}
ICALL(void, Cmd_DrawMesh, Il2CppObject* self, Il2CppObject* mesh, float* m, Il2CppObject* mat, int sub, int pass, Il2CppObject* mpb) {
  (void)pass; Op o; o.kind = 5; memcpy(o.m, m, 64); o.mesh = (NMeshO*)N(mesh); o.mat = (NMaterial*)N(mat); o.sub = sub;
  if (mpb) if (auto* b = (NBlockO*)N(mpb)) o.mpb = b->tex;
  ((NCmdO*)N(self))->ops.push_back(o);
}
ICALL(void, Cmd_Validate, Il2CppObject*, int, int) {}
ICALL(void, Gfx_Execute, Il2CppObject* cb) { execute((NCmdO*)N(cb)); }
ICALL(void, Gfx_SetNullRT) { bindTarget(nullptr); }
ICALL(void, Gfx_SetRTSimple, void*, void*, int, int, int) { bindTarget(nullptr); }
ICALL(int, Gfx_MaxInstances) { return 511; }
ICALL(void, GL_Clear, bool, bool, float* c, float) { glClearColor(c[0], c[1], c[2], c[3]); glClear(GL_COLOR_BUFFER_BIT); }
ICALL(void, GL_Proj, float* m, bool, float* o) { memcpy(o, m, 64); }
ICALL(void, GL_Viewport, float*) {}

ICALL(void*, MPB_Create) { auto* b = new NBlockO(); registerNative(b); return b; }
ICALL(void, MPB_Destroy, void*) {}
ICALL(void, MPB_Clear, Il2CppObject* self, bool) { ((NBlockO*)N(self))->tex.clear(); }
ICALL(void, MPB_SetTexture, Il2CppObject* self, int id, Il2CppObject* tex) { ((NBlockO*)N(self))->tex[id] = tex ? N(tex) : nullptr; }

// formats / system info
ICALL(bool, GFU_CanDecompress, int, bool) { return false; }
ICALL(int, GFU_FromRTF, int f, bool) { return f == 20 ? 59 : 8; }
ICALL(int, GFU_FromTF, int f, bool) { return f == 14 ? 59 : f == 1 ? 5 : 8; }
ICALL(bool, GFU_IsCompressed, int) { return false; }
ICALL(bool, GFU_IsSRGB, int f) { return f == 4 || f == 57; }
ICALL(int, SI_GetCompatibleFormat, int f, int) { return f; }
ICALL(int, SI_GetGraphicsFormat, int) { return 8; }
ICALL(bool, SI_IsFormatSupported, int, int) { return true; }
ICALL(bool, SI_SupportsTF, int) { return true; }

void minuRegisterInput(std::map<std::string, void*>& impl);
void minuRegisterGfx(std::map<std::string, void*>& impl) {
  minuRegisterInput(impl);
  REG("UnityEngine.Texture2D::Internal_CreateImpl(UnityEngine.Texture2D,System.Int32,System.Int32,System.Int32,UnityEngine.Experimental.Rendering.GraphicsFormat,UnityEngine.Experimental.Rendering.TextureCreationFlags,System.IntPtr)", T2D_Create);
  REG("UnityEngine.Texture2D::SetPixelDataImplArray(System.Array,System.Int32,System.Int32,System.Int32,System.Int32)", T2D_SetPixelDataArray);
  REG("UnityEngine.Texture2D::ApplyImpl(System.Boolean,System.Boolean)", T2D_Apply);
  REG("UnityEngine.Texture2D::get_isReadable()", T_isReadable);
  REG("UnityEngine.Texture::get_isReadable()", T_isReadable);
  REG("UnityEngine.Texture::GetDataWidth()", T_GetDataWidth);
  REG("UnityEngine.Texture::GetDataHeight()", T_GetDataHeight);
  REG("UnityEngine.Texture::set_filterMode(UnityEngine.FilterMode)", T_set_filterMode);
  REG("UnityEngine.Texture::set_wrapMode(UnityEngine.TextureWrapMode)", T_set_wrapMode);
  REG("UnityEngine.RenderTexture::Internal_Create(UnityEngine.RenderTexture)", RT_Create);
  REG("UnityEngine.RenderTexture::get_width()", RT_width);
  REG("UnityEngine.RenderTexture::get_height()", RT_height);
  REG("UnityEngine.RenderTexture::set_width(System.Int32)", RT_set_width);
  REG("UnityEngine.RenderTexture::set_height(System.Int32)", RT_set_height);
  REG("UnityEngine.RenderTexture::set_depth(System.Int32)", RT_set_depth);
  REG("UnityEngine.RenderTexture::set_graphicsFormat(UnityEngine.Experimental.Rendering.GraphicsFormat)", RT_set_graphicsFormat);
  REG("UnityEngine.RenderTexture::SetSRGBReadWrite(System.Boolean)", RT_SetSRGB);
  REG("UnityEngine.RenderTexture::SetRenderTextureDescriptor_Injected(UnityEngine.RenderTextureDescriptor&)", RT_SetDescriptor);
  REG("UnityEngine.RenderTexture::GetDescriptor_Injected(UnityEngine.RenderTextureDescriptor&)", RT_GetDescriptor);
  REG("UnityEngine.RenderTexture::GetColorBuffer_Injected(UnityEngine.RenderBuffer&)", RT_GetBuffer);
  REG("UnityEngine.RenderTexture::GetDepthBuffer_Injected(UnityEngine.RenderBuffer&)", RT_GetBuffer);
  REG("UnityEngine.Mesh::Internal_Create(UnityEngine.Mesh)", Mesh_Create);
  REG("UnityEngine.Mesh::MarkDynamicImpl()", Mesh_MarkDynamic);
  REG("UnityEngine.Mesh::get_canAccess()", Mesh_canAccess);
  REG("UnityEngine.Mesh::get_subMeshCount()", Mesh_get_subMeshCount);
  REG("UnityEngine.Mesh::set_subMeshCount(System.Int32)", Mesh_set_subMeshCount);
  REG("UnityEngine.Mesh::UploadMeshDataImpl(System.Boolean)", Mesh_Upload);
  REG("UnityEngine.Mesh::SetVertexBufferParamsFromArray(System.Int32,UnityEngine.Rendering.VertexAttributeDescriptor[])", Mesh_SetVBParams);
  REG("UnityEngine.Mesh::SetIndexBufferParams(System.Int32,UnityEngine.Rendering.IndexFormat)", Mesh_SetIBParams);
  REG("UnityEngine.Mesh::InternalSetVertexBufferDataFromArray(System.Int32,System.Array,System.Int32,System.Int32,System.Int32,System.Int32,UnityEngine.Rendering.MeshUpdateFlags)", Mesh_SetVBData);
  REG("UnityEngine.Mesh::InternalSetIndexBufferDataFromArray(System.Array,System.Int32,System.Int32,System.Int32,System.Int32,UnityEngine.Rendering.MeshUpdateFlags)", Mesh_SetIBData);
  REG("UnityEngine.Mesh::SetAllSubMeshesAtOnceFromArray(UnityEngine.Rendering.SubMeshDescriptor[],System.Int32,System.Int32,UnityEngine.Rendering.MeshUpdateFlags)", Mesh_SetSubmeshes);
  REG("UnityEngine.Rendering.CommandBuffer::InitBuffer()", Cmd_Init);
  REG("UnityEngine.Rendering.CommandBuffer::ReleaseBuffer()", Cmd_Release);
  REG("UnityEngine.Rendering.CommandBuffer::Clear()", Cmd_Clear);
  REG("UnityEngine.Rendering.CommandBuffer::ClearRenderTarget_Injected(System.Boolean,System.Boolean,UnityEngine.Color&,System.Single)", Cmd_ClearRT);
  REG("UnityEngine.Rendering.CommandBuffer::SetViewMatrix_Injected(UnityEngine.Matrix4x4&)", Cmd_SetView);
  REG("UnityEngine.Rendering.CommandBuffer::SetProjectionMatrix_Injected(UnityEngine.Matrix4x4&)", Cmd_SetProj);
  REG("UnityEngine.Rendering.CommandBuffer::SetRenderTargetSingle_Internal_Injected(UnityEngine.Rendering.RenderTargetIdentifier&,UnityEngine.Rendering.RenderBufferLoadAction,UnityEngine.Rendering.RenderBufferStoreAction,UnityEngine.Rendering.RenderBufferLoadAction,UnityEngine.Rendering.RenderBufferStoreAction)", Cmd_SetRT);
  REG("UnityEngine.Rendering.CommandBuffer::Internal_DrawMesh_Injected(UnityEngine.Mesh,UnityEngine.Matrix4x4&,UnityEngine.Material,System.Int32,System.Int32,UnityEngine.MaterialPropertyBlock)", Cmd_DrawMesh);
  REG("UnityEngine.Rendering.CommandBuffer::ValidateAgainstExecutionFlags(UnityEngine.Rendering.CommandBufferExecutionFlags,UnityEngine.Rendering.CommandBufferExecutionFlags)", Cmd_Validate);
  REG("UnityEngine.Graphics::ExecuteCommandBuffer(UnityEngine.Rendering.CommandBuffer)", Gfx_Execute);
  REG("UnityEngine.Graphics::Internal_SetNullRT()", Gfx_SetNullRT);
  REG("UnityEngine.Graphics::Internal_SetRTSimple_Injected(UnityEngine.RenderBuffer&,UnityEngine.RenderBuffer&,System.Int32,UnityEngine.CubemapFace,System.Int32)", Gfx_SetRTSimple);
  REG("UnityEngine.Graphics::Internal_GetMaxDrawMeshInstanceCount()", Gfx_MaxInstances);
  REG("UnityEngine.GL::GLClear_Injected(System.Boolean,System.Boolean,UnityEngine.Color&,System.Single)", GL_Clear);
  REG("UnityEngine.GL::GetGPUProjectionMatrix_Injected(UnityEngine.Matrix4x4&,System.Boolean,UnityEngine.Matrix4x4&)", GL_Proj);
  REG("UnityEngine.GL::Viewport_Injected(UnityEngine.Rect&)", GL_Viewport);
  REG("UnityEngine.MaterialPropertyBlock::CreateImpl()", MPB_Create);
  REG("UnityEngine.MaterialPropertyBlock::DestroyImpl(System.IntPtr)", MPB_Destroy);
  REG("UnityEngine.MaterialPropertyBlock::Clear(System.Boolean)", MPB_Clear);
  REG("UnityEngine.MaterialPropertyBlock::SetTextureImpl(System.Int32,UnityEngine.Texture)", MPB_SetTexture);
  REG("UnityEngine.Experimental.Rendering.GraphicsFormatUtility::CanDecompressFormat(UnityEngine.Experimental.Rendering.GraphicsFormat,System.Boolean)", GFU_CanDecompress);
  REG("UnityEngine.Experimental.Rendering.GraphicsFormatUtility::GetGraphicsFormat_Native_RenderTextureFormat(UnityEngine.RenderTextureFormat,System.Boolean)", GFU_FromRTF);
  REG("UnityEngine.Experimental.Rendering.GraphicsFormatUtility::GetGraphicsFormat_Native_TextureFormat(UnityEngine.TextureFormat,System.Boolean)", GFU_FromTF);
  REG("UnityEngine.Experimental.Rendering.GraphicsFormatUtility::IsCompressedTextureFormat(UnityEngine.TextureFormat)", GFU_IsCompressed);
  REG("UnityEngine.Experimental.Rendering.GraphicsFormatUtility::IsSRGBFormat(UnityEngine.Experimental.Rendering.GraphicsFormat)", GFU_IsSRGB);
  REG("UnityEngine.SystemInfo::GetCompatibleFormat(UnityEngine.Experimental.Rendering.GraphicsFormat,UnityEngine.Experimental.Rendering.FormatUsage)", SI_GetCompatibleFormat);
  REG("UnityEngine.SystemInfo::GetGraphicsFormat(UnityEngine.Experimental.Rendering.DefaultFormat)", SI_GetGraphicsFormat);
  REG("UnityEngine.SystemInfo::IsFormatSupported(UnityEngine.Experimental.Rendering.GraphicsFormat,UnityEngine.Experimental.Rendering.FormatUsage)", SI_IsFormatSupported);
  REG("UnityEngine.SystemInfo::SupportsTextureFormatNative(UnityEngine.TextureFormat)", SI_SupportsTF);
}


// ---- input (SDL) ---------------------------------------------------------------------------------------------------
static bool g_mouseNow[3], g_mousePrev[3];
static float g_mx = 0, g_my = 0;
extern int g_screenW_pub, g_screenH_pub;
static bool g_scr = false; static float g_scrX, g_scrY; static bool g_scrDown;
void gfxScriptMouse(bool active, float x, float yTop, bool down) { g_scr = active; g_scrX = x * g_screenW_pub / 570.f; g_scrY = g_screenH_pub - yTop * g_screenH_pub / 320.f; g_scrDown = down; }
// The Switch SDL build reports the face buttons by *position* (Xbox layout): SDL_A is the bottom button = Switch B, SDL_B = right = Switch A,
// SDL_X = left = Switch Y, SDL_Y = top = Switch X.  Everything below is written in Switch labels, so swap them back there.
#ifdef __SWITCH__
#define BTN_A SDL_CONTROLLER_BUTTON_B
#define BTN_B SDL_CONTROLLER_BUTTON_A
#define BTN_X SDL_CONTROLLER_BUTTON_Y
#define BTN_Y SDL_CONTROLLER_BUTTON_X
#else
#define BTN_A SDL_CONTROLLER_BUTTON_A
#define BTN_B SDL_CONTROLLER_BUTTON_B
#define BTN_X SDL_CONTROLLER_BUTTON_X
#define BTN_Y SDL_CONTROLLER_BUTTON_Y
#endif
bool gameInBooth();   // fastpath.cpp: the booth is updating = a day is being played
void padShortcut(int which);   // fastpath.cpp: 0 = booth shutter toggle, 1 = call next traveler
static SDL_GameController* g_pad = nullptr;
static float g_padX = 640, g_padY = 360;
static bool g_padCancel = false;
// Cursor visibility (the game draws its own cursor in play.ui.CursorEnt.draw, hooked in fastpath.cpp): shown while a stick / D-pad / A / mouse is used,
// hidden by touch or by the shortcut buttons (X Y L R B +), and after kCursorIdle seconds without use.
static bool g_curShow = false; static float g_curIdle = 99.f; static const float kCursorIdle = 2.5f;
bool gfxCursorVisible() { return g_curShow; }
static bool g_padA = false, g_padB = false, g_padStart = false, g_padXb = false, g_padYb = false;
static int g_lastMX = -1, g_lastMY = -1;
// Real touch points for Unity's Input.touchCount / GetTouch (the game's InputWrapper switches to touch mode by itself).
// A finger is reported Began for >= 1 frame, Moved/Stationary while held, Ended for 1 frame, then dropped. A tap shorter than one frame
// still produces Began then Ended on the next frame.
struct Finger { bool used = false; SDL_FingerID sid = 0; int id = 0; float x = 0, y = 0, px = 0, py = 0; int phase = 0; bool endNext = false; bool upSeen = false; };
static Finger g_fing[10]; static int g_fingNext = 0;
static bool g_touchAsMouse = true;    // this Steam build's InputWrapper only ever enters Mouse mode (Touch mode is never set), so fingers must drive the mouse. RT_TOUCH_NATIVE=1 reports real Unity touches instead
static int g_scrFinger = -1; static bool g_scrWasDown = false;
static void winToGame(float wx, float wy, float* gx, float* gy) {
  int ww, wh; SDL_GetWindowSize(g_win, &ww, &wh);
  float sa = (float)g_screenW_pub / g_screenH_pub, wa = (float)ww / wh;
  float vw = wa > sa ? wh * sa : ww, vh = wa > sa ? wh : ww / sa;
  float ox = (ww - vw) / 2, oy = (wh - vh) / 2;
  *gx = (wx - ox) / vw * g_screenW_pub; *gy = g_screenH_pub - (wy - oy) / vh * g_screenH_pub;
}
static Finger* findFinger(SDL_FingerID sid) { for (auto& f : g_fing) if (f.used && f.sid == sid && !f.upSeen) return &f; return nullptr; }
static Finger* newFinger(SDL_FingerID sid) { for (auto& f : g_fing) if (!f.used) { f = Finger(); f.used = true; f.sid = sid; f.id = g_fingNext++; return &f; } return nullptr; }
static void fingersBeginFrame() {   // advance last frame's phases
  for (auto& f : g_fing) {
    if (!f.used) continue;
    if (f.phase == 3) { f.used = false; continue; }                       // Ended was reported once
    if (f.endNext || f.upSeen) { f.phase = 3; f.endNext = false; f.upSeen = false; continue; }
    if (f.phase == 0 || f.phase == 1) f.phase = 2;                        // Began/Moved -> Stationary until it moves again
    f.px = f.x; f.py = f.y;
  }
}
int gfxTouchCount() { if (g_touchAsMouse) return 0; int n = 0; for (auto& f : g_fing) if (f.used) n++; return n; }
void gfxGetTouch(int idx, uint8_t* t) {   // UnityEngine.Touch, 0x44 bytes
  memset(t, 0, 0x44);
  int n = 0;
  for (auto& f : g_fing) if (f.used) {
    if (n++ != idx) continue;
    *(int*)(t + 0) = f.id; ((float*)(t + 4))[0] = f.x; ((float*)(t + 4))[1] = f.y;
    ((float*)(t + 12))[0] = f.x; ((float*)(t + 12))[1] = f.y;
    ((float*)(t + 20))[0] = f.x - f.px; ((float*)(t + 20))[1] = f.y - f.py;
    *(int*)(t + 0x20) = 1; *(int*)(t + 0x24) = f.phase; *(int*)(t + 0x28) = 0;
    *(float*)(t + 0x2c) = 1.f; *(float*)(t + 0x30) = 1.f;
    return;
  }
}
void gfxPumpEvents(bool* quit) {
  SDL_Event e;
  int ww, wh; SDL_GetWindowSize(g_win, &ww, &wh);
  static Uint32 lastTick = 0; Uint32 now = SDL_GetTicks();
  float dt = lastTick ? (now - lastTick) / 1000.f : 1.f / 60; lastTick = now;
  if (dt > 0.1f) dt = 0.1f;   // cursor speed is per second, not per frame: the game can run at 15-30 fps
  static bool inited = false;
  if (!inited) {
    inited = true; SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");   // fingers are handled below, no synthetic mouse
    if (getenv("RT_TOUCH_NATIVE")) g_touchAsMouse = false;
  }
  fingersBeginFrame();
  (void)gameInBooth();   // (logs transitions with RT_PADLOG)
  { extern int g_hostFrame; static const char* ps = getenv("RT_PAD_SHORTCUT"); static int pw[4] = {-1, -1, -1, -1}, pf[4] = {-1, -1, -1, -1}; if (ps && pw[0] < 0) sscanf(ps, "%d,%d;%d,%d", &pw[0], &pf[0], &pw[1], &pf[1]); for (int i = 0; ps && i < 2; i++) if (g_hostFrame == pf[i]) padShortcut(pw[i]); }   // test: press pad shortcuts at given frames ("which,frame;which,frame")
  static const bool synth = getenv("RT_SYNTH_TOUCH") != nullptr;   // test: scripted clicks arrive as real SDL finger events
  if (synth && g_scr) {
    static bool prevDown = false; static float lx = -1, ly = -1;
    float sa = (float)g_screenW_pub / g_screenH_pub, wa = (float)ww / wh;
    float vw = wa > sa ? wh * sa : ww, vh = wa > sa ? wh : ww / sa, ox = (ww - vw) / 2, oy = (wh - vh) / 2;
    SDL_Event fe; memset(&fe, 0, sizeof fe); fe.tfinger.touchId = 1; fe.tfinger.fingerId = 7;
    fe.tfinger.x = (ox + g_scrX / g_screenW_pub * vw) / ww; fe.tfinger.y = (oy + (g_screenH_pub - g_scrY) / g_screenH_pub * vh) / wh;
    if (g_scrDown && !prevDown) { fe.type = SDL_FINGERDOWN; SDL_PushEvent(&fe); }
    else if (g_scrDown && (g_scrX != lx || g_scrY != ly)) { fe.type = SDL_FINGERMOTION; SDL_PushEvent(&fe); }
    else if (!g_scrDown && prevDown) { fe.type = SDL_FINGERUP; SDL_PushEvent(&fe); }
    prevDown = g_scrDown; lx = g_scrX; ly = g_scrY;
  }
  bool touchEvent = false;
#ifdef __SWITCH__
  static int s_evTotal = 0, s_fingerTotal = 0, s_diagFrame = 0;   // diagnostics: svcOutputDebugString (nroloader --log-sd mirrors it to the SD card)
  if ((s_diagFrame++ % 180) == 0) { char d[300]; int n = snprintf(d, sizeof d, "[papers-diag] f%d joys=%d pad=%d touchdev=%d ev=%d fingers=%d focus=%d opmode=%d padA=%d", s_diagFrame, SDL_NumJoysticks(), g_pad ? 1 : 0, SDL_GetNumTouchDevices(), s_evTotal, s_fingerTotal, (int)appletGetFocusState(), (int)appletGetOperationMode(), (int)g_padA); svcOutputDebugString(d, n); }
#endif
  while (SDL_PollEvent(&e)) {
#ifdef __SWITCH__
    s_evTotal++; if (e.type == SDL_FINGERDOWN) s_fingerTotal++;
#endif
    if (e.type == SDL_QUIT) *quit = true;
    if (e.type == SDL_CONTROLLERDEVICEADDED && !g_pad) g_pad = SDL_GameControllerOpen(e.cdevice.which);
    if (e.type == SDL_FINGERDOWN || e.type == SDL_FINGERMOTION || e.type == SDL_FINGERUP) touchEvent = true;
    if (e.type == SDL_FINGERDOWN) {
      Finger* f = newFinger(e.tfinger.fingerId);
      if (f) { f->phase = 0; winToGame(e.tfinger.x * ww, e.tfinger.y * wh, &f->x, &f->y); f->px = f->x; f->py = f->y; }
      g_padX = e.tfinger.x * ww; g_padY = e.tfinger.y * wh;
    } else if (e.type == SDL_FINGERMOTION) {
      Finger* f = findFinger(e.tfinger.fingerId);
      if (f) { winToGame(e.tfinger.x * ww, e.tfinger.y * wh, &f->x, &f->y); if (f->phase != 0) f->phase = 1; }
      g_padX = e.tfinger.x * ww; g_padY = e.tfinger.y * wh;
    } else if (e.type == SDL_FINGERUP) {
      Finger* f = findFinger(e.tfinger.fingerId);
      if (f) { winToGame(e.tfinger.x * ww, e.tfinger.y * wh, &f->x, &f->y); if (f->phase == 0) f->endNext = true; else f->upSeen = true; }
      g_padX = e.tfinger.x * ww; g_padY = e.tfinger.y * wh;
    }
  }
  // scripted pointer (test runs): behaves as one finger
  if (g_scr && !g_touchAsMouse) {
    if (g_scrDown && g_scrFinger < 0) { Finger* f = newFinger(-100); if (f) { f->phase = 0; f->x = f->px = g_scrX; f->y = f->py = g_scrY; g_scrFinger = (int)(f - g_fing); } }
    else if (g_scrFinger >= 0) {
      Finger& f = g_fing[g_scrFinger];
      if (g_scrDown) { if (f.x != g_scrX || f.y != g_scrY) { f.x = g_scrX; f.y = g_scrY; if (f.phase != 0) f.phase = 1; } }
      else { f.upSeen = true; g_scrFinger = -1; }
    }
  }
  bool touchHeld = false;
  if (g_touchAsMouse) { for (auto& f : g_fing) if (f.used && f.phase != 3) touchHeld = true; }   // mouse-emulation mode: any held finger = left button down (a sub-frame tap still gives one frame)
  if (!g_pad) { for (int i = 0; i < SDL_NumJoysticks(); i++) if (SDL_IsGameController(i)) { g_pad = SDL_GameControllerOpen(i); break; } }
  for (int i = 0; i < 3; i++) g_mousePrev[i] = g_mouseNow[i];
  int x, y; unsigned b = SDL_GetMouseState(&x, &y);
  bool mouseMoved = (x != g_lastMX || y != g_lastMY);
  g_lastMX = x; g_lastMY = y;
  if (g_pad) {
    auto ax = [&](SDL_GameControllerAxis a) { float v = SDL_GameControllerGetAxis(g_pad, a) / 32767.f; return fabsf(v) < 0.2f ? 0.f : v; };
    float lx = ax(SDL_CONTROLLER_AXIS_LEFTX), ly = ax(SDL_CONTROLLER_AXIS_LEFTY);
    float rx = ax(SDL_CONTROLLER_AXIS_RIGHTX), ry = ax(SDL_CONTROLLER_AXIS_RIGHTY);
    float sp = (float)ww * 0.667f * dt;   // px per frame at 60 fps was ww/90; now px per second so a slow frame rate doesn't slow the cursor
    // R stick = fine (slow) cursor, L stick = normal, D-pad = slow steps
    g_padX += lx * sp + rx * sp * 0.35f; g_padY += ly * sp + ry * sp * 0.35f;
    if (SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) g_padX -= sp * 0.5f;
    if (SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) g_padX += sp * 0.5f;
    if (SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_DPAD_UP)) g_padY -= sp * 0.5f;
    if (SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) g_padY += sp * 0.5f;
    // A / ZR / R = click (grab, stamp), B / ZL / L = right click (back)
    g_padA = SDL_GameControllerGetButton(g_pad, BTN_A);   // Vita: Cross is the only click button
    g_padB = false;                                       // Vita has no right click
    bool padMoved = fabsf(lx) + fabsf(ly) + fabsf(rx) + fabsf(ry) > 0.f || SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT) || SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_DPAD_UP) || SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    static bool prevShort = false; bool shortNow = SDL_GameControllerGetButton(g_pad, BTN_X) || SDL_GameControllerGetButton(g_pad, BTN_Y) || SDL_GameControllerGetButton(g_pad, BTN_B) || SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) || SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) || SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_START);
    if (padMoved || SDL_GameControllerGetButton(g_pad, BTN_A)) { g_curShow = true; g_curIdle = 0; }
    if (shortNow && !prevShort && !padMoved) { g_curShow = false; g_curIdle = 99.f; }   // a shortcut button pressed on its own = playing by buttons
    prevShort = shortNow;
    g_padCancel = SDL_GameControllerGetButton(g_pad, BTN_B);   // Circle = cancel/back (Vita DirectionalNavigation cancel) -> Escape
    // Vita layout: R = toggle the booth shutter, L = call the next traveler. The game's own click handlers are run from inside its update (fastpath.cpp)
    static bool lPrev = false, rPrev = false;
    bool lNow = SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER), rNow = SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
    if (rNow && !rPrev) padShortcut(0);
    if (lNow && !lPrev) padShortcut(1);
    lPrev = lNow; rPrev = rNow;
    g_padStart = SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_START);
    g_padXb = SDL_GameControllerGetButton(g_pad, BTN_X); g_padYb = SDL_GameControllerGetButton(g_pad, BTN_Y);
  }
  if (mouseMoved && !touchEvent) { g_curShow = true; g_curIdle = 0; }
  if (g_scr) { g_curShow = true; g_curIdle = 0; }      // scripted test runs keep the cursor visible
  if (touchEvent || touchHeld) { g_curShow = false; g_curIdle = 99.f; }
  if (g_curShow) { g_curIdle += dt; if (g_curIdle > kCursorIdle) g_curShow = false; }
  if (g_padX < 0) g_padX = 0; if (g_padX > ww - 1) g_padX = (float)ww - 1;
  if (g_padY < 0) g_padY = 0; if (g_padY > wh - 1) g_padY = (float)wh - 1;
  if (mouseMoved || (b & (SDL_BUTTON_LMASK | SDL_BUTTON_RMASK))) { g_padX = (float)x; g_padY = (float)y; }   // touch / mouse wins and re-seeds the pad cursor
  g_mouseNow[0] = (b & SDL_BUTTON_LMASK) || g_padA || touchHeld; g_mouseNow[1] = (b & SDL_BUTTON_RMASK) || g_padB; g_mouseNow[2] = b & SDL_BUTTON_MMASK;
  // map window -> game screen (letterbox aware: RT aspect == screen aspect)
  float sa = (float)g_screenW_pub / g_screenH_pub, wa = (float)ww / wh;
  float vw = wa > sa ? wh * sa : ww, vh = wa > sa ? wh : ww / sa;
  float ox = (ww - vw) / 2, oy = (wh - vh) / 2;
  g_mx = (g_padX - ox) / vw * g_screenW_pub; g_my = g_screenH_pub - (g_padY - oy) / vh * g_screenH_pub;
  if (g_scr && g_touchAsMouse && !synth) { g_mx = g_scrX; g_my = g_scrY; g_mouseNow[0] = g_scrDown; }
}
ICALL(bool, In_GetKeyInt, int kc) {
  { static bool lg = getenv("RT_KEYLOG") != nullptr; static bool seen[1024]; if (lg && kc >= 0 && kc < 1024 && !seen[kc]) { seen[kc] = true; fprintf(stderr, "[key] game polls KeyCode %d\n", kc); } }
  int sdlk = kc;
  switch (kc) { case 273: sdlk = SDLK_UP; break; case 274: sdlk = SDLK_DOWN; break; case 275: sdlk = SDLK_RIGHT; break; case 276: sdlk = SDLK_LEFT; break; case 13: sdlk = SDLK_RETURN; break; case 27: sdlk = SDLK_ESCAPE; break; case 304: sdlk = SDLK_LSHIFT; break; case 306: sdlk = SDLK_LCTRL; break; default: break; }
  if (kc == 27 && (g_padStart || (g_padCancel && !gameInBooth()))) return true;   // B = back in menus, but it must not open the pause menu during a day (+ still does)
  if (kc == 32 && g_padXb) return true;   // X = SPACE (booth upgrade: toggle inspect)
  if (kc == 9 && g_padYb) return true;    // Y = TAB (booth upgrade: toggle stamp bar)
  const uint8_t* ks = SDL_GetKeyboardState(nullptr);
  SDL_Scancode sc = SDL_GetScancodeFromKey(sdlk);
  return sc != SDL_SCANCODE_UNKNOWN && ks[sc];
}
ICALL(bool, In_GetMouseButton, int b) { return b >= 0 && b < 3 && g_mouseNow[b]; }
ICALL(bool, In_GetMouseButtonDown, int b) { return b >= 0 && b < 3 && g_mouseNow[b] && !g_mousePrev[b]; }
ICALL(void, In_mousePosition, float* v) { v[0] = g_mx; v[1] = g_my; v[2] = 0; }
ICALL(void, In_GetTouch, int idx, void* t) { gfxGetTouch(idx, (uint8_t*)t); }
void minuRegisterInput(std::map<std::string, void*>& impl) {
  REG("UnityEngine.Input::GetKeyInt(UnityEngine.KeyCode)", In_GetKeyInt);
  REG("UnityEngine.Input::GetMouseButton(System.Int32)", In_GetMouseButton);
  REG("UnityEngine.Input::GetMouseButtonDown(System.Int32)", In_GetMouseButtonDown);
  REG("UnityEngine.Input::get_mousePosition_Injected(UnityEngine.Vector3&)", In_mousePosition);
  REG("UnityEngine.Input::GetTouch_Injected(System.Int32,UnityEngine.Touch&)", In_GetTouch);
}



