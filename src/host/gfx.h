#pragma once
#include "minu.h"
bool gfxInit(int w, int h, bool visible);
void gfxSplash();
void gfxPresent(void* managedRT);
bool gfxScreenshot(void* managedRT, const char* path);
void minuRegisterGfx(std::map<std::string, void*>& impl);

void gfxPumpEvents(bool* quit);

void gfxScriptMouse(bool active, float x, float yTop, bool down);
