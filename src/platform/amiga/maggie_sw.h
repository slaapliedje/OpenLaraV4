// Maggie (Apollo Vampire SAGA texture-mapping unit, registers at $DFF250) behind the software GAPI:
// gapi/sw.h keeps its transform, clipping and edge walking, and hands each scanline span to Maggie
// instead of filling pixels itself. Built with -DOL_MAGGIE (and -DOL_EMU for the emulator build,
// where a software model of Maggie draws the spans: Amiberry has no Maggie).
//
// The level's 8-bit texture pages are converted to DXT1 (Maggie's texture format) with the level
// palette, index 0 transparent like the software renderer. Light rows become Maggie's intensity,
// flat-coloured faces use a white texture tinted by the palette colour, and the underwater palette
// becomes a colour multiply. No AmigaOS headers here: memory and cache calls go through the os*
// functions in platform/amiga/main.cpp.
#ifndef H_MAGGIE_SW
#define H_MAGGIE_SW

void* osMagAlloc(int size);              // MEMF_ANY | MEMF_CLEAR
void  osMagFree(void *ptr);
void  osMagCacheFlush();                 // CacheClearU: textures written by the CPU, read by Maggie

namespace Mag {

// --- registers (layout from MaggieLibrary/maggie_internal.h) ---------------------------------
#ifdef OL_EMU
    struct EmuStart { void operator = (uint16 len) volatile; };
    #define MAG_START_T EmuStart
#else
    #define MAG_START_T uint16
#endif

    struct Regs {
        void   *texture;        //  0 DXT1 texture, 32-byte aligned
        void   *pixDest;        //  4 first pixel of the span
        void   *depthDest;      //  8 z-buffer address
        uint16  unused0;        // 12
        MAG_START_T startLength;// 14 span length: writing it starts the span
        uint16  texSize;        // 16 log2 size (8 = 256)
        uint16  mode;           // 18 bit0 bilinear, bit1 z-buffer, bit2 16-bit output
        uint16  unused1;        // 20
        uint16  modulo;         // 22 bytes per pixel
        uint32  uDeltaDelta;    // 24
        uint32  vDeltaDelta;    // 28
        uint32  uCoord;         // 32 8:24, 1.0 = whole texture width
        uint32  vCoord;         // 36
        int32   uDelta;         // 40 per pixel, 8:24
        int32   vDelta;         // 44
        uint16  light;          // 48 intensity, 0xFFFF = full texture colour
        int16   lightDelta;     // 50
        uint32  lightRGBA;      // 52 colour multiply 0x00RRGGBB
        uint32  depthStart;     // 56
        int32   depthDelta;     // 60
    } __attribute__((packed));

#ifdef OL_EMU
    Regs emuRegs;
    #define MAG_REGS (&emuRegs)
#else
    #define MAG_REGS ((volatile Regs*)0xDFF250)
#endif

// --- DXT1 encoder (Maggie reads standard little-endian DXT1, blocks row-major) ---------------
    inline uint16 rgb565(int r, int g, int b) { return uint16(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)); }

    void encodeBlock(const uint16 *c, const uint8 *opaque, uint8 *dst) {
        int n = 0, anyTrans = 0, best = -1;
        uint16 lo = 0, hi = 0;
        for (int i = 0; i < 16; i++) { if (!opaque[i]) anyTrans = 1; else n++; }
        for (int i = 0; i < 16; i++) {               // endpoints: the two most distant opaque colours
            if (!opaque[i]) continue;
            for (int j = i; j < 16; j++) {
                if (!opaque[j]) continue;
                int dr = ((c[i] >> 11) & 31) - ((c[j] >> 11) & 31);
                int dg = (((c[i] >> 5) & 63) - ((c[j] >> 5) & 63)) >> 1;
                int db = (c[i] & 31) - (c[j] & 31);
                int d = dr * dr + dg * dg + db * db;
                if (d > best) { best = d; lo = c[i]; hi = c[j]; }
            }
        }
        uint16 col0, col1;
        int r[4], g[4], b[4], ncol;
        if (n == 0) { col0 = col1 = 0; ncol = 0; }
        else if (anyTrans || lo == hi) { col0 = lo < hi ? lo : hi; col1 = lo < hi ? hi : lo; ncol = 3; } // index 3 = transparent
        else { col0 = lo > hi ? lo : hi; col1 = lo > hi ? hi : lo; ncol = 4; }
        r[0] = (col0 >> 11) & 31; g[0] = (col0 >> 5) & 63; b[0] = col0 & 31;
        r[1] = (col1 >> 11) & 31; g[1] = (col1 >> 5) & 63; b[1] = col1 & 31;
        if (ncol == 4) {
            r[2] = (2 * r[0] + r[1]) / 3; g[2] = (2 * g[0] + g[1]) / 3; b[2] = (2 * b[0] + b[1]) / 3;
            r[3] = (r[0] + 2 * r[1]) / 3; g[3] = (g[0] + 2 * g[1]) / 3; b[3] = (b[0] + 2 * b[1]) / 3;
        } else {
            r[2] = (r[0] + r[1]) / 2; g[2] = (g[0] + g[1]) / 2; b[2] = (b[0] + b[1]) / 2;
        }
        uint32 pix = 0;
        for (int i = 0; i < 16; i++) {
            int idx = 3;
            if (opaque[i] && ncol) {
                int cr = (c[i] >> 11) & 31, cg = (c[i] >> 5) & 63, cb = c[i] & 31, bd = 1 << 30;
                for (int k = 0; k < ncol; k++) {
                    int dr = cr - r[k], dg = (cg - g[k]) >> 1, db = cb - b[k];
                    int d = dr * dr + dg * dg + db * db;
                    if (d < bd) { bd = d; idx = k; }
                }
            }
            pix |= uint32(idx) << (i * 2);
        }
        dst[0] = col0 & 0xFF; dst[1] = col0 >> 8;
        dst[2] = col1 & 0xFF; dst[3] = col1 >> 8;
        dst[4] = pix & 0xFF;  dst[5] = (pix >> 8) & 0xFF; dst[6] = (pix >> 16) & 0xFF; dst[7] = pix >> 24;
    }

    // 8-bit indexed size x size image -> DXT1 (index 0 transparent when alpha0)
    void encodeIndexed(const uint8 *src, const uint16 *pal565, int size, uint8 *dst, bool alpha0 = true) {
        uint16 c[16]; uint8 op[16];
        for (int by = 0; by < size; by += 4)
            for (int bx = 0; bx < size; bx += 4) {
                for (int i = 0; i < 4; i++)
                    for (int j = 0; j < 4; j++) {
                        uint8 idx = src[(by + i) * size + bx + j];
                        c[i * 4 + j]  = pal565[idx];
                        op[i * 4 + j] = idx != 0 || !alpha0;
                    }
                encodeBlock(c, op, dst);
                dst += 8;
            }
    }

