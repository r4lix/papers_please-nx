// Minimal il2cpp embedding API bindings (resolved from the loaded GameAssembly.dll).
#pragma once
#ifdef RT_FULL
#include "rt_shim.h"
#else
#include <windows.h>
#endif
#include <cstdint>
#include <cstddef>
#include <cstdio>

struct Il2CppClass;
struct MethodInfo;
struct Il2CppObject { Il2CppClass* klass; void* monitor; };
struct Il2CppString { Il2CppObject o; int32_t length; uint16_t chars[1]; };
struct Il2CppArray { Il2CppObject o; void* bounds; uintptr_t max_length; /* data at +0x20 */ };

#define IL2CPP_FUNCS(X) \
  X(void, il2cpp_set_data_dir, (const char*)) \
  X(void*, il2cpp_init, (const char*)) \
  X(void*, il2cpp_domain_get, ()) \
  X(void**, il2cpp_domain_get_assemblies, (void*, size_t*)) \
  X(void*, il2cpp_assembly_get_image, (void*)) \
  X(const char*, il2cpp_image_get_name, (void*)) \
  X(Il2CppClass*, il2cpp_class_from_name, (void*, const char*, const char*)) \
  X(Il2CppObject*, il2cpp_object_new, (Il2CppClass*)) \
  X(MethodInfo*, il2cpp_class_get_method_from_name, (Il2CppClass*, const char*, int)) \
  X(Il2CppObject*, il2cpp_runtime_invoke, (MethodInfo*, void*, void**, Il2CppObject**)) \
  X(uint32_t, il2cpp_format_exception, (Il2CppObject*, char*, int)) \
  X(uint32_t, il2cpp_format_stack_trace, (Il2CppObject*, char*, int)) \
  X(void, il2cpp_add_internal_call, (const char*, void*)) \
  X(Il2CppString*, il2cpp_string_new, (const char*)) \
  X(Il2CppString*, il2cpp_string_new_utf16, (const uint16_t*, int)) \
  X(Il2CppArray*, il2cpp_array_new, (Il2CppClass*, uintptr_t)) \
  X(Il2CppClass*, il2cpp_object_get_class, (Il2CppObject*)) \
  X(const char*, il2cpp_class_get_name, (Il2CppClass*)) \
  X(void*, il2cpp_class_get_field_from_name, (Il2CppClass*, const char*)) \
  X(size_t, il2cpp_field_get_offset, (void*)) \
  X(void, il2cpp_field_set_value, (Il2CppObject*, void*, void*)) \
  X(void, il2cpp_field_get_value, (Il2CppObject*, void*, void*)) \
  X(Il2CppClass*, il2cpp_class_get_parent, (Il2CppClass*)) \
  X(Il2CppClass*, il2cpp_class_from_system_type, (void*)) \
  X(void*, il2cpp_type_get_object, (void*)) \
  X(void*, il2cpp_class_get_type, (Il2CppClass*)) \
  X(void*, il2cpp_field_get_type, (void*)) \
  X(Il2CppClass*, il2cpp_class_from_type, (void*)) \
  X(void*, il2cpp_thread_attach, (void*)) \
  X(void, il2cpp_gc_disable, ()) \
  X(void, il2cpp_gc_enable, ()) \
  X(void, il2cpp_gc_collect, (int)) \
  X(int32_t, il2cpp_gc_is_disabled, ()) \
  X(uint32_t, il2cpp_gchandle_new, (Il2CppObject*, int32_t)) \
  X(void, il2cpp_gchandle_free, (uint32_t)) \
  X(int64_t, il2cpp_gc_get_used_size, ()) \
  X(int64_t, il2cpp_gc_get_heap_size, ()) \
  X(void*, il2cpp_resolve_icall, (const char*))

#ifdef RT_FULL
#define DECL(R, N, A) extern GuestFn<R A> N;
#else
#define DECL(R, N, A) extern R(*N) A;
#endif
IL2CPP_FUNCS(DECL)
#undef DECL

bool il2cpp_load(const char* gameDir);

