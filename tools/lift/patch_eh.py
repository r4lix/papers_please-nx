import re
p = 'tools/lift/lift.py'
s = open(p).read()

s = s.replace("    ap.add_argument('--allflags', action='store_true')", "    ap.add_argument('--allflags', action='store_true')\n    ap.add_argument('--eh', action='store_true')", 1)
s = s.replace("    global ALLFLAGS\n    ALLFLAGS = args.allflags\n", "    global ALLFLAGS, EH_WRAP\n    ALLFLAGS = args.allflags\n    EH_WRAP = args.eh\n", 1)
s = s.replace("ALLFLAGS = False\n", "ALLFLAGS = False\nEH_WRAP = False\n", 1)

old = "    tail = ['#undef SPILL', '#undef RELOAD', '}']\n    return '\\n'.join(head + body + tail) + '\\n', unimpl_here"
assert old in s, 'tail not found'
new = r"""    tail = ['#undef SPILL', '#undef RELOAD', '}']
    if EH_WRAP and fl.root in EH_ROOTS:
        head.append('  uint64_t ent_rsp = r4; uint64_t resume = 0;')
        pre = ['  for (;;) { try {']
        sw = ['  if (resume) { uint64_t rr = resume - g_delta; resume = 0; switch (rr) {']
        for t in sorted(labels):
            sw.append('  case %s: goto L_%x;' % (hx(t), t))
        sw.append('  default: rt_badjump(%s, rr + g_delta); } }' % hx(fl.root))
        post = ['  } catch (GuestResume& e_) { if (e_.entry_rsp != ent_rsp) throw; for (int i_ = 0; i_ < 16; i_++) { c->r[i_] = e_.r[i_]; c->x[i_] = e_.x[i_]; } RELOAD; resume = e_.rip; } }']
        return '\n'.join(head + pre + sw + body + post + tail) + '\n', unimpl_here
    return '\n'.join(head + body + tail) + '\n', unimpl_here"""
s = s.replace(old, new, 1)

old = "    print('decoded %d functions"
new = r"""    if only is None and EH_WRAP:
        chunk_begins = sorted(chunks)
        def chunk_of(a):
            i = _bis.bisect_right(chunk_begins, a) - 1
            return chunk_begins[i] if i >= 0 and chunks[chunk_begins[i]][0] <= a < chunks[chunk_begins[i]][1] else None
        chunk_root = {}
        for r_, ms_ in groups.items():
            for m_ in ms_:
                chunk_root[m_] = r_
        extras = collections.defaultdict(set)
        for r_, fl in list(funcs.items()):
            insns = fl.insns
            for a, ins in insns.items():
                if ins.mn == 'lea' and len(ins.ops) == 2 and ins.ops[0].type == X86_OP_REG and ins.ops[0].reg == X86_REG_RAX and ins.ops[1].type == X86_OP_MEM and ins.ops[1].mem.base == X86_REG_RIP:
                    x = a + ins.size + ins.ops[1].mem.disp
                    nxt = a + ins.size
                    ok = False
                    for _ in range(4):
                        n_ = insns.get(nxt)
                        if n_ is None:
                            break
                        if n_.mn == 'ret':
                            ok = True
                            break
                        nxt += n_.size
                    if ok and img.in_code(x):
                        ch = chunk_of(x)
                        if ch is not None:
                            rt_ = chunk_root.get(ch)
                            if rt_ is not None and rt_ != r_ and rt_ in funcs and x not in funcs[rt_].insns:
                                extras[rt_].add(x)
        print('EH continuation entries:', sum(len(v) for v in extras.values()), 'in', len(extras), 'functions')
        for r_, xs in extras.items():
            old_fl = funcs[r_]
            fl = FuncLift(img, r_, old_fl.ranges, set(old_fl.group) | xs)
            decode_func(img, fl, known)
            funcs[r_] = fl
            for t in list(fl.calls) + list(fl.tails.values()):
                if t not in known and t not in funcs and t not in extra_seen and img.in_code(t) and not in_pdata(t):
                    extra_seen.add(t); known.add(t); queue.append((t, None, {t}))
        drain()
    print('decoded %d functions"""
s = s.replace(old, new, 1)
open(p, 'w').write(s)
print('ok')
