#ifndef H_GAPI_MAGGIE
#define H_GAPI_MAGGIE

// Maggie back end: the Apollo Vampire V4's texture-mapping unit (SAGA, registers at $DFF250) driven
// straight from the engine's fixed-function path - no software-renderer palette or lightmap, no
// library in between.
//   textures   every RGBA texture of the engine as DXT1 (an opaque copy and one with 1-bit alpha),
//              any square power of two up to 1024: also the texture/LEVEL_n.png replacements
//   colour     vertex colour x light: a per-polygon RGB tint and Maggie's per-pixel intensity
//   blending   add and multiply (Maggie's blend modes); alpha blending is drawn alpha-tested
//   depth      16-bit z-buffer, test and write separately (transparent faces don't write)
//   filtering  bilinear (F8), mipmaps (F9), perspective-correct spans (F6)
// The CPU transforms, lights and clips; platform/amiga/maggie_raster.h walks the polygons with
// subpixel accuracy and hands Maggie one span per line.

#include "core.h"
#include "platform/amiga/maggie_sw.h"    // registers, DXT1 encoder, z-buffer, span

#define PROFILE_MARKER(title)
#define PROFILE_LABEL(id, name, label)
#define PROFILE_TIMING(time)

#define COLOR_16
#define COLOR_FMT_565
#define CONV_COLOR(r,g,b) (((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))

#define SW_MAX_DIST  (20.0f * 1024.0f)
#define SW_FOG_START (12.0f * 1024.0f)
#define SW_NEAR      32.0f               // near plane (w); polygons crossing it are clipped

namespace GAPI {

    using namespace Core;

    typedef ::Vertex Vertex;
    typedef uint16 ColorSW;

    ColorSW *swColor;                    // the frame being drawn (set by the platform code)
    short4   swClipRect;
    bool     swNearClip = true;
    static const bool magColored = false;    // (the software back end's palette-colour faces)

    int cullMode, blendMode;
    bool depthTest = true, depthWrite = true, alphaTest = false;

    float  ambient;
    int32  lightsCount;
    extern uint32 magLightsVersion;
    uint32 magRcp255[256];               // (255 << 16) / n, rounded up so n * rcp >> 16 = 255
    struct LightSW {
        float  intensity;                // 0..1
        vec3   pos;
        float  radius;                   // 1 / radius
    } lights[MAX_LIGHTS], lightsRel[MAX_LIGHTS];

// Shader
    struct Shader {
        void init(Pass pass, int type, int *def, int defCount) {}
        void deinit() {}
        void bind() {}
        void setParam(UniformType uType, const vec4  &value, int count = 1) {}
        void setParam(UniformType uType, const mat4  &value, int count = 1) {}
    };

// Texture
    struct Texture;
    Texture *curTex = NULL;              // bound to sDiffuse

    struct Texture {
        uint8      *memory;              // RGBA (kept: backgrounds are copied from it, updates re-encode)
        int        width, height, depth, origWidth, origHeight, origDepth;
        TexFormat  fmt;
        uint32     opt;

        uint8      *dxtMem, *dxt;        // opaque copy
        uint8      *dxtMemA, *dxtA;      // 1-bit alpha copy (the same block when nothing is transparent)
        int        sizeLog2;             // square size Maggie sees
        int32      uMul, vMul;           // texCoord (0..32767) -> Maggie 8:24
        bool       dirty;

        Texture(int width, int height, int depth, uint32 opt) : memory(0), width(width), height(height), depth(depth), origWidth(width), origHeight(height), origDepth(depth), fmt(FMT_RGBA), opt(opt),
                                                                 dxtMem(0), dxt(0), dxtMemA(0), dxtA(0), sizeLog2(0), uMul(512), vMul(512), dirty(false) {}

        void init(void *data) {
            ASSERT((opt & OPT_PROXY) == 0);
            opt &= ~(OPT_CUBEMAP | OPT_MIPMAPS);
            memory = new uint8[width * height * 4];
            if (data)
                update(data);
            else
                memset(memory, 0, width * height * 4);
        }

        void freeDXT() {
            Mag::sync();                 // no span may still read it
            if (dxtMemA) osMagFree(dxtMemA);
            if (dxtMem)  osMagFree(dxtMem);
            dxtMem = dxt = dxtMemA = dxtA = NULL;
        }

        void deinit() {
            if (curTex == this) curTex = NULL;
            freeDXT();
            delete[] memory;
            memory = NULL;
        }

        void generateMipMap() {}

        void update(void *data) {
            if (fmt == FMT_RGBA)
                memcpy(memory, data, width * height * 4);
            dirty = true;
        }

        // DXT1 copies, made when the texture is first drawn (the title pictures never are: they are
        // copied to the screen)
        void encode() {
            dirty = false;
            int side = width > height ? width : height;
            int log2 = 5;
            while ((1 << log2) < side && log2 < 10) log2++;
            int size = 1 << log2;
            if (log2 != sizeLog2) freeDXT();
            sizeLog2 = log2;
            uMul = 512 * min(width, size) / size;
            vMul = 512 * min(height, size) / size;

            // a square copy when the texture is not one (padded with its own edge texels)
            const uint8 *src = memory;
            uint8 *tmp = NULL;
            if (width != size || height != size) {
                tmp = new uint8[size * size * 4];
                for (int y = 0; y < size; y++) {
                    const uint8 *row = memory + min(y, height - 1) * width * 4;
                    uint8 *out = tmp + y * size * 4;
                    int w = min(width, size);
                    memcpy(out, row, w * 4);
                    for (int x = w; x < size; x++) memcpy(out + x * 4, row + (w - 1) * 4, 4);
                }
                src = tmp;
            }
            bool anyAlpha = false;
            for (int i = 0, n = size * size; i < n && !anyAlpha; i++) anyAlpha = src[i * 4 + 3] < 128;

            // DXT1 mip chain, level after level down to 32x32 (Maggie picks the level per span)
            int bytes = 0;
            for (int l = log2; l >= 5; l--) bytes += (1 << l) * (1 << l) / 2;
            if (!dxt) dxt = Mag::allocTex(bytes, &dxtMem);
            if (anyAlpha) {
                if (!dxtMemA) dxtA = Mag::allocTex(bytes, &dxtMemA);
                if (!dxtMemA || !dxtA) { dxtA = dxt; anyAlpha = false; }
            } else {
                if (dxtMemA) { osMagFree(dxtMemA); dxtMemA = NULL; }
                dxtA = dxt;
            }
            uint8 *level = NULL;             // the current level's RGBA when it is not src
            const uint8 *cur = src;
            int offset = 0;
            for (int l = log2; l >= 5 && dxt; l--) {
                int n = 1 << l;
                Mag::encodeRGBA(cur, n, n, dxt + offset, false);
                if (anyAlpha) Mag::encodeRGBA(cur, n, n, dxtA + offset, true);
                offset += n * n / 2;
                if (l == 5) break;
                // half size: mean of the opaque texels of each 2x2, transparent if most are
                int h = n >> 1;
                uint8 *next = new uint8[h * h * 4];
                for (int y = 0; y < h; y++)
                    for (int x = 0; x < h; x++) {
                        const uint8 *q[4] = { cur + ((y * 2) * n + x * 2) * 4, cur + ((y * 2) * n + x * 2 + 1) * 4,
                                              cur + ((y * 2 + 1) * n + x * 2) * 4, cur + ((y * 2 + 1) * n + x * 2 + 1) * 4 };
                        int r = 0, g = 0, b = 0, k = 0;
                        for (int i = 0; i < 4; i++) if (q[i][3] >= 128) { r += q[i][0]; g += q[i][1]; b += q[i][2]; k++; }
                        uint8 *o = next + (y * h + x) * 4;
                        if (k) { o[0] = uint8(r / k); o[1] = uint8(g / k); o[2] = uint8(b / k); } else { o[0] = q[0][0]; o[1] = q[0][1]; o[2] = q[0][2]; }
                        o[3] = k >= 2 ? 255 : 0;
                    }
                delete[] level;
                level = next;
                cur = next;
            }
            delete[] level;
            delete[] tmp;
            osMagCacheFlush();
        }

