"""Extract everything the native host needs from the user's Steam copy of Papers, Please.

  python -I tools/prepare_data.py "<Steam>/PapersPlease" <outdir>

Writes <outdir>/art.dat, <outdir>/audio/<name>.wav (PCM16), <outdir>/audio.json (clip order used by HostUnity),
<outdir>/loc/*.zip and Version.txt.  Needs UnityPy.
"""
import sys, os, json, struct, shutil, wave
import UnityPy

game = sys.argv[1]
out = sys.argv[2]
D = os.path.join(game, "PapersPlease_Data")
os.makedirs(os.path.join(out, "audio"), exist_ok=True)
env = UnityPy.load(D)
shared = [f for p, f in env.files.items() if p.replace("\\", "/").endswith("sharedassets0.assets")][0]
lvl = [f for p, f in env.files.items() if p.replace("\\", "/").endswith("/level0")][0]

names = {}
for pid, o in shared.objects.items():
    t = o.type.name
    if t == "TextAsset":
        d = o.read()
        raw = d.m_Script
        if isinstance(raw, str):
            raw = raw.encode("utf-8", "surrogateescape")
        open(os.path.join(out, "art.dat"), "wb").write(bytes(raw))
        print("art.dat", len(raw))
    elif t == "AudioClip":
        d = o.read()
        names[pid] = d.m_Name
        for n, data in d.samples.items():
            open(os.path.join(out, "audio", n if n.endswith(".wav") else n + ".wav"), "wb").write(data)

# HostUnity MonoBehaviour in level0: parse the serialized fields by hand
for pid, o in lvl.objects.items():
    if o.type.name != "MonoBehaviour":
        continue
    raw = o.get_raw_data()
    # PPtr<GameObject> m_GameObject (12), m_Enabled (1 + pad 3), PPtr<MonoScript> m_Script (12), m_Name string
    off = 12 + 4 + 12
    ln = struct.unpack_from("<i", raw, off)[0]
    off += 4 + ((ln + 3) & ~3)
    # Candidate: HostUnity has 5 PPtrs, 3 shader PPtrs ... fields: screenQuad, screenShader, screenPausedShader, quadShader,
    # artAsset, audioMixer (6 PPtrs = 72 bytes), audioPaths (array of strings), audioClips (array of PPtr), 2 more PPtrs.
    off += 6 * 12
    try:
        n = struct.unpack_from("<i", raw, off)[0]
        if not (0 < n < 1000):
            continue
        off += 4
        paths = []
        for i in range(n):
            l = struct.unpack_from("<i", raw, off)[0]
            s = raw[off + 4:off + 4 + l].decode("utf-8")
            off += 4 + ((l + 3) & ~3)
            paths.append(s)
        m = struct.unpack_from("<i", raw, off)[0]
        off += 4
        clips = []
        for i in range(m):
            fid, p = struct.unpack_from("<iq", raw, off)
            off += 12
            clips.append(names.get(p, "?"))
        json.dump({"audioPaths": paths, "audioClips": clips}, open(os.path.join(out, "audio.json"), "w"), indent=1)
        print("HostUnity audio paths", len(paths), "clips", len(clips))
    except Exception as e:
        print("skip mb", pid, e)

os.makedirs(os.path.join(out, "loc"), exist_ok=True)
for f in os.listdir(os.path.join(D, "StreamingAssets", "loc")):
    shutil.copy(os.path.join(D, "StreamingAssets", "loc", f), os.path.join(out, "loc", f))
shutil.copy(os.path.join(D, "StreamingAssets", "Version.txt"), os.path.join(out, "Version.txt"))
print("done")
