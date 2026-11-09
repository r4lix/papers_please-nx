#pragma once
#include "minu.h"

struct NShader : NObj { NShader() { type = N_Shader; } };
struct NText : NObj { std::string bytes; NText() { type = N_Text; } };
struct NMixer : NObj { NMixer() { type = N_Mixer; } };
struct NGroup : NObj { NGroup() { type = N_Mixer; } };
struct NClip : NObj { std::vector<int16_t> pcm; int channels = 2, freq = 44100; bool loaded = false; NClip() { type = N_Clip; } };
struct NMaterial : NObj { std::string shader; std::map<int, NObj*> tex; NMaterial() { type = N_Material; } };
struct NGameObject : NObj { bool active = true; NGameObject() { type = N_GameObject; } };
struct NRenderer : NObj { NGameObject* go = nullptr; NMaterial* material = nullptr; bool enabled = true; NRenderer() { type = N_Renderer; } };
struct NTransform : NObj { NGameObject* go = nullptr; float pos[3] = {0,0,0}; NTransform() { type = N_Transform; } };
struct NBehaviour : NObj { NGameObject* go = nullptr; NBehaviour() { type = N_Behaviour; } };
struct NSource : NObj { NGameObject* go = nullptr; NClip* clip = nullptr; bool loop = false; float volume = 1.f; bool playing = false, paused = false; double t = 0; double pos = 0; std::string group; NSource() { type = N_Source; } };

struct NCamera : NObj { NGameObject* go = nullptr; float ortho = 540; NCamera() { type = N_Camera; } };
Il2CppObject* buildHostUnity();



// audio.cpp
void audioInit();
void audioRegister(NSource* s);
void audioPlay(NSource* s);
void audioStop(NSource* s);
void audioSetClip(NSource* s, NClip* c);
void audioLock(bool on);
void audioSetMixerFloat(const std::string& name, float db);