        void bind(int sampler) {
            Core::active.textures[sampler] = this;
            if (sampler != sDiffuse) return;
            curTex = (!this || (opt & OPT_PROXY) || !memory) ? NULL : this;
        }

        void unbind(int sampler) {}

        void setFilterQuality(int value) {
            if (value > Settings::LOW)
                opt &= ~OPT_NEAREST;
            else
                opt |= OPT_NEAREST;
        }
    };

// Mesh
    struct Mesh {
        Index        *iBuffer;
        GAPI::Vertex *vBuffer;

        int          iCount;
        int          vCount;
        bool         dynamic;

        Mesh(bool dynamic) : iBuffer(NULL), vBuffer(NULL), dynamic(dynamic) {}

        void init(Index *indices, int iCount, ::Vertex *vertices, int vCount, int aCount) {
            this->iCount  = iCount;
            this->vCount  = vCount;

            iBuffer = new Index[iCount];
            vBuffer = new Vertex[vCount];

            update(indices, iCount, vertices, vCount);
        }

        void deinit() {
            delete[] iBuffer;
            delete[] vBuffer;
        }

        void update(Index *indices, int iCount, ::Vertex *vertices, int vCount) {
            if (indices) {
                memcpy(iBuffer, indices, iCount * sizeof(indices[0]));
            }

            if (vertices) {
                memcpy(vBuffer, vertices, vCount * sizeof(vertices[0]));
            }
        }

        void bind(const MeshRange &range) const {}

        void initNextRange(MeshRange &range, int &aIndex) const {
            range.aIndex = -1;
        }
    };

    void magFree();

    void init() {
        LOG("Vendor   : %s\n", "Apollo");
        LOG("Renderer : %s\n", "Maggie (SAGA)");
        LOG("Version  : %s\n", "1.0");

        support.maxAniso       = 0;
        support.maxVectors     = 0;
        support.shaderBinary   = false;
        support.VAO            = false;
        support.VBO            = false;
        support.depthTexture   = false;
        support.shadowSampler  = false;
        support.discardFrame   = false;
        support.texNPOT        = true;       // padded to a square power of two when encoded
        support.tex3D          = false;
        support.texRG          = false;
        support.texBorder      = false;
        support.texMaxLevel    = false;
        support.colorFloat     = false;
        support.colorHalf      = false;
        support.texFloatLinear = false;
        support.texFloat       = false;
        support.texHalfLinear  = false;
        support.texHalf        = false;
        support.texMinSize     = 32;         // Maggie's smallest texture

        if (!Mag::init()) LOG("! Maggie: no memory\n");
        magRcp255[0] = 0;
        for (int i = 1; i < 256; i++) magRcp255[i] = ((255u << 16) + i - 1) / i;
        swClipRect = short4(0, 0, Core::width, Core::height);
    }

    void deinit() {
        Mag::deinit();
        magFree();
    }

    void resize() {
        swClipRect = short4(0, 0, Core::width, Core::height);
    }

    inline mat4::ProjRange getProjRange() {
        return mat4::PROJ_ZERO_POS;
    }

    mat4 ortho(float l, float r, float b, float t, float znear, float zfar) {
        mat4 m;
        m.ortho(getProjRange(), l, r, b, t, znear, zfar);
        return m;
    }

    mat4 perspective(float fov, float aspect, float znear, float zfar, float eye) {
        mat4 m;
        m.perspective(getProjRange(), fov, aspect, znear, zfar, eye);
        return m;
    }

    bool beginFrame() {
        Mag::setDepthSize(Core::width, Core::height);
        Mag::setup();
        return true;
    }

    void endFrame() {
        Mag::sync();                     // the frame is complete before it is shown
    }

    void resetState() {}

    void bindTarget(Texture *texture, int face) {}

    void discardTarget(bool color, bool depth) {}

    void copyTarget(Texture *dst, int xOffset, int yOffset, int x, int y, int width, int height) {}

    void setVSync(bool enable) {}

    void waitVBlank() {}

    void clear(bool color, bool depth) {
        if (color) {
            Mag::sync();                 // a span still being drawn would land on the cleared frame
            memset(swColor, 0, Core::width * Core::height * sizeof(ColorSW));
        }
        if (depth && Mag::zOn && !Mag::zClean) {
            Mag::sync();
            Mag::clearDepth();
        }
    }

    void setClearColor(const vec4 &color) {}

    void setViewport(const short4 &v) {}

    void setScissor(const short4 &s) {
        swClipRect.x = max(0, int(s.x));
        swClipRect.y = max(0, int(Core::active.viewport.w - (s.y + s.w)));
        swClipRect.z = min(Core::width, int(s.x + s.z));
        swClipRect.w = min(Core::height, int(Core::active.viewport.w - s.y));
    }

    void setDepthTest(bool enable) {
        depthTest = enable;
        Mag::zTest = enable;
    }

    void setDepthWrite(bool enable) {
        depthWrite = enable;
    }

    void setColorWrite(bool r, bool g, bool b, bool a) {}

    void setAlphaTest(bool enable) {
        alphaTest = enable;
    }

    void setCullMode(int rsMask) {
        cullMode = rsMask;
    }

    void setBlendMode(int rsMask) {
        blendMode = rsMask;
    }

    void setViewProj(const mat4 &mView, const mat4 &mProj) {}

    void updateLights(vec4 *lightPos, vec4 *lightColor, int count) {
        ambient = clamp(active.material.y, 0.0f, 1.0f);
        if (++magLightsVersion == 0) magLightsVersion = 1;

        lightsCount = 0;
        for (int i = 0; i < count; i++) {
            if (lightColor[i].w >= 1.0f) {
                continue;
            }
            LightSW &light = lights[lightsCount++];
            vec4 &c = lightColor[i];
            light.intensity = (c.x + c.y + c.z) * (1.0f / 3.0f);
            light.pos    = lightPos[i].xyz();
            light.radius = lightColor[i].w;
        }
    }