// --- state -----------------------------------------------------------------------------------
    #define MAG_MAX_PAGES 32

    // two DXT1 copies of each page: opaque (index 0 drawn, for opaque faces: with bilinear filtering
    // a transparent texel next to a face's texture cell would leave see-through dots along edges)
    // and alpha (index 0 transparent, for alpha-tested faces and sprites); one copy if no index 0
    uint8   *pageMem[MAG_MAX_PAGES], *pageTex[MAG_MAX_PAGES];
    uint8   *pageMemA[MAG_MAX_PAGES], *pageTexA[MAG_MAX_PAGES];
    int      pageCount;
    const Tile8 *tiles;                  // level pages (GAPI texture proxy), set by setTiles()
    int      tilesCount;
    const Tile8 *uploadedTiles;
    uint16   pal565[256];
    uint32   palRGB[256];
    bool     palValid;
    uint8   *whiteMem, *whiteTex;        // 32x32 white: flat-coloured faces
    uint8   *sink;                       // never freed (see hwSink in the fixed-engine port)
    float    rcp[1024];                  // 1/n for span deltas
    uint32   rcpi[1024];                 // 65536/n

    uint8   *curTex;
    uint16   curTexSize, curMode;
    uint32   curRGB;
    uint32   tint;                       // 0x00RRGGBB multiply (water), 0xFFFFFF = none
    bool     shading;

    // depth buffer: the software renderer has none (draw order only); Maggie tests and writes 16-bit
    // depth per pixel, smaller = nearer. Values are the engine's z/w * 65535, linear in screen space.
    // Profiling build (-DOL_PROFILE, ./build.sh mag prof): ENV:OLDBG=n stops the renderer early at
    // stage n - 4 no draw calls, 5 after the corner transform, 6 before lighting, 1 before
    // rasterising, 2 before the span registers, 3 no dynamic lights - and counts are printed at exit.
    // Time the same off-screen frames at each stage: the differences are the stage costs.
