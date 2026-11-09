#pragma once
#include "rt_shim.h"
#include <map>
#include <string>
#include <atomic>

struct RtImage {
  uint8_t* mem = nullptr;
  size_t size = 0;
  uint64_t pref = 0, base = 0, delta = 0, entry = 0;
  uint64_t tlsStart = 0, tlsEnd = 0, tlsIndexVar = 0;
  uint32_t tlsZero = 0;
  std::map<std::string, uint64_t> exports;
};
extern RtImage g_img;

bool rt_load_image(const void* file, size_t fsize, const char* name);
uint64_t rt_export(const char* name);
uint32_t rt_alloc_tid();
void rt_thread_setup(size_t guestStackBytes, uint32_t tid = 0);
void rt_register_region(uint64_t base, size_t size);          // for VirtualQuery (guest stacks)
bool rt_region_lookup(uint64_t addr, uint64_t* base, uint64_t* size);
// cooperative stop-the-world (Boehm SuspendThread / GetThreadContext emulation)
uint32_t rt_suspend_thread(uint32_t tid);
uint32_t rt_resume_thread(uint32_t tid);
bool rt_thread_context(uint32_t tid, uint8_t* context);       // fills a Win64 CONTEXT
struct BlockedScope { BlockedScope(); ~BlockedScope(); };
uint32_t rt_thread_id();
bool rt_run_dllmain();
void rt_register_named(const char* key, void* shim);   // key = "dll!function" (lowercase) or "*!function"
void* rt_find_named(const std::string& key);
bool rt_shim_call(Ctx* c, uint64_t target);
const char* rt_shim_name(uint64_t target);
void rt_win32_register();                              // registers all Win32/baselib shims
void rt_init_table();
