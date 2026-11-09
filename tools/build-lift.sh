#!/bin/sh
# builds local/host/hosthyb.exe : host + lifted code (hybrid test mode, x64 Windows)
cd "$(dirname "$0")/.."
SDL=/c/Users/klaud/Desktop/switch/tools/pc-sdk/SDL2-2.32.10/x86_64-w64-mingw32
LIFT=${LIFT_DIR:-local/lift_full}
OBJ=${OBJ_DIR:-local/lift_obj}
mkdir -p $OBJ
ls $LIFT/lift_*.cpp | xargs -P 20 -I{} sh -c 'o='$OBJ'/$(basename {} .cpp).o; [ "$o" -nt {} ] || g++ -std=c++17 -O1 -mcx16 -w -c -DLIFT_HYBRID -Isrc/rt -I'$LIFT' {} -o $o'
g++ -std=c++17 -O1 -mcx16 -w -c -DLIFT_HYBRID -Isrc/rt src/rt/rt_common.cpp -o $OBJ/rt_common.o
g++ -std=c++17 -O1 -mcx16 -w -c -DLIFT_HYBRID -Isrc/rt src/rt/rt_hybrid.cpp -o $OBJ/rt_hybrid.o
g++ -std=c++17 -O1 -mcx16 -w -c -DLIFT_HYBRID -Isrc/rt $LIFT/lift_table.cpp -o $OBJ/lift_table.o
g++ -std=c++17 -O1 -g -w -DLIFT_HYBRID -DSDL_MAIN_HANDLED -Isrc/rt -I$SDL/include/SDL2 -o ${OUT_EXE:-local/host/hosthyb.exe} src/host/host.cpp src/host/il2cpp_api.cpp src/host/minu.cpp src/host/minu_core.cpp src/host/gfx.cpp src/host/scene.cpp src/host/audio.cpp src/host/liftglue.cpp $OBJ/*.o -L$SDL/lib -lSDL2
