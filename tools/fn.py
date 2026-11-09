"""Disassemble one function of the Steam GameAssembly.dll with il2cpp names:  python -I tools/fn.py 0x1809E89F0 [maxlines]"""
import sys, json, struct, os, pickle, pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
HERE = os.path.dirname(os.path.abspath(__file__))
LOCAL = os.path.join(HERE, "..", "local")
P = r"C:\Program Files (x86)\Steam\steamapps\common\PapersPlease\GameAssembly.dll"
cache = os.path.join(LOCAL, "dump", "names.pkl")
if os.path.exists(cache):
    names = pickle.load(open(cache, "rb"))
else:
    j = json.load(open(os.path.join(LOCAL, "dump", "script.json"), encoding="utf-8"))
    names = {}
    for m in j.get("ScriptMethod", []):
        names[m["Address"] + 0x180000000] = m["Name"]
    # metadata usage slots (string literals / method refs / types)
    for k in ("ScriptString", "ScriptMetadata", "ScriptMetadataMethod"):
        for m in j.get(k, []):
            names[m["Address"] + 0x180000000] = "[%s] %s" % (k[6:], m.get("Name") or m.get("Value"))
    pickle.dump(names, open(cache, "wb"))
pe = pefile.PE(P)
img = pe.get_memory_mapped_image()
base = 0x180000000
va = int(sys.argv[1], 16)
mx = int(sys.argv[2]) if len(sys.argv) > 2 else 400
pd = [s for s in pe.sections if s.Name.startswith(b".pdata")][0].get_data()
end = None
for i in range(len(pd) // 12):
    b, e, u = struct.unpack_from("<III", pd, i * 12)
    if b + base <= va < e + base:
        end = e + base; start = b + base; break
if end is None: end = va + 0x400; start = va
print("; function %x-%x %s" % (start, end, names.get(start, "")))
md = Cs(CS_ARCH_X86, CS_MODE_64)
code = img[start - base:end - base]
n = 0
for ins in md.disasm(code, start):
    note = ""
    if ins.mnemonic in ("call", "jmp") and ins.op_str.startswith("0x"):
        t = int(ins.op_str, 16)
        note = "  ; " + names.get(t, "")
    elif "rip" in ins.op_str:
        # resolve rip-relative target
        try:
            disp = ins.op_str.split("rip")[1].split("]")[0].replace(" ", "")
            t = ins.address + ins.size + int(disp, 16)
            nm = names.get(t, "")
            note = "  ; [%x] %s" % (t, nm)
        except Exception:
            pass
    print("%x: %-8s %s%s" % (ins.address, ins.mnemonic, ins.op_str, note))
    n += 1
    if n >= mx: break
