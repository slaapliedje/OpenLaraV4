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
    #define MAG_GUARD     6000           // pixels: polygons reaching further off screen are clipped (hardware back end)

#ifdef _GAPI_SW
    // per polygon: flat colours pick their palette colour; the mode follows the depth test
    #define MAG_PRIM_BEGIN(pr, a) { if (magColored) Mag::bindFlat((a)->u >> 16); Mag::depthMode(); }
#endif

// --- rasteriser ---------------------------------------------------------------------------------
    struct MagEdge {
        int32 x, sx;                     // 16.16 x at the centre of the current line, step per line
        int32 l, sl;                     // intensity << 15
        int32 yEnd;                      // first line below the edge
        int   i;                         // vertex it ends at
    };

    // the next edge of a chain (dir: +1 right side, n - 1 left side) that covers line y
    inline bool magEdgeNext(MagEdge &e, MagVtx *const *p, int n, int dir, int32 y, int bottom) {
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
        int32 sx = int32(float(Q->x - P->x) * inv);
        int32 sl = int32(float(Q->l - P->l) * inv * (1.0f / 4096.0f));
        e.sx = sx;
        e.x  = (P->x << 12) + sx * lines + (sx >> 4) * frac;
        e.sl = sl;
        e.l  = P->l + sl * lines + (sl >> 4) * frac;
        return true;
    }

#ifdef OL_EMU
    const void *magTracePrim = NULL;
