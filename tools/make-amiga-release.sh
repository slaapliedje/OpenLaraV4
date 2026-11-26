#!/bin/bash
# make-amiga-release.sh [OUT]: the downloadable OpenLara package for the Vampire V4 - no game data in it.
# The user copies their own Tomb Raider CD image (.cue + image data) into the unpacked OpenLaraMain drawer
# and runs MakeGameData there (src/platform/amiga/installer/makegamedata.c).
# Build the Amiga parts first: src/platform/amiga/build.sh hw, installer/build.sh.
set -e
cd "$(dirname "$0")/.."
A=src/platform/amiga
OUT=${1:-tools/dist}
LHA=${LHA:-$(command -v lha-unix || echo "$HOME/dev/toolchains/amigaos-gcc16/bin/lha")}
for f in $A/OpenLaraHW $A/launcher/OpenLaraLaunch $A/installer/MakeGameData $A/installer/icons/OpenLara.info $A/installer/icons/Drawer.info; do
  [ -f "$f" ] || { echo "missing $f: build the Amiga parts first"; exit 1; }
done
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
D=$STAGE/OpenLaraMain
mkdir -p "$D"
cp $A/OpenLaraHW $A/installer/MakeGameData "$D/"
cp $A/launcher/OpenLaraLaunch "$D/OpenLara 4-3"
cp $A/launcher/OpenLaraLaunch "$D/OpenLara 16-9"
for n in "OpenLara 4-3" "OpenLara 16-9" MakeGameData; do cp $A/installer/icons/OpenLara.info "$D/$n.info"; done
cp $A/installer/icons/Drawer.info "$STAGE/OpenLaraMain.info"
cat > "$D/ReadMe.txt" <<'EOF'
OpenLara for the Apollo Vampire V4
==================================

OpenLara is an open-source re-implementation of the Tomb Raider (1996) engine. This is its port to the
Vampire V4 (V4SA and the V4 accelerators): 68080 CPU, Maggie 3D texturing, Arne audio. The game data is
not included - you need your own copy of Tomb Raider for the PC.

Getting the game
----------------
The classic PC version, which comes as a CD image:
  GOG:   https://www.gog.com/en/game/tomb_raider_123   (Tomb Raider 1+2+3)
  Steam: https://store.steampowered.com/app/224960/Tomb_Raider_I/   (Tomb Raider I, 1996)
or your original Tomb Raider PC CD, ripped as a .cue + .bin image.
(Tomb Raider I-III Remastered is a different build and does not work.)

Installing
----------
1. Unpack this archive somewhere with about 400 MB free (the game data is ~390 MB).
2. Copy your CD image into the OpenLaraMain drawer: the .cue file and every file it names (a .bin, or
   GOG's / Steam's image file, and any separate music track files).
3. Double-click MakeGameData (or run it from the Shell). It writes DATA, FMV and the CD music
   (audio/1/track_NN.aiff) into the drawer. Afterwards the CD image can be deleted.
   Music tracks shipped as Ogg/MP3 files can't be converted on the Amiga: use the PC tool in the source
   (tools/make-v4-package.py, or make-v4-package.exe on Windows), which builds a complete package.

Playing
-------
Double-click "OpenLara 4-3" (640x480) or "OpenLara 16-9" (640x360, widescreen).
F10 quits. Esc is the inventory; the V4's USB joypad works too.
From the Shell: SetEnv OLRES 640x480 (any 16-bit screen mode of that size), then OpenLaraHW.

Source and more switches: https://github.com/slaapliedje/OpenLaraV4 (src/platform/amiga/README.md)
OpenLara: https://github.com/XProger/OpenLara (BSD 2-clause licence).
Tomb Raider is a trademark of its owners; OpenLara is not affiliated with them.
EOF
mkdir -p "$OUT"
ARC=$(realpath "$OUT")/OpenLaraV4.lha
rm -f "$ARC"
( cd "$STAGE" && "$LHA" ao5q "$ARC" OpenLaraMain OpenLaraMain.info )   # lh5, readable by every Amiga LhA
ls -l "$ARC"