    // Distance fog as the engine's shaders do it: colour x exp(-distance * density), density from
    // setFog (FOG_NONE = 0 for the sky, the UI, ...). Maggie cannot add the fog colour; the levels'
    // fog colours are black or dark (0.25 at most), so fading to black is close. A table, since the
    // 68080 has no exponent instruction (FETOX would trap into the 060 package).
    #define MAG_FOG_SHIFT 7              // table step: 128 units
    #define MAG_FOG_SIZE  256            // up to 32K units
    int32  magFogTab[MAG_FOG_SIZE + 1];
    float  magFogDensity = -1.0f;        // the table's density; < 0 = none built yet
    bool   magFogOn = false, magFogDirty = true;

    // Under water (the room being drawn is a water room, set by the level's room setup): colours
    // x (0.6, 0.9, 0.9), as the engine's shaders do.
    bool  magWater = false;
    float magFogFloor = 0.0f;

    void setFog(const vec4 &params) {
        float d = params.w;
        // Maggie can only darken: a fade towards a fog colour becomes a fade to that colour's brightness
        float c = d > 0.0f ? max(params.x, max(params.y, params.z)) : 0.0f;
        if (d == magFogDensity && c == magFogFloor) return;
        magFogDensity = d; magFogFloor = c;
        magFogOn = d > 0.0f;
        magFogDirty = true;              // positions cached under the old density carry its fade
        for (int i = 0; i <= MAG_FOG_SIZE; i++) {
            float f = magFogOn ? expf(-float(i << MAG_FOG_SHIFT) * d) : 1.0f;
            magFogTab[i] = int32(256.0f * (f + (1.0f - f) * c) + 0.5f);
        }
    }

    void transformLights() {
        memcpy(lightsRel, lights, sizeof(LightSW) * lightsCount);

        mat4 mModelInv = mModel.inverseOrtho();
        for (int i = 0; i < lightsCount; i++) {
            lightsRel[i].pos = mModelInv * lights[i].pos;
        }
    }

    // per polygon: its colour multiply (the mode is set once per draw call)
    // per polygon: its colour multiply, and the white texture for faces without one (tint bit 24)
    uint8  *magDipTex;
    uint16  magDipSize;
    #define MAG_PRIM_BEGIN(pr, a) { if ((pr).tint >> 24) Mag::texture(Mag::whiteTex, 5); else Mag::texture(magDipTex, magDipSize); Mag::colour((pr).tint); }

    #include "platform/amiga/maggie_raster.h"

// --- transform ----------------------------------------------------------------------------------
    mat4  magViewProj;                   // mViewProj the cached product belongs to
    mat4  magScreenProj;                 // viewport * mViewProj
    bool  magScreenProjValid = false;
    int32 magScreenW = 0, magScreenH = 0;

    // Fixed-function lighting: with lights on, colour x (ambient + lights); without, the colour as
    // it is (rooms and sprites carry their light in the vertices). Returns 0..512: the shaders let
    // light go to twice the texture, and the fog is applied before anything is clamped.
    inline int32 magLightFactor(const Vertex &vx) {
        float x = float(vx.coord.x), y = float(vx.coord.y), z = float(vx.coord.z);
        float nx = float(vx.normal.x), ny = float(vx.normal.y), nz = float(vx.normal.z);    // length 32767
        float sum = 0.0f;
        for (int i = 0; i < lightsCount; i++) {
            const LightSW &light = lightsRel[i];
            float dx = (light.pos.x - x) * light.radius, dy = (light.pos.y - y) * light.radius, dz = (light.pos.z - z) * light.radius;
            float att = dx * dx + dy * dy + dz * dz;
            if (magBits(att) >= 0x3F800000) continue;        // beyond the light's radius
            float lum = nx * dx + ny * dy + nz * dz;
            if (magBits(lum) <= 0) continue;                 // facing away
            sum += lum * (1.0f - att) * light.intensity / sqrtf(att);
        }
        int32 f = int32((ambient + sum * (1.0f / 32767.0f)) * 256.0f);
        return f > 512 ? 512 : f;
    }

    // distance fade to black, 0..256 (w: view depth, close to the shaders' distance)
    inline int32 magFog(float w) {
        if (!magFogOn || magBits(w) <= 0) return 256;
        int32 i = int32(w) >> MAG_FOG_SHIFT;
        return magFogTab[i < MAG_FOG_SIZE ? i : MAG_FOG_SIZE];
    }

    struct MagClipVtx {
        float x, y, z, w;                // screen space times w
        int32 u, v, l;
    };

    inline void magProject(MagVtx &o, float x, float y, float z, float w, float invw, int32 u, int32 v, int32 l) {
        int32 ix = int32(x * invw * 16.0f), iy = int32(y * invw * 16.0f);
        o.x = ix > MAG_XY_MAX ? MAG_XY_MAX : (ix < -MAG_XY_MAX ? -MAG_XY_MAX : ix);
        o.y = iy > MAG_XY_MAX ? MAG_XY_MAX : (iy < -MAG_XY_MAX ? -MAG_XY_MAX : iy);
        int32 iz = int32(z * invw * (65533.0f * 32768.0f));
        o.z = (iz < 0 ? 0 : (iz > 65533 * 32768 ? 65533 * 32768 : iz)) + 32768;
        o.q = magBits(w) > 0x41800000 ? invw : 1.0f / 16.0f;     // w > 16
        o.u = u;
        o.v = v;
        o.l = l;
    }

    inline float magCross(const MagVtx *a, const MagVtx *b, const MagVtx *c) {
        return (float(b->x - a->x) * float(c->y - a->y) - float(c->x - a->x) * float(b->y - a->y)) * (1.0f / 256.0f);
    }

    // Colour of a polygon's corners -> one RGB tint (the brightest corner's hue, at full strength)
    // and an intensity per corner (<< 15, kept off both ends: see the span's light).
    // rgb[k]: 0x00RRGGBB with channels 0..255.
    inline uint32 magTint(const uint32 *rgb, int count, int32 *l) {
        int32 mx[4], best = 0, bi = 0;
        bool grey = true;
        for (int k = 0; k < count; k++) {
            int32 r = (rgb[k] >> 16) & 255, g = (rgb[k] >> 8) & 255, b = rgb[k] & 255;
            int32 m = r > g ? r : g;
            if (b > m) m = b;
            mx[k] = m;
            if (r != g || g != b) grey = false;
            if (m > best) { best = m; bi = k; }
        }
        for (int k = 0; k < count; k++) {
            int32 i = mx[k] * 514;           // 128 = the plain texture, as in the engine's shaders; brighter clamps
            l[k] = (i < 16 ? 16 : (i > 0xFFEF ? 0xFFEF : i)) << 15;
        }
        if (grey || best == 0) return 0xFFFFFF;
        int32 r = (rgb[bi] >> 16) & 255, g = (rgb[bi] >> 8) & 255, b = rgb[bi] & 255;
        const uint32 k = magRcp255[best];            // 255 / best, 16.16
        return (((r * k) >> 16) << 16) | (((g * k) >> 16) << 8) | ((b * k) >> 16);
    }

