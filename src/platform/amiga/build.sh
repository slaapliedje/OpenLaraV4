#!/bin/bash
# OpenLara main engine for the Apollo Vampire V4
#   ./build.sh           -> OpenLara          software renderer
#   ./build.sh mag       -> OpenLaraMag       software renderer's palette/lightmap look, spans drawn by Maggie
#   ./build.sh hw        -> OpenLaraHW        Maggie back end (gapi/maggie.h): true-colour textures, blending
#   ./build.sh mag emu   -> OpenLaraMagEmu    Maggie build with a software Maggie, for emu.sh (Amiberry)
#   ./build.sh mag prof  -> OpenLaraMagProf   stage profiling (ENV:OLDBG, see maggie_sw.h)
# Needs bebbo m68k-amigaos gcc 6.5. C code as 68060 (-m68080 traps on core 12846).
#   -fbbb=- turns off bebbo gcc's extra optimiser: it miscompiles (base register reused after a postincrement)
set -e
TC=${TC:-$HOME/dev/toolchains/amigaos}
CXX=$TC/bin/m68k-amigaos-g++
CC=$TC/bin/m68k-amigaos-gcc
# -falign-*: the 68080 is sensitive to where hot loops sit (a 2-3 ms swing on LEVEL2 from unrelated edits); aligned, timings are stable
FLAGS="-m68060 -mhard-float -O2 -fbbb=- -fomit-frame-pointer -falign-loops=16 -falign-jumps=16 -falign-functions=16 -falign-labels=8 -DNDEBUG -D__AMIGA__ ${EXTRA:-}"
OUT=OpenLara; B=build
for a in "$@"; do
  case $a in
    mag) FLAGS="$FLAGS -DOL_MAGGIE"; OUT=${OUT}Mag; B=${B}-mag ;;
    hw) FLAGS="$FLAGS -DOL_MAGGIE -DOL_MAGGIE_HW"; OUT=${OUT}HW; B=${B}-hw ;;
    emu) FLAGS="$FLAGS -DOL_EMU"; OUT=${OUT}Emu; B=${B}-emu ;;
    prof) FLAGS="$FLAGS -DOL_PROFILE"; OUT=${OUT}Prof; B=${B}-prof ;;
    *) echo "unknown option $a"; exit 1 ;;
  esac
done
mkdir -p $B
[ -f $B/stb_vorbis.o ] || $CC $FLAGS -c ../../libs/stb_vorbis/stb_vorbis.c -o $B/stb_vorbis.o
[ -f $B/minimp3.o ]    || $CXX $FLAGS -std=c++11 -fno-exceptions -fno-rtti -c ../../libs/minimp3/minimp3.cpp -o $B/minimp3.o
[ -f $B/tinflate.o ]   || $CC $FLAGS -c ../../libs/tinf/tinflate.c -o $B/tinflate.o
# AMMX kernels (68080 only; the emulator build keeps the C versions)
ASMOBJ=""
case "$FLAGS" in *OL_EMU*) ;; *) $TC/bin/vasmm68k_mot -m68080 -Fhunk -quiet -o $B/mixammx.o asm/mixammx.s; ASMOBJ="$B/mixammx.o"; FLAGS="$FLAGS -DOL_AMMX -DOL_ARNE" ;; esac
$CXX $FLAGS -std=c++11 -fno-exceptions -fno-rtti -I../../ -Isdk-include -c main.cpp -o $B/main.o
$CXX -m68060 -mhard-float -Wl,-Map=$B/map.txt $B/main.o $ASMOBJ $B/stb_vorbis.o $B/minimp3.o $B/tinflate.o -lm -o $OUT
cp $OUT $B/$OUT.debug                  # unstripped, for crashaddr.py
$TC/bin/m68k-amigaos-strip $OUT
ls -l $OUT
