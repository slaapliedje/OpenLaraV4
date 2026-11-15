#!/bin/bash
# OpenLara main engine for the Apollo Vampire V4
#   ./build.sh           -> OpenLara          software renderer
#   ./build.sh mag       -> OpenLaraMag       spans drawn by Maggie (SAGA texture unit)
#   ./build.sh mag emu   -> OpenLaraMagEmu    Maggie build with a software Maggie, for emu.sh (Amiberry)
# Needs bebbo m68k-amigaos gcc 6.5. C code as 68060 (-m68080 traps on core 12846).
#   -fbbb=- turns off bebbo gcc's extra optimiser: it miscompiles (base register reused after a postincrement)
set -e
TC=${TC:-$HOME/dev/toolchains/amigaos}
CXX=$TC/bin/m68k-amigaos-g++
CC=$TC/bin/m68k-amigaos-gcc
FLAGS="-m68060 -mhard-float -O2 -fbbb=- -fomit-frame-pointer -DNDEBUG -D__AMIGA__"
OUT=OpenLara; B=build
for a in "$@"; do
  case $a in
    mag) FLAGS="$FLAGS -DOL_MAGGIE"; OUT=${OUT}Mag; B=${B}-mag ;;
    emu) FLAGS="$FLAGS -DOL_EMU"; OUT=${OUT}Emu; B=${B}-emu ;;
    *) echo "unknown option $a"; exit 1 ;;
  esac
done
mkdir -p $B
[ -f $B/stb_vorbis.o ] || $CC $FLAGS -c ../../libs/stb_vorbis/stb_vorbis.c -o $B/stb_vorbis.o
[ -f $B/minimp3.o ]    || $CXX $FLAGS -std=c++11 -fno-exceptions -fno-rtti -c ../../libs/minimp3/minimp3.cpp -o $B/minimp3.o
[ -f $B/tinflate.o ]   || $CC $FLAGS -c ../../libs/tinf/tinflate.c -o $B/tinflate.o
$CXX $FLAGS -std=c++11 -fno-exceptions -fno-rtti -I../../ -Isdk-include -c main.cpp -o $B/main.o
$CXX -m68060 -mhard-float -Wl,-Map=$B/map.txt $B/main.o $B/stb_vorbis.o $B/minimp3.o $B/tinflate.o -lm -o $OUT
cp $OUT $B/$OUT.debug                  # unstripped, for crashaddr.py
$TC/bin/m68k-amigaos-strip $OUT
ls -l $OUT
