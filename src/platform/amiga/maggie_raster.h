// Transform and rasteriser of the Maggie build (included inside namespace GAPI by gapi/sw.h).
//
// The 68080 runs integer code at roughly a cycle per instruction, but an FPU add or multiply costs
// 4-5 cycles, a float to int conversion as much again and a taken FPU branch about 12. So:
//   - polygons off the screen or facing away are dropped before any lighting or dividing;
//   - float compares are done on the bit patterns (integer compares);
//   - vertices are handed to the rasteriser as integers (28.4 pixels), and everything per edge,
//     per line and per span is integer arithmetic. Floats are left for the transform itself, one
//     reciprocal per polygon and edge, and the perspective-correct spans.
//
// Subpixel: vertices keep 1/16 pixel and a pixel is drawn when its centre is inside the polygon
// (top-left rule), textured as seen from that centre. With whole-pixel vertices every small camera
// move (Lara breathing) made edges and textures jump a pixel at a time.
// Texture and depth are planes over the polygon (gradients once per polygon); light runs down the
// edges, since a quad's four lights are not a plane. A quad is drawn in one pass when it is a
// parallelogram with matching texture corners (then the planes of three corners fit the fourth),
// otherwise as two triangles, like the hardware renderers do.
#ifndef H_MAGGIE_RASTER
#define H_MAGGIE_RASTER

    union MagFI { float f; int32 i; };
    inline int32 magBits(float f) { MagFI u; u.f = f; return u.i; }      // positive floats order like ints
    // float -> int rounded to nearest (the FPU's mode): one FMOVE.L. C's int32(f) must truncate, so gcc
    // adds an FINTRZ before it, about as dear again. For gradients, steps and positions nearest is as
    // good; every path that makes a shared vertex must use the same one (no crack between neighbours).
#if defined(__mc68000__) || defined(__m68k__)
    inline int32 magInt(float f) { int32 r; __asm__("fmove.l %1,%0" : "=d"(r) : "f"(f)); return r; }
#else
    inline int32 magInt(float f) { return int32(lrintf(f)); }
#endif

    struct MagVtx {
        int32 x, y;                      // 28.4 pixels
        int32 z;                         // depth (1..65534) << 15
        int32 u, v;                      // 16.16 texels (u: palette index << 16 for flat colours)
        int32 l;                         // Maggie intensity (16..0xFFEF) << 15
        float q;                         // 1/w
    };

    struct MagPrim {
        MagVtx *v[4];                    // clockwise on screen
        int32   n;                       // 3, or 4 for a flat quad
        float   area;                    // cross product of v0 v1 v2 in pixels, > 0
        uint32  tint;                    // hardware back end: colour multiply 0x00RRGGBB
    };

    MagVtx  *magVtxBuf  = NULL;
    MagPrim *magPrimBuf = NULL;
    int      magVtxCap = 0, magPrimCap = 0;

    void magFree() {
        free(magVtxBuf);  magVtxBuf = NULL;  magVtxCap = 0;
        free(magPrimBuf); magPrimBuf = NULL; magPrimCap = 0;
    }

    #define MAG_MIN_AREA  0.002f         // pixels^2 (cross product): smaller polygons cover no pixel centre worth drawing
    #define MAG_XY_MAX    (8192 << 4)    // vertex clamp: an edge's 16.16 step per line then fits 32 bits
    #define MAG_SMALL     3              // pixels: polygons smaller than this both ways are drawn with one texel (hardware back end)
    #define MAG_GUARD     6000           // pixels: polygons reaching further off screen are clipped (hardware back end)

#ifdef _GAPI_SW
    // per polygon: flat colours pick their palette colour; the mode follows the depth test
    #define MAG_PRIM_BEGIN(pr, a) { if (magColored) Mag::bindFlat((a)->u >> 16); Mag::depthMode(); }
#endif