    bool magDbgCulled = false;

    // work done once per numbered position under the current matrix (see DIP)
    struct MagXform {
        uint32 stamp;                    // valid for this matrix and scissor (hash entries: z << 16 | stamp)
        uint32 key1;                     // hash entries: x << 16 | y
        float  x, y, w;                  // screen space times w
        int32  code;                     // clip bits; bit 6: the projected values below are filled in
        float  px, py;                   // pixels
        int32  ix, iy, iz;               // as MagVtx
        float  q;
        int32  fog;                      // distance fade, 0..256
    } *magPos = NULL;
    int    magPosCap = 0;
    #define MAG_HASH_SIZE 1024
    MagXform magHash[MAG_HASH_SIZE];
    uint32 magXformStamp = 0;            // one per matrix/scissor change; never wraps in practice (table entries)
    short4 magXformClip;
    mat4   magModel, magM;               // mModel the cache belongs to, and screenProj * mModel
    vec3   magCam;                       // camera position in object space
    vec4   magViewPos;
    float  magPlane[6][7];               // clip planes in object space: x y z w, then |x| |y| |z|
    uint32 magLightsVersion = 1, magLightsFor = 0;   // lightsRel is current for this light set and model

    // Multiply blending (Lara's blob shadow): Maggie has no blending on current cores, and the CPU
    // cannot read back what Maggie drew (stale data cache), so the shadow is a screen-door of black
    // pixels - one in four in the middle, one in eight in the fading ring - from two 32x32 textures
    // mapped one texel per pixel.
    uint8 *magStippleMem[2] = { NULL, NULL }, *magStipple[2] = { NULL, NULL };

    bool magStippleInit() {
        if (magStipple[0] && magStipple[1]) return true;
        uint8 *rgba = new uint8[32 * 32 * 4];
        for (int t = 0; t < 2; t++) {
            for (int y = 0; y < 32; y++)
                for (int x = 0; x < 32; x++) {
                    bool on = t == 0 ? ((x + 2 * y) & 3) == 0 : ((y & 1) == 0 && ((x + y) & 3) == 0);
                    uint8 *o = rgba + (y * 32 + x) * 4;
                    o[0] = o[1] = o[2] = 0; o[3] = on ? 255 : 0;
                }
            if (!magStipple[t]) magStipple[t] = Mag::allocTex(32 * 32 / 2, &magStippleMem[t]);
            if (magStipple[t]) Mag::encodeRGBA(rgba, 32, 32, magStipple[t], true);
        }
        delete[] rgba;
        return magStipple[0] && magStipple[1];
    }

