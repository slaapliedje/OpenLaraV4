#!/bin/bash
# Builds tools/dist/make-v4-package.exe (Windows, one file) from make-v4-package.py with PyInstaller under
# Wine, with the Amiga files bundled (build them first: src/platform/amiga/build.sh hw, installer/build.sh).
# One-time setup of the build prefix (Windows Python 3.12 + PyInstaller):
#   export WINEPREFIX=~/dev/toolchains/wine-pybuild WINEARCH=win64
#   wineboot -i; wine python-3.12.8-amd64.exe /quiet InstallAllUsers=0 PrependPath=1 TargetDir='C:\Python312'
#   wine 'C:\Python312\python.exe' -m pip install pyinstaller numpy   (numpy: better music resampling)
set -e
cd "$(dirname "$0")"
export WINEPREFIX=${WINEPREFIX:-$HOME/dev/toolchains/wine-pybuild} WINEDEBUG=-all
A=../src/platform/amiga
for f in $A/OpenLaraHW $A/launcher/OpenLaraLaunch $A/installer/sfxstub $A/installer/icons/OpenLara.info $A/installer/icons/Drawer.info; do
  [ -f "$f" ] || { echo "missing $f: build the Amiga parts first"; exit 1; }
done
rm -rf build dist
wine 'C:\Python312\python.exe' -m PyInstaller --onefile --console --name make-v4-package \
  --add-data "$A/OpenLaraHW;amiga" \
  --add-data "$A/launcher/OpenLaraLaunch;amiga/launcher" \
  --add-data "$A/installer/sfxstub;amiga/installer" \
  --add-data "$A/installer/icons/OpenLara.info;amiga/installer/icons" \
  --add-data "$A/installer/icons/Drawer.info;amiga/installer/icons" \
  make-v4-package.py
rm -rf build make-v4-package.spec
ls -l dist/make-v4-package.exe
