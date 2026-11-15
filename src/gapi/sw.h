#ifndef H_GAPI_SW
#define H_GAPI_SW

#include "core.h"

#ifdef OL_MAGGIE
    #include "platform/amiga/maggie_sw.h"   // spans drawn by the Vampire's Maggie unit
#endif

#define PROFILE_MARKER(title)
#define PROFILE_LABEL(id, name, label)
#define PROFILE_TIMING(time)

//#define DITHER_FILTER

#if defined(_OS_LINUX) || defined(_OS_TNS) || defined(_OS_AMIGA)
    #define COLOR_16
#endif

#ifdef COLOR_16
    #if defined(_OS_LINUX) || defined(_OS_TNS) || defined(_OS_AMIGA)
        #define COLOR_FMT_565
        #define CONV_COLOR(r,g,b) (((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))
    #else
        #define COLOR_FMT_555
        #define CONV_COLOR(r,g,b) (((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3))
    #endif
#else 
    #define COLOR_FMT_888
    #define CONV_COLOR(r,g,b) ((r << 16) | (g << 8) | b)
#endif

#define SW_MAX_DIST  (20.0f * 1024.0f)
#define SW_FOG_START (12.0f * 1024.0f)

namespace GAPI {

    using namespace Core;

    typedef ::Vertex Vertex;

    #ifdef COLOR_16
        typedef uint16 ColorSW;
    #else
        typedef uint32 ColorSW;
    #endif
    typedef uint16 DepthSW;

    uint8   *swLightmap;
    uint8   swLightmapNone[32 * 256];
    uint8   swLightmapShade[32 * 256];
    ColorSW *swPalette;
    ColorSW swPaletteColor[256];
    ColorSW swPaletteWater[256];
    ColorSW swPaletteGray[256];
    uint8   swGradient[256];
    Tile8   *curTile;
#ifdef OL_MAGGIE
    bool    magColored;                  // current mesh range is flat-coloured (u = palette index)
    bool    magAffine;                   // current polygon walks plain u, v (see magPrepare)
#endif

    uint8 ambient;
    int32 lightsCount;

    struct LightSW {
        uint32 intensity;
        vec3   pos;
        float  radius;
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
    struct Texture {
        uint8      *memory;
        int        width, height, origWidth, origHeight;
        TexFormat  fmt;
        uint32     opt;

        Texture(int width, int height, int depth, uint32 opt) : memory(0), width(width), height(height), origWidth(width), origHeight(height), fmt(FMT_RGBA), opt(opt) {}

        void init(void *data) {
            ASSERT((opt & OPT_PROXY) == 0);

            opt &= ~(OPT_CUBEMAP | OPT_MIPMAPS);

            memory = new uint8[width * height * 4];
            if (data) {
                update(data);
            }
        }

        void deinit() {
            if (memory) {
                delete[] memory;
            }
        }

        void generateMipMap() {}

        void update(void *data) {
            memcpy(memory, data, width * height * 4);
        }

        void bind(int sampler) {
            Core::active.textures[sampler] = this;

            if (!this || (opt & OPT_PROXY)) return;
            ASSERT(memory);

            curTile = NULL;
        }

