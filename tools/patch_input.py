p = 'src/host/gfx.cpp'
s = open(p, encoding='utf-8', newline='').read().replace('\r\n', '\n')
a = s.index('void gfxPumpEvents(bool* quit) {')
b = s.index('ICALL(bool, In_GetKeyInt')
new = '''static SDL_GameController* g_pad = nullptr;
static float g_padX = 640, g_padY = 360;
static bool g_padA = false, g_padB = false, g_padStart = false;
static int g_lastMX = -1, g_lastMY = -1;
void gfxPumpEvents(bool* quit) {
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    if (e.type == SDL_QUIT) *quit = true;
    if (e.type == SDL_CONTROLLERDEVICEADDED && !g_pad) g_pad = SDL_GameControllerOpen(e.cdevice.which);
  }
  if (!g_pad) { for (int i = 0; i < SDL_NumJoysticks(); i++) if (SDL_IsGameController(i)) { g_pad = SDL_GameControllerOpen(i); break; } }
  for (int i = 0; i < 3; i++) g_mousePrev[i] = g_mouseNow[i];
  int x, y; unsigned b = SDL_GetMouseState(&x, &y);
  int ww, wh; SDL_GetWindowSize(g_win, &ww, &wh);
  bool mouseMoved = (x != g_lastMX || y != g_lastMY);
  g_lastMX = x; g_lastMY = y;
  if (g_pad) {
    auto ax = [&](SDL_GameControllerAxis a) { float v = SDL_GameControllerGetAxis(g_pad, a) / 32767.f; return fabsf(v) < 0.2f ? 0.f : v; };
    float lx = ax(SDL_CONTROLLER_AXIS_LEFTX), ly = ax(SDL_CONTROLLER_AXIS_LEFTY);
    float rx = ax(SDL_CONTROLLER_AXIS_RIGHTX), ry = ax(SDL_CONTROLLER_AXIS_RIGHTY);
    float sp = (float)ww / 90.f;
    g_padX += (lx + rx) * sp; g_padY += (ly + ry) * sp;
    if (SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) g_padX -= sp * 0.5f;
    if (SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) g_padX += sp * 0.5f;
    if (SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_DPAD_UP)) g_padY -= sp * 0.5f;
    if (SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) g_padY += sp * 0.5f;
    g_padA = SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_A) || SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
    g_padB = SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_B) || SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
    g_padStart = SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_START);
  }
  if (g_padX < 0) g_padX = 0; if (g_padX > ww - 1) g_padX = (float)ww - 1;
  if (g_padY < 0) g_padY = 0; if (g_padY > wh - 1) g_padY = (float)wh - 1;
  if (mouseMoved || (b & (SDL_BUTTON_LMASK | SDL_BUTTON_RMASK))) { g_padX = (float)x; g_padY = (float)y; }   // touch / mouse wins and re-seeds the pad cursor
  g_mouseNow[0] = (b & SDL_BUTTON_LMASK) || g_padA; g_mouseNow[1] = (b & SDL_BUTTON_RMASK) || g_padB; g_mouseNow[2] = b & SDL_BUTTON_MMASK;
  // map window -> game screen (letterbox aware: RT aspect == screen aspect)
  float sa = (float)g_screenW_pub / g_screenH_pub, wa = (float)ww / wh;
  float vw = wa > sa ? wh * sa : ww, vh = wa > sa ? wh : ww / sa;
  float ox = (ww - vw) / 2, oy = (wh - vh) / 2;
  g_mx = (g_padX - ox) / vw * g_screenW_pub; g_my = g_screenH_pub - (g_padY - oy) / vh * g_screenH_pub;
  if (g_scr) { g_mx = g_scrX; g_my = g_scrY; g_mouseNow[0] = g_scrDown; }
}
'''
s = s[:a] + new + s[b:]
# START = Escape
s = s.replace('  const uint8_t* ks = SDL_GetKeyboardState(nullptr);', '  if (kc == 27 && g_padStart) return true;\n  const uint8_t* ks = SDL_GetKeyboardState(nullptr);', 1)
open(p, 'w', encoding='utf-8', newline='').write(s)
print('ok')
