# OpenLara on the Apollo Vampire V4

A port of OpenLara's main engine to the Apollo Vampire V4 (V4SA and the V4 accelerators: 68080 CPU,
SAGA chipset), with a native back end for **Maggie**, SAGA's texture-mapping unit.

Work in progress. Tested on a V4SA with core 12846 and CoffinOS r65, with the Tomb Raider 1 PC data.

## What it does

- **Maggie back end** (`src/gapi/maggie.h`, `maggie_raster.h`, `maggie_sw.h`): the CPU transforms,
  lights and clips; the rasteriser walks each polygon with subpixel accuracy and hands Maggie one span
  per line. True-colour DXT1 textures with mipmaps, bilinear filtering, a 16-bit z-buffer,
  perspective-correct texturing within a pixel error bound, Maggie's add and multiply blending, and
  CPU blending for what Maggie cannot do (Lara's shadow, translucent UI). Depth is log2(w) (a
  float's bits as an integer), exact at every perspective piece: z/w's 16 bits ran out a sector or
  two from the camera and let rooms behind walls show through.
- **Big-endian** level loading (the PC data is little-endian).
- **Sound on Arne**, SAGA's 16 DMA channels (`arne.h`): each sound effect plays one-shot on its own
  channel and the CD music plays from RAM on one stereo channel, looped, with no mixing by the CPU.
  What is left for the software mixer (AHI) is the FMV soundtracks and Ogg music; its volume step
  uses an **AMMX** kernel (`asm/mixammx.s`).
- The PC **FMVs** (`FMV/*.RPL`) and **CD audio**: 16-bit PCM AIFF preferred
  (`audio/1/track_NN.aiff`, 44.1, 22.05 or 11.025 kHz; read into RAM by the sound task while it
  plays), or Ogg.
- Keyboard and the V4's USB joypad; the game pauses when another screen is in front.

## Building

Needs bebbo's m68k-amigaos gcc 6.5 (`TC`, default `~/dev/toolchains/amigaos`) and its `vasmm68k_mot`.

```
cd src/platform/amiga
./build.sh hw          # OpenLaraHW: the Maggie back end
./build.sh hw prof     # with stage profiling and counters
```

Don't build with `-m68080`: gcc emits Line-A instructions that trap on current cores. C is compiled
for the 68060; the 68080-only code (AMMX) is in assembly.

## Running

Copy `OpenLaraHW` next to the game's `DATA/` folder (and optionally `FMV/` and `audio/1/`). Pick a
16-bit screen mode of the size wanted with `SetEnv OLRES 640x480` (default 320x240), then run
`OpenLaraHW`, or `OpenLaraHW DATA/LEVEL2.PHD` to start a level. F10 quits.

Environment variables (ENV: or local): `OLRES`, `OLDRAWDIST` (sectors, 8-45), `OLPERSPERR` (texture
error bound in pixels), `OLNOARNE` (mix the effects in software), `OLNOSOUND`, `OLREVERB`,
`OLAHIUNIT`, and A/B switches `OLZPROJ` (z/w depth), `OLNOZ`, `OLNOPERSP`, `OLNOCELLS`, `OLNONCULL`, `OLNOFACES`,
`OLNOALPHA`.

Off-screen benchmark runs: `OpenLaraHW DATA/LEVEL2.PHD 60 RAM:out [script]` renders 60 frames
without a screen and writes per-frame times to `RAM:out.txt`. The `tools/` scripts drive the V4 over
the network (`V4_HOST`).
