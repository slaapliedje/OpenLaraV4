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

    void setFog(const vec4 &params) {}

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
    // it is (rooms and sprites carry their light in the vertices). Returns 0..256.
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
        return f > 256 ? 256 : f;
    }

    // distance fade to black, 0..256
    inline int32 magFog(float w) {
        if (magBits(w) <= magBits(SW_FOG_START)) return 256;
        float f = 1.0f - (w - SW_FOG_START) * (1.0f / (SW_MAX_DIST - SW_FOG_START));
        return magBits(f) > 0 ? int32(f * 256.0f) : 0;
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
        return uint32((r * 255 / best) << 16) | uint32((g * 255 / best) << 8) | uint32(b * 255 / best);
    }

    bool magDbgCulled = false;

    // transformed positions under the current matrix, by object-space position
    #define MAG_XFORM_SIZE 2048
    struct MagXform {
        uint32 key1, key2;               // x, y / z, stamp
        float  x, y, w;                  // screen space times w
        int32  code;
    } magXform[MAG_XFORM_SIZE];
    uint16 magXformStamp = 0;
    mat4   magXformMatrix;
    short4 magXformClip;

    void DIP(Mesh *mesh, const MeshRange &range) {
        if (MAG_STAGE(4)) return;
        MAG_COUNT(7, 1);
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

        if (magVtxCap < iCount * 2 + 16) {
            magVtxCap = iCount * 2 + 256;
            magVtxBuf = (MagVtx*)realloc(magVtxBuf, magVtxCap * sizeof(MagVtx));
        }
        if (magPrimCap < iCount + 16) {
            magPrimCap = iCount + 256;
            magPrimBuf = (MagPrim*)realloc(magPrimBuf, magPrimCap * sizeof(MagPrim));
        }
        if (!magVtxBuf || !magPrimBuf) { magVtxCap = magPrimCap = 0; return; }

        // object space -> screen space times w
        if (!magScreenProjValid || magScreenW != Core::width || magScreenH != Core::height ||
            memcmp(&magViewProj, &mViewProj, sizeof(mat4)) != 0) {
            mat4 vp;
            vp.viewport(0.0f, (float)Core::height, (float)Core::width, -(float)Core::height, 0.0f, 1.0f);
            magScreenProj = vp * mViewProj;
            magViewProj = mViewProj;
            magScreenW = Core::width; magScreenH = Core::height;
            magScreenProjValid = true;
        }
        const mat4 m = magScreenProj * mModel;
        const bool lit = lightsCount > 0 && !MAG_STAGE(3);
        if (lit) transformLights();

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
        vec3 camPos(0.0f);
        if (normalCull) camPos = mModel.inverseOrtho() * Core::viewPos.xyz();
        const float camX = camPos.x, camY = camPos.y, camZ = camPos.z;
        const float cullSide = flip ? -1.0f : 1.0f;

        // a new stamp empties the transform cache: whenever the matrix changes (a room's draw calls,
        // one per texture tile, share it and many of their corners)
        if (magXformStamp == 0 || memcmp(&m, &magXformMatrix, sizeof(mat4)) != 0 || memcmp(&swClipRect, &magXformClip, sizeof(short4)) != 0) {
            magXformMatrix = m;
            magXformClip = swClipRect;
            if (++magXformStamp == 0) { memset(magXform, 0, sizeof(magXform)); magXformStamp = 1; }
        }
        const uint32 stamp = magXformStamp;

        if (MAG_STAGE(8)) return;
        MagVtx  *vo = magVtxBuf;
        MagPrim *po = magPrimBuf;

        int i = 0;
        while (i < iCount) {
            const Index *idx = indices + iStart + i;
            const Vertex *vx[4];
            vx[0] = vertices + vStart + idx[0];
            vx[1] = vertices + vStart + idx[1];
            vx[2] = vertices + vStart + idx[2];
            // quads come as two triangles 012 023
            const bool isTriangle = vx[0]->normal.w == 1 || i + 6 > iCount || idx[3] != idx[0] || idx[4] != idx[2];
            const int count = isTriangle ? 3 : 4;
            i += isTriangle ? 3 : 6;
            if (i > iCount) break;
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

            float cx[4], cy[4], cw[4];
            int32 behind = 0, outside = 15, tooFar = 0;
            for (int k = 0; k < count; k++) {
                // neighbouring faces repeat the same positions: look the corner up before transforming it
                const short4 &c = vx[k]->coord;
                const uint32 key1 = (uint32(uint16(c.x)) << 16) | uint16(c.y), key2 = (uint32(uint16(c.z)) << 16) | stamp;
                uint32 h = uint32(c.x * 73 + c.y * 151 + c.z * 283);
                MagXform &e = magXform[(h ^ (h >> 9)) & (MAG_XFORM_SIZE - 1)];
                if (e.key1 != key1 || e.key2 != key2) {
                    e.key1 = key1; e.key2 = key2;
                    float x = float(c.x), y = float(c.y), z = float(c.z);
                    float w = m.e30 * x + m.e31 * y + m.e32 * z + m.e33;
                    float sx = m.e00 * x + m.e01 * y + m.e02 * z + m.e03;
                    float sy = m.e10 * x + m.e11 * y + m.e12 * z + m.e13;
                    e.x = sx; e.y = sy; e.w = w;
                    int32 wb = magBits(w);
                    // bits 0..3: left of, right of, above, below the scissor rectangle - the portal a room is
                    // seen through (sign bits; valid for w > 0),
                    // bit 4 too far, bit 5 behind the near plane
                    e.code = (uint32(magBits(sx - clipL * w)) >> 31) | ((uint32(magBits(clipR * w - sx)) >> 31) << 1) |
                             ((uint32(magBits(sy - clipT * w)) >> 31) << 2) | ((uint32(magBits(clipB * w - sy)) >> 31) << 3) |
                             (wb > farBits ? 16 : 0) | (wb < nearBits ? 32 : 0);
                }
                cx[k] = e.x; cy[k] = e.y; cw[k] = e.w;
                outside &= e.code;
                tooFar |= e.code & 16;
                behind += e.code >> 5;
            }
            if ((tooFar && !ortho) || behind == count) continue;
            if (MAG_STAGE(5)) continue;
            if (behind && !swNearClip) continue;
            if (!behind && outside) continue;

            // screen positions and facing (polygons crossing the near plane: after clipping)
            float px[4], py[4], iw[4];
            float area = 0.0f, area2 = 0.0f;
            bool flat = false, reverse = false;
            if (!behind) {
                for (int k = 0; k < count; k++) {
                    float r = 1.0f / cw[k];
                    iw[k] = r; px[k] = cx[k] * r; py[k] = cy[k] * r;
                }
                float abx = px[1] - px[0], aby = py[1] - py[0], acx = px[2] - px[0], acy = py[2] - py[0];
                area = abx * acy - acx * aby;
                if (!isTriangle) {
                    // a parallelogram with matching texture corners: one plane fits all four
                    const short4 &A = vx[0]->coord, &B = vx[1]->coord, &C = vx[2]->coord, &D = vx[3]->coord;
                    const short4 &TA = vx[0]->texCoord, &TB = vx[1]->texCoord, &TC = vx[2]->texCoord, &TD = vx[3]->texCoord;
                    flat = (uint32(A.x + C.x - B.x - D.x + 1) <= 2) && (uint32(A.y + C.y - B.y - D.y + 1) <= 2) &&
                           (uint32(A.z + C.z - B.z - D.z + 1) <= 2) &&
                           (uint32(TA.x + TC.x - TB.x - TD.x + 96) <= 192) && (uint32(TA.y + TC.y - TB.y - TD.y + 96) <= 192);
                    float adx = px[3] - px[0], ady = py[3] - py[0];
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

            // colour: vertex light (x face colour for palette-coloured faces) x lighting x distance fade
            uint32 rgb[4];
            int32  l[4];
            for (int k = 0; k < count; k++) {
                const Vertex &v = *vx[k];
                int32 f = lit ? magLightFactor(v) : 256;
                if (lit) MAG_COUNT(5, lightsCount);
                if (!ortho) f = (f * magFog(cw[k])) >> 8;
                int32 r = v.light.x, g = v.light.y, b = v.light.z;
                if (flatDraw) { r = r * v.color.x / 255; g = g * v.color.y / 255; b = b * v.color.z / 255; }
                rgb[k] = (uint32((r * f) >> 8) << 16) | (uint32((g * f) >> 8) << 8) | uint32((b * f) >> 8);
            }
            uint32 tint = magTint(rgb, count, l);
            // all corners on one texel: a plain coloured face (UI bars and frames)
            if (vx[0]->texCoord.x == vx[1]->texCoord.x && vx[0]->texCoord.x == vx[2]->texCoord.x &&
                vx[0]->texCoord.y == vx[1]->texCoord.y && vx[0]->texCoord.y == vx[2]->texCoord.y) tint |= 0x01000000;

            if (!behind) {
                for (int k = 0; k < count; k++) {
                    const Vertex &v = *vx[k];
                    float z = 0.0f;
                    if (needZ) z = m.e20 * float(v.coord.x) + m.e21 * float(v.coord.y) + m.e22 * float(v.coord.z) + m.e23;
                    magProject(vo[k], cx[k], cy[k], z, cw[k], iw[k], v.texCoord.x * uMul - half, v.texCoord.y * vMul - half, l[k]);
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

            // crossing the near plane: Sutherland-Hodgman against w >= nearW (at most count + 1
            // vertices), drawn as a triangle fan
            MagClipVtx cv[4];
            for (int k = 0; k < count; k++) {
                const Vertex &v = *vx[k];
                cv[k].x = cx[k]; cv[k].y = cy[k]; cv[k].w = cw[k];
                cv[k].z = m.e20 * float(v.coord.x) + m.e21 * float(v.coord.y) + m.e22 * float(v.coord.z) + m.e23;
                cv[k].u = v.texCoord.x * uMul - half;
                cv[k].v = v.texCoord.y * vMul - half;
                cv[k].l = l[k];
            }
            MagVtx *first = vo;
            for (int k = 0; k < count; k++) {
                const MagClipVtx &a = cv[k];
                const MagClipVtx &b = cv[k + 1 == count ? 0 : k + 1];
                bool aIn = a.w >= nearW, bIn = b.w >= nearW;
                if (aIn) { magProject(*vo, a.x, a.y, a.z, a.w, 1.0f / a.w, a.u, a.v, a.l); vo++; }
                if (aIn != bIn) {
                    float t = (nearW - a.w) / (b.w - a.w);
                    magProject(*vo, a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, nearW, 1.0f / nearW,
                               a.u + int32(float(b.u - a.u) * t), a.v + int32(float(b.v - a.v) * t),
                               a.l + int32(float(b.l - a.l) * t));
                    vo++;
                }
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

        if (MAG_STAGE(1)) return;
        for (const MagPrim *pr = magPrimBuf; pr < po; pr++) magDraw(*pr);
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