// --- rasteriser ---------------------------------------------------------------------------------
// magDraw finds the polygon's rows and picks one of three walkers: small polygons (one texel), affine
// (all integer, the common case) and perspective. Each is its own function, so the cheap ones do not
// pay for the registers the perspective one saves on entry, and each is specialised (templates) on
// what a polygon keeps for all its rows (light plane or edge light, depth on or off), so no row tests
// them. Edges are walked in runs: the inner loop goes from one corner to the next with both edges in
// locals (registers); a new edge is set up between runs.
    struct MagEdge {
        int32 x, sx;                     // 16.16 x at the centre of the current line, step per line
        int32 l, sl;                     // intensity << 15
        int32 yEnd;                      // first line below the edge
        int   i;                         // vertex it ends at
    };

    struct MagRows {                     // what every walker gets from magDraw
        MagVtx *const *p;
        int     n, top, bot;
        int32   y, yEnd;                 // lines drawn, clipped to the screen
        int32   xmin, xmax;              // 28.4
    };

    // the next edge of a chain (dir: +1 right side, n - 1 left side) that covers line y
    __attribute__((noinline)) bool magEdgeNext(MagEdge &e, MagVtx *const *p, int n, int dir, int32 y, int bottom) {
        int from;
        do {
            from = e.i;
            if (from == bottom) return false;
            e.i = from + dir;
            if (e.i >= n) e.i -= n;
            e.yEnd = (p[e.i]->y + 7) >> 4;
        } while (e.yEnd <= y);
        const MagVtx *P = p[from], *Q = p[e.i];
        float inv = 65536.0f / float(Q->y - P->y);
        int32 t = (y << 4) + 8 - P->y;                   // from P down to the line's centre, 28.4
        int32 lines = t >> 4, frac = t & 15;
        int32 sx = magInt(float(Q->x - P->x) * inv);
        int32 sl = magInt(float(Q->l - P->l) * inv * (1.0f / 4096.0f));
        e.sx = sx;
        e.x  = (P->x << 12) + sx * lines + (sx >> 4) * frac;
        e.sl = sl;
        e.l  = P->l + sl * lines + (sl >> 4) * frac;
        return true;
    }

    // Edge walking shared by the walkers. Expects locals y, yEnd, R (the MagRows) and runs ROW for
    // every line from y to yEnd with xL, xR (16.16) and lL, lR (light << 15) at the line's centre.
    // ROW must not change them, and sees the line number y only in the emulator build (trace).
    // Leaves y on the first line not drawn.
    #define MAG_WALK(...) {                                                                              \
        MagEdge eL, eR;                                                                              \
        eL.i = eR.i = R.top;                                                                         \
        int32 xL = 0, sxL = 0, lL = 0, slL = 0, yeL = y;                                             \
        int32 xR = 0, sxR = 0, lR = 0, slR = 0, yeR = y;                                             \
        while (y < yEnd) {                                                                           \
            if (y >= yeL) {                                                                          \
                if (!magEdgeNext(eL, R.p, R.n, R.n - 1, y, R.bot)) break;                            \
                xL = eL.x; sxL = eL.sx; lL = eL.l; slL = eL.sl; yeL = eL.yEnd;                       \
            }                                                                                        \
            if (y >= yeR) {                                                                          \
                if (!magEdgeNext(eR, R.p, R.n, 1, y, R.bot)) break;                                  \
                xR = eR.x; sxR = eR.sx; lR = eR.l; slR = eR.sl; yeR = eR.yEnd;                       \
            }                                                                                        \
            int32 stop = yeL < yeR ? yeL : yeR;                                                      \
            if (stop > yEnd) stop = yEnd;                                                            \
            int32 rows = stop - y;                                                                   \
            do {                                                                                     \
                __VA_ARGS__                                                                          \
                xL += sxL; xR += sxR;                                                                \
                if (EDGE_LIGHT) { lL += slL; lR += slR; }                                            \
                MAG_TRACE_Y();                                                                       \
            } while (--rows > 0);                                                                    \
            y = stop;                                                                                \
        }                                                                                            \
    }

    // light along a span from the edges' lights, 15 bits, rounded towards zero so it stays between its
    // ends; returns the light at xs and the step per pixel in dl (both << 1 for Maggie's 16 bits)
    __attribute__((always_inline)) inline int32 magSpanLight(int32 xa, int32 xb, int32 xs, int32 lL, int32 lR, int32 &dlOut) {
        int32 w = xb - xa;
        int32 la = lL >> 16, d = (lR >> 16) - la, dl;
        if (w < 1024) {
            uint32 r = Mag::rcpi[w];
            dl = d >= 0 ? int32((uint32(d) * r) >> 16) : -int32((uint32(-d) * r) >> 16);
            if (dl > 16383) dl = 16383;
            if (dl < -16383) dl = -16383;
        } else
            dl = d / w;
        dlOut = dl << 1;
        return (la + dl * (xs - xa)) << 1;
    }

#ifdef OL_EMU
    const void *magTracePrim = NULL;
    #define MAG_TRACE_ROW() { if (&pr == magTracePrim && y >= Mag::pickY - 6 && y <= Mag::pickY + 6)                       \
        printf("   row %ld: xa %ld xb %ld xs %ld len %ld clip %d..%d y %d..%d\n", (long)y, (long)xa, (long)xb, (long)xs, (long)len, \
               int(clipX1), int(clipX2), int(swClipRect.y), int(swClipRect.w)); }
    #define MAG_TRACE_Y() y++