    void DIP(Mesh *mesh, const MeshRange &range) {
        if (MAG_STAGE(4)) return;
        const Index  *indices  = mesh->iBuffer;
        const Vertex *vertices = mesh->vBuffer;
        const int iStart = range.iStart, iCount = range.iCount, vStart = range.vStart;

        // texture and mode of this draw call
        Texture *tex = curTex;
        if (tex && tex->fmt != FMT_RGBA) tex = NULL;
        if (tex && (tex->dirty || !tex->dxt)) tex->encode();
        const bool blendAdd = blendMode == RS_BLEND_ADD, blendMul = blendMode == RS_BLEND_MULT;
        const bool cutout = alphaTest || blendMode == RS_BLEND_ALPHA || blendMode == RS_BLEND_PREMULT || !depthTest;
        const bool needZ = Mag::zOn && depthTest;
        const bool flatDraw = vertices[vStart + indices[iStart]].color.w == 142;    // palette-coloured faces
        if (tex && tex->dxt && !flatDraw) {
            magDipTex = cutout || blendAdd || blendMul ? tex->dxtA : tex->dxt;
            magDipSize = uint16(tex->sizeLog2) | (Mag::mipmaps && depthTest && tex->sizeLog2 > 5 ? 0x10 : 0);
        } else {
            magDipTex = Mag::whiteTex;
            magDipSize = 5;
            tex = NULL;
        }
        // bilinear on everything in the 3D scene (the engine marks its atlas tiles "nearest"); 2D stays sharp
        const bool filter = Mag::bilinear && tex && depthTest;
        Mag::setMode(0x0004 | (filter ? 0x0001 : 0) | (needZ ? 0x0002 : 0) | (needZ && (!depthWrite || blendAdd || blendMul) ? 0x0008 : 0) |
                     (blendAdd ? 0x0040 : 0) | (blendMul ? 0x0080 : 0));
        const int32 uMul = tex ? tex->uMul : 0, vMul = tex ? tex->vMul : 0;
        // Maggie's bilinear blends the texel at u with the next one: half a texel back puts the blend
        // around the texel centres, and keeps a cell's last texel from mixing with its neighbour's
        const int32 half = filter ? (1 << (23 - tex->sizeLog2)) : 0;

        if (magVtxCap < iCount * 3 + 16) {
            magVtxCap = iCount * 3 + 256;
            magVtxBuf = (MagVtx*)realloc(magVtxBuf, magVtxCap * sizeof(MagVtx));
        }
        if (magPrimCap < iCount * 2 + 16) {
            magPrimCap = iCount * 2 + 256;
            magPrimBuf = (MagPrim*)realloc(magPrimBuf, magPrimCap * sizeof(MagPrim));
        }
        if (!magVtxBuf || !magPrimBuf) { magVtxCap = magPrimCap = 0; return; }

        // Everything that depends only on the matrices is kept while they stay the same: a room's draw
        // calls (one per texture tile) share them, and many of their corners.
        bool sameView = magScreenProjValid && magScreenW == Core::width && magScreenH == Core::height &&
                        memcmp(&magViewProj, &mViewProj, sizeof(mat4)) == 0;
        if (!sameView) {
            mat4 vp;
            vp.viewport(0.0f, (float)Core::height, (float)Core::width, -(float)Core::height, 0.0f, 1.0f);
            magScreenProj = vp * mViewProj;          // object space -> screen space times w
            magViewProj = mViewProj;
            magScreenW = Core::width; magScreenH = Core::height;
            magScreenProjValid = true;
        }
        if (!sameView || magXformStamp == 0 || magFogDirty || memcmp(&mModel, &magModel, sizeof(mat4)) != 0 ||
            memcmp(&Core::viewPos, &magViewPos, sizeof(vec4)) != 0 ||
            memcmp(&swClipRect, &magXformClip, sizeof(short4)) != 0) {
            magFogDirty = false;
            magModel = mModel;
            magViewPos = Core::viewPos;
            magXformClip = swClipRect;
            magM = magScreenProj * mModel;
            // the camera in object space: the model matrix is a rotation and a translation
            float dx = Core::viewPos.x - mModel.e03, dy = Core::viewPos.y - mModel.e13, dz = Core::viewPos.z - mModel.e23;
            magCam = vec3(mModel.e00 * dx + mModel.e10 * dy + mModel.e20 * dz,
                          mModel.e01 * dx + mModel.e11 * dy + mModel.e21 * dz,
                          mModel.e02 * dx + mModel.e12 * dy + mModel.e22 * dz);
            magLightsFor = 0;
            {   // object-space planes of the scissor rectangle, near and far: P . (x, y, z, 1) < 0 = outside
                const float cl = float(swClipRect.x), ct = float(swClipRect.y), cr = float(swClipRect.z), cb = float(swClipRect.w);
                const bool persp = Core::mProj.e33 != 1.0f;
                const float r0[4] = { magM.e00, magM.e01, magM.e02, magM.e03 }, r1[4] = { magM.e10, magM.e11, magM.e12, magM.e13 },
                            r3[4] = { magM.e30, magM.e31, magM.e32, magM.e33 };
                for (int k = 0; k < 4; k++) {
                    magPlane[0][k] = r0[k] - cl * r3[k];
                    magPlane[1][k] = cr * r3[k] - r0[k];
                    magPlane[2][k] = r1[k] - ct * r3[k];
                    magPlane[3][k] = cb * r3[k] - r1[k];
                    magPlane[4][k] = r3[k];
                    magPlane[5][k] = -r3[k];
                }
                magPlane[4][3] -= persp ? SW_NEAR : 0.0f;
                magPlane[5][3] += SW_MAX_DIST;
                for (int j = 0; j < 6; j++)
                    for (int k = 0; k < 3; k++) magPlane[j][4 + k] = fabsf(magPlane[j][k]);
            }
            // a new stamp empties the position cache
            // a new stamp empties both position caches; the hash keeps only its low 16 bits
            if ((++magXformStamp & 0xFFFF) == 0) {
                memset(magHash, 0, sizeof(magHash));
                magXformStamp++;
            }
        }
        const mat4 &m = magM;
        const uint32 stamp = magXformStamp;
        MagXform *posTable = NULL;
    #ifdef OL_ROOM_CELLS
        if (range.posBase >= 0 && gMeshPosCount > 0) {
            if (magPosCap < gMeshPosCount) {         // a new level: the table grows, emptied
                magPosCap = gMeshPosCount + 1024;
                magPos = (MagXform*)realloc(magPos, magPosCap * sizeof(MagXform));
                if (magPos) memset(magPos, 0, magPosCap * sizeof(MagXform)); else magPosCap = 0;
            }
            if (magPos) posTable = magPos + range.posBase;
        }
    #endif
        const bool lit = lightsCount > 0 && !MAG_STAGE(3);
        if (lit && magLightsFor != magLightsVersion) { transformLights(); magLightsFor = magLightsVersion; }

        const float clipL = float(swClipRect.x), clipT = float(swClipRect.y), clipR = float(swClipRect.z), clipB = float(swClipRect.w);
        // orthographic projections (UI, 2D) have w = 1 everywhere: only perspective ones get the near plane
        const bool ortho = Core::mProj.e33 == 1.0f;
        const float nearW = ortho ? 0.0f : SW_NEAR;
        const int32 nearBits = magBits(nearW), farBits = magBits(SW_MAX_DIST);
        // which way round is the front: the engine's default (cmFront) is clockwise on this screen
        const bool cullNone = cullMode == 0, flip = cullMode == RS_CULL_BACK;

        // Faces whose corners share one normal carry their face normal (room geometry): those facing
        // away from the camera are dropped before any corner is transformed.
        const bool normalCull = !ortho && !cullNone && Mag::normalCull;
        const float camX = magCam.x, camY = magCam.y, camZ = magCam.z;
        const float cullSide = flip ? -1.0f : 1.0f;

        if (MAG_STAGE(8)) return;
        MagVtx  *vo = magVtxBuf;
        MagPrim *po = magPrimBuf;

        // Opaque room geometry comes sorted into cells (MeshBuilder::cellSort) with a box each: a cell
        // whose box is wholly outside the scissor rectangle, behind the near plane or beyond the far
        // distance is skipped without touching its faces. Everything else is one segment.
    #ifdef OL_ROOM_CELLS
        const MeshCell *cell = range.cellCount ? gMeshCells + range.cellStart : NULL;
        const int segments = (cell && Mag::roomCells && !ortho) ? range.cellCount : 1;
        if (segments == 1) cell = NULL;
    #else
        const int segments = 1;
    #endif
        for (int seg = 0; seg < segments; seg++) {
        int segStart = iStart, segCount = iCount;
    #ifdef OL_ROOM_CELLS
        if (cell) {
            const MeshCell &c = cell[seg];
            bool out = false;
            for (int k = 0; k < 6 && !out; k++) {
                const float *P = magPlane[k];
                out = magBits(P[0] * c.cx + P[1] * c.cy + P[2] * c.cz + P[3] + P[4] * c.ex + P[5] * c.ey + P[6] * c.ez) < 0;
            }
            MAG_COUNT(6, 1);
            if (out) { MAG_COUNT(7, 1); continue; }
            segStart = c.iStart; segCount = c.iCount;
        }
    #endif
        int i = 0;
        while (i < segCount) {
            const Index *idx = indices + segStart + i;
            const Vertex *vx[4];
            vx[0] = vertices + vStart + idx[0];
            vx[1] = vertices + vStart + idx[1];
            vx[2] = vertices + vStart + idx[2];
            // quads come as two triangles 012 023
            const bool isTriangle = vx[0]->normal.w == 1 || i + 6 > segCount || idx[3] != idx[0] || idx[4] != idx[2];
            const int count = isTriangle ? 3 : 4;
            i += isTriangle ? 3 : 6;
            if (i > segCount) break;
            if (!isTriangle) vx[3] = vertices + vStart + idx[5];
            MAG_COUNT(0, 1);
            magDbgCulled = false;
            if (MAG_STAGE(9)) continue;
            if (normalCull) {
                const short4 &n = vx[0]->normal, &n1 = vx[1]->normal, &n2 = vx[2]->normal;
                if (n.x == n1.x && n.y == n1.y && n.z == n1.z && n.x == n2.x && n.y == n2.y && n.z == n2.z &&
                    abs(n.x) + abs(n.y) + abs(n.z) > 30000) {
                    float d = (float(n.x) * (camX - float(vx[0]->coord.x)) + float(n.y) * (camY - float(vx[0]->coord.y)) +
                               float(n.z) * (camZ - float(vx[0]->coord.z))) * cullSide;
                    // more than 4 units behind the face's plane (the normal has length 32767)
                    if (magBits(d + 131068.0f) < 0) { if (!MAG_STAGE(7)) continue; magDbgCulled = true; }
                }
            }
            MAG_COUNT(1, count);

            // Each corner's position: transformed, clip-coded and (below) projected once. Room geometry
            // has its positions numbered at load (MeshRange::posBase + coord.w), so neighbouring faces
            // and the room's other draw calls find the work done in a table; anything else is done here.
            MagXform *e[4];
            MagXform own[4];
            int32 behind = 0, outside = 15, tooFar = 0;
            for (int k = 0; k < count; k++) {
                const short4 &c = vx[k]->coord;
                MagXform *x;
                if (posTable) {
                    x = posTable + uint16(c.w);
                    if (x->stamp == stamp) goto known;
                    x->stamp = stamp;
                } else {
                    // models and dynamic geometry: a small hash of positions under this matrix
                    const uint32 key1 = (uint32(uint16(c.x)) << 16) | uint16(c.y), key2 = (uint32(uint16(c.z)) << 16) | uint16(stamp);
                    uint32 h = uint32(c.x * 73 + c.y * 151 + c.z * 283);
                    x = magHash + ((h ^ (h >> 9)) & (MAG_HASH_SIZE - 1));
                    if (x->key1 == key1 && x->stamp == key2) goto known;
                    // taken by another position: not if an earlier corner of this polygon is using it
                    if ((k > 0 && x == e[0]) || (k > 1 && x == e[1]) || (k > 2 && x == e[2])) x = own + k;
                    else { x->key1 = key1; x->stamp = key2; }
                }
                {
                    float fx = float(c.x), fy = float(c.y), fz = float(c.z);
                    float w = m.e30 * fx + m.e31 * fy + m.e32 * fz + m.e33;
                    float sx = m.e00 * fx + m.e01 * fy + m.e02 * fz + m.e03;
                    float sy = m.e10 * fx + m.e11 * fy + m.e12 * fz + m.e13;
                    x->x = sx; x->y = sy; x->w = w;
                    int32 wb = magBits(w);
                    // bits 0..3: left of, right of, above, below the scissor rectangle - the portal a room is
                    // seen through (sign bits; valid for w > 0), bit 4 too far, bit 5 behind the near plane
                    x->code = (uint32(magBits(sx - clipL * w)) >> 31) | ((uint32(magBits(clipR * w - sx)) >> 31) << 1) |
                              ((uint32(magBits(sy - clipT * w)) >> 31) << 2) | ((uint32(magBits(clipB * w - sy)) >> 31) << 3) |
                              (wb > farBits ? 16 : 0) | (wb < nearBits ? 32 : 0);
                }
            known:
                e[k] = x;
                outside &= x->code;
                tooFar |= x->code & 16;
                behind += (x->code >> 5) & 1;
            }
            if ((tooFar && !ortho) || behind == count) continue;
            if (MAG_STAGE(5)) continue;
            if (behind && !swNearClip) continue;
            if (!behind && outside) continue;

            // screen positions and facing (polygons crossing the near plane: after clipping)
            float area = 0.0f, area2 = 0.0f;
            bool flat = false, reverse = false;
            bool clip = behind != 0;         // through the clipper: near plane or guard band
            if (!behind) {
                // divide, convert and fade once per position
                for (int k = 0; k < count; k++) {
                    MagXform *x = e[k];
                    if (!(x->code & 64)) {
                        const short4 &c = vx[k]->coord;
                        float r = 1.0f / x->w;
                        x->px = x->x * r; x->py = x->y * r;
                        int32 ix = int32(x->px * 16.0f), iy = int32(x->py * 16.0f);
                        x->ix = ix > MAG_XY_MAX ? MAG_XY_MAX : (ix < -MAG_XY_MAX ? -MAG_XY_MAX : ix);
                        x->iy = iy > MAG_XY_MAX ? MAG_XY_MAX : (iy < -MAG_XY_MAX ? -MAG_XY_MAX : iy);
                        float z = m.e20 * float(c.x) + m.e21 * float(c.y) + m.e22 * float(c.z) + m.e23;
                        int32 iz = int32(z * r * (65533.0f * 32768.0f));
                        x->iz = (iz < 0 ? 0 : (iz > 65533 * 32768 ? 65533 * 32768 : iz)) + 32768;
                        x->q = magBits(x->w) > 0x41800000 ? r : 1.0f / 16.0f;    // w > 16
                        x->fog = ortho ? 256 : magFog(x->w);
                        x->code |= 64;
                        // far off screen (a corner close to the camera): the clamp above would bend the
                        // polygon, so it goes through the clipper instead
                        if (uint32(ix + MAG_GUARD * 16) > uint32(MAG_GUARD * 32) || uint32(iy + MAG_GUARD * 16) > uint32(MAG_GUARD * 32))
                            x->code |= 128;
                    }
                    clip |= (x->code & 128) != 0;
                }
                float abx = e[1]->px - e[0]->px, aby = e[1]->py - e[0]->py, acx = e[2]->px - e[0]->px, acy = e[2]->py - e[0]->py;
                area = abx * acy - acx * aby;
                if (!isTriangle) {
                    // a parallelogram with matching texture corners: one plane fits all four
                    const short4 &A = vx[0]->coord, &B = vx[1]->coord, &C = vx[2]->coord, &D = vx[3]->coord;
                    const short4 &TA = vx[0]->texCoord, &TB = vx[1]->texCoord, &TC = vx[2]->texCoord, &TD = vx[3]->texCoord;
                    flat = (uint32(A.x + C.x - B.x - D.x + 1) <= 2) && (uint32(A.y + C.y - B.y - D.y + 1) <= 2) &&
                           (uint32(A.z + C.z - B.z - D.z + 1) <= 2) &&
                           (uint32(TA.x + TC.x - TB.x - TD.x + 96) <= 192) && (uint32(TA.y + TC.y - TB.y - TD.y + 96) <= 192);
                    float adx = e[3]->px - e[0]->px, ady = e[3]->py - e[0]->py;
                    area2 = acx * ady - adx * acy;
                }
                if (flip) { area = -area; area2 = -area2; }
                if (cullNone && magBits(area) < 0 && magBits(area2) <= 0) { area = -area; area2 = -area2; reverse = true; }
                else reverse = flip;
                if (isTriangle || flat) {
                    if (magBits(area - MAG_MIN_AREA) < 0) continue;      // facing away, or nothing to draw
                } else if (magBits(area - MAG_MIN_AREA) < 0 && magBits(area2 - MAG_MIN_AREA) < 0) continue;
            }
            if (magDbgCulled && !behind) MAG_COUNT(6, 1);     // profiling: the normal test was wrong
            if (MAG_STAGE(6)) continue;
            // no alpha blending in Maggie: mostly transparent faces (screen fades, faint overlays) are
            // left out, the rest drawn solid
            if (blendMode == RS_BLEND_ALPHA && vx[0]->light.w < 128) continue;

            // colour: vertex light (x face colour for palette-coloured faces) x lighting x distance fade.
            // Room faces carry it worked out at load (MeshBuilder::roomColours): used as it is when
            // no light and no fade applies.
            int32  l[4];
            uint32 tint;
        #ifdef ROOM_COLOUR_DONE
            if (!lit && !clip && vx[0]->color.w == ROOM_COLOUR_DONE) {
                const ubyte4 &c = vx[0]->color;
                tint = (uint32(c.x) << 16) | (uint32(c.y) << 8) | c.z;
                for (int k = 0; k < count; k++) {
                    int32 i = ((vx[k]->texCoord.z * e[k]->fog) >> 8) * 514;     // as the loop below and magTint
                    l[k] = (i < 16 ? 16 : (i > 0xFFEF ? 0xFFEF : i)) << 15;
                }
            } else
        #endif
            {
                uint32 rgb[4];
                for (int k = 0; k < count; k++) {
                    const Vertex &v = *vx[k];
                    int32 f = lit ? magLightFactor(v) : 256;
                    f = (f * (behind ? magFog(e[k]->w) : e[k]->fog)) >> 8;
                    int32 r = v.light.x, g = v.light.y, b = v.light.z;
                    if (flatDraw) { r = r * v.color.x / 255; g = g * v.color.y / 255; b = b * v.color.z / 255; }
                    r = (r * f) >> 8; g = (g * f) >> 8; b = (b * f) >> 8;            // f may be up to 512
                    if ((r | g | b) > 255) { r = r > 255 ? 255 : r; g = g > 255 ? 255 : g; b = b > 255 ? 255 : b; }
                    rgb[k] = (uint32(r) << 16) | (uint32(g) << 8) | uint32(b);
                }
                tint = magTint(rgb, count, l);
            }
            if (magWater && !ortho)
                tint = ((((tint >> 16) & 255) * 154 >> 8) << 16) | ((((tint >> 8) & 255) * 230 >> 8) << 8) | ((tint & 255) * 230 >> 8);
            if (blendMul)                    // how dark: the vertex colour, black in the middle (see the draw below)
                for (int k = 0; k < count; k++) l[k] = int32(vx[k]->light.x) << 16;
            // all corners on one texel: a plain coloured face (UI bars and frames)
            if (vx[0]->texCoord.x == vx[1]->texCoord.x && vx[0]->texCoord.x == vx[2]->texCoord.x &&
                vx[0]->texCoord.y == vx[1]->texCoord.y && vx[0]->texCoord.y == vx[2]->texCoord.y) tint |= 0x01000000;

            if (!clip) {
                for (int k = 0; k < count; k++) {
                    const Vertex &v = *vx[k];
                    const MagXform *x = e[k];
                    MagVtx &o = vo[k];
                    o.x = x->ix; o.y = x->iy; o.z = x->iz; o.q = x->q;
                    o.u = v.texCoord.x * uMul - half;
                    o.v = v.texCoord.y * vMul - half;
                    o.l = l[k];
                }
                // clockwise for the rasteriser
                MagVtx *a = vo, *b = vo + 1, *c = vo + 2, *d = vo + 3;
                if (isTriangle || flat) {
                    if (reverse) { po->v[0] = a; po->v[1] = isTriangle ? c : d; po->v[2] = isTriangle ? b : c; po->v[3] = b; }
                    else         { po->v[0] = a; po->v[1] = b; po->v[2] = c; po->v[3] = d; }
                    po->n = count; po->area = area; po->tint = tint; po++;
                } else {
                    if (magBits(area - MAG_MIN_AREA) >= 0) {
                        po->v[0] = a; po->v[1] = reverse ? c : b; po->v[2] = reverse ? b : c;
                        po->n = 3; po->area = area; po->tint = tint; po++;
                    }
                    if (magBits(area2 - MAG_MIN_AREA) >= 0) {
                        po->v[0] = a; po->v[1] = reverse ? d : c; po->v[2] = reverse ? c : d;
                        po->n = 3; po->area = area2; po->tint = tint; po++;
                    }
                }
                vo += count;
                continue;
            }

            // crossing the near plane or reaching far off screen: Sutherland-Hodgman against w >= nearW
            // and the guard band (-MAG_GUARD..MAG_GUARD pixels each way), drawn as a triangle fan
            MagClipVtx cb[2][10];
            MagClipVtx *cv = cb[0];
            for (int k = 0; k < count; k++) {
                const Vertex &v = *vx[k];
                cv[k].x = e[k]->x; cv[k].y = e[k]->y; cv[k].w = e[k]->w;
                cv[k].z = m.e20 * float(v.coord.x) + m.e21 * float(v.coord.y) + m.e22 * float(v.coord.z) + m.e23;
                cv[k].u = v.texCoord.x * uMul - half;
                cv[k].v = v.texCoord.y * vMul - half;
                cv[k].l = l[k];
            }
            int nIn = count;
            for (int pl = 0; pl < 5 && nIn >= 3; pl++) {
                // distance to the plane: a * x + b * y + c * w + d
                static const float PL[5][4] = {
                    { 0.0f, 0.0f, 1.0f, 0.0f }, { 1.0f, 0.0f, float(MAG_GUARD), 0.0f }, { -1.0f, 0.0f, float(MAG_GUARD), 0.0f },
                    { 0.0f, 1.0f, float(MAG_GUARD), 0.0f }, { 0.0f, -1.0f, float(MAG_GUARD), 0.0f } };
                const float *P = PL[pl];
                const float pd = pl ? 0.0f : -nearW;
                float dist[10];
                bool anyOut = false;
                for (int k = 0; k < nIn; k++) {
                    dist[k] = P[0] * cv[k].x + P[1] * cv[k].y + P[2] * cv[k].w + pd;
                    anyOut |= dist[k] < 0.0f;
                }
                if (!anyOut) continue;
                MagClipVtx *out = cv == cb[0] ? cb[1] : cb[0];
                int nNew = 0;
                for (int k = 0; k < nIn; k++) {
                    int k2 = k + 1 == nIn ? 0 : k + 1;
                    const MagClipVtx &a = cv[k], &b = cv[k2];
                    bool aIn = dist[k] >= 0.0f, bIn = dist[k2] >= 0.0f;
                    if (aIn) out[nNew++] = a;
                    if (aIn != bIn) {
                        float t = dist[k] / (dist[k] - dist[k2]);
                        MagClipVtx &o = out[nNew++];
                        o.x = a.x + (b.x - a.x) * t; o.y = a.y + (b.y - a.y) * t;
                        o.z = a.z + (b.z - a.z) * t; o.w = a.w + (b.w - a.w) * t;
                        o.u = a.u + int32(float(b.u - a.u) * t); o.v = a.v + int32(float(b.v - a.v) * t);
                        o.l = a.l + int32(float(b.l - a.l) * t);
                    }
                }
                cv = out; nIn = nNew;
            }
            if (nIn < 3) continue;
            MagVtx *first = vo;
            for (int k = 0; k < nIn; k++) {
                const MagClipVtx &a = cv[k];
                float w = a.w < nearW ? nearW : a.w;
                magProject(*vo, a.x, a.y, a.z, w, 1.0f / w, a.u, a.v, a.l);
                vo++;
            }
            int nOut = int(vo - first);
            for (int k = 1; k + 1 < nOut; k++) {             // fan, same winding
                MagVtx *b = first + k, *c = first + k + 1;
                float ar = magCross(first, b, c);
                if (flip) ar = -ar;
                bool rev = flip;
                if (cullNone && magBits(ar) < 0) { ar = -ar; rev = !rev; }
                if (magBits(ar - MAG_MIN_AREA) < 0) continue;
                po->v[0] = first; po->v[1] = rev ? c : b; po->v[2] = rev ? b : c;
                po->n = 3; po->area = ar; po->tint = tint; po++;
            }
        }

        }   // segments

        if (MAG_STAGE(1)) return;
    #ifdef OL_EMU
        {   // local variable OLPICK=x,y: the polygons covering that pixel
            const int pickX = Mag::pickX, pickY = Mag::pickY;
            if (pickX >= 0)
                for (const MagPrim *pr = magPrimBuf; pr < po; pr++) {
                    int32 X = pickX * 16 + 8, Y = pickY * 16 + 8;
                    bool in = true;
                    for (int k = 0; k < pr->n && in; k++) {
                        const MagVtx *p0 = pr->v[k], *p1 = pr->v[(k + 1) % pr->n];
                        in = float(p1->x - p0->x) * float(Y - p0->y) - float(p1->y - p0->y) * float(X - p0->x) >= 0.0f;
                    }
                    if (!in) continue;
                    magTracePrim = pr;
                    printf("PICK tex %p log2 %d cutout %d blend %d depth %d/%d cull %d n %d tint %08lx area %d\n", tex, tex ? tex->sizeLog2 : 0,
                           int(cutout) + 10 * int(flatDraw), int(blendMode), int(depthTest), int(depthWrite), int(cullNone), pr->n, (unsigned long)pr->tint, int(pr->area));
                    if (tex && tex->memory) {
                        printf("  tex %dx%d orig %dx%d uMul %ld vMul %ld; column under corner 0, rows -2..+70:\n", tex->width, tex->height, tex->origWidth, tex->origHeight, (long)tex->uMul, (long)tex->vMul);
                        int tx = int(pr->v[0]->u >> 16) + 20, ty = int(pr->v[0]->v >> 16);
                        for (int y = ty - 2; y < ty + 70; y++)
                            if (y >= 0 && y < tex->height && tx < tex->width) {
                                const uint8 *t = tex->memory + (y * tex->width + tx) * 4;
                                printf("   y%d %02x%02x%02x%02x", y, t[0], t[1], t[2], t[3]);
                                if ((y & 7) == 7) printf("\n");
                            }
                        printf("\n");
                    }
                    for (int k = 0; k < pr->n; k++)
                        printf("  %d,%d z %ld u %08lx v %08lx l %ld\n", int(pr->v[k]->x >> 4), int(pr->v[k]->y >> 4), (long)pr->v[k]->z,
                               (unsigned long)pr->v[k]->u, (unsigned long)pr->v[k]->v, (long)pr->v[k]->l);
                }
        }
    #endif
    #ifdef OL_EMU
        const uint16 pickBefore = Mag::pickX >= 0 ? swColor[Mag::pickY * Core::width + Mag::pickX] : 0;
    #endif
        if (blendMul) {
            if (po == magPrimBuf || !magStippleInit()) return;
            // dense where every corner is dark; then texture = screen position, one texel per pixel
            for (MagPrim *pr = magPrimBuf; pr < po; pr++) {
                int32 sum = 0;
                for (int k = 0; k < pr->n; k++) sum += pr->v[k]->l >> 16;
                pr->tint = sum < 64 * pr->n ? 0 : 1;
            }
            for (MagVtx *v = magVtxBuf; v < vo; v++) { v->u = v->x << 15; v->v = v->y << 15; v->q = 1.0f; v->l = 0x7FFF << 15; }
            Mag::setMode(0x0004 | (needZ ? 0x000A : 0));     // nearest, depth tested but not written
            const uint8 *keepTex = magDipTex; const uint16 keepSize = magDipSize;
            magDipSize = 5;
            for (MagPrim *pr = magPrimBuf; pr < po; pr++) {
                magDipTex = magStipple[pr->tint];
                pr->tint = 0;
                magDraw(*pr);
            }
            magDipTex = (uint8*)keepTex; magDipSize = keepSize;
            return;
        }
        for (const MagPrim *pr = magPrimBuf; pr < po; pr++) magDraw(*pr);
    #ifdef OL_EMU
        if (Mag::pickX >= 0 && swColor[Mag::pickY * Core::width + Mag::pickX] != pickBefore)
            printf("PIX %04x -> %04x: iStart %d iCount %d prims %d tex %p flat %d blend %d depth %d/%d mode size %x\n", pickBefore,
                   swColor[Mag::pickY * Core::width + Mag::pickX], iStart, iCount, int(po - magPrimBuf), tex, int(flatDraw), int(blendMode),
                   int(depthTest), int(depthWrite), int(magDipSize));
    #endif
    }

