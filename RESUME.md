# Papers, Please PC port (from the Steam copy) - RESUME

Goal: Switch port built from the user's Steam copy (`C:\Program Files (x86)\Steam\steamapps\common\PapersPlease`,
Unity 2020.3.34f1, x64 IL2CPP). The existing Android-runner NSP (`ports/wip/papers-nsp`, title F0000) already works;
this is the "from the PC build" variant the user asked for (user: "go ahead, RE7 was much harder than that").

## What the game is (verified 2026-11-06)
* Haxe -> C# -> IL2CPP. Assembly-CSharp: ~1350 types / 10k methods (play.*, data.*, app.* + haxe std). Cpp2IL cannot
  recover IL (bodies are `throw null`), so the logic can only be run as machine code.
* Unity part is a thin host: `HostUnity : MonoBehaviour` (fields: screenQuad MeshRenderer, screenShader/screenPausedShader/
  quadShader, artAsset TextAsset, audioMixer, audioPaths List<string>, audioClips List<AudioClip>, pixelPerfectCamera,
  phantomCursor MeshRenderer) + `QuadBatcher` (quads -> Mesh -> CommandBuffer.DrawMesh) + Unity* platform classes (nested
  in HostUnity: UnityPlatformSys/Audio/Disk) + `SteamManager`.