#else
    #define MAG_TRACE_ROW() {}
    #define MAG_TRACE_Y() {}
#endif

#ifdef _GAPI_MAGGIE
    // Small polygons (a third of those drawn in LEVEL2: distant faces, model detail) take one texel, light
    // and depth for the whole polygon, the mean of the corners, instead of planes over it: no
    // gradient setup. The edges are walked as for any polygon, so exactly the same pixels are drawn
    // and neighbours still meet without cracks.
    __attribute__((noinline)) void magDrawSmall(const MagRows &R) {
        MagVtx *const *p = R.p;
        const int n = R.n;
        const MagVtx *a = p[0];
        int32 su = 0, sv = 0, sl = 0, sz = 0;
        for (int i = 1; i < n; i++) { su += p[i]->u - a->u; sv += p[i]->v - a->v; sl += p[i]->l - a->l; sz += p[i]->z - a->z; }
        int32 u, v, l, z;
        if (n == 4) { u = a->u + (su >> 2); v = a->v + (sv >> 2); l = a->l + (sl >> 2); z = a->z + (sz >> 2); }
        else        { u = a->u + su / 3;    v = a->v + sv / 3;    l = a->l + sl / 3;    z = a->z + sz / 3; }
        l >>= 15;
        const uint32 zs2 = uint32(z) << 1;
        const bool zs = Mag::zSpan;
        const int32 clipX1 = swClipRect.x, clipX2 = swClipRect.z, pitch = Core::width;
        int32 y = R.y, yEnd = R.yEnd;
        int32 lineOffset = y * pitch;
        bool first = true;
        const bool EDGE_LIGHT = false;
        MAG_WALK({
            int32 xa = (xL + 0x7FFF) >> 16, xb = (xR + 0x7FFF) >> 16;
            int32 xs = xa < clipX1 ? clipX1 : xa;
            int32 xe = xb > clipX2 ? clipX2 : xb;
            int32 len = xe - xs;
            if (len > 0) {
                if (first) { first = false; Mag::polySteps(0, 0, 0); Mag::lightStep(0); }
                int32 offset = lineOffset + xs;
                Mag::spanSteps(swColor + offset, len, u, v, l, offset, zs2, zs);
            }
            lineOffset += pitch;
        })
    }
