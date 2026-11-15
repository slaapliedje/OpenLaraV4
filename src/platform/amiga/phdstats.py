#!/usr/bin/env python3
"""Level statistics of a TR1 PC .PHD, in the same format as the Amiga build's headless dump
(main.cpp dumpLevelStats), so a big-endian loading mistake shows up as a mismatching line.
    ./phdstats.py DATA/GYM.PHD
"""
import struct
import sys

M = 0xFFFFFFFF


class R:
    def __init__(self, d):
        self.d, self.p = d, 0

    def take(self, fmt):
        v = struct.unpack_from('<' + fmt, self.d, self.p)
        self.p += struct.calcsize('<' + fmt)
        return v

    def u8(self): return self.take('B')[0]
    def u16(self): return self.take('H')[0]
    def i16(self): return self.take('h')[0]
    def u32(self): return self.take('I')[0]
    def i32(self): return self.take('i')[0]
    def skip(self, n): self.p += n
    def arr(self, fmt, n): return list(self.take('%d%s' % (n, fmt))) if n else []


def main(path):
    r = R(open(path, 'rb').read())
    assert r.u32() == 0x20, 'not a TR1 PC level'
    r.skip(r.u32() * 65536)        # 8-bit tiles
    r.skip(4)
    rooms = []
    for _ in range(r.u16()):
        info = r.take('iiii')
        r.skip(4)                  # data size
        verts = [r.take('hhhh')[:3] for _ in range(r.u16())]
        r.skip(r.u16() * 10)       # quads
        r.skip(r.u16() * 8)        # triangles
        r.skip(r.u16() * 4)        # sprites
        portals = [r.take('H3h12h') for _ in range(r.u16())]
        z, x = r.u16(), r.u16()
        r.skip(z * x * 8)          # sectors
        r.skip(2)                  # ambient
        r.skip(r.u16() * 18)       # lights
        r.skip(r.u16() * 18)       # static meshes in the room
        r.skip(4)                  # alternate room, flags
        rooms.append((info, verts, portals))
    floors = r.arr('H', r.u32())
    meshData = r.u32(); r.skip(meshData * 2)
    meshOffsets = r.u32(); r.skip(meshOffsets * 4)
    anims = [r.take('IBBHiiHHHHHHHH') for _ in range(r.u32())]
    states = r.u32(); r.skip(states * 6)
    ranges = r.u32(); r.skip(ranges * 8)
    commands = r.u32(); r.skip(commands * 2)
    nodes = r.arr('I', r.u32())
    frames = r.arr('H', r.u32())
    models = r.u32(); r.skip(models * 18)
    statics = [r.take('IH6h6hH') for _ in range(r.u32())]
    objtex = [r.take('HH16B') for _ in range(r.u32())]
    sprtex = [r.take('HBBHHhhhh') for _ in range(r.u32())]
    r.skip(r.u32() * 8)            # sprite sequences
    cameras = r.u32(); r.skip(cameras * 16)
    sources = r.u32(); r.skip(sources * 16)
    boxes = [r.take('iiiihH') for _ in range(r.u32())]
    overlaps = r.u32(); r.skip(overlaps * 2)
    r.skip(len(boxes) * 2 * 6)     # zones
    r.skip(r.u32() * 2)            # animated textures
    ents = [r.take('hhiiihhH') for _ in range(r.u32())]

    print('rooms %d floors %d meshData %d meshOffsets %d anims %d states %d nodes %d frames %d models %d statics %d' %
          (len(rooms), len(floors), meshData, meshOffsets, len(anims), states, len(nodes), len(frames), models, len(statics)))
    print('objTex %d sprTex %d cameras %d soundSources %d boxes %d overlaps %d entities %d' %
          (len(objtex), len(sprtex), cameras, sources, len(boxes), overlaps, len(ents)))
    a = b = c = 0
    for info, verts, portals in rooms:
        a += info[0] + info[1] * 3 + info[2] * 5 + info[3] * 7
        for v in verts: b += v[0] + v[1] * 3 + v[2] * 5
        for p in portals: c += p[0] + p[2] * 3 + p[4 + 2 * 3] * 5   # roomIndex, normal.y, vertices[2].x
    i0, v0, _ = rooms[0]
    print('room0 %d %d %d %d v%d  roomsum %d vertsum %d portalsum %d' % (i0[0], i0[1], i0[2], i0[3], len(v0), a & M, b & M, c & M))
    a = sum(v * (i + 1) for i, v in enumerate(floors))
    b = sum(v * (i + 1) for i, v in enumerate(frames))
    c = sum(v * (i + 1) for i, v in enumerate(nodes))
    print('floorsum %d framesum %d nodesum %d' % (a & M, b & M, c & M))
    a = sum(an[0] + an[3] * 3 + an[4] * 5 + an[8] * 7 for an in anims)
    b = sum(bx[0] + bx[3] * 3 + bx[4] * 5 + bx[5] * 7 for bx in boxes)
    c = sum((e[0] & 0xFFFF) + e[1] * 3 + e[7] * 5 for e in ents)
    print('animsum %d boxsum %d entsum %d' % (a & M, b & M, c & M))
    # object texture: attribute, tile, then (xh, x, yh, y) x 4 -> texCoord[2].x = byte 2*4+1, texCoord[3].y = 3*4+3
    a = sum((t[1] & 0x3FFF) + t[2 + 9] * 3 + t[2 + 15] * 5 for t in objtex)
    b = sum(s[0] + s[5] * 3 + s[8] * 5 for s in sprtex)
    c = sum(s[0] + s[1] * 3 + s[2] * 5 + s[13] * 7 for s in statics)
    print('objtexsum %d sprtexsum %d staticsum %d' % (a & M, b & M, c & M))
    e = ents[0]
    print('entity0 type %d room %d pos %d %d %d rot %d flags %04x' % (e[0], e[1], e[2], e[3], e[4], e[5] & 0xFFFF, e[7]))


if __name__ == '__main__':
    main(sys.argv[1])