    // Full-screen backgrounds (title picture, logos): scaled to the screen once (RGB565, letterboxed)
    // and copied in each frame - sharper and far cheaper than a DXT1 texture of a photo.
    ColorSW  *swBackground = NULL;
    Texture  *swBackgroundSrc = NULL;

    void drawBackground(Texture *tex) {
        if (!tex || !tex->memory || tex->origWidth <= 0 || tex->origHeight <= 0) return;
        int W = Core::width, H = Core::height;
        if (tex != swBackgroundSrc || !swBackground) {
            delete[] swBackground;
            swBackground = new ColorSW[W * H];
            memset(swBackground, 0, W * H * sizeof(ColorSW));
            // fit the image's aspect into the screen
            int dw = W, dh = W * tex->origHeight / tex->origWidth;
            if (dh > H) { dh = H; dw = H * tex->origWidth / tex->origHeight; }
            int ox = (W - dw) / 2, oy = (H - dh) / 2;
            for (int y = 0; y < dh; y++) {
                const uint8 *row = tex->memory + (y * tex->origHeight / dh) * tex->width * 4;
                ColorSW *dst = swBackground + (oy + y) * W + ox;
                for (int x = 0; x < dw; x++) {
                    const uint8 *p = row + (x * tex->origWidth / dw) * 4;
                    dst[x] = CONV_COLOR(p[0], p[1], p[2]);
                }
            }
            swBackgroundSrc = tex;
        }
        Mag::sync();                     // nothing still being drawn into the frame
        memcpy(swColor, swBackground, W * H * sizeof(ColorSW));
    }

    vec4 copyPixel(int x, int y) {
        return vec4(0.0f);
    }
}

#endif