#endif

    // gradients per pixel of a value known at a, b, c:  d/dx = (B - A) * kbx + (C - A) * kcx
    struct MagGrad {
        float kbx, kcx, kby, kcy;
        int32 X0, Y0, fx, fy, dyRow;     // pixel next to vertex a, a's offset from its centre (28.4), first line - Y0
        MagGrad(const MagVtx *a, const MagVtx *b, const MagVtx *c, float area, int32 y) {
            float inv = 0.0625f / area;
            kbx = float(c->y - a->y) * inv; kcx = float(a->y - b->y) * inv;
            kby = float(a->x - c->x) * inv; kcy = float(b->x - a->x) * inv;
            X0 = (a->x + 7) >> 4; Y0 = (a->y + 7) >> 4;
            fx = (X0 << 4) + 8 - a->x; fy = (Y0 << 4) + 8 - a->y;
            dyRow = y - Y0;
        }
        // Integer rows: the value at the centre of the pixel next to vertex a, then row(y) + x * step,
        // all wrapping 32-bit arithmetic. Returns row(first line), steps per pixel and per line.
        __attribute__((always_inline)) inline uint32 plane(int32 A, int32 B, int32 C, int32 &dx, int32 &dy) const {
            float fb = float(B - A), fc = float(C - A);
            dx = magInt(fb * kbx + fc * kcx);
            dy = magInt(fb * kby + fc * kcy);
            return uint32(A + (dx >> 4) * fx + (dy >> 4) * fy) + uint32(dyRow) * uint32(dy) - uint32(X0) * uint32(dx);
        }
    };

    // The common case, all integers. Texture and depth steps go to Maggie once; a triangle's light is a
    // plane too (one step for the polygon), a quad's runs down its edges.
    template <bool LIGHT_PLANE, bool ZS>
    __attribute__((noinline)) void magDrawAffine(const MagRows &R, const MagPrim &pr) {
        MagVtx *const *p = R.p;
        const MagVtx *a = p[0], *b = p[1], *c = p[2];
        int32 y = R.y, yEnd = R.yEnd;
        const MagGrad g(a, b, c, pr.area, y);
        int32 dz = 0, dzY = 0, du = 0, dv = 0, duY = 0, dvY = 0, dlx = 0, dlY = 0;
        uint32 zRow = 0, uRow = 0, vRow = 0, lRow = 0;
        if (ZS) zRow = g.plane(a->z, b->z, c->z, dz, dzY);
        if (!magColored) {
            uRow = g.plane(a->u, b->u, c->u, du, duY);
            vRow = g.plane(a->v, b->v, c->v, dv, dvY);
        }
        if (LIGHT_PLANE) lRow = g.plane(a->l, b->l, c->l, dlx, dlY);
        const int32 clipX1 = swClipRect.x, clipX2 = swClipRect.z, pitch = Core::width;
        int32 lineOffset = y * pitch;
        bool first = true;               // the per-polygon registers go out with the first span: an earlier
                                         // write would wait for the span in flight instead of working beside it
        const bool EDGE_LIGHT = !LIGHT_PLANE;
        MAG_WALK({
            int32 xa = (xL + 0x7FFF) >> 16, xb = (xR + 0x7FFF) >> 16;
            int32 xs = xa < clipX1 ? clipX1 : xa;
            int32 xe = xb > clipX2 ? clipX2 : xb;
            int32 len = xe - xs;
            MAG_TRACE_ROW();
            if (len > 0) {
                int32 l;
                if (LIGHT_PLANE) {
                    l = int32((lRow + uint32(xs) * uint32(dlx)) >> 15);
                    if (uint32(l) > 0xFFFF) l = l < 0 ? 0 : 0xFFFF;    // (a rounding step outside the range)
                } else {
                    int32 dl;
                    l = magSpanLight(xa, xb, xs, lL, lR, dl);
                    Mag::lightStep(dl);
                }
                if (first) {
                    first = false;
                    Mag::polySteps(du, dv, dz << 1);
                    if (LIGHT_PLANE) Mag::lightStep(dlx / 32768);     // towards zero: a span's light stays between its ends
                }
                int32 offset = lineOffset + xs;
                MAG_COUNT(5, (dv < 0 ? -dv : dv) > (du < 0 ? -du : du) ? len : 0);     // pixels of spans that run along v
                Mag::spanSteps(swColor + offset, len, int32(uRow + uint32(xs) * uint32(du)), int32(vRow + uint32(xs) * uint32(dv)),
                               l, offset, ZS ? (zRow + uint32(xs) * uint32(dz)) << 1 : 0, ZS);
            }
            lineOffset += pitch;
            if (ZS) zRow += uint32(dzY);
            uRow += uint32(duY); vRow += uint32(dvY);
            if (LIGHT_PLANE) lRow += uint32(dlY);
        })
    }

    // Perspective: per line 1/w, u/w, v/w at the line's first pixel, then affine pieces.
    template <bool ROW_CONST>
    __attribute__((noinline)) void magDrawPersp(const MagRows &R, const MagPrim &pr) {
        MagVtx *const *p = R.p;
        const int n = R.n;
        const MagVtx *a = p[0], *b = p[1], *c = p[2];
        int32 y = R.y, yEnd = R.yEnd;
        const MagGrad g(a, b, c, pr.area, y);
        const int32 X0 = g.X0;
        int32 dz = 0, dzY = 0;
        uint32 zRow = 0;
        if (Mag::zSpan) zRow = g.plane(a->z, b->z, c->z, dz, dzY);
        const int32 dz2 = dz << 1;

        float ax = float(a->x) * 0.0625f, ay = float(a->y) * 0.0625f;
        float ox = float(X0) + 0.5f - ax, oy = float(y) + 0.5f - ay;
        float fb = b->q - a->q, fc = c->q - a->q;
        const float qdx = fb * g.kbx + fc * g.kcx, qdy = fb * g.kby + fc * g.kcy;
        float qRow = a->q + ox * qdx + oy * qdy;          // 1/w, u/w, v/w at pixel (X0, y), near the polygon
        float ua = float(a->u) * a->q;
        fb = float(b->u) * b->q - ua; fc = float(c->u) * c->q - ua;
        const float udx = fb * g.kbx + fc * g.kcx, udy = fb * g.kby + fc * g.kcy;
        float uqRow = ua + ox * udx + oy * udy;
        float va = float(a->v) * a->q;
        fb = float(b->v) * b->q - va; fc = float(c->v) * c->q - va;
        const float vdx = fb * g.kbx + fc * g.kcx, vdy = fb * g.kby + fc * g.kcy;
        float vqRow = va + ox * vdx + oy * vdy;
        const float pieceK = ROW_CONST ? 0.0f : (4.0f * MAG_PERSP_ERR) / (magBits(qdx) < 0 ? -qdx : qdx);  // inf when qdx = 0: one piece
        (void)n;

        const int32 clipX1 = swClipRect.x, clipX2 = swClipRect.z, pitch = Core::width;
        int32 lineOffset = y * pitch;
        const bool EDGE_LIGHT = true;
        MAG_WALK({
            int32 xa = (xL + 0x7FFF) >> 16, xb = (xR + 0x7FFF) >> 16;
            int32 xs = xa < clipX1 ? clipX1 : xa;
            int32 xe = xb > clipX2 ? clipX2 : xb;
            int32 len = xe - xs;
            if (len > 0) {
                int32 dl;
                int32 l = magSpanLight(xa, xb, xs, lL, lR, dl);
                int32 offset = lineOffset + xs;
                uint32 z = (zRow + uint32(xs) * uint32(dz)) << 1;
                float fxs = float(xs - X0);
                if (ROW_CONST) {
                    // 1/w the same all along the line (floors and ceilings: the camera never rolls): one
                    // exact affine span, u = (u/w) / (1/w) and its step (du/w) / (1/w), one divide
                    float r = 1.0f / (qRow + fxs * qdx);
                    float u0 = uqRow + fxs * udx, v0 = vqRow + fxs * vdx;
                    Mag::span(swColor + offset, len, magInt(u0 * r), magInt(v0 * r), magInt(udx * r), magInt(vdx * r),
                              l, dl, offset, z, dz2);
                } else {
                    // Affine pieces, as many as the error bound asks for; one piece when 1/w hardly
                    // changes along the span. Only pixel centres inside the polygon are evaluated
                    // (1/w > 0 there): the last piece ends on the last pixel.
                    // Both ends first: their divides overlap, and the last piece's end is the span's end.
                    float q0 = qRow + fxs * qdx, u0 = uqRow + fxs * udx, v0 = vqRow + fxs * vdx;
                    int32 m = len - 1;
                    float fm = float(m);
                    float qe = q0 + qdx * fm, ue = u0 + udx * fm, ve = v0 + vdx * fm;
                    float r = 1.0f / q0, re = 1.0f / qe;
                    // One affine piece is off by about |dq/dx| len^2 / (4 qlo) pixels, a piece of S by S^2 / len^2 of that:
                    // S^2 <= 4 ERR qlo / |dq/dx| = qlo * pieceK. Pieces are a power of two long (8..512), so a piece's
                    // steps are shifts of the integer u, v at its ends; only the last piece, of any length, divides.
                    // S = 2^floor(log2(x) / 2) read from the float's exponent. 1/w > 0 at the pixel centres drawn.
                    int32 qlo = magBits(q0) < magBits(qe) ? magBits(q0) : magBits(qe);
                    MagFI lo; lo.i = qlo;
                    float x = lo.f * pieceK;
                    int32 sh = 12;                   // one piece
                    if (magBits(x - float(m * len)) < 0) {
                        sh = ((magBits(x) >> 23) - 127) >> 1;
                        if (sh < 3) sh = 3;
                        if (sh > 9) sh = 9;
                    }
                    float uer = ue * re, ver = ve * re;
                    int32 iua = magInt(u0 * r), iva = magInt(v0 * r);
                    if (len > (1 << sh)) {
                        const int32 S = 1 << sh;
                        const float fS = float(S), qS = qdx * fS, uS = udx * fS, vS = vdx * fS;
                        do {                         // whole pieces
                            r = 1.0f / (q0 + qS);
                            u0 += uS; v0 += vS; q0 += qS;
                            int32 iub = magInt(u0 * r), ivb = magInt(v0 * r);
                            Mag::span(swColor + offset, S, iua, iva, (iub - iua) >> sh, (ivb - iva) >> sh, l, dl, offset, z, dz2);
                            l += dl << sh; z += uint32(dz2) << sh; offset += S; len -= S;
                            iua = iub; iva = ivb;
                        } while (len > S);
                    }
                    {                                // the last piece: len pixels, its last centre len - 1 away
                        int32 sdu = 0, sdv = 0;
                        m = len - 1;
                        if (m > 0) {
                            float rm = m < 1024 ? Mag::rcp[m] : 1.0f / float(m);
                            sdu = magInt((uer - float(iua)) * rm);
                            sdv = magInt((ver - float(iva)) * rm);
                        }
                        Mag::span(swColor + offset, len, iua, iva, sdu, sdv, l, dl, offset, z, dz2);
                    }
                }
            }
            lineOffset += pitch;
            zRow += uint32(dzY);
            qRow += qdy; uqRow += udy; vqRow += vdy;
        })
    }

    void magDraw(const MagPrim &pr) {
        MagRows R;
        MagVtx *const *p = pr.v;
        const int n = pr.n;
        int top = 0, bot = 0;
        int32 xmin = p[0]->x, xmax = xmin;
        for (int i = 1; i < n; i++) {
            if (p[i]->y < p[top]->y) top = i;
            if (p[i]->y > p[bot]->y) bot = i;
            if (p[i]->x < xmin) xmin = p[i]->x;
            if (p[i]->x > xmax) xmax = p[i]->x;
        }
        int32 y = (p[top]->y + 7) >> 4, yEnd = (p[bot]->y + 7) >> 4;
        if (y < swClipRect.y) y = swClipRect.y;
        if (yEnd > swClipRect.w) yEnd = swClipRect.w;
        if (y >= yEnd || xmax < (swClipRect.x << 4) || xmin > (swClipRect.z << 4)) return;
        MAG_COUNT(2, 1);
        R.p = p; R.n = n; R.top = top; R.bot = bot; R.y = y; R.yEnd = yEnd; R.xmin = xmin; R.xmax = xmax;

        MAG_PRIM_BEGIN(pr, p[0]);

    #ifdef _GAPI_MAGGIE
        if (xmax - xmin < MAG_SMALL * 16 && p[bot]->y - p[top]->y < MAG_SMALL * 16) { magDrawSmall(R); return; }
    #endif

        if (!magColored && Mag::perspective) {           // 1/w varying noticeably over the polygon?
            int32 qa = magBits(p[0]->q), qb = qa;
            for (int i = 1; i < n; i++) {
                int32 q = magBits(p[i]->q);
                if (q < qa) qa = q;
                if (q > qb) qb = q;
            }
            // Affine texturing puts a texel up to about size * (ratio - 1) / 4 pixels from its place
            // inside a triangle, and four times that at the fourth corner of a quad (its plane comes
            // from three): affine only when that stays under MAG_PERSP_ERR pixels.
            MagFI lo, hi; lo.i = qa; hi.i = qb;
            int32 ext = xmax - xmin, ey = p[bot]->y - p[top]->y;
            if (ey > ext) ext = ey;
            if (magBits(lo.f * (n == 3 ? 64.0f * MAG_PERSP_ERR : 16.0f * MAG_PERSP_ERR) - (hi.f - lo.f) * float(ext)) < 0) {
                // 1/w the same all along each line? Here: it changes by less than 1/4096 across the
                // polygon's width (d(1/w)/dx from the plane, as the walker computes it)
                const MagVtx *a = p[0], *b = p[1], *c = p[2];
                float inv = 0.0625f / pr.area;
                float qdx = (b->q - a->q) * (float(c->y - a->y) * inv) + (c->q - a->q) * (float(a->y - b->y) * inv);
                float aq = magBits(qdx) < 0 ? -qdx : qdx;
                if (magBits(lo.f * (1.0f / 4096.0f) - aq * float(xmax - xmin) * 0.0625f) >= 0) magDrawPersp<true>(R, pr);
                else magDrawPersp<false>(R, pr);
                return;
            }
        }

        if (n == 3) { if (Mag::zSpan) magDrawAffine<true, true>(R, pr);  else magDrawAffine<true, false>(R, pr); }
        else        { if (Mag::zSpan) magDrawAffine<false, true>(R, pr); else magDrawAffine<false, false>(R, pr); }
    }

