import sys
bs = chr(92)
# repair: literal "\n" sequences that should have been real line continuations / escapes
p = 'src/host/il2cpp_api.h'
s = open(p).read()
s = s.replace(' ' + bs + 'n  X(', ' ' + bs + '\n  X(')
open(p, 'w').write(s)

p = 'src/host/main_full.cpp'
s = open(p).read()
a = s.index('    if (frame % 300 == 0) printf("[gc] frame')
b = s.index('    if (frame % 600 == 0) {') if '    if (frame % 600 == 0) {' in s else s.index('    if (hidden) std::this_thread')
new = (
    '    if (frame % 300 == 0) printf("[gc] frame %d used %.1f MB heap %.1f MB disabled=%d' + bs + 'n", frame, (double)il2cpp_gc_get_used_size() / 1048576.0, (double)il2cpp_gc_get_heap_size() / 1048576.0, (int)il2cpp_gc_is_disabled());\n'
    '    if (frame == 900) { printf("[gc] forcing collection' + bs + 'n"); il2cpp_gc_collect(0); printf("[gc] done used %.1f MB heap %.1f MB' + bs + 'n", (double)il2cpp_gc_get_used_size() / 1048576.0, (double)il2cpp_gc_get_heap_size() / 1048576.0); }\n'
)
# find the end of the broken region: up to the line that starts the mem print (#ifdef __SWITCH__)
end = s.index('#ifdef __SWITCH__\n    if (frame % 600')
s = s[:a] + new + s[end:]
open(p, 'w').write(s)
print('ok')