#ifdef OL_PROFILE
    int      dbg = 0;
    unsigned cnt[8];             // polygons, corners, drawn, spans, pixels
    #define MAG_STAGE(n)   (Mag::dbg == (n))
    #define MAG_COUNT(i, n) (Mag::cnt[i] += (n))
#else
    #define MAG_STAGE(n)   false
    #define MAG_COUNT(i, n)
#endif
    bool     perspective = true;         // F6 toggles: spans split into perspective-correct pieces
    #define  MAG_PERSP_STEP 64           // pixels per affine piece
    #define  MAG_PERSP_RATIO 1.3f        // 1/w varying less than this across a polygon/span: affine
    bool     zEnabled = true;            // F7 toggles
    uint8   *zMem;
    uint16  *zbuf;
    int      zWidth, zHeight;
    bool     zOn;                        // this frame
    bool     zTest = true;               // GAPI::setDepthTest (off for 2D: UI text, sprites on screen)
    bool     zSpan;                      // current spans test/write depth
    bool     alphaTest;                  // GAPI::setAlphaTest: transparent texels (alpha page)
    bool     bilinear = true;            // F8 toggles: filtered textures on 3D faces
    bool     mipmaps = true;             // F9 toggles (hardware back end): Maggie's automatic mip level
    bool     curFlat;                    // the bound texture is the white one (flat colour)

    // Maggie mode: bit2 16-bit output, bit1 z test/write, bit0 bilinear (3D textured faces only:
    // not UI text, which would blur, nor flat colours)
    inline void depthMode() {
        bool z = zOn && zTest;
        uint16 mode = 0x0004 | (z ? 0x0002 : 0) | (bilinear && zTest && !curFlat ? 0x0001 : 0);
        zSpan = z;
        if (mode != curMode) {
            curMode = mode;
            MAG_REGS->mode = mode;
        }
    }

    // The hardware back end (gapi/maggie.h) composes the mode itself:
    // bit 0 bilinear, 1 z test, 2 16-bit output, 3 no z write, 6 add, 7 multiply
    bool zClean = false;                 // nothing drawn into the depth buffer since its last clear

    inline void setMode(uint16 mode) {
        zSpan = (mode & 2) != 0;
        if (zSpan) zClean = false;
        if (mode != curMode) {
            curMode = mode;
            MAG_REGS->mode = mode;
        }
    }

    // RGBA8888 (alpha < 128 = transparent when alpha is set) size x size -> DXT1. Endpoints are the
    // darkest and brightest opaque texel of each block: quick, and fine for game textures.
    void encodeRGBA(const uint8 *src, int stride, int size, uint8 *dst, bool alpha) {
        for (int by = 0; by < size; by += 4)
            for (int bx = 0; bx < size; bx += 4, dst += 8) {
                int r[16], g[16], b[16], lum[16], lo = -1, hi = -1, trans = 0;
                uint32 opaque = 0;
                for (int i = 0; i < 16; i++) {
                    const uint8 *p = src + ((by + (i >> 2)) * stride + bx + (i & 3)) * 4;
                    r[i] = p[0]; g[i] = p[1]; b[i] = p[2];
                    if (alpha && p[3] < 128) { trans = 1; continue; }
                    opaque |= 1u << i;
                    lum[i] = r[i] * 2 + g[i] * 5 + b[i];
                    if (lo < 0 || lum[i] < lum[lo]) lo = i;
                    if (hi < 0 || lum[i] > lum[hi]) hi = i;
                }
                if (!opaque) { dst[0] = dst[1] = dst[2] = dst[3] = 0; dst[4] = dst[5] = dst[6] = dst[7] = 0xFF; continue; }
                uint16 c0 = rgb565(r[hi], g[hi], b[hi]), c1 = rgb565(r[lo], g[lo], b[lo]);
                int pr[4], pg[4], pb[4], n;
                if (trans || c0 == c1) {             // 3 colours + transparent: col0 <= col1
                    if (c0 > c1) { uint16 t = c0; c0 = c1; c1 = t; int k = lo; lo = hi; hi = k; }
                    n = 3;
                } else {                             // 4 colours: col0 > col1
                    if (c0 < c1) { uint16 t = c0; c0 = c1; c1 = t; int k = lo; lo = hi; hi = k; }
                    n = 4;
                }
                pr[0] = r[hi]; pg[0] = g[hi]; pb[0] = b[hi];
                pr[1] = r[lo]; pg[1] = g[lo]; pb[1] = b[lo];
                if (n == 4) {
                    pr[2] = (2 * pr[0] + pr[1]) / 3; pg[2] = (2 * pg[0] + pg[1]) / 3; pb[2] = (2 * pb[0] + pb[1]) / 3;
                    pr[3] = (pr[0] + 2 * pr[1]) / 3; pg[3] = (pg[0] + 2 * pg[1]) / 3; pb[3] = (pb[0] + 2 * pb[1]) / 3;
                } else {
                    pr[2] = (pr[0] + pr[1]) / 2; pg[2] = (pg[0] + pg[1]) / 2; pb[2] = (pb[0] + pb[1]) / 2;
                }
                uint32 pix = 0;
                for (int i = 0; i < 16; i++) {
                    int idx = 3;
                    if (opaque & (1u << i)) {
                        int bd = 1 << 30;
                        for (int k = 0; k < n; k++) {
                            int dr = r[i] - pr[k], dg = g[i] - pg[k], db = b[i] - pb[k];
                            int d = dr * dr * 2 + dg * dg * 4 + db * db;
                            if (d < bd) { bd = d; idx = k; }
                        }
                    }
                    pix |= uint32(idx) << (i * 2);
                }
                dst[0] = c0 & 0xFF; dst[1] = c0 >> 8;
                dst[2] = c1 & 0xFF; dst[3] = c1 >> 8;
                dst[4] = pix & 0xFF; dst[5] = (pix >> 8) & 0xFF; dst[6] = (pix >> 16) & 0xFF; dst[7] = pix >> 24;
            }
    }

    uint8* allocTex(int bytes, uint8 **mem) {
        *mem = (uint8*)osMagAlloc(bytes + 32);
        return *mem ? (uint8*)((uint32(*mem) + 31) & ~31U) : NULL;
    }

    void freePages() {
        for (int i = 0; i < pageCount; i++) {
            if (pageMem[i]) { osMagFree(pageMem[i]); pageMem[i] = pageTex[i] = NULL; }
            if (pageMemA[i]) { osMagFree(pageMemA[i]); pageMemA[i] = NULL; }
            pageTexA[i] = NULL;
        }
        pageCount = 0;
        uploadedTiles = NULL;
    }

    void uploadPages() {
        freePages();
        int count = min(tilesCount, MAG_MAX_PAGES);
        for (int t = 0; t < count; t++) {
            pageTex[t] = allocTex(256 * 256 / 2, &pageMem[t]);
            if (pageTex[t]) encodeIndexed(tiles[t].index, pal565, 256, pageTex[t], false);
            pageTexA[t] = pageTex[t];
            pageMemA[t] = NULL;
            bool anyZero = false;
            for (int i = 0; i < 65536 && !anyZero; i++) anyZero = tiles[t].index[i] == 0;
            if (anyZero) {
                uint8 *a = allocTex(256 * 256 / 2, &pageMemA[t]);
                if (a) { encodeIndexed(tiles[t].index, pal565, 256, a, true); pageTexA[t] = a; }
            }
        }
        pageCount = count;
        uploadedTiles = tiles;
        osMagCacheFlush();
    }

    void setTiles(const Tile8 *t, int count) {
        tiles = t;
        tilesCount = count;
    }

    void setPalette(const Color24 *pal) {
        for (int i = 0; i < 256; i++) {
            pal565[i] = rgb565(pal[i].r, pal[i].g, pal[i].b);
            palRGB[i] = (uint32(pal[i].r) << 16) | (uint32(pal[i].g) << 8) | pal[i].b;
        }
        palValid = true;
        uploadedTiles = NULL;            // re-encode with the new palette
    }

    bool init() {
        rcp[0] = 0.0f;
        for (int i = 1; i < 1024; i++) rcp[i] = 1.0f / float(i);
        rcpi[0] = 0;
        for (int i = 1; i < 1024; i++) rcpi[i] = 65536 / i;
        if (!sink) {
            uint8 *raw = (uint8*)osMagAlloc(128 + 32);
            if (raw) sink = (uint8*)((uint32(raw) + 31) & ~31U);
        }
        whiteTex = allocTex(32 * 32 / 2, &whiteMem);
        if (whiteTex) {
            uint8 idx[32 * 32];
            uint16 pal[256];
            memset(idx, 1, sizeof(idx));
            pal[1] = 0xFFFF;
            encodeIndexed(idx, pal, 32, whiteTex);
        }
        tint = 0xFFFFFF;
        shading = true;
        osMagCacheFlush();
        return whiteTex && sink;
    }

    void setDepthSize(int width, int height) {
        if (zMem && width == zWidth && height == zHeight) return;
        if (zMem) { osMagFree(zMem); zMem = NULL; zbuf = NULL; }
        zMem = (uint8*)osMagAlloc(width * height * 2 + 64);
        if (zMem) zbuf = (uint16*)((uint32(zMem) + 63) & ~63U);
        zWidth = width; zHeight = height;
    }

    void clearDepth() {                  // to far; the 080's data cache writes through
        if (!zbuf) return;
        uint32 *p = (uint32*)zbuf, *e = p + zWidth * zHeight / 2;
        while (p < e) { p[0] = p[1] = p[2] = p[3] = p[4] = p[5] = p[6] = p[7] = 0xFFFFFFFF; p += 8; }
        zClean = true;
    }

    void setup() {                       // start of a frame: registers to a known state
        volatile Regs *m = MAG_REGS;
        zOn = zEnabled && zbuf;
        if (zOn) clearDepth();
        zSpan = zOn && zTest;
        curMode = zSpan ? 0x0006 : 0x0004; // 16-bit output, point sampling, z test/write
        m->mode = curMode;
        m->modulo = 2;
        m->uDeltaDelta = 0;
        m->vDeltaDelta = 0;
        curTex = NULL; curTexSize = 0; curRGB = 0xFFFFFFFF;
    }

    // a tiny span into the never-freed sink: accepted only once the span in flight has finished
    void sinkSpan() {
        if (!sink) return;
        volatile Regs *m = MAG_REGS;
        m->texture = sink; m->texSize = 5; curTex = NULL; curTexSize = 0;
        if (zSpan) { m->depthDest = sink + 64; m->depthStart = 0xFFFF0000; m->depthDelta = 0; }
        m->pixDest = sink + 32;
        m->uCoord = 0; m->vCoord = 0; m->uDelta = 0; m->vDelta = 0;
        m->light = 0; m->lightDelta = 0;
        m->startLength = 4;
    }

    void sync() {                        // Maggie has finished writing: the CPU may use the frame
        sinkSpan();
        sinkSpan();
    }

    void deinit() {
        sync();                          // nothing may still be heading for memory we free
        freePages();
        if (zMem) { osMagFree(zMem); zMem = NULL; zbuf = NULL; }
        if (whiteMem) { osMagFree(whiteMem); whiteMem = whiteTex = NULL; }
    }

    inline void texture(uint8 *tex, uint16 size) {
        if (tex != curTex || size != curTexSize) {
            volatile Regs *m = MAG_REGS;
            m->texture = tex;
            m->texSize = size;
            curTex = tex; curTexSize = size;
        }
    }

    inline void colour(uint32 rgb) {
        if (rgb != curRGB) { MAG_REGS->lightRGBA = rgb; curRGB = rgb; }
    }

    inline uint32 mulRGB(uint32 a, uint32 b) {
        return (((((a >> 16) & 255) * ((b >> 16) & 255)) / 255) << 16) |
               (((((a >> 8) & 255) * ((b >> 8) & 255)) / 255) << 8) |
               (((a & 255) * (b & 255)) / 255);
    }

    // Select what the next spans sample: the page of the bound tile, or white x palette colour for
    // flat-coloured faces. Returns false if nothing can be drawn.
    bool bindTile(const Tile8 *tile) {
        if (tiles && palValid && uploadedTiles != tiles) uploadPages();
        int t = int(tile - tiles);
        if (t < 0 || t >= pageCount || !pageTex[t]) return false;
        // 2D draws (depth test off: text, bars, pictures) never set alpha test but rely on index 0
        texture(alphaTest || !zTest ? pageTexA[t] : pageTex[t], 8);
        curFlat = false;
        colour(tint);
        return true;
    }

    void bindFlat(int index) {
        texture(whiteTex, 5);
        curFlat = true;
        colour(mulRGB(palRGB[index & 255], tint));
    }

    // light row (16.16, 0..31, from the software lightmap: row 16 = plain texture) -> intensity
    inline int32 rowToLight(int32 row16) {
        int32 l = ((32 << 16) - row16) >> 4;
        return l < 0 ? 0 : (l > 0xFFFF ? 0xFFFF : l);
    }

    // One span. u/v in 16.16 texels of a 256 page (= Maggie 8:24 normalised), l = light row 16.16.
    // Maggie holds one span: work everything out first, then write the registers in one burst
    // ending with startLength; the CPU walks the next scanline while Maggie draws this one.
    __attribute__((always_inline)) inline void span(uint16 *dst, int32 len, int32 u, int32 v, int32 du, int32 dv, int32 L, int32 dl,
                     int32 offset = 0, uint32 z = 0, int32 dz = 0) {
        MAG_COUNT(3, 1); MAG_COUNT(4, len);
        if (MAG_STAGE(2)) return;
        volatile Regs *m = MAG_REGS;     // first write waits for the previous span
        if (zSpan) { m->depthDest = zbuf + offset; m->depthStart = z; m->depthDelta = dz; }
        m->pixDest = dst;
        m->uCoord = u;
        m->vCoord = v;
        m->uDelta = du;
        m->vDelta = dv;
        m->light = uint16(L);
        m->lightDelta = int16(dl);
        m->startLength = uint16(len);
    }

