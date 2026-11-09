// Audio: AudioSource state + software mixer over SDL2 audio. Clips are PCM16 WAVs from local\data\audio.
#include "minu_objs.h"
#include <SDL.h>
#include <mutex>
#include <cmath>
#include <cstring>
#include <fstream>
#include <algorithm>

extern std::string g_dataDir;

static std::recursive_mutex g_mx;
static std::vector<NSource*> g_sources;
static std::map<std::string, float> g_mixer;  // "<group>Atten" -> dB
static SDL_AudioDeviceID g_dev = 0;
static const int kRate = 44100;

void audioLock(bool on) { if (on) g_mx.lock(); else g_mx.unlock(); }
void audioRegister(NSource* s) { std::lock_guard<std::recursive_mutex> l(g_mx); g_sources.push_back(s); }
void audioSetMixerFloat(const std::string& name, float db) { std::lock_guard<std::recursive_mutex> l(g_mx); g_mixer[name] = db; }

static bool loadWav(NClip* c) {
  if (c->loaded) return !c->pcm.empty();
  c->loaded = true;
  std::ifstream f(g_dataDir + "/audio/" + c->name + ".wav", std::ios::binary);
  if (!f) { printf("[audio] missing clip %s\n", c->name.c_str()); return false; }
  std::vector<char> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (d.size() < 44 || memcmp(d.data(), "RIFF", 4)) return false;
  size_t p = 12; int fmt = 0, bits = 16;
  while (p + 8 <= d.size()) {
    uint32_t sz = *(uint32_t*)&d[p + 4];
    if (!memcmp(&d[p], "fmt ", 4)) { fmt = *(uint16_t*)&d[p + 8]; c->channels = *(uint16_t*)&d[p + 10]; c->freq = *(uint32_t*)&d[p + 12]; bits = *(uint16_t*)&d[p + 22]; }
    else if (!memcmp(&d[p], "data", 4)) {
      size_t n = std::min<size_t>(sz, d.size() - p - 8);
      if (bits == 16) { c->pcm.resize(n / 2); memcpy(c->pcm.data(), &d[p + 8], n / 2 * 2); }
      else if (bits == 8) { c->pcm.resize(n); for (size_t i = 0; i < n; i++) c->pcm[i] = ((uint8_t)d[p + 8 + i] - 128) << 8; }
      else { printf("[audio] unsupported %d-bit wav %s\n", bits, c->name.c_str()); return false; }
      (void)fmt;
      return true;
    }
    p += 8 + sz + (sz & 1);
  }
  return false;
}

void audioSetClip(NSource* s, NClip* c) { std::lock_guard<std::recursive_mutex> l(g_mx); s->clip = c; if (c) loadWav(c); s->pos = 0; }
void audioPlay(NSource* s) { std::lock_guard<std::recursive_mutex> l(g_mx); if (s->clip) loadWav(s->clip); s->playing = s->clip && !s->clip->pcm.empty(); s->paused = false; s->pos = 0; }
void audioStop(NSource* s) { std::lock_guard<std::recursive_mutex> l(g_mx); s->playing = false; s->pos = 0; }

static float groupGain(const std::string& g) {
  auto it = g_mixer.find(g + "Atten");
  return it == g_mixer.end() ? 1.f : powf(10.f, it->second / 20.f);
}

static void SDLCALL mixCb(void*, Uint8* out, int len) {
  int16_t* o = (int16_t*)out; int frames = len / 4;
  std::vector<float> acc(frames * 2, 0.f);
  {
    std::lock_guard<std::recursive_mutex> l(g_mx);
    for (NSource* s : g_sources) {
      if (!s->playing || s->paused || !s->clip || s->clip->pcm.empty()) continue;
      NClip* c = s->clip; int ch = c->channels; size_t nf = c->pcm.size() / ch;
      float gain = s->volume * groupGain(s->group);
      double step = (double)c->freq / kRate;
      for (int i = 0; i < frames; i++) {
        size_t ip = (size_t)s->pos;
        if (ip >= nf) { if (s->loop) { s->pos = fmod(s->pos, (double)nf); ip = (size_t)s->pos; } else { s->playing = false; s->pos = 0; break; } }
        float fr = (float)(s->pos - ip);
        size_t ip2 = ip + 1 < nf ? ip + 1 : (s->loop ? 0 : ip);
        for (int k = 0; k < 2; k++) {
          int kc = ch == 1 ? 0 : k;
          float a = c->pcm[ip * ch + kc], b = c->pcm[ip2 * ch + kc];
          acc[i * 2 + k] += (a + (b - a) * fr) * gain;
        }
        s->pos += step;
      }
    }
  }
  for (int i = 0; i < frames * 2; i++) { float v = acc[i]; o[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
}

void audioShutdown() { if (g_dev) { SDL_PauseAudioDevice(g_dev, 1); SDL_CloseAudioDevice(g_dev); g_dev = 0; } }

void audioInit() {
  SDL_AudioSpec want{}, have{};
  want.freq = kRate; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024; want.callback = mixCb;
  g_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
  if (!g_dev) { printf("[audio] SDL_OpenAudioDevice: %s\n", SDL_GetError()); return; }
  SDL_PauseAudioDevice(g_dev, 0);
  printf("[audio] device %d Hz ch %d\n", have.freq, have.channels);
}
