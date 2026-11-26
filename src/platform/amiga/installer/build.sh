#!/bin/bash
# Builds what tools/make-v4-package.py packs besides OpenLaraHW (../build.sh hw):
#   sfxstub                 the self-extractor (overlay header first, so AmigaDOS stops loading at the archive)
#   ../launcher/OpenLaraLaunch   the 4:3 / 16:9 Workbench launcher
#   MakeGameData            builds the game data from a CD image on the Amiga (tools/make-amiga-release.sh)
#   icons/*.info            the icons (mkicons.py)
set -e
cd "$(dirname "$0")"
TC=${TC:-$HOME/dev/toolchains/amigaos}
CC=$TC/bin/m68k-amigaos-gcc
CRT0=$($CC -noixemul -m68020 -print-file-name=crt0.o)
$CC -Os -m68020 -noixemul -nostartfiles -Wall -Wno-unused-variable -o sfxstub ovlhead.s "$CRT0" sfx.c
$CC -O2 -m68020 -noixemul -s -o ../launcher/OpenLaraLaunch ../launcher/launch.c
$CC -O2 -m68020 -noixemul -s -Wall -o MakeGameData makegamedata.c   # the CD image -> game data, on the Amiga
python3 mkicons.py
ls -l sfxstub ../launcher/OpenLaraLaunch icons/*.info
