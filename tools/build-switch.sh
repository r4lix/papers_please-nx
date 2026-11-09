#!/bin/sh
# builds local/switch/papers.nro (aarch64, devkitA64) from the lifted code in $LIFT_DIR (default local/lift_direct)
cd "$(dirname "$0")/.."
DKP=/c/devkitPro
export DEVKITPRO=C:/devkitPro
CXX=$DKP/devkitA64/bin/aarch64-none-elf-g++
LIFT=${LIFT_DIR:-local/lift_direct}
OBJ=${OBJ_DIR:-local/lift_obj_nx}
HOBJ=local/hostobj_nx
mkdir -p $OBJ $HOBJ local/switch
ARCH="-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE"
INC="-I$DKP/libnx/include -I$DKP/portlibs/switch/include -I$DKP/portlibs/switch/include/SDL2 -Isrc/rt -Isrc/host"
FLAGS="-std=gnu++17 -O1 -w -fexceptions -ffunction-sections -D__SWITCH__ -DRT_FULL -DGFX_ES $ARCH $INC"
python -I tools/fp_patch.py $LIFT || exit 1
ls $LIFT/lift_*.cpp | xargs -P ${JOBS:-10} -I{} sh -c 'o='$OBJ'/$(basename {} .cpp).o; [ "$o" -nt {} ] || '$CXX' '"$FLAGS"' -I'$LIFT' -c {} -o $o'
for f in rt_common rt_full win32_shims rt_eh; do $CXX $FLAGS -c src/rt/$f.cpp -o $OBJ/$f.o || exit 1; done
$CXX $FLAGS -I$LIFT -c $LIFT/lift_table.cpp -o $OBJ/lift_table.o || exit 1
for f in main_full il2cpp_api minu minu_core gfx scene audio fastpath; do $CXX $FLAGS -c src/host/$f.cpp -o $HOBJ/$f.o || exit 1; done
$CXX -specs=$DKP/libnx/switch.specs -g $ARCH -Wl,--gc-sections -o local/switch/papers.elf $HOBJ/*.o $OBJ/*.o -L$DKP/libnx/lib -L$DKP/portlibs/switch/lib -lSDL2 -lEGL -lGLESv2 -lglapi -ldrm_nouveau -lnx -lm || exit 1
$DKP/tools/bin/nacptool --create "Papers, Please" "Lucas Pope (native Switch port)" "1.0" local/switch/papers.nacp
$DKP/tools/bin/elf2nro local/switch/papers.elf local/switch/papers.nro --icon=icon.jpg --nacp=local/switch/papers.nacp
ls -la local/switch/papers.nro
