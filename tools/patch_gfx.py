p = 'src/host/gfx.cpp'
s = open(p, encoding='utf-8-sig', newline='').read().replace('\r\n', '\n')

# 1. GLES2 mode
s = s.replace('''  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
  g_win = SDL_CreateWindow("Papers, Please", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | (visible ? 0 : SDL_WINDOW_HIDDEN));''',
'''#ifdef GFX_ES
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
  g_win = SDL_CreateWindow("Papers, Please", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h, SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN);
#else
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
  g_win = SDL_CreateWindow("Papers, Please", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | (visible ? 0 : SDL_WINDOW_HIDDEN));
#endif''', 1)

s = s.replace('''  const char* vs =
      "#version 120\\nattribute''', '''#ifdef GFX_ES
#define GLSLV ""
#define GLSLF "precision mediump float;\\n"
#else
#define GLSLV "#version 120\\n"
#define GLSLF "#version 120\\n"
#endif
  const char* vs =
      GLSLV "attribute''', 1)
s = s.replace('''  const char* fs =
      "#version 120\\nvarying vec4 vCol;''', '''  const char* fs =
      GLSLF "varying vec4 vCol;''', 1)
s = s.replace('''const char* bvs = "#version 120\\nattribute vec2 p;''', '''const char* bvs = GLSLV "attribute vec2 p;''', 1)
s = s.replace('''const char* bfs = "#version 120\\nvarying vec2 uv;''', '''const char* bfs = GLSLF "varying vec2 uv;''', 1)

# 2. BGRA -> RGBA on ES
s = s.replace('''  if (t->fmt == 59 || t->fmt == 57) fmt = GL_BGRA;
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, t->w, t->h, 0, fmt, GL_UNSIGNED_BYTE, data);''', '''  if (t->fmt == 59 || t->fmt == 57) {
#ifdef GFX_ES
    tmp.assign(t->px.begin(), t->px.end());
    for (size_t i = 0; i + 3 < tmp.size(); i += 4) std::swap(tmp[i], tmp[i + 2]);
    data = tmp.data();
#else
    fmt = GL_BGRA;
#endif
  }
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, t->w, t->h, 0, fmt, GL_UNSIGNED_BYTE, data);''', 1)
s = s.replace('#include <unordered_map>', '#include <unordered_map>\n#include <algorithm>', 1)
open(p, 'w', encoding='utf-8', newline='').write(s)
print('patched', 'GFX_ES' in s)
