"""Make selected lifted functions overridable: renames F_<va> to F_<va>_orig in the shard that defines it and generates
lift_fp.cpp with a wrapper F_<va> that calls a hook (set by src/host/fastpath.cpp) or the original.  Idempotent.
usage: python -I tools/fp_patch.py <lift_dir>"""
import sys, re, os, glob
d = sys.argv[1]
here = os.path.dirname(os.path.abspath(__file__))
vas = []
for l in open(os.path.join(here, 'fp_vas.txt')):
    l = l.split('#')[0].strip()
    if l: vas.append(l.split()[0].lower().replace('0x', ''))
decls = open(os.path.join(d, 'lift_decls.h')).read()
shards = sorted(glob.glob(os.path.join(d, 'lift_[0-9]*.cpp')))
done = set()
for va in vas:
    pat = 'void F_%s(Ctx* c) {' % va
    if 'void F_%s_orig(Ctx*);' % va in decls: done.add(va); continue
    for f in shards:
        s = open(f, encoding='utf-8').read()
        if pat in s:
            s = s.replace(pat, 'void F_%s_orig(Ctx* c) {' % va, 1)
            open(f, 'w', encoding='utf-8').write(s)
            done.add(va)
            decls += 'void F_%s_orig(Ctx*);\n' % va
            break
    else:
        print('fp_patch: function', va, 'not found'); sys.exit(1)
open(os.path.join(d, 'lift_decls.h'), 'w').write(decls)
out = ['#include "x64rt.h"', '#include "lift_decls.h"', 'struct FpEntry { uint64_t va; LiftFn* hook; LiftFn orig; };']
for va in vas:
    out.append('static LiftFn g_hook_%s = nullptr;' % va)
    out.append('void F_%s(Ctx* c) { if (g_hook_%s) g_hook_%s(c); else F_%s_orig(c); }' % (va, va, va, va))
out.append('extern const FpEntry g_fp_entries[] = {')
for va in vas: out.append('{0x%sull, &g_hook_%s, F_%s_orig},' % (va, va, va))
out.append('{0, nullptr, nullptr}};')
new = '\n'.join(out) + '\n'
p = os.path.join(d, 'lift_fp.cpp')
if not os.path.exists(p) or open(p).read() != new: open(p, 'w').write(new)
print('fp_patch: %d functions' % len(vas))