#endif
    void magDraw(const MagPrim &pr) {
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
        const int32 clipX1 = swClipRect.x, clipX2 = swClipRect.z;
        int32 y = (p[top]->y + 7) >> 4, yEnd = (p[bot]->y + 7) >> 4;
        if (y < swClipRect.y) y = swClipRect.y;
        if (yEnd > swClipRect.w) yEnd = swClipRect.w;
        if (y >= yEnd || xmax < (clipX1 << 4) || xmin > (clipX2 << 4)) return;
        MAG_COUNT(2, 1);

        const MagVtx *a = p[0], *b = p[1], *c = p[2];
        MAG_PRIM_BEGIN(pr, a);

        // gradients per pixel of a value known at a, b, c:  d/dx = (B - A) * kbx + (C - A) * kcx
        float inv = 0.0625f / pr.area;
        float kbx = float(c->y - a->y) * inv, kcx = float(a->y - b->y) * inv;
        float kby = float(a->x - c->x) * inv, kcy = float(b->x - a->x) * inv;

        // Integer rows: the value at the centre of the pixel next to vertex a, then
        // row(y) + x * step, all wrapping 32-bit arithmetic.
        const int32 X0 = (a->x + 7) >> 4, Y0 = (a->y + 7) >> 4;
        const int32 fx = (X0 << 4) + 8 - a->x, fy = (Y0 << 4) + 8 - a->y;    // 0..15, 28.4
        const int32 dyRow = y - Y0;

        int32 dz = 0, dzY = 0;
        uint32 zRow = 0;
        if (Mag::zSpan) {
            float fb = float(b->z - a->z), fc = float(c->z - a->z);
            dz  = int32(fb * kbx + fc * kcx);
            dzY = int32(fb * kby + fc * kcy);
            zRow = uint32(a->z + (dz >> 4) * fx + (dzY >> 4) * fy) + uint32(dyRow) * uint32(dzY) - uint32(X0) * uint32(dz);
        }
        const int32 dz2 = dz << 1;

        bool affine = true, rowConst = false;
        int32 du = 0, dv = 0, duY = 0, dvY = 0;
        uint32 uRow = 0, vRow = 0;
        float qdx = 0.0f, qdy = 0.0f, udx = 0.0f, udy = 0.0f, vdx = 0.0f, vdy = 0.0f;
        double qRow = 0.0, uqRow = 0.0, vqRow = 0.0;     // perspective: u/w, v/w, 1/w at pixel (0, y)
        if (!magColored) {
            if (Mag::perspective) {                      // 1/w varying noticeably over the polygon?
                int32 qa = magBits(a->q), qb = qa;
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
                affine = magBits(lo.f * (n == 3 ? 64.0f * MAG_PERSP_ERR : 16.0f * MAG_PERSP_ERR) - (hi.f - lo.f) * float(ext)) >= 0;
            }
            if (affine) {
                float fb = float(b->u - a->u), fc = float(c->u - a->u);
                du  = int32(fb * kbx + fc * kcx);
                duY = int32(fb * kby + fc * kcy);
                uRow = uint32(a->u + (du >> 4) * fx + (duY >> 4) * fy) + uint32(dyRow) * uint32(duY) - uint32(X0) * uint32(du);
                fb = float(b->v - a->v); fc = float(c->v - a->v);
                dv  = int32(fb * kbx + fc * kcx);
                dvY = int32(fb * kby + fc * kcy);
                vRow = uint32(a->v + (dv >> 4) * fx + (dvY >> 4) * fy) + uint32(dyRow) * uint32(dvY) - uint32(X0) * uint32(dv);
            } else {
                float ax = float(a->x) * 0.0625f, ay = float(a->y) * 0.0625f;
                float ox = 0.5f - ax, oy = float(y) + 0.5f - ay;
                float fb = b->q - a->q, fc = c->q - a->q;
                qdx = fb * kbx + fc * kcx;
                qdy = fb * kby + fc * kcy;
                qRow = double(a->q) + double(ox * qdx) + double(oy * qdy);
                float ua = float(a->u) * a->q;
                fb = float(b->u) * b->q - ua; fc = float(c->u) * c->q - ua;
                udx = fb * kbx + fc * kcx;
                udy = fb * kby + fc * kcy;
                uqRow = double(ua) + double(ox) * udx + double(oy) * udy;
                float va = float(a->v) * a->q;
                fb = float(b->v) * b->q - va; fc = float(c->v) * c->q - va;
                vdx = fb * kbx + fc * kcx;
                vdy = fb * kby + fc * kcy;
                vqRow = double(va) + double(ox) * vdx + double(oy) * vdy;
                // 1/w the same all along each line (floors and ceilings: the camera never rolls)? Then a
                // line is one exact affine span: u = (u/w) / (1/w) and its step (du/w) / (1/w), one divide.
                // Here: 1/w changes by less than 1/4096 across the polygon's width.
                {
                    int32 qlo = magBits(a->q);
                    for (int i = 1; i < n; i++) if (magBits(p[i]->q) < qlo) qlo = magBits(p[i]->q);
                    MagFI lo; lo.i = qlo;
                    float aq = magBits(qdx) < 0 ? -qdx : qdx;
                    rowConst = magBits(lo.f * (1.0f / 4096.0f) - aq * float(xmax - xmin) * 0.0625f) >= 0;
                }
            }
        }

        MagEdge L, R;
        L.i = R.i = top;
        L.yEnd = R.yEnd = y;
        const int32 pitch = Core::width;
        int32 lineOffset = y * pitch;

        if (affine) {
            // The common case, all integers. Texture and depth steps go to Maggie once; a triangle's
            // light is a plane too (one step for the polygon), a quad's runs down its edges.
            const bool zs = Mag::zSpan, lightPlane = n == 3;
            int32 dlx = 0, dlY = 0;
            uint32 lRow = 0;
            bool first = true;               // the per-polygon registers go out with the first span: an earlier
                                             // write would wait for the span in flight instead of working beside it
            if (lightPlane) {
                float fb = float(b->l - a->l), fc = float(c->l - a->l);
                dlx = int32(fb * kbx + fc * kcx);
                dlY = int32(fb * kby + fc * kcy);
                lRow = uint32(a->l + (dlx >> 4) * fx + (dlY >> 4) * fy) + uint32(dyRow) * uint32(dlY) - uint32(X0) * uint32(dlx);
            }
            for (; y < yEnd; y++, lineOffset += pitch) {
                if (y >= L.yEnd && !magEdgeNext(L, p, n, n - 1, y, bot)) break;
                if (y >= R.yEnd && !magEdgeNext(R, p, n, 1, y, bot)) break;
                int32 xa = (L.x + 0x7FFF) >> 16, xb = (R.x + 0x7FFF) >> 16;
                int32 xs = xa < clipX1 ? clipX1 : xa;
                int32 xe = xb > clipX2 ? clipX2 : xb;
                int32 len = xe - xs;
            #ifdef OL_EMU
                if (&pr == magTracePrim && y >= Mag::pickY - 6 && y <= Mag::pickY + 6)
                    printf("   row %ld: xa %ld xb %ld xs %ld len %ld clip %d..%d y %d..%d Li %d Ri %d\n", (long)y, (long)xa, (long)xb, (long)xs, (long)len,
                           int(clipX1), int(clipX2), int(swClipRect.y), int(swClipRect.w), L.i, R.i);
            #endif
                if (len > 0) {
                    int32 l;
                    if (lightPlane) {
                        l = int32((lRow + uint32(xs) * uint32(dlx)) >> 15);
                        if (uint32(l) > 0xFFFF) l = l < 0 ? 0 : 0xFFFF;    // (a rounding step outside the range)
                    } else {
                        int32 w = xb - xa;
                        int32 la = L.l >> 16, d = (R.l >> 16) - la, dl;
                        if (w < 1024) {
                            uint32 r = Mag::rcpi[w];
                            dl = d >= 0 ? int32((uint32(d) * r) >> 16) : -int32((uint32(-d) * r) >> 16);
                            if (dl > 16383) dl = 16383;
                            if (dl < -16383) dl = -16383;
                        } else
                            dl = d / w;
                        l = (la + dl * (xs - xa)) << 1;
                        Mag::lightStep(dl << 1);
                    }
                    if (first) {
                        first = false;
                        Mag::polySteps(du, dv, dz2);
                        if (lightPlane) Mag::lightStep(dlx / 32768);     // towards zero: a span's light stays between its ends
                    }
                    int32 offset = lineOffset + xs;
                    Mag::spanSteps(swColor + offset, len, int32(uRow + uint32(xs) * uint32(du)), int32(vRow + uint32(xs) * uint32(dv)),
                                   l, offset, (zRow + uint32(xs) * uint32(dz)) << 1, zs);
                }
                L.x += L.sx; L.l += L.sl;
                R.x += R.sx; R.l += R.sl;
                zRow += uint32(dzY);
                uRow += uint32(duY); vRow += uint32(dvY);
                lRow += uint32(dlY);
            }
            return;
        }

        for (; y < yEnd; y++, lineOffset += pitch) {
            if (y >= L.yEnd && !magEdgeNext(L, p, n, n - 1, y, bot)) break;
            if (y >= R.yEnd && !magEdgeNext(R, p, n, 1, y, bot)) break;
            int32 xa = (L.x + 0x7FFF) >> 16, xb = (R.x + 0x7FFF) >> 16;
            int32 xs = xa < clipX1 ? clipX1 : xa;
            int32 xe = xb > clipX2 ? clipX2 : xb;
            int32 len = xe - xs;
            if (len > 0) {
                // light: 15 bits along the span, rounded towards zero so it stays between its ends
                int32 w = xb - xa;
                int32 la = L.l >> 16, d = (R.l >> 16) - la, dl;
                if (w < 1024) {
                    uint32 r = Mag::rcpi[w];
                    dl = d >= 0 ? int32((uint32(d) * r) >> 16) : -int32((uint32(-d) * r) >> 16);
                    if (dl > 16383) dl = 16383;
                    if (dl < -16383) dl = -16383;
                } else
                    dl = d / w;
                int32 l = (la + dl * (xs - xa)) << 1;
                dl <<= 1;
                int32 offset = lineOffset + xs;
                uint32 z = (zRow + uint32(xs) * uint32(dz)) << 1;
                if (affine) {
                    Mag::span(swColor + offset, len, int32(uRow + uint32(xs) * uint32(du)), int32(vRow + uint32(xs) * uint32(dv)),
                              du, dv, l, dl, offset, z, dz2);
                } else if (rowConst) {
                    float fxs = float(xs);
                    float r = 1.0f / (float(qRow) + fxs * qdx);
                    float u0 = float(uqRow) + fxs * udx, v0 = float(vqRow) + fxs * vdx;
                    Mag::span(swColor + offset, len, int32(u0 * r), int32(v0 * r), int32(udx * r), int32(vdx * r),
                              l, dl, offset, z, dz2);
                } else {
                    // Affine pieces, as many as the error bound asks for; one piece when 1/w hardly
                    // changes along the span. Only pixel centres inside the polygon are evaluated
                    // (1/w > 0 there): the last piece ends on the last pixel.
                    float fxs = float(xs);
                    float q0 = float(qRow) + fxs * qdx, u0 = float(uqRow) + fxs * udx, v0 = float(vqRow) + fxs * vdx;
                    float qe = q0 + qdx * float(len - 1);
                    // one affine piece is off by about len * (ratio - 1) / 4 pixels, n pieces by 1 / n^2 of that.
                    // Pieces are a power of two long (8..512), so a piece's steps are shifts of the integer
                    // u, v at its ends; only the last piece, of any length, divides.
                    int32 sh = 12;                   // one piece
                    {
                        float qlo = q0 < qe ? q0 : qe;
                        float e = (q0 < qe ? qe - q0 : q0 - qe) * float(len), t = qlo * (4.0f * MAG_PERSP_ERR);
                        if (magBits(t - e) < 0) {
                            // pieces of S pixels are off by e / t * (S / len)^2 of the bound: S^2 <= len^2 * t / e,
                            // S = 2^floor(log2(x) / 2) read from the float's exponent
                            float x = float(len * len) * t / e;
                            sh = ((magBits(x) >> 23) - 127) >> 1;
                            if (sh < 3) sh = 3;
                            if (sh > 9) sh = 9;
                        }
                    }
                    const int32 S = 1 << sh;
                    const float fS = float(S), qS = qdx * fS, uS = udx * fS, vS = vdx * fS;
                    float r = 1.0f / q0;
                    int32 iua = int32(u0 * r), iva = int32(v0 * r);
                    while (len > S) {                // whole pieces
                        r = 1.0f / (q0 + qS);
                        u0 += uS; v0 += vS; q0 += qS;
                        int32 iub = int32(u0 * r), ivb = int32(v0 * r);
                        Mag::span(swColor + offset, S, iua, iva, (iub - iua) >> sh, (ivb - iva) >> sh, l, dl, offset, z, dz2);
                        l += dl << sh; z += uint32(dz2) << sh; offset += S; len -= S;
                        iua = iub; iva = ivb;
                    }
                    {                                // the last piece: len pixels, its last centre len - 1 away
                        int32 sdu = 0, sdv = 0, m = len - 1;
                        if (m > 0) {
                            float fm = float(m);
                            r = 1.0f / (q0 + qdx * fm);
                            float rm = m < 1024 ? Mag::rcp[m] : 1.0f / fm;
                            sdu = int32(((u0 + udx * fm) * r - float(iua)) * rm);
                            sdv = int32(((v0 + vdx * fm) * r - float(iva)) * rm);
                        }
                        Mag::span(swColor + offset, len, iua, iva, sdu, sdv, l, dl, offset, z, dz2);
                    }
                }
            }
            L.x += L.sx; L.l += L.sl;
            R.x += R.sx; R.l += R.sl;
            zRow += uint32(dzY);
            if (affine) { uRow += uint32(duY); vRow += uint32(dvY); }
            else { qRow += qdy; uqRow += udy; vqRow += vdy; }
        }
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
                               a.u + int32(float(b.u - a.u) * t), a.v + int32(float(b.v - a.v) * t),
                               a.l + int32(float(b.l - a.l) * t));
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