#ifdef OL_EMU
    // --- software Maggie for the emulator build (point sampling) -------------------------------
    void EmuStart::operator = (uint16 len) volatile {
        Regs *m = &emuRegs;
        uint16 *dst = (uint16*)m->pixDest;
        uint16 *zb = (uint16*)m->depthDest;
        bool zt = (m->mode & 2) != 0;
        uint32 z = m->depthStart;
        int32 dz = m->depthDelta;
        const uint8 *tex = (const uint8*)m->texture;
        int ts = m->texSize & 15, size = 1 << ts, shift = 24 - ts, bpr = size >> 2;
        uint32 u = m->uCoord, v = m->vCoord;
        int32 du = m->uDelta, dv = m->vDelta, l = m->light, dl = m->lightDelta;
        uint32 rgb = m->lightRGBA;
        int32 mr = (rgb >> 16) & 255, mg = (rgb >> 8) & 255, mb = rgb & 255;
        for (int i = 0; i < len; i++, dst++, zb++, u += du, v += dv, l += dl, z += dz) {
            uint16 zz = uint16(z >> 16);
            if (zt && zz > *zb) continue;
            int32 tx = (u >> shift) & (size - 1), ty = (v >> shift) & (size - 1);
            const uint8 *b = tex + ((ty >> 2) * bpr + (tx >> 2)) * 8;
            uint16 c0 = b[0] | (b[1] << 8), c1 = b[2] | (b[3] << 8);
            uint32 bits = b[4] | (b[5] << 8) | (b[6] << 16) | (uint32(b[7]) << 24);
            int32 idx = (bits >> (((ty & 3) * 4 + (tx & 3)) * 2)) & 3;
            int32 r0 = (c0 >> 11) & 31, g0 = (c0 >> 5) & 63, b0 = c0 & 31;
            int32 r1 = (c1 >> 11) & 31, g1 = (c1 >> 5) & 63, b1 = c1 & 31;
            int32 r, g, bb;
            if (c0 > c1) {
                switch (idx) {
                    case 0  : r = r0; g = g0; bb = b0; break;
                    case 1  : r = r1; g = g1; bb = b1; break;
                    case 2  : r = (2 * r0 + r1) / 3; g = (2 * g0 + g1) / 3; bb = (2 * b0 + b1) / 3; break;
                    default : r = (r0 + 2 * r1) / 3; g = (g0 + 2 * g1) / 3; bb = (b0 + 2 * b1) / 3; break;
                }
            } else {
                if (idx == 3) continue;  // transparent
                switch (idx) {
                    case 0  : r = r0; g = g0; bb = b0; break;
                    case 1  : r = r1; g = g1; bb = b1; break;
                    default : r = (r0 + r1) / 2; g = (g0 + g1) / 2; bb = (b0 + b1) / 2; break;
                }
            }
            uint32 f = uint32(l) & 0xFFFF;
            r  = int32((r  * f) >> 16) * mr / 255;
            g  = int32((g  * f) >> 16) * mg / 255;
            bb = int32((bb * f) >> 16) * mb / 255;
            if (m->mode & 0x00C0) {          // blending: add (bit 6), multiply (bit 7)
                int32 dr = (*dst >> 11) & 31, dg = (*dst >> 5) & 63, db = *dst & 31;
                if (m->mode & 0x0040) { r = min(31, r + dr); g = min(63, g + dg); bb = min(31, bb + db); }
                else                  { r = r * dr / 31; g = g * dg / 63; bb = bb * db / 31; }
            }
            *dst = uint16((r << 11) | (g << 5) | bb);
            if (zt && !(m->mode & 0x0008)) *zb = zz;
        }
    }
#endif
}

#endif
