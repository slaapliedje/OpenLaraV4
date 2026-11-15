#!/bin/bash
# OpenLara main engine for the Apollo Vampire V4 (software renderer experiment)
#   -fbbb=- turns off bebbo gcc's extra optimiser: it miscompiles (base register reused after a postincrement)
#   ./build.sh   -> OpenLara (needs bebbo m68k-amigaos gcc 6.5; C code as 68060: -m68080 traps on core 12846)
set -e
TC=${TC:-$HOME/dev/toolchains/amigaos}
CXX=$TC/bin/m68k-amigaos-g++
CC=$TC/bin/m68k-amigaos-gcc
FLAGS="-m68060 -mhard-float -O2 -fbbb=- -fomit-frame-pointer -DNDEBUG -D__AMIGA__"
mkdir -p build
$CC  $FLAGS -c ../../libs/stb_vorbis/stb_vorbis.c -o build/stb_vorbis.o
$CXX $FLAGS -std=c++11 -fno-exceptions -fno-rtti -c ../../libs/minimp3/minimp3.cpp -o build/minimp3.o
$CC  $FLAGS -c ../../libs/tinf/tinflate.c -o build/tinflate.o
$CXX $FLAGS -std=c++11 -fno-exceptions -fno-rtti -I../../ -Isdk-include -c main.cpp -o build/main.o
$CXX -m68060 -mhard-float build/main.o build/stb_vorbis.o build/minimp3.o build/tinflate.o -lm -o OpenLara
$TC/bin/m68k-amigaos-strip OpenLara
ls -l OpenLara