* Scene `level0` is tiny (camera, listener, 2 quads, HostUnity, SteamManager). `sharedassets0.assets` = **Art.dat**
  (3.19 MB TextAsset with all game art), 111 AudioClips (FSB in sharedassets0.resource), 4 shaders, 2 materials, mixer.
  StreamingAssets = loc/*.zip + Version.txt. So the whole game is self-contained and a mini-Unity can replace the scene.
* GameAssembly.dll: sections .text 0x1000 (runtime, 1.4 MB), `il2cpp` 0x155000 (generated code, 9.2 MB), .rdata, .data, .pdata
  (41,746 RUNTIME_FUNCTIONs = exact function bounds). 2.43M instructions, only ~40 distinct mnemonic families that matter
  (mov/lea/call/cmp/test/jcc/add/sub/movzx/SSE scalar+packed, few AVX/FMA, lock xadd/cmpxchg, rep stos). Imports: KERNEL32
  (153), USER32 1, ADVAPI32 3, ole32 6, OLEAUT32 10, SHELL32 2, WS2_32 4, IPHLPAPI 3, baselib.dll 5 (semaphores/thread id).
  243 exports = full il2cpp embedding API.

## Plan
A. **Windows host first** (done up to A2): our own process loads the REAL GameAssembly.dll, calls il2cpp_init, creates the
   HostUnity object and drives Start/Update. We supply the Unity side ("minu": icalls ~400 names in
   `local/dump/icall_names.txt`, mini renderer, audio, input). Everything outside the DLL is portable C++ (SDL2/GL) so it
   can be reused on Switch.
B. **Switch**: no x64 on Switch -> static recompiler x64 -> C++ (planned `tools/lift/`): per-function C++ from .pdata
   bounds, lazy flags, real guest stack, indirect calls through an address->function table, jump tables, Win32 import shims,
   exceptions via .pdata unwind + C++ EH tables (to do), Boehm GC stop-the-world (cooperative). Host (x64) test bed:
   differential-test lifted code against the real DLL loaded in-process.
C. NSP via `tools/nsp/mknsp.py --nro` or a native exefs (decide later), data = Art.dat + audio (converted) + loc zips.

## Tooling (all under local/, not part of the repo)
* `local/tools/il2cppdumper` (v6.7.46; run with DOTNET_ROLL_FORWARD=Major, stdin redirect crashes only at the final
  "press any key") -> `local/dump/{dump.cs,il2cpp.h,script.json,stringliteral.json,DummyDll}`. CodeRegistration 0x180b51e00,
  MetadataRegistration 0x180b585f0, metadata v27.
* `local/tools/cpp2il.exe` (2022.1.0-pre-release.21) -> `local/cpp2il_il` (dummy dll with real signatures), `local/cpp2il_cs`.
* `local/tools/{census.py,census2.py,stats2.py,showns.py,icalls.py,listassets.py}`; `local/dump/acs.txt` = dump.cs of
  Assembly-CSharp only. Python needs `pip install capstone pefile` (done) and UnityPy (present).
* Host build: `g++ -O1 -g -o local\host\host.exe src\host\host.cpp src\host\il2cpp_api.cpp` (WinLibs gcc on PATH), run from
  the project dir: `.\local\host\host.exe`. Unimplemented icalls get a logging stub ("[icall-stub] name").
* Gotchas: PowerShell `$Matches` is clobbered by a second -match (use python); run python with `-I` for untrusted data.

## Status log
* 2026-11-06 (A, Windows host): **the real game code runs.** `host.exe` loads the real GameAssembly.dll in-process, il2cpp_init,
  builds HostUnity by hand (`src/host/scene.cpp`: Art.dat TextAsset, 111 audio clips + paths, dummy shaders/materials, 2 quad
  MeshRenderers, PixelPerfectCamera), runs `Start` (prints "[Host] Detected platform DESKTOP", "[Game] Version", save dir) and
  then `Update` every frame; draw calls reach our command-buffer executor (`gfx.cpp`, SDL2 + GL 2.1 on the host) with real
  textures (the game decodes its own art from Art.dat) and a 570x320 render target. APPDATA/LOCALAPPDATA/USERPROFILE are
  redirected to `local/sandbox` so the real Steam saves are never touched (keep this!).
* Files: `src/host/{host.cpp (main loop, stub thunks, VEH debug), il2cpp_api.*, minu.* (object model/glue/Screen/App),
  minu_core.cpp (GameObject/Component/Object/Camera/Transform/Material/Audio/Matrix4x4...), gfx.cpp (textures, RT, mesh,
  CommandBuffer, GL executor, SDL input), scene.cpp}`, `tools/{build-host.ps1,prepare_data.py,fn.py}`.
  Build+run: `powershell -ExecutionPolicy Bypass -File tools\build-host.ps1; .\local\host\host.exe --hidden --frames 420 --shot local\shot.tga`
  (convert: `python -I -c "from PIL import Image; Image.open(chr(108)+...)"` = PIL open tga, save png). `--hidden` = no window.
  Data: `python -I tools/prepare_data.py "<Steam>/PapersPlease" local/data` (art.dat, audio wavs, audio.json, loc zips).
* Debug helpers: `tools/fn.py 0x<VA> [n]` disassembles any function with il2cpp names; VEH in host.cpp prints `[throw] frames:`
  (VAs inside GameAssembly, look up with fn.py) and `[AV] ...`. Unimplemented icalls print `[icall-stub] <name> (rcx rdx r8 r9)`.
  il2cpp gotchas: `il2cpp_field_set_value(obj, field, value)` takes the object pointer itself for reference types;
  il2cpp GC is disabled in the host (`il2cpp_gc_disable`) because our native-held managed objects are unrooted - must root
  with gchandles before enabling; array data starts at +0x20 (length at +0x18); UnityEngine.Object m_CachedPtr at +0x10 and the
  native struct keeps its instance id at +8 (GetInstanceID reads it).
* 2026-11-06 (later) **Phase A works end to end on Windows.** Black-screen cause: the title intro (TitleScreen.update) slides the logo
  up in sync with the music position (`Music` -> PlatformAudio.isPlaying/getPlayPosition -> AudioSource.time), so the fix was real
  audio: `src/host/audio.cpp` (AudioSource state + SDL2 mixer over the extracted PCM16 wavs, mixer group atten by name via
  AudioMixer.SetFloat("<group>Atten"), `--hidden` uses SDL_AUDIODRIVER=dummy). Input: Camera.ScreenToWorldPoint now returns
  quad-local coords (v/screen - 0.5) because HostUnity.Update does worldToLocal(screenQuad) then floor((x+0.5)*gameW).
  Verified by script: title -> STORY -> NEW -> letter -> day 1 booth with bulletin all render and respond to clicks.
  Host test flags: `--click frame:x:y` (570x320 game px, top-left origin, held 4 frames, repeatable), `--shotat frame:path.tga`.
  Real exe is fullscreen 1920x1080 for comparison (running it through Steam relaunch ignores env redirects, don't).
  Memory: ~1.4 GB working set after startup (GC disabled; steady state grows ~0.1 MB/s) - startup transient needs GC or
  trimming before Switch.
* NEXT: (1) phase B decision/prototype: static x64->C++ lifter vs emulator (see below), (2) enable GC on host (root natives),
  (3) shaders/blend verification vs real exe, window scaling/fullscreen, saves path, loc/language zips, (4) Switch packaging.

## Phase B: static x64 -> C++ lifter (started 2026-11-06, evening)
Decision: static lifter (not an emulator): per-function C++, guest memory == host memory (identity mapped, PE relocated; `A(va)` adds
`g_delta`), guest registers are C++ locals spilled to `Ctx` around calls, real guest stack, flags computed lazily-by-liveness.
* `tools/lift/lift.py <dll> <outdir> [--indirect] [--allflags] [--shard N] [--only 0xVA,..]` -> `lift_NNNN.cpp` (64 fns each),
  `lift_decls.h`, `lift_table.cpp` (`{va, fn, size|psz<<16|EH<<31}`), `lift_stats.txt`. Whole DLL (32,002 functions incl. 1,176
  non-pdata leaf fns, 2.42M insns) lifts in ~90 s, 234 MB of C++; compiles in ~3 min total on 20 cores at -O1 (≈70 MB of objects).
  Only ~55 insn instances unimplemented (ldmxcsr/stmxcsr, rcr, pcmpistri, a few x87/AVX) -> trap with `rt_unimpl` at runtime.
  `--indirect` = all direct calls go through `rt_call` (needed for the hybrid harness).
* Function discovery: pdata (chained unwind entries grouped into their parent), call/tail targets not in pdata. Jump tables are
  recognised (`lea B,[rip+x]; mov r32,[B+I*4+D]; add; jmp`) and emitted as `switch` over runtime target.
* Runtime `src/rt/`: `x64rt.h` (Ctx, L8..L128/S8..S128 memcpy loads/stores, flag helpers), `x64rt_sse.h` (SSE/AVX lane helpers),
  `rt_common.cpp` (hash table va->fn, `rt_call`, cpuid/rdtsc/cmpxchg16b), `rt_hybrid.cpp` (x64 Windows ONLY test harness).
* HYBRID harness (`tools/build-lift.sh` -> `local/host/hosthyb.exe`): the real DLL is loaded natively; env `LIFT=1` patches native
  entries of lifted functions with `mov dword [rsp-8],idx; jmp bridge` (bridge switches to a 64 MB C++ stack, builds a Ctx, runs the
  lifted fn, preserves *every* register incl. r9-r11 because MSVC LTCG assumes untouched volatiles survive); lifted code calls
  anything not lifted natively via `rt_native_call` (switches to the guest stack). Knobs: `LIFT_RANGE=lo:hi` (table indexes),
  `LIFT_SKIP=i,j`, `LIFT_EH=1` (also patch functions that have SEH/C++ EH handlers; default skipped), `LIFT_TRACE=idx`,
  `LIFT_PASS=idx` (run native original via trampoline), `LIFT_CHECKFREE=1`. `tools/bisect.py lo hi [n]` binary-searches the culprit
  function(s) (success = process prints `done`; host now `_exit(0)`s to skip DLL teardown, which used to crash on freed globals).
* Findings: (1) bridge must not clobber r9 (4th arg) when picking its Ctx pointer; (2) tail `jmp [mem]` thunks: bridge saves RA in
  `Ctx::aux` and rewrites it on exit because native callee's `call` overwrites the slot; (3) ~3,400 functions have EH handlers
  (3250 use `__CxxFrameHandler3` @0x18012a7bc, rest `__C_specific_handler`); `SetThreadName` (0x1800bb570) uses RaiseException
  0x406D1388 inside __try -> cannot unwind through the bridge, so EH functions stay native in hybrid mode.
* TODO next: finish bisecting the remaining lifted-code bugs; then EH (parse FuncInfo/scope tables, lift catch funclets, implement
  RaiseException/_CxxThrowException via C++ throw + per-function try/catch), then full-lift mode with own Win32 shims (no native DLL),
  loader (map PE, relocs, IAT -> shims, fake TEB for gs:, TLS), threads, Boehm GC stack scanning of the guest stack,
  then aarch64 build with devkitA64 (gcc) and GLES/SDL2 port of gfx/audio.

### Phase B milestone (2026-11-06 night): the whole game runs through the lifted code
`LIFT=1 ./local/host/hosthyb.exe --hidden --frames 5200 --click 1500:285:244 --click 1700:37:90 ...` plays title -> STORY -> NEW ->
letter -> day-1 booth with ~32,000 lifted functions (screens identical to the native run; ~2.5 min for 5200 frames in the hybrid
harness). Bugs found/fixed on the way (all in tools/lift/lift.py or src/rt/):
* flag liveness ignored `bnd jne` (MPX prefix) -> stack cookie checks "failed" (`rt_fastfail`); flag_info must strip `bnd `.
* jump tables: bound heuristic picked unrelated `cmp`; base register `lea` may be outside the straight-line window (fallback = the unique
  rip-relative lea of that register in the function); 2-level (byte index -> dword table) works.
* local-vs-tail decision for jmp/jcc must be "target inside this function's pdata chunks", not "not a known start" (extras discovered
  from other functions polluted `known` and turned local jumps into native tail calls = runaway recursion).
* variable-count shifts: a 32-bit destination must still be zero-extended when the count is 0 (found by the unit tester).
* `cvtss2si` used the double path; `stmxcsr/ldmxcsr/fnstcw/...` stubs; `pcmpistri` implemented (`sse_pcmpistri`); `int 0x29` -> `rt_fastfail`.
* gs: accesses must use the *current* thread's TEB (`rt_gs()` = `mov %gs:0x30`), not a global (another thread's TLS got used).
* `RT_BARRIER()` (compiler memory barrier) at every label so spin loops re-read memory.
* hybrid bridge: per-thread state via TEB.ArbitraryUserPointer, lazy init for threads created by the OS, r9-r11 preserved, rax passed
  through to native callees (`__chkstk` takes its size in rax), CreateThread hooked, RaiseException(0x406D1388) swallowed.
* gcc sometimes dies with an ICE under 20-way parallel compile; just rerun `tools/build-lift.sh` (it only rebuilds missing objects).
Unit tester: `tools/lift/gen_unittest.py` + `src/rt/unit_main.cpp` run every distinct instruction form of the DLL (20,896) natively and
lifted on random inputs and compare regs/xmm/flags/memory: all pass except NaN-payload propagation order of SSE ops (irrelevant).
Build: gen -> `g++ -c ... local/unit/*.cpp` -> link with `src/rt/unit_main.cpp` (see shell history; add a script when reusing).
STILL TO DO: (1) discover address-taken functions (reloc DIR64 targets in code, script.json method pointers, exports) - leaf functions
without pdata that are only reached via vtables/closures are NOT lifted yet (hybrid runs them natively; Switch cannot);
(2) tiny (<5 byte) functions cannot be patched in hybrid mode (irrelevant for the Switch build);
(3) exceptions (unwind through lifted frames), (4) full mode without the native DLL: PE loader + relocations + IAT shims
(~190 imports: kernel32 etc., baselib.dll) + fake TEB/PEB/TLS + threads, (5) GC: Boehm must scan the guest stack, (6) aarch64 build
(devkitA64) + GLES/SDL2 port of gfx/audio + NRO/NSP packaging.

### Phase B milestone 2 (2026-11-06 late night): NO native DLL needed any more
`tools/build-full.sh` -> `local/host/papers_full.exe` (Windows test build of the final architecture): loads GameAssembly.dll as *data*
(own PE loader: sections, DIR64 relocs, IAT -> shims, TLS), runs the lifted code (`tools/lift/lift.py --eh`, direct calls), own Win32/baselib
shims (`src/rt/win32_shims.cpp`), fake TEB/TLS/guest stack per thread (`rt_full.cpp`), real exception handling (`src/rt/rt_eh.cpp`:
RaiseException/RtlUnwindEx/RtlVirtualUnwind on the DLL's own pdata + its personality routines; catch = C++ `GuestResume` exception caught by
a landing pad generated for every function with handlers; resume labels = jump targets + call return addresses + catch continuations
found from funclet `lea rax,[cont]; ret`). The same scripted playthrough (title -> day 1 booth) renders identically.
Run: `papers_full.exe --game local/gamedir --data local/data --save local/savefull --icalls local/dump/icall_names.txt --hidden --frames N`
(gamedir = GameAssembly.dll + il2cpp_data/Metadata/global-metadata.dat; env RT_VERBOSE=1 / RT_TRACE=1 for shim logging).
Notable fixes: function discovery now also adds address-taken roots (DIR64 relocs, exports), rip-relative `lea` targets and gap scan
(+3,9k functions); GC: `GC_DONT_GC=1` is injected via the fake environment (Boehm stop-the-world cannot suspend our fake threads);
VirtualQuery must report MEM_FREE gaps (Boehm walks the whole address space); GetProcAddress/LoadLibrary serve all registered shims
(pinvoke), `api-ms-win-*` names succeed; StreamingAssets = `C:\game\sa` -> `--data` dir.
NEXT: aarch64 build (devkitA64), gfx -> GLES2, input, NRO/NSP packaging, memory (1.4 GB startup with GC off).

### Phase B milestone 3 (2026-11-07): full mode on Switch, memory fixed, GC works
* Switch NRO (`JOBS=12 sh tools/build-switch.sh` -> `local/switch/papers.nro`, 57 MB) runs in Eden at 60 FPS: title, STORY/ENDLESS menu,
  letter screen render correctly. Distribution folder (NRO + game/ + data/ + icall_names.txt + save/, 167 MB) = `local/dist/switch/papers/`
  (same layout as `%APPDATA%\eden\sdmc\switch\papers\`). Still NRO only (no NSP before 1.0, see memory). Not yet tested on real hardware.
* Memory growth root causes (all fixed):
  1. The game creates + `Object.DestroyImmediate`s two Texture2D per frame (315x197, 129x38 RGBA, ~270 KB). `Obj_Destroy/DestroyImmediate` were
     empty icalls so the pixel buffers + GL textures leaked ~50 MB/s. Now `gfxReleaseNative()` frees GL texture/mesh buffers (small NObj shell stays,
     ~0.3 KB/frame) and pixel buffers are recycled through `takePx/givePx` (pool of 12) + a static swizzle buffer, because on newlib the
     alloc/free churn of 250 KB blocks fragmented the arena (+250 MB / 600 frames) even though live bytes were flat.
  2. Boehm GC crashed in `GC_mark_from` (F_18010c430): the `VirtualQuery` shim returned BaseAddress = region start and RegionSize = whole region.
     Windows semantics are BaseAddress = queried page, RegionSize = bytes from that page to the end of the region. Boehm computes the main stack
     base as `page + RegionSize`, so it scanned 16 MB past the stack block into unmapped pages. Fixed in `VirtualQuery_` (win32_shims.cpp).
* GC now works: forced collection takes the managed heap from 634 MB to 136 MB (il2cpp init + scene build leave ~500 MB garbage); main_full.cpp
  runs `il2cpp_gc_collect(0)` once right after `Start()` + `il2cpp_gc_enable()`. Steady state on Eden: gc used 136 MB, heap 647 MB, newlib arena
  915 MB at start growing ~1 MB / 600 frames afterwards (NObj shells).
* Debugging recipe that found the GC bug: build one lifted shard with `-O0 -g` (e.g. `g++ ... -c local/lift_direct/lift_0109.cpp`, relink into a
  separate exe), gdb `run`, `up`, `p/x r6` etc. print guest registers (they are C++ locals r0..r15); hardware watchpoint on the mark-stack slot
  (`watch *(long*)($bot+0x1dd8) if ...`) showed the huge range being chunked by GC_mark_from.
* Left: real hardware test (memory ~0.9 GB resident; NRO from hbmenu applet mode may not have enough, title takeover / NSP should), gameplay
  tests with the gamepad on Eden, optional wav -> smaller format, dropping the pinned-forever gchandle for transient natives (shells), NSP at 1.0.

### 2026-11-07 afternoon: OLED crash after ~10,000 frames = guest stack drift (fixed)
* OLED (SwitchLink devid `switch`, files under /switch/papers/, upload script `local/deploy_oled.ps1`) crashed after ~165 s of play: data abort in
  `Matrix4x4.get_identity` (static_fields null) right after the first *automatic* GC. Reproduced on Windows with a 11,500-frame run
  (clicks in `local/clicks.txt` reach the day-1 booth); there it died in Boehm's `GC_clear_stack_inner` (F_180105980) recursing below the stack.
* Root cause: the shim thunk set `t_guest_sp = c->r[4]` at every shim entry and never restored it, and top-level host->guest calls
  (`rt_guest_call`) start below `t_guest_sp` => every frame started a little deeper; after ~10,000 frames (~1.6 KB/frame) the 16 MB guest stack was
  exhausted. Fix: RAII `SpScope` in `makeShim` (rt_shim.h) saves/restores it. Verified: Windows 12,500 frames incl. the auto GC OK.
* Also: `VirtualFree(MEM_DECOMMIT)` now zeroes the range (Windows semantics); `RT_GCSTRESS=N` env = forced collection every N frames (debug).
* New NRO md5 C47846FB deployed to the OLED (previous = papers.nro.bak_20261107); not retested on hardware yet.

## 2026-11-07 evening: startup time (30 s on OLED) -> native fast paths
* Cause: startup (`HostUnity.Start` -> `play.Bootstrap`) is guest-CPU bound. A sampling profiler with stack unwinding
  (`RT_PROF=file` on the Windows build, then `python -I local/profincl.py N [funcname]`) showed ~50% in the Haxe PNG decode
  (`format.png.Tools.extract32` incl. pure-Haxe zlib inflate, 625 images, 35 MB raw), ~5% XXTea, rest XML/regex/TabParser.
* Fix: `src/host/fastpath.cpp` replaces `haxe.zip.Uncompress.run` and `format.png.Tools.extract32` with native code
  (own inflate, PNG unfilter + BGRA convert; reads the Haxe List/DynamicObject header directly; falls back to the lifted
  original when a precondition fails). `RT_FP_VERIFY=1` runs both and compares (0 mismatches over all 625 images),
  `RT_NOFASTPATH=1` disables. Lift direct calls bypass `rt_call`, so `tools/fp_patch.py` (run by both build scripts; list in
  `tools/fp_vas.txt`) renames the chosen `F_<va>` to `F_<va>_orig` and generates `lift_fp.cpp` wrappers with a hook pointer.
* `[time]` stage lines in main_full.cpp (also on Switch in papers.log). Windows Start 3.4 s -> 1.8 s; heap after startup gc 646 -> 392 MB;
  Eden: first frame at 14.3 s (Start 13 s). Remaining startup: font XML parse (~25%), TabParser/regex ctor, Res ctor.
  Next candidates: native `app.XXTea.btea_decode`, XML parser, -O2 for hot shards.
* Vita dump (PCSE01056, user-provided, in this folder) is encrypted (needs zRIF + psvpfstools); not used.
* Vita EU update pkg (`EP2741-PCSB01084_00-...bg_1...pkg`, v1.01): `tools/vitapkg.py` lists/extracts it (type-2 key, CTR key = AES-ECB(iv)).
  Extracted to `local/vita_eu`. Contents: data/*.pak, eboot.bin, sce_module, sce_pfs, sce_sys. The paks/eboot are still PFS-encrypted
  (random bytes, same as `PCSE01056`), so they need psvpfstools + the game's zRIF; the pkg itself does not carry the key.
* Vita EU v1.01 decrypted 2026-11-07 into `local/vita_dec` (psvpfsparser built from github.com/motoharu-gosuto/psvpfstools in an Ubuntu 22.04
  container on Unraid; needed `-include cstring -include cstdint`, a `FindLIBTOMCRYPT.cmake` copy and `-ltomcrypt`; run with the user's zRIF
  from `work.bin`'s content id EP2741-PCSB01084_00-PAPERSPLEASE00EU and the public F00D service cma.henkaku.xyz). Result: data/*.pak are
  plain "FILELINK_____END" archives of the Vita engine (custom engine, not Unity); eboot.bin is still a retail SELF (SCE header, NPDRM, needs keys).
  The US dump `PCSE01056` has a different content id, so this zRIF does not apply to it. The zRIF is not stored in the repo.

### 2026-11-08: Vita eboot decrypted, input findings, touch fix
* Vita EU 1.01 fully decrypted (PSVitaNPDRMDecryptor from github.com/SuleMareVientu, user ran it): `local/vita_dec_full/PCSB01084/`; `tools/fself2elf.py eboot.bin out.elf` -> `local/vita_re/eboot.elf`
  (plain ARM ELF, 5 segments, imports by NID). Vita game = **Haxe/OpenFL/lime-console hxcpp** (`lime.console.vita.Touch`, `wfEngine`), i.e. the same Haxe game logic as
  the PC build (Unity build is Haxe->C#). Vita input classes: ui.TouchDrag/TouchScroll/TouchGlow, booth.MagnifierTouchHit, Upgrade BOOTH_MULTITOUCH, ui.MouseCursor.
* The Steam Unity build's `InputWrapper` only ever sets mode Mouse (Touch mode is dead code), so touch must drive the mouse: gfx.cpp now tracks SDL fingers
  (`g_touchAsMouse`, default; `RT_TOUCH_NATIVE=1` reports real Unity touches, `RT_SYNTH_TOUCH=1` makes scripted --click arrive as SDL finger events: verified title->booth).
* Platform kind: `HostUnity.detectPlatformKind` (0x1809eaaf0/0x1809a53a0) uses SystemInfo.deviceType (our stub returns 2=Console!): 3=Desktop -> DESKTOP, 1=Handheld + deviceModel
  StartsWith("iPad"?) -> TABLET/PHONE, else getSuggestedHandheldPlatformKind (aspect/inches). Lever for a touch-style UI on Switch: return 1 + model, small Screen.dpi/inches. Untested.
* Cursor speed is now px/second (dt-based); main loop prints `[fps] frame N: x fps (Update, present ms)` every 120 frames.

### 2026-11-08 pm: gameplay perf root cause = Time.frameCount
* `Time.frameCount` icall used to tick on every call. HostUnity's texture cache (Dictionary<uint,CachedTexture>, lastUsedFrame) therefore evicted everything each frame and rebuilt ~27 textures
  (~2.4 MB) per frame in the booth (OLED Update 30-45 ms, growing; arena growth). Now `g_hostFrame` (minu_core.cpp) is set once per frame by main_full.cpp: Windows booth Update 4.2 -> 0.8 ms, 0 textures/frame.
* `[fps]` lines now end with `tex +N (MB)` = textures created in the last 120 frames; `RT_TEXLOG=1` logs every creation; `RT_PROF_FROM=<frame>` delays the Windows sampler.
* Scripted Windows runs still stop at the day-1 bulletin (clicks.txt never starts the day), so real gameplay cost is only measurable on the OLED.

## 2026-11-08 eve: shortcuts / Vita controls
* PC build upgrades (Loc.csv, bought at night): SPACE = toggle INSPECT, TAB = toggle stamp bar, RULEBOOK SHORTCUTS, DOUBLE-CLICK = quick inspect, MULTITOUCH. Game polls KeyCode 9 (Tab), 32 (Space), 13 (Enter), 275/276 (arrows), 27 (Esc) (+ t/l/p/o/126 debug-ish); log with RT_KEYLOG=1.
* Switch mapping now: A/R/ZR click, B/L/ZL right-click, **X = Space, Y = Tab**, + = Esc, L stick cursor, R stick fine, D-pad steps. Arrows (rulebook shortcuts?) not bound yet - semantics unverified.
* Vita eboot (Haxe/OpenFL): has ui.MouseCursor (gamepad stick cursor + buttons: gamepadStickX/Y, onGamepadButtonDown), ui.DirectionalNavigation/Node (d-pad focus nav in menus, confirm/cancel), ui.TouchDrag/TouchGlow/TouchScroll, same 5 upgrades (+ Upgrade-INSPECT_HOTKEY etc). The exact Vita button ids are in compiled code, NOT decoded (manual in sce_sys/manual has no controls page).

## 2026-11-08 night: Vita controls decoded (tools/vita_reloc.py + local/vita_dis.py/vita_xref.py/vita_callers.py)
Method: `fself2elf.py` ELF -> `python -I tools/vita_reloc.py eboot.elf eboot.img` (applies SCE relocs: fmt0 = 12 B hdr/addend/offset with code2+dist2 pair, fmt1 = 8 B hdr/addend) -> capstone Thumb disasm; movw/movt xrefs to hxcpp strings find the dyn wrappers, which hold the method pointers.
Lime button ids: A0 B1 X2 Y3 BACK4 START6 LSTICK7 RSTICK8 L9 R10 DPAD up/down/left/right 11..14 (assumed lime standard; Vita physical mapping not confirmed).
* ui.MouseCursor: left stick axes 0/1 -> cursor; **button 0 (Cross) = click** (down/up); no other button.
* GameScreen.onGamepadAxisMove (0x812927xx): axes 0,2 -> stickX, 1,3 -> stickY = **both sticks pan the booth view** (|v|>0.01).
* GameScreen.onGamepadButtonDown (0x812928fe): base handler (dirNav) then, when not busy: **L(9) -> BorderCam action (strings speech-announce/border_next/resolve; probably call-next/announce, unverified)**; **R(10) -> Booth handler**; **D-pad up/down (11/12) -> vertical screen pan, left/right (13/14) -> horizontal pan 504 px** (0x81539538).
* Booth.onGamepadButtonDown (0x815e6ed2): **R(10) = toggle Shutter (booth window)**; **Y/Triangle(3) = toggle InspectUi (needs hotkey upgrade)**; **X/Square(2) = toggle StampBar (needs hotkey upgrade)**.
* Menus: ui.DirectionalNavigation (d-pad focus, confirm/cancel), pause via Start (pauseButton).
PC Switch port: X (top, Triangle position) = Space (inspect), Y (left, Square) = Tab (stamp bar) already matches. Missing: R = shutter toggle, L = call-next/announce (PC has no keys for them -> would need synthetic clicks at the button positions).

## 2026-11-08 night (2): pad shortcuts at code level (user request: "why not implement it at the code level?")
Final Switch layout: **A = click**, **X = Space (inspect)**, **Y = Tab (stamp bar)**, **R = toggle booth shutter**, **L = call next traveler**, ZR = click, B/ZL = right-click, + = Esc, sticks/D-pad = cursor.
R/L are not synthetic clicks: `fastpath.cpp` hooks `Booth.update` (0x1801ca450) and `Border.update` (0x180445e40) (fp_vas.txt) and, on a pending press (<=30 frames old), runs the game's own `Booth.shutterSwitch_onClick` (0x1801c9d90) / `Border.outerBoothButton_onClick` (0x180441700) from inside the update via rt_guest_call (they ignore the Button arg). Test: `RT_PAD_SHORTCUT="0,4500;1,4700" RT_PADLOG=1` + `$(cat local/clicks.txt)` on papers_full.exe -> shutter opens, entrant arrives (first scripted Windows run that reaches real gameplay; screenshots local/pad_c2.png). Hook-call counters print 0 in the report (cosmetic, handler verifiably runs).
* 2026-11-08 late: Switch SDL reports face buttons by position (SDL_A = Switch B) -> BTN_A/B/X/Y macros in gfx.cpp swap them under __SWITCH__ (user: 'it still B to click').
* Right click removed (Vita has none): only A clicks; B/ZL/ZR unbound.
* B = Escape (cancel/back), same as +.
* Cursor visibility: gfx.cpp g_curShow (stick/D-pad/A/mouse show, touch + X Y L R B + hide, 2.5 s idle hides; scripted runs always show); fastpath.cpp hooks play.ui.CursorEnt.draw (0x180536a20) to skip drawing.
* B = Escape only outside a day (gameInBooth(): Booth.update ran in the last 6 frames); + always Escape/pause.

## 2026-11-09: NSP (title 01005CB000100000, tools/NSP.md row)
* NSP = nroloader + NRO + local/nsp/papers (= local/dist/switch/papers). Diag variant 0F5000 (--log-sd, [papers-diag] lines in gfx.cpp) showed input OK on the OLED.
* Exit crash fix: Application.Quit called exit(0) on the game thread (static destructors) -> now sets g_appQuit, loop ends, audioShutdown, main() does _Exit on the real main thread (nro md5 e9e2e3ac...). Unverified on hardware.

* 2026-11-09 exit error: dialog 'The software was closed because an error occurred' (no code, no atmosphere crash report) after a CLEAN exit (log: NRO returned 0, final commit); appletSetTerminateResult(0) did not help. Test modes: SD file /papers_exitmode digit (1 = SetTerminateResult+svcExitProcess, 2 = svcExitProcess, 3 = +appletUnlockExit). Splash: Vita startup.png (ENTRY GRANTED stamp) via gfxSplash(), gfxInit moved to the start of real_main.

## 2026-11-09 pm: NSP on hardware, exit error SOLVED
* NSP 01005CB000100000 (ports/NSP, share, /nsp_test on the OLED) runs on the OLED: input OK, ENTRY GRANTED splash (Vita startup image, gfxSplash) shown while loading, quit from the title screen (+) is clean.
* Exit error cause + fix: see tools/NSP.md "NRO-mode titles must end with ISelfController::Exit" (main() sets `__nx_applet_exit_mode = 1` when ProgramId == 0x01005CB000100000, commits the virtual SD with fsdevCommitDevice("sdmc") before _Exit). Found with help from an Opus subagent reading libnx's applet.o disassembly.
* Test titles installed on the OLED for the investigation (ExitTest*, LoaderElf*, Papers Diag) each reserve ~1 GB: uninstall them (system memory free went 10.8 -> 3.1 GB).