        void bindTileIndices(Tile8 *tile) {
            curTile = (Tile8*)tile;
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


    int cullMode, blendMode;

    ColorSW *swColor;
    DepthSW *swDepth;
    short4  swClipRect;

    struct VertexSW {
        int32 x, y, z, w;
        int32 u, v, l;

        inline VertexSW operator + (const VertexSW &p) const {
            VertexSW ret;
            ret.x = x + p.x;
            ret.y = y;
            ret.z = z + p.z;
            ret.w = w + p.w;
            ret.u = u + p.u;
            ret.v = v + p.v;
            ret.l = l + p.l;
            return ret;
        }

        inline VertexSW operator - (const VertexSW &p) const {
            VertexSW ret;
            ret.x = x - p.x;
            ret.y = y;
            ret.z = z - p.z;
            ret.w = w - p.w;
            ret.u = u - p.u;
            ret.v = v - p.v;
            ret.l = l - p.l;
            return ret;
        }

        inline VertexSW operator * (const int32 s) const {
            VertexSW ret;
            ret.x = x * s;
            ret.y = y;
            ret.z = z * s;
            ret.w = w * s;
            ret.u = u * s;
            ret.v = v * s;
            ret.l = l * s;
            return ret;
        }

        inline VertexSW operator / (const int32 s) const {
            VertexSW ret;
            ret.x = x / s;
            ret.y = y;
            ret.z = z / s;
            ret.w = w / s;
            ret.u = u / s;
            ret.v = v / s;
            ret.l = l / s;
            return ret;
        }
    };

    Array<VertexSW> swVertices;
    Array<Index>    swIndices;
    Array<int32>    swTriangles;
    Array<int32>    swQuads;

    void init() {
        LOG("Renderer : %s\n", "Software");
        LOG("Version  : %s\n", "0.1");
        swDepth = NULL;
    #ifdef OL_MAGGIE
        if (Mag::init()) LOG("Maggie   : on\n");
    #endif
    }

    void deinit() {
    #ifdef OL_MAGGIE
        Mag::deinit();
    #endif
        delete[] swDepth;
        swVertices.clear();
        swIndices.clear();
        swTriangles.clear();
        swQuads.clear();
    }

    void resize() {
        delete[] swDepth;
        //swDepth = new DepthSW[Core::width * Core::height];
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
    #ifdef OL_MAGGIE
        Mag::setDepthSize(Core::width, Core::height);
        Mag::setup();
    #endif
        return true;
    }

    void endFrame() {
    #ifdef OL_MAGGIE
        Mag::sync();                     // the frame is complete before it is shown
    #endif
    }

    void resetState() {}

    void bindTarget(Texture *texture, int face) {}

    void discardTarget(bool color, bool depth) {}

    void copyTarget(Texture *dst, int xOffset, int yOffset, int x, int y, int width, int height) {}

    void setVSync(bool enable) {}

    void waitVBlank() {}

    void clear(bool color, bool depth) {
        if (color) {
        #ifdef OL_MAGGIE
            Mag::sync();                 // Maggie may still be writing the last spans
        #endif
            memset(swColor, 0x00, Core::width * Core::height * sizeof(ColorSW));
        }

        if (depth) {
            //memset(swDepth, 0xFF, Core::width * Core::height * sizeof(DepthSW));
        }
    }

    void setClearColor(const vec4 &color) {}

    void setViewport(const short4 &v) {}

    void setScissor(const short4 &s) {
        swClipRect.x = s.x;
        swClipRect.y = Core::active.viewport.w - (s.y + s.w);
        swClipRect.z = s.x + s.z;
        swClipRect.w = Core::active.viewport.w - s.y;
    }

    void setDepthTest(bool enable) {
    #ifdef OL_MAGGIE
        Mag::zTest = enable;
    #endif
    }

    void setDepthWrite(bool enable) {}

    void setColorWrite(bool r, bool g, bool b, bool a) {}

    void setAlphaTest(bool enable) {}

    void setCullMode(int rsMask) {}

    void setBlendMode(int rsMask) {}

    void setViewProj(const mat4 &mView, const mat4 &mProj) {}

    void updateLights(vec4 *lightPos, vec4 *lightColor, int count) {
        ambient = clamp(int32(active.material.y * 255), 0, 255);

        lightsCount = 0;
        for (int i = 0; i < count; i++) {
            if (lightColor[i].w >= 1.0f) {
                continue;
            }
            LightSW &light = lights[lightsCount++];
            vec4 &c = lightColor[i];
            light.intensity = uint32(((c.x + c.y + c.z) / 3.0f) * 255.0f);
            light.pos    = lightPos[i].xyz();
            light.radius = lightColor[i].w;
        }
    }

    void setFog(const vec4 &params) {}

    bool checkBackface(const VertexSW *a, const VertexSW *b, const VertexSW *c) {
        return ((b->x - a->x) >> 16) * (c->y - a->y) -
               ((c->x - a->x) >> 16) * (b->y - a->y) <= 0;
    }

    inline void sortVertices(VertexSW *&t, VertexSW *&m, VertexSW *&b) {
        if (t->y > m->y) swap(t, m);
        if (t->y > b->y) swap(t, b);
        if (m->y > b->y) swap(m, b);
    }

    inline void sortVertices(VertexSW *&t, VertexSW *&m, VertexSW *&b, VertexSW *&o) {
        if (t->y > m->y) swap(t, m);
        if (o->y > b->y) swap(o, b);
        if (t->y > o->y) swap(t, o);
        if (m->y > b->y) swap(m, b);
        if (m->y > o->y) swap(m, o);
    }

    inline void step(VertexSW &v, const VertexSW &d) {
    #ifdef OL_MAGGIE
        v.w += d.w;                      // 1/w for the perspective divide
    #endif
        v.u += d.u;
        v.v += d.v;
        v.l += d.l;
    }

    inline void step(VertexSW &v, const VertexSW &d, int32 count) {
    #ifdef OL_MAGGIE
        v.w += d.w * count;
    #endif
        v.u += d.u * count;
        v.v += d.v * count;
        v.l += d.l * count;
    }

    // https://www.flipcode.com/archives/Texturing_As_In_Unreal.shtml
    const int uvDither[8] = {
        32768, 16384,           0, 49152,   // (xx yy) for (y & 1 == 0)
        49152,     0,       32768, 16384    // (xx yy) for (y & 1 == 1)
    };

    void drawLine(const VertexSW &L, const VertexSW &R, int32 y) {
    #ifdef OL_MAGGIE
        {
            int32 xa = L.x >> 16;
            int32 x1 = xa;
            int32 x2 = R.x >> 16;
            int32 f = x2 - x1;
            if (f <= 0) return;
            float rf = f < 1024 ? Mag::rcp[f] : 1.0f / float(f);
            if (magAffine) {             // plain u, v (magPrepare)
                int32 u = L.u, v = L.v;
                int32 du = int32(float(R.u - L.u) * rf), dv = int32(float(R.v - L.v) * rf);
                int32 lL = Mag::shading ? Mag::rowToLight(L.l >> 3) : 0xFFFF;
                int32 lR = Mag::shading ? Mag::rowToLight(R.l >> 3) : 0xFFFF;
                int32 dl = int32(float(lR - lL) * rf);
                int32 z  = L.z;
                int32 dz = Mag::zOn ? int32(float(R.z - L.z) * rf) : 0;
                if (x1 < swClipRect.x) {
                    int32 k = swClipRect.x - x1;
                    u += du * k; v += dv * k; lL += dl * k; z += dz * k;
                    x1 = swClipRect.x;
                }
                if (x2 > swClipRect.z) x2 = swClipRect.z;
                if (x2 <= x1) return;
                if (magColored) Mag::bindFlat(L.u >> 16);
                Mag::depthMode();
                int32 offset = y * Core::width + x1;
                Mag::span(swColor + offset, x2 - x1, u, v, du, dv, lL, dl, offset, uint32(z) << 1, dz << 1);
                return;
            }
            // u/w, v/w, 1/w across the span (perspective form, see projectPush)
            float qL = float(L.w), uqL = float(L.u), vqL = float(L.v);
            float dq = (float(R.w) - qL) * rf, duq = (float(R.u) - uqL) * rf, dvq = (float(R.v) - vqL) * rf;
            int32 lL = Mag::shading ? Mag::rowToLight(L.l >> 3) : 0xFFFF;
            int32 lR = Mag::shading ? Mag::rowToLight(R.l >> 3) : 0xFFFF;
            int32 dl = int32(float(lR - lL) * rf);
            int32 z  = L.z;              // depth << 15
            int32 dz = Mag::zOn ? int32(float(R.z - L.z) * rf) : 0;
            if (x1 < swClipRect.x) {
                int32 k = swClipRect.x - x1;
                lL += dl * k; z += dz * k;
                x1 = swClipRect.x;
            }
            if (x2 > swClipRect.z) x2 = swClipRect.z;
            if (x2 <= x1) return;
            if (magColored) Mag::bindFlat(int32(uqL / max(qL, 1.0f) + 0.5f));   // U/Q = palette index
            Mag::depthMode();
            int32 offset = y * Core::width + x1;
            // pieces of at most MAG_PERSP_STEP pixels, affine inside; one piece when perspective is off
            // or 1/w hardly changes along the span (then exact endpoints are as good)
            float t = float(x1 - xa), te = float(x2 - xa);
            float q0 = qL + dq * t, qe = qL + dq * te;
            int32 step = 4096;
            if (Mag::perspective) {
                float qa = q0 < qe ? q0 : qe, qb = q0 < qe ? qe : q0;
                if (qb > qa * MAG_PERSP_RATIO) step = MAG_PERSP_STEP;
            }
            float r0 = 65536.0f / q0;
            float u0 = (uqL + duq * t) * r0, v0 = (vqL + dvq * t) * r0;
            for (int32 xs = x1; xs < x2; ) {
                int32 xe = min(xs + step, x2);
                int32 n = xe - xs;
                float tn = float(xe - xa);
                float r1 = 65536.0f / (xe == x2 ? qe : qL + dq * tn);
                float u1 = (uqL + duq * tn) * r1, v1 = (vqL + dvq * tn) * r1;
                float rn = n < 1024 ? Mag::rcp[n] : 1.0f / float(n);
                Mag::span(swColor + offset, n, int32(u0), int32(v0), int32((u1 - u0) * rn), int32((v1 - v0) * rn),
                          lL, dl, offset, uint32(z) << 1, dz << 1);
                lL += dl * n; z += dz * n; offset += n;
                u0 = u1; v0 = v1;
                xs = xe;
            }
            return;
        }
    #endif
        int32 x1 = L.x >> 16;
        int32 x2 = R.x >> 16;

        int32 f = x2 - x1;
        if (f == 0) return;

        VertexSW dS = (R - L) / f;
        VertexSW S  = L;

        if (x1 < swClipRect.x) {
            x1 = swClipRect.x - x1;
            S.z += dS.z * x1;
            step(S, dS, x1);
            x1 = swClipRect.x;
        }
        if (x2 > swClipRect.z) x2 = swClipRect.z;

        int32 i = y * Core::width;

    #ifdef DITHER_FILTER
        const int *dithY = uvDither + ((y & 1) * 4);
    #endif

        for (int x = i + x1; x < i + x2; x++) {
            S.z += dS.z;

            DepthSW z = DepthSW(uint32(S.z) >> 16);

            {//if (swDepth[x] >= z) {
            #ifdef DITHER_FILTER
                const int *dithX = dithY + (x & 1);

                uint32 u = uint32(S.u + dithX[0]) >> 16;
                uint32 v = uint32(S.v + dithX[2]) >> 16;
            #else
                uint32 u = uint32(S.u) >> 16;
                uint32 v = uint32(S.v) >> 16;
            #endif

                uint8 index = curTile->index[(v << 8) + u];

                if (index != 0) {
                    index = swLightmap[((S.l >> (16 + 3)) << 8) + index];

                    swColor[x] = swPalette[index];
                    //swDepth[x] = z;
                }
            }

            step(S, dS);
        }
    }

    void drawPart(const VertexSW &a, const VertexSW &b, const VertexSW &c, const VertexSW &d) {
        VertexSW L, R, dL, dR;
        int32 minY, maxY;

        int32 f = c.y - a.y;
        dL = (c - a) / f;
        dR = (d - b) / f;

        L = a;
        R = b;

        minY = a.y;
        maxY = c.y;

        if (maxY < swClipRect.y || minY >= swClipRect.w) return;

        if (minY < swClipRect.y) {
            minY = swClipRect.y - minY;
            L.x += dL.x * minY;
            L.z += dL.z * minY;
            R.x += dR.x * minY;
            R.z += dR.z * minY;
            step(L, dL, minY);
            step(R, dR, minY);
            minY = swClipRect.y;
        }

        if (maxY > swClipRect.w) maxY = swClipRect.w;

        for (int y = minY; y < maxY; y++) {
            drawLine(L, R, y);
            L.x += dL.x;
            L.z += dL.z;
            R.x += dR.x;
            R.z += dR.z;
            step(L, dL);
            step(R, dR);
        }
    }

#ifdef OL_MAGGIE
    // Only polygons whose 1/w varies noticeably get perspective-correct spans: their vertices are
    // converted to u/w, v/w (two multiplies each); the rest walk plain u, v as before.

    inline void magPrepare(VertexSW **v, VertexSW *tmp, int count) {
        int32 qa = v[0]->w, qb = v[0]->w;
        for (int i = 1; i < count; i++) {
            if (v[i]->w < qa) qa = v[i]->w;
            if (v[i]->w > qb) qb = v[i]->w;
        }
        magAffine = !Mag::perspective || float(qb) <= float(qa) * MAG_PERSP_RATIO;
        if (magAffine) return;
        // perspective form: U = u * 1024 / w = u * Q / 65536, interpolated linearly with Q
        for (int i = 0; i < count; i++) {
            tmp[i] = *v[i];
            float q = float(v[i]->w) * (1.0f / 65536.0f);
            tmp[i].u = int32(float(v[i]->u) * q);
            tmp[i].v = int32(float(v[i]->v) * q);
            v[i] = tmp + i;
        }
    }
#endif

    void drawTriangle(Index *indices) {
    /*
             t
            /\ <----- top triangle
         m /__\/ n
           \  /\
             \  \ <-- bottom triangle
               \ \
                 \\
                   \
                    b
    */
        VertexSW _n;
        VertexSW *t = swVertices.items + indices[0];
        VertexSW *m = swVertices.items + indices[1];
        VertexSW *b = swVertices.items + indices[2];
        VertexSW *n = &_n;

        if (checkBackface(t, m, b))
            return;

        int32 cx1 = swClipRect.x << 16;
        int32 cx2 = swClipRect.z << 16;

        if (t->x < cx1 && m->x < cx1 && b->x < cx1)
            return;

        if (t->x > cx2 && m->x > cx2 && b->x > cx2)
            return;

    #ifdef OL_MAGGIE
        VertexSW magTmp[3], *magV[3] = { t, m, b };
        magPrepare(magV, magTmp, 3);
        t = magV[0]; m = magV[1]; b = magV[2];
    #endif
        sortVertices(t, m, b);

        if (b->y < swClipRect.y || t->y > swClipRect.w)
            return;

        *n = ((*b - *t) / (b->y - t->y) * (m->y - t->y)) + *t;
        n->y = m->y;

        if (m->x > n->x) {
            swap(m, n);
        }

        if (m->y != t->y) drawPart(*t, *t, *m, *n);
        if (m->y != b->y) drawPart(*m, *n, *b, *b);
    }

    void drawQuad(Index *indices) {
    /*
             t
            /\ <----- top triangle
         m /__\/ n
           \  /\
            \   \ <-- quad
           p \/__\ o
             /\  / 
               \/ <-- bottom triangle
               b
    */
        VertexSW _n;
        VertexSW _p;
        VertexSW *t = swVertices.items + indices[0];
        VertexSW *m = swVertices.items + indices[1];
        VertexSW *b = swVertices.items + indices[2];
        VertexSW *o = swVertices.items + indices[3];
        VertexSW *n = &_n;
        VertexSW *p = &_p;

        if (checkBackface(t, m, b))
            return;

        int32 cx1 = swClipRect.x << 16;
        int32 cx2 = swClipRect.z << 16;

        if (t->x < cx1 && m->x < cx1 && o->x < cx1 && b->x < cx1)
            return;

        if (t->x > cx2 && m->x > cx2 && o->x > cx2 && b->x > cx2)
            return;

    #ifdef OL_MAGGIE
        VertexSW magTmp[4], *magV[4] = { t, m, b, o };
        magPrepare(magV, magTmp, 4);
        t = magV[0]; m = magV[1]; b = magV[2]; o = magV[3];
    #endif
        sortVertices(t, m, b, o);

        if (b->y < swClipRect.y || t->y > swClipRect.w)
            return;

        if (checkBackface(t, b, m) == checkBackface(t, b, o)) {

            VertexSW d = (*b - *t) / (b->y - t->y);

            *n = *t + d * (m->y - t->y);
            *p = *t + d * (o->y - t->y);

            n->y = m->y;
            p->y = o->y;

        } else {

            if (o->y != t->y) {
                *n = *t + ((*o - *t) / (o->y - t->y) * (m->y - t->y));
                n->y = m->y;
            }

            if (m->y != b->y) {
                *p = *b + ((*m - *b) / (m->y - b->y) * (o->y - b->y));
                p->y = o->y;
            }

        }

        if (o->y != t->y && m->x > n->x) swap(m, n);
        if (m->y != b->y && p->x > o->x) swap(p, o);

        if (t->y != m->y) drawPart(*t, *t, *m, *n);
        if (m->y != o->y) drawPart(*m, *n, *p, *o);
        if (o->y != b->y) drawPart(*p, *o, *b, *b);
    }

    void applyLighting(VertexSW &result, const Vertex &vertex, float depth) {
        vec3 coord  = vec3(float(vertex.coord.x), float(vertex.coord.y), float(vertex.coord.z));
        vec3 normal = vec3(float(vertex.normal.x), float(vertex.normal.y), float(vertex.normal.z)).normal();
        float lighting = 0.0f;
        for (int i = 0; i < lightsCount; i++) {
            LightSW &light = lightsRel[i];
            vec3 dir = (light.pos - coord) * light.radius;
            float att = dir.length2();
            float lum = normal.dot(dir / sqrtf(att));
            lighting += (max(0.0f, lum) * max(0.0f, 1.0f - att)) * light.intensity;
        }

        lighting += result.l;

        depth -= SW_FOG_START;
        if (depth > 0.0f) {
            lighting *= clamp(1.0f - depth / (SW_MAX_DIST - SW_FOG_START), 0.0f, 1.0f);
        }

        result.l = (255 - min(255, int32(lighting))) << 16;
    }

#ifdef SW_NEAR_CLIP
    // Near-plane clipping (instead of dropping every primitive with a vertex behind the camera,
    // which left holes in floors and walls close to the camera): primitives are walked whole, those
    // crossing w = SW_NEAR are clipped in clip space and drawn as a triangle fan.
    #define SW_NEAR 32.0f
    bool swNearClip = true;              // false: drop primitives crossing the near plane (old behaviour)

    struct ClipVertex {
        vec4  c;                         // clip space (after the viewport matrix)
        int32 u, v, l;
    };

    inline void projectPush(const vec4 &c, int32 u, int32 v, int32 l) {
        float invw = 1.0f / c.w;
        float x = clamp(c.x * invw, -16384.0f, 16384.0f);
        float y = clamp(c.y * invw, -16384.0f, 16384.0f);
        float z = c.z * invw;
        VertexSW r;
        r.x = int32(x) << 16;
        r.y = int32(y);
    #ifdef OL_MAGGIE
        r.z = int32(clamp(z, 0.0f, 1.0f) * 65535.0f) << 15;
    #else
        r.z = uint32(clamp(z, 0.0f, 1.0f) * 65535.0f) << 16;
    #endif
    #ifdef OL_MAGGIE
        // w holds Q = 2^26 / w (w >= 16) for perspective-correct texturing (see magPrepare)
        r.w = int32(67108864.0f * (c.w > 16.0f ? invw : 1.0f / 16.0f));
        r.u = u;
        r.v = v;
    #else
        r.w = int32(c.w) << 16;
        r.u = u;
        r.v = v;
    #endif
        r.l = l;
        swIndices.push(swVertices.push(r));
    }

    bool transform(const Index *indices, const Vertex *vertices, int iStart, int iCount, int vStart) {
        swVertices.reset();
        swIndices.reset();
        swTriangles.reset();
        swQuads.reset();

        mat4 swMatrix;
        swMatrix.viewport(0.0f, (float)Core::height, (float)Core::width, -(float)Core::height, 0.0f, 1.0f);
        swMatrix = swMatrix * mViewProj * mModel;

        const bool colored = vertices[vStart + indices[iStart]].color.w == 142;
        // orthographic projections (UI, 2D) have w = 1 everywhere: only perspective ones get the near plane
        const float nearW = (Core::mProj.e33 == 1.0f) ? 0.0f : SW_NEAR;

        int i = 0;
        while (i < iCount) {
            const Index *idx = indices + iStart + i;
            bool isTriangle = vertices[vStart + idx[0]].normal.w == 1;
            int count = isTriangle ? 3 : 4;
            i += isTriangle ? 3 : 6;
            if (i > iCount) break;

            ClipVertex cv[4];
            int behind = 0, k;
            for (k = 0; k < count; k++) {
                const Vertex &vertex = vertices[vStart + idx[k == 3 ? 5 : k]];   // quads come as 012 023
                ClipVertex &o = cv[k];
                o.c = swMatrix * vec4(vertex.coord.x, vertex.coord.y, vertex.coord.z, 1.0f);
                if (o.c.w > SW_MAX_DIST) break;
                if (o.c.w < nearW) {
                    behind++;
                    if (!swNearClip) break;
                }
                VertexSW lit;
                lit.l = ((vertex.light.x * ambient) >> 8);
                applyLighting(lit, vertex, o.c.w > 0.0f ? o.c.w : 0.0f);
                o.l = lit.l;
                if (colored) {
                    o.u = vertex.color.x << 16;
                    o.v = 0;
                } else {
                    o.u = vertex.texCoord.x << 16;
                    o.v = vertex.texCoord.y << 16;
                }
            }
            if (k < count || behind == count) continue;     // too far, or dropped, or wholly behind

            if (!behind) {
                for (k = 0; k < count; k++) projectPush(cv[k].c, cv[k].u, cv[k].v, cv[k].l);
                if (isTriangle)
                    swTriangles.push(swIndices.length - 3);
                else
                    swQuads.push(swIndices.length - 4);
                continue;
            }

            // Sutherland-Hodgman against w >= nearW: at most count + 1 vertices
            int base = swIndices.length, n = 0;
            for (k = 0; k < count; k++) {
                const ClipVertex &a = cv[k];
                const ClipVertex &b = cv[(k + 1) % count];
                bool aIn = a.c.w >= nearW, bIn = b.c.w >= nearW;
                if (aIn) { projectPush(a.c, a.u, a.v, a.l); n++; }
                if (aIn != bIn) {
                    float t = (nearW - a.c.w) / (b.c.w - a.c.w);
                    vec4 c = a.c + (b.c - a.c) * t;
                    c.w = nearW;
                    projectPush(c, a.u + int32(float(b.u - a.u) * t), a.v + int32(float(b.v - a.v) * t),
                                   a.l + int32(float(b.l - a.l) * t));
                    n++;
                }
            }
            for (k = 1; k + 1 < n; k++) {               // fan, same winding
                swIndices.push(swIndices[base]);
                swIndices.push(swIndices[base + k]);
                swIndices.push(swIndices[base + k + 1]);
                swTriangles.push(swIndices.length - 3);
            }
        }

        return colored;
    }
#else
    bool transform(const Index *indices, const Vertex *vertices, int iStart, int iCount, int vStart) {
        swVertices.reset();
        swIndices.reset();
        swTriangles.reset();
        swQuads.reset();

        mat4 swMatrix;
        swMatrix.viewport(0.0f, (float)Core::height, (float)Core::width, -(float)Core::height, 0.0f, 1.0f);
        swMatrix = swMatrix * mViewProj * mModel;

        const bool colored = vertices[vStart + indices[iStart]].color.w == 142;
        int vIndex = 0;
        bool isTriangle = false;

        for (int i = 0; i < iCount; i++) {
            const Index  index   = indices[iStart + i];
            const Vertex &vertex = vertices[vStart + index];

            vIndex++;

            if (vIndex == 1) {
                isTriangle = vertex.normal.w == 1;
            } else {
                if (vIndex == 4) { // loader splits quads to two triangles with indices 012[02]3, we ignore [02] to make it quad again!
                    vIndex++;
                    i++;
                    continue;
                }
            }

            vec4 c;
            c = swMatrix * vec4(vertex.coord.x, vertex.coord.y, vertex.coord.z, 1.0f);

            if (c.w < 0.0f || c.w > SW_MAX_DIST) { // skip primitive
                if (isTriangle) {
                    i += 3 - vIndex;
                } else {
                    i += 6 - vIndex;
                }
                vIndex = 0;
                continue;
            }

            c.x /= c.w;
            c.y /= c.w;
            c.z /= c.w;
            c.x = clamp(c.x, -16384.0f, 16384.0f);
            c.y = clamp(c.y, -16384.0f, 16384.0f);

            VertexSW result;
            result.x = int32(c.x) << 16;
            result.y = int32(c.y);
        #ifdef OL_MAGGIE
            result.z = int32(clamp(c.z, 0.0f, 1.0f) * 65535.0f) << 15;   // 31 bits: edge stepping is signed
        #else
            result.z = uint32(clamp(c.z, 0.0f, 1.0f) * 65535.0f) << 16;
        #endif
            result.w = int32(c.w);

            if (colored) {
                result.u = vertex.color.x << 16;
                result.v = 0;
            } else {
                result.u = (vertex.texCoord.x << 16);// / result.w;
                result.v = (vertex.texCoord.y << 16);// / result.w;
            }
            result.w = result.w << 16;
            result.l = ((vertex.light.x * ambient) >> 8);

            applyLighting(result, vertex, c.w);

            swIndices.push(swVertices.push(result));

            if (isTriangle && vIndex == 3) {
                swTriangles.push(swIndices.length - 3);
                vIndex = 0;
            } else if (vIndex == 6) {
                swQuads.push(swIndices.length - 4);
                vIndex = 0;
            }
        }

        return colored;
    }
#endif

    void transformLights() {
        memcpy(lightsRel, lights, sizeof(LightSW) * lightsCount);

        mat4 mModelInv = mModel.inverseOrtho();
        for (int i = 0; i < lightsCount; i++) {
            lightsRel[i].pos = mModelInv * lights[i].pos;
        }
    }

    void DIP(Mesh *mesh, const MeshRange &range) {
        if (curTile == NULL) {
            //uint32 *tex = (uint32*)Core::active.textures[0]->memory; // TODO
            return;
        }

        transformLights();

        bool colored = transform(mesh->iBuffer, mesh->vBuffer, range.iStart, range.iCount, range.vStart);

        Tile8 *oldTile = curTile;

        if (colored) {
            curTile = (Tile8*)swGradient;
        }

    #ifdef OL_MAGGIE
        magColored = colored;
        if (!colored && !Mag::bindTile(oldTile)) {
            curTile = oldTile;
            return;
        }
    #endif

        for (int i = 0; i < swQuads.length; i++) {
            drawQuad(&swIndices[swQuads[i]]);
        }

        for (int i = 0; i < swTriangles.length; i++) {
            drawTriangle(&swIndices[swTriangles[i]]);
        }

        curTile = oldTile;
    }

    void initPalette(Color24 *palette, uint8 *lightmap) {
        for (uint32 i = 0; i < 256; i++) {
            const Color24 &p = palette[i];
            swPaletteColor[i] = CONV_COLOR(p.r, p.g, p.b);
            swPaletteWater[i] = CONV_COLOR((uint32(p.r) * 150) >> 8, (uint32(p.g) * 230) >> 8, (uint32(p.b) * 230) >> 8);
            swPaletteGray[i]  = CONV_COLOR((i * 57) >> 8, (i * 29) >> 8, (i * 112) >> 8);
            swGradient[i]     = i;
        }

        for (uint32 i = 0; i < 256 * 32; i++) {
            swLightmapNone[i]  = i % 256;
            swLightmapShade[i] = lightmap[i];
        }

        swLightmap = swLightmapShade;
        swPalette  = swPaletteColor;
    #ifdef OL_MAGGIE
        Mag::setPalette(palette);
    #endif
    }

    void setPalette(ColorSW *palette) {
        swPalette = palette;
    #ifdef OL_MAGGIE
        Mag::tint = (palette == swPaletteWater) ? 0x96E6E6 : 0xFFFFFF;   // as the water palette: r*150, g,b*230 /256
    #endif
    }

    void setShading(bool enabled) {
        swLightmap = enabled ? swLightmapShade : swLightmapNone;
    #ifdef OL_MAGGIE
        Mag::shading = enabled;
    #endif
    }

    vec4 copyPixel(int x, int y) {
        return vec4(0.0f); // TODO: read from framebuffer
    }
}

#endif
