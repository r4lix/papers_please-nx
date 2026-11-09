#!/bin/sh
# builds local/host/papers_full.exe : host + lifted code + own Win32 shims (no native GameAssembly.dll). Windows test build.
cd "$(dirname "$0")/.."
SDL=/c/Users/klaud/Desktop/switch/tools/pc-sdk/SDL2-2.32.10/x86_64-w64-mingw32
LIFT=${LIFT_DIR:-local/lift_direct}
OBJ=${OBJ_DIR:-local/lift_obj_full}
mkdir -p $OBJ
FLAGS="-std=c++17 -O1 -mcx16 -w -DRT_FULL -Isrc/rt -Isrc/host"
python -I tools/fp_patch.py $LIFT || exit 1
ls $LIFT/lift_*.cpp | xargs -P 12 -I{} sh -c 'o='$OBJ'/$(basename {} .cpp).o; [ "$o" -nt {} ] || g++ '"$FLAGS"' -I'$LIFT' -c {} -o $o'
for f in rt_common rt_full win32_shims rt_eh; do g++ $FLAGS -c src/rt/$f.cpp -o $OBJ/$f.o || exit 1; done
g++ $FLAGS -I$LIFT -c $LIFT/lift_table.cpp -o $OBJ/lift_table.o || exit 1
mkdir -p local/hostobj
for f in main_full il2cpp_api minu minu_core gfx scene audio fastpath; do g++ $FLAGS -DSDL_MAIN_HANDLED -I$SDL/include/SDL2 -c src/host/$f.cpp -o local/hostobj/$f.o || exit 1; done
g++ -O1 -o ${OUT_EXE:-local/host/papers_full.exe} local/hostobj/*.o $OBJ/*.o -L$SDL/lib -lSDL2 -static-libgcc -static-libstdc++