#ifdef _GAPI_SW
// --- transform ----------------------------------------------------------------------------------
    mat4  magViewProj;                   // mViewProj the cached product belongs to
    mat4  magScreenProj;                 // viewport * mViewProj
    bool  magScreenProjValid = false;
    int32 magScreenW = 0, magScreenH = 0;

    // light of a vertex, 0..255: the room's or entity's ambient, plus the dynamic lights
    inline int32 magLight(const Vertex &vx, float w) {
        int32 L = (vx.light.x * ambient) >> 8;
        if (lightsCount > 0 && !MAG_STAGE(3)) {
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
                sum += lum * (1.0f - att) * float(light.intensity) / sqrtf(att);
            }
            L += int32(sum * (1.0f / 32767.0f));
        }
        if (magBits(w) > magBits(SW_FOG_START)) {
            float f = 1.0f - (w - SW_FOG_START) * (1.0f / (SW_MAX_DIST - SW_FOG_START));
            L = magBits(f) > 0 ? int32(float(L) * f) : 0;
        }
        return L > 255 ? 255 : L;
    }

    // the software lightmap's row for light L is plain texture at 127 and brighter above; Maggie's
    // intensity stops at plain texture. << 15, kept off both ends (see the span's light)
    inline int32 magIntensity(int32 L) {
        int32 i = (L + 1) << 9;
        return (i > 0xFFEF ? 0xFFEF : i) << 15;
    }

    struct MagClipVtx {
        float x, y, z, w;                // screen space times w
        int32 u, v, l;
    };

    inline void magProject(MagVtx &o, float x, float y, float z, float w, float invw, int32 u, int32 v, int32 l) {
        int32 ix = magInt(x * invw * 16.0f), iy = magInt(y * invw * 16.0f);
        o.x = ix > MAG_XY_MAX ? MAG_XY_MAX : (ix < -MAG_XY_MAX ? -MAG_XY_MAX : ix);
        o.y = iy > MAG_XY_MAX ? MAG_XY_MAX : (iy < -MAG_XY_MAX ? -MAG_XY_MAX : iy);
        int32 iz = magInt(z * invw * (65533.0f * 32768.0f));
        o.z = (iz < 0 ? 0 : (iz > 65533 * 32768 ? 65533 * 32768 : iz)) + 32768;
        o.q = magBits(w) > 0x41800000 ? invw : 1.0f / 16.0f;     // w > 16
        o.u = u;
        o.v = v;
        o.l = l;
    }

    inline float magCross(const MagVtx *a, const MagVtx *b, const MagVtx *c) {
        return (float(b->x - a->x) * float(c->y - a->y) - float(c->x - a->x) * float(b->y - a->y)) * (1.0f / 256.0f);
    }

    void magDIP(const Index *indices, const Vertex *vertices, int iStart, int iCount, int vStart) {
        const bool colored = vertices[vStart + indices[iStart]].color.w == 142;
        magColored = colored;
        if (!colored && !Mag::bindTile(curTile)) return;

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
        if (lightsCount > 0) transformLights();

        const float screenW = float(Core::width), screenH = float(Core::height);
        // orthographic projections (UI, 2D) have w = 1 everywhere: only perspective ones get the near plane
        const float nearW = (Core::mProj.e33 == 1.0f) ? 0.0f : SW_NEAR;
        const int32 nearBits = magBits(nearW), farBits = magBits(SW_MAX_DIST);
        const bool needZ = Mag::zOn && Mag::zTest;
        const bool shade = Mag::shading;

        MagVtx  *vo = magVtxBuf;
        MagPrim *po = magPrimBuf;

        int i = 0;
        while (i < iCount) {
            const Index *idx = indices + iStart + i;
            const Vertex *vx[4];
            vx[0] = vertices + vStart + idx[0];
            vx[1] = vertices + vStart + idx[1];
            vx[2] = vertices + vStart + idx[2];
            const bool isTriangle = vx[0]->normal.w == 1;
            const int count = isTriangle ? 3 : 4;
            i += isTriangle ? 3 : 6;
            if (i > iCount) break;
            if (!isTriangle) vx[3] = vertices + vStart + idx[5];           // quads come as 012 023
            MAG_COUNT(0, 1); MAG_COUNT(1, count);

            float cx[4], cy[4], cw[4];
            int32 behind = 0, outside = 15, tooFar = 0;
            for (int k = 0; k < count; k++) {
                float x = float(vx[k]->coord.x), y = float(vx[k]->coord.y), z = float(vx[k]->coord.z);
                float w = m.e30 * x + m.e31 * y + m.e32 * z + m.e33;
                float sx = m.e00 * x + m.e01 * y + m.e02 * z + m.e03;
                float sy = m.e10 * x + m.e11 * y + m.e12 * z + m.e13;
                cx[k] = sx; cy[k] = sy; cw[k] = w;
                int32 wb = magBits(w);
                tooFar |= wb > farBits;
                behind += wb < nearBits;
                // left of, right of, above, below the screen (sign bits; valid for w > 0)
                outside &= (uint32(magBits(sx)) >> 31) | ((uint32(magBits(screenW * w - sx)) >> 31) << 1) |
                           ((uint32(magBits(sy)) >> 31) << 2) | ((uint32(magBits(screenH * w - sy)) >> 31) << 3);
            }
            if (tooFar || behind == count) continue;
            if (MAG_STAGE(5)) continue;

            if (behind == 0) {
                if (outside) continue;
                float px[4], py[4], iw[4];
                for (int k = 0; k < count; k++) {
                    float r = 1.0f / cw[k];
                    iw[k] = r; px[k] = cx[k] * r; py[k] = cy[k] * r;
                }
                float abx = px[1] - px[0], aby = py[1] - py[0], acx = px[2] - px[0], acy = py[2] - py[0];
                float area = abx * acy - acx * aby, area2 = 0.0f;
                bool flat = false;
                if (isTriangle) {
                    if (magBits(area - MAG_MIN_AREA) < 0) continue;      // facing away, or nothing to draw
                } else {
                    // a parallelogram with matching texture corners: one plane fits all four
                    const short4 &A = vx[0]->coord, &B = vx[1]->coord, &C = vx[2]->coord, &D = vx[3]->coord;
                    const short4 &TA = vx[0]->texCoord, &TB = vx[1]->texCoord, &TC = vx[2]->texCoord, &TD = vx[3]->texCoord;
                    flat = (uint32(A.x + C.x - B.x - D.x + 1) <= 2) && (uint32(A.y + C.y - B.y - D.y + 1) <= 2) &&
                           (uint32(A.z + C.z - B.z - D.z + 1) <= 2) &&
                           (colored || ((uint32(TA.x + TC.x - TB.x - TD.x + 1) <= 2) && (uint32(TA.y + TC.y - TB.y - TD.y + 1) <= 2)));
                    if (flat) {
                        if (magBits(area - MAG_MIN_AREA) < 0) continue;
                    } else {
                        float adx = px[3] - px[0], ady = py[3] - py[0];
                        area2 = acx * ady - adx * acy;
                        if (magBits(area - MAG_MIN_AREA) < 0 && magBits(area2 - MAG_MIN_AREA) < 0) continue;
                    }
                }
                if (MAG_STAGE(6)) continue;
                for (int k = 0; k < count; k++) {
                    const Vertex &v = *vx[k];
                    float z = 0.0f;
                    if (needZ) z = m.e20 * float(v.coord.x) + m.e21 * float(v.coord.y) + m.e22 * float(v.coord.z) + m.e23;
                    int32 l = shade ? magIntensity(magLight(v, cw[k])) : (0xFFEF << 15);
                    if (colored) magProject(vo[k], cx[k], cy[k], z, cw[k], iw[k], v.color.x << 16, 0, l);
                    else         magProject(vo[k], cx[k], cy[k], z, cw[k], iw[k], v.texCoord.x << 16, v.texCoord.y << 16, l);
                }
                if (isTriangle || flat) {
                    po->v[0] = vo; po->v[1] = vo + 1; po->v[2] = vo + 2; po->v[3] = vo + 3;
                    po->n = count; po->area = area; po++;
                } else {
                    if (magBits(area - MAG_MIN_AREA) >= 0) {
                        po->v[0] = vo; po->v[1] = vo + 1; po->v[2] = vo + 2;
                        po->n = 3; po->area = area; po++;
                    }
                    if (magBits(area2 - MAG_MIN_AREA) >= 0) {
                        po->v[0] = vo; po->v[1] = vo + 2; po->v[2] = vo + 3;
                        po->n = 3; po->area = area2; po++;
                    }
                }
                vo += count;
                continue;
            }

            // crossing the near plane: Sutherland-Hodgman against w >= nearW (at most count + 1
            // vertices), drawn as a triangle fan
            if (!swNearClip) continue;
            MagClipVtx cv[4];
            for (int k = 0; k < count; k++) {
                const Vertex &v = *vx[k];
                cv[k].x = cx[k]; cv[k].y = cy[k]; cv[k].w = cw[k];
                cv[k].z = m.e20 * float(v.coord.x) + m.e21 * float(v.coord.y) + m.e22 * float(v.coord.z) + m.e23;
                cv[k].u = colored ? (v.color.x << 16) : (v.texCoord.x << 16);
                cv[k].v = colored ? 0 : (v.texCoord.y << 16);
                cv[k].l = shade ? magIntensity(magLight(v, cw[k] > 0.0f ? cw[k] : 0.0f)) : (0xFFEF << 15);
            }
            MagVtx *first = vo;
            for (int k = 0; k < count; k++) {
                const MagClipVtx &a = cv[k];
                const MagClipVtx &b = cv[(k + 1) % count];
                bool aIn = a.w >= nearW, bIn = b.w >= nearW;
                if (aIn) { magProject(*vo, a.x, a.y, a.z, a.w, 1.0f / a.w, a.u, a.v, a.l); vo++; }
                if (aIn != bIn) {
                    float t = (nearW - a.w) / (b.w - a.w);
                    magProject(*vo, a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, nearW, 1.0f / nearW,
                               a.u + magInt(float(b.u - a.u) * t), a.v + magInt(float(b.v - a.v) * t),
                               a.l + magInt(float(b.l - a.l) * t));
                    vo++;
                }
            }
            int nOut = int(vo - first);
            for (int k = 1; k + 1 < nOut; k++) {             // fan, same winding
                float area = magCross(first, first + k, first + k + 1);
                if (magBits(area - MAG_MIN_AREA) < 0) continue;
                po->v[0] = first; po->v[1] = first + k; po->v[2] = first + k + 1;
                po->n = 3; po->area = area; po++;
            }
        }

        if (MAG_STAGE(1)) return;
        for (const MagPrim *pr = magPrimBuf; pr < po; pr++) magDraw(*pr);
    }

#endif // _GAPI_SW

#endif
