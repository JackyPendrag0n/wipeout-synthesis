// Native replacements for WipEout Fusion's VU1 drawing programs; see vu1_native_draw.h.
// Every float operation mirrors the recompiled VU code (src/vu1/gen): operands normalized with
// vn(), FMAC results through f_mul / f_madd / f_add (exact VU overflow and underflow handling),
// MADD chains in the program's order, DIV as queueQ does, FTOI/ITOF as ftoi().
#if defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#endif
#include "vu1_native_draw.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace vu1native
{
    using namespace vu1fast;

    namespace
    {
        struct Qw
        {
            float f[4];
            uint32_t u(int i) const { return fbits(f[i]); }
            int32_t s(int i) const { return static_cast<int32_t>(fbits(f[i])); }
        };

        inline Qw ldq(const uint8_t *mem, uint32_t qaddr)
        {
            Qw q;
            std::memcpy(q.f, mem + ((qaddr & 0x3FFu) << 4), 16);
            return q;
        }

        inline float vmul(float a, float b) { uint32_t fl; return f_mul(vn(a), vn(b), fl); }
        inline float vmadd(float acc, float a, float b) { uint32_t fl; return f_madd(vn(acc), vn(a), vn(b), fl); }
        inline float vadd(float a, float b) { uint32_t fl; return f_add(vn(a), vn(b), fl); }
        inline float vsub(float a, float b) { uint32_t fl; return f_sub(vn(a), vn(b), fl); }
        inline float vmax(float a, float b) { const float x = vn(a), y = vn(b); return x > y ? x : y; }
        inline float vmin(float a, float b) { const float x = vn(a), y = vn(b); return x < y ? x : y; }
        inline float vdiv(float num, float den)
        {
            num = vn(num);
            den = vn(den);
            float r;
            if (den == 0.0f)
                r = std::signbit(num) != std::signbit(den) ? -std::numeric_limits<float>::max() : std::numeric_limits<float>::max();
            else
                r = num / den;
            return vr(r);
        }
        inline uint32_t ftoi4(float v) { return static_cast<uint32_t>(ftoi(vn(v), 16.0)); }
        inline uint32_t ftoi0(float v) { return static_cast<uint32_t>(ftoi(vn(v), 1.0)); }
        inline float itof0(uint32_t bits) { return static_cast<float>(static_cast<int32_t>(bits)); }

        // 4-lane FMAC, as the recompiled code does it: the host result when every lane is a normal
        // float below FLT_MAX, else the exact lane helpers (which agree with it on such lanes).
        inline bool allok(v4f r)
        {
            const v4u m = (v4u)r & 0x7FFFFFFFu;
            const v4i ok = (v4i)((m > 0x007FFFFFu) & (m < 0x7F7FFFFFu));
            return (ok[0] & ok[1] & ok[2] & ok[3]) != 0;
        }
        inline v4f mul4(v4f a, float b)
        {
            const float bc = vn(b);
            const v4f r = vn4(a) * bc;
            if (__builtin_expect(allok(r), 1))
                return r;
            v4f o;
            for (int i = 0; i < 4; ++i) o[i] = vmul(a[i], bc);
            return o;
        }
        inline v4f madd4(v4f acc, v4f a, float b)
        {
            const float bc = vn(b);
            const v4f r = vn4(acc) + vn4(a) * bc;
            if (__builtin_expect(allok(r), 1))
                return r;
            v4f o;
            for (int i = 0; i < 4; ++i) o[i] = vmadd(acc[i], a[i], bc);
            return o;
        }
        // FTOI4 on 4 lanes (ftoi(v, 16.0): v * 16 is exact in float, saturated to int32).
        inline v4u ftoi4v(v4f v)
        {
#if defined(__SSE2__)
            const v4f x = vn4(v) * 16.0f;
            const v4i r = (v4i)_mm_cvttps_epi32((__m128)x);
            const v4i big = (v4i)(x >= 2147483648.0f);
            return (v4u)((r & ~big) | (big & 0x7FFFFFFF));
#else
            return v4u{ftoi4(v[0]), ftoi4(v[1]), ftoi4(v[2]), ftoi4(v[3])};
#endif
        }

        // ACC = m3 * 1 (MULAw with vf00w); ACC += m2 * z; ACC += m1 * y; r = ACC + m0 * x.
        inline v4f transform4(const float (*m)[4], const float *p)
        {
            v4f acc = mul4(ld4(m[3]), 1.0f);
            acc = madd4(acc, ld4(m[2]), p[2]);
            acc = madd4(acc, ld4(m[1]), p[1]);
            return madd4(acc, ld4(m[0]), p[0]);
        }
        inline void transform(const float (*m)[4], const float *p, float *out)
        {
            const v4f r = transform4(m, p);
            std::memcpy(out, &r, 16);
        }

        // CLIPw.xyz: +x, -x, +y, -y, +z, -z against |w| (bitwise, as the recompiler does).
        inline uint32_t clipFlags(const float *v)
        {
            const uint32_t wb = fbits(v[3]);
            const int32_t lim = (wb & 0x7F800000u) ? static_cast<int32_t>(wb & 0x7FFFFFFFu) : 0x007FFFFF;
            uint32_t cf = 0;
            for (int i = 0; i < 3; ++i)
            {
                if (clipExceeds(fbits(v[i]), 0u, lim)) cf |= 1u << (2 * i);
                if (clipExceeds(fbits(v[i]), 0x80000000u, lim)) cf |= 2u << (2 * i);
            }
            return cf;
        }

        // One output vertex: STQ * Q (w replaced), colour, FTOI4(p * Q) (w replaced by 0x8000 for ADC).
        inline void vertexOut(uint8_t *o, const float *stq, uint32_t stqW, const void *colour, const float *p, float q, bool adc)
        {
            v4f s = mul4(ld4(stq), q);
            std::memcpy(o, &s, 12);
            std::memcpy(o + 12, &stqW, 4);
            std::memcpy(o + 16, colour, 16);
            v4u x = ftoi4v(mul4(ld4(p), q));
            if (adc) x[3] = 0x8000u;
            std::memcpy(o + 32, &x, 16);
        }

        // The tag must be PACKED with registers STQ, RGBAQ, XYZ2 or XYZF2 (in that order).
        inline bool tagOk(const Qw &tag, uint32_t &n)
        {
            const uint64_t lo = static_cast<uint64_t>(tag.u(0)) | static_cast<uint64_t>(tag.u(1)) << 32;
            const uint32_t regs = tag.u(2) & 0xFFFu;
            n = static_cast<uint32_t>(lo & 0x7FFF);
            return ((lo >> 58) & 3) == 0 && ((lo >> 60) & 15) == 3 && (regs == 0x512u || regs == 0x412u);
        }

        struct FloatEnv
        {
#if defined(__x86_64__) || defined(__i386__)
            unsigned int csr;
            FloatEnv() : csr(_mm_getcsr()) { _mm_setcsr((csr & ~0x6000u) | 0x6000u | 0x8040u); }
            ~FloatEnv() { _mm_setcsr(csr); }
#endif
        };
    }

    // The VU's triangle clipper (shared code: setup L1628 / L16b0, planes L1700..L1a90 with edge
    // routines L1bd0 / L1d00 and intersections L1e20 / L1ed0, output L1a98..L1bb8). Polygon
    // vertices are 4 qwords: clip-space position, ITOF0(output colour), input STQ, input position.
    // Sutherland-Hodgman against -z, +z, -x, +x, -y, +y (|c| <= w), each pass from one ring to the
    // other; the last pass keeps 3 qwords with FTOI0 colour. Then two kicks: the packet at VU mem
    // 0x18 as it is, and a fan at 0xbe: header = mem 0xa8 with NLOOP|EOP = count, per vertex
    // STQ*Q (w = vf30.w), colour, FTOI4(screen*Q) where screen = mem 20..23 * position and the
    // XYZ w lane = the last pass's first vertex's position.w * Q (the VU's stale vf24.w).
    namespace
    {
        struct PolyV { float q[4][4]; };

        // cur - (next - cur) * Q, Q = d(cur) / (d(next) - d(cur)), d = c[axis] - s * w.
        inline PolyV intersect(const PolyV &a, const PolyV &b, int axis, float s)
        {
            const float da = vsub(a.q[0][axis], vmul(a.q[0][3], s));
            const float db = vsub(b.q[0][axis], vmul(b.q[0][3], s));
            const float q = vdiv(da, vsub(db, da));
            PolyV r;
            for (int j = 0; j < 4; ++j)
                for (int i = 0; i < 4; ++i)
                    r.q[j][i] = vsub(a.q[j][i], vmul(vsub(b.q[j][i], a.q[j][i]), q));
            return r;
        }

        // Appends the clipper's kicks for triangle in[0..2]; false when the packets are of a shape
        // this does not model (the batch then runs on the VU).
        bool clipTriangle(const vu1fast::Ctx &c, const uint8_t *mem, const Qw &hdr, const PolyV in[3], std::vector<uint8_t> &out, std::vector<uint32_t> &sizes)
        {
            float z31[4];
            for (int i = 0; i < 4; ++i) z31[i] = vmul(c.vf[31][i], 0.0f);   // MULx vf31, vf31, vf00x at batch start
            static std::vector<PolyV> ring[2];
            ring[0].assign(in, in + 3);
            struct Plane { int axis; float s; uint32_t bit; };
            static const Plane planes[6] = {{2, -1.0f, 1u << 5}, {2, 1.0f, 1u << 4}, {0, -1.0f, 1u << 1}, {0, 1.0f, 1u << 0}, {1, -1.0f, 1u << 3}, {1, 1.0f, 1u << 2}};
            int r = 0;
            for (const Plane &pl : planes)
            {
                const std::vector<PolyV> &src = ring[r];
                std::vector<PolyV> &dst = ring[r ^ 1];
                dst.clear();
                const size_t m = src.size();
                for (size_t j = 0; j < m; ++j)
                {
                    PolyV a;
                    for (int q = 0; q < 4; ++q)
                        for (int i = 0; i < 4; ++i) a.q[q][i] = vadd(z31[i], src[j].q[q][i]);
                    const PolyV &b = src[j + 1 < m ? j + 1 : 0];
                    const bool aOut = (clipFlags(a.q[0]) & pl.bit) != 0, bOut = (clipFlags(b.q[0]) & pl.bit) != 0;
                    if (!aOut)
                        dst.push_back(a);
                    if (aOut != bOut)
                        dst.push_back(intersect(a, b, pl.axis, pl.s));
                }
                r ^= 1;
                if (dst.empty())
                    return true;
            }
            const std::vector<PolyV> &poly = ring[r];
            const uint32_t n = static_cast<uint32_t>(poly.size());

            // Kick 1: the packet at 0x18.
            uint32_t q = 0;
            for (;;)
            {
                const Qw tag = ldq(mem, 0x18 + q);
                const uint64_t lo = static_cast<uint64_t>(tag.u(0)) | static_cast<uint64_t>(tag.u(1)) << 32;
                const uint32_t nloop = lo & 0x7FFF, flg = (lo >> 58) & 3, nreg = ((lo >> 60) & 15) ? (lo >> 60) & 15 : 16;
                q += 1 + (flg == 0 ? nloop * nreg : flg == 1 ? (nloop * nreg + 1) / 2 : nloop);
                if (q > 0x40u)
                    return false;
                if ((lo >> 15) & 1)
                    break;
            }
            size_t at = out.size();
            out.resize(at + 16 * q);
            std::memcpy(out.data() + at, mem + 0x18 * 16, 16 * q);
            sizes.push_back(16 * q);

            // Kick 2: the fan.
            Qw h = hdr;
            const uint32_t y = ldq(mem, 24 + (hdr.u(3) & 0xFFFFu)).u(1);
            const uint32_t x = (n + 0x8000u) & 0xFFFFu;
            std::memcpy(&h.f[0], &x, 4);
            std::memcpy(&h.f[1], &y, 4);
            uint32_t tn;
            if (!tagOk(h, tn) || tn != n)
                return false;
            at = out.size();
            out.resize(at + 16 + 48 * n);
            uint8_t *o = out.data() + at;
            std::memcpy(o, h.f, 16);
            o += 16;
            const Qw m0 = ldq(mem, 20), m1 = ldq(mem, 21), m2 = ldq(mem, 22), m3 = ldq(mem, 23);
            const float w24 = poly[0].q[3][3], w30 = c.vf[30][3];
            for (uint32_t k = 0; k < n; ++k, o += 48)
            {
                const float *p = poly[k].q[0];
                float sc[4];
                for (int i = 0; i < 4; ++i)
                    sc[i] = vmadd(vmadd(vmadd(vmul(m0.f[i], p[0]), m1.f[i], p[1]), m2.f[i], p[2]), m3.f[i], p[3]);
                const float qk = vdiv(1.0f, sc[3]);
                float stq[4] = {vmul(poly[k].q[2][0], qk), vmul(poly[k].q[2][1], qk), vmul(poly[k].q[2][2], qk), w30};
                uint32_t col[4], xyz[4];
                for (int i = 0; i < 4; ++i) col[i] = ftoi0(poly[k].q[1][i]);
                for (int i = 0; i < 3; ++i) xyz[i] = ftoi4(vmul(vadd(sc[i], 0.0f), qk));
                xyz[3] = ftoi4(vmul(w24, qk));
                std::memcpy(o, stq, 16);
                std::memcpy(o + 16, col, 16);
                std::memcpy(o + 32, xyz, 16);
            }
            sizes.push_back(16 + 48 * n);
            return true;
        }
    }

    // Strip/list loop (group 3841391935 L1278..L1418, and L0f28..L10c8 with stride 4; group 1920671007 L2278..L2548 after its
    // lighting pass L22e0..L2410). Per vertex: XYZ, RGBA, STQ (+ normal when lit). Out per vertex:
    // STQ*Q (w = vf28.w, never written by the loop: the lit variant's lighting pass leaves the raw
    // colour of the last vertex there), RGBA, FTOI4(p*Q) with word 3 = 0x8000 when culled.
    // Cull rule (checked against the VU over millions of batches): header.w & 3 == 3 means lists,
    // tested at every third vertex, else every vertex from the third; a tested vertex whose last
    // three vertices all lie outside one clip plane gets ADC; one with any vertex outside but no
    // shared plane goes through the VU's clipper, so the batch is left to the VU.
    bool ClippedStrip(const vu1fast::Ctx &c, uint32_t top, const uint8_t *mem, bool lit, std::vector<uint8_t> &pkt, std::vector<uint32_t> &sizes, uint8_t *litOut, bool wide)
    {
        FloatEnv env;
        const Qw hdr = ldq(mem, top);
        uint32_t n;
        if (!tagOk(hdr, n) || n == 0)
            return false;
        const uint32_t stride = lit || wide ? 4u : 3u;
        if ((top & 0x3FFu) + 1 + stride * n > 0x400u)
            return false;
        const bool lists = (hdr.u(3) & 3u) == 3u;
        const float (*m)[4] = c.vf + 1;    // vf1..vf4: object to screen
        const float (*cm)[4] = c.vf + 13;  // vf13..vf16: object to clip space
        float stqW = c.vf[28][3];

        // Lighting pass of the lit variant: colour.xyz = FTOI0(ITOF0(colour) * min(ambient
        // + sum of light colour * max(L.n, 0), 1)); light rows vf05..vf07, colours VU mem 8..11.
        static std::vector<Qw> colours;
        colours.resize(n);
        if (lit)
        {
            const Qw l0 = ldq(mem, 8), l1 = ldq(mem, 9), l2 = ldq(mem, 10), l3 = ldq(mem, 11);
            for (uint32_t k = 0; k < n; ++k)
            {
                const uint32_t base = top + 1 + stride * k;
                const Qw nrm = ldq(mem, base + 3);
                Qw col = ldq(mem, base + 1);
                float l[4];
                for (int i = 0; i < 4; ++i)
                {
                    float acc = vmul(c.vf[5][i], nrm.f[0]);
                    acc = vmadd(acc, c.vf[6][i], nrm.f[1]);
                    l[i] = vmax(vmadd(acc, c.vf[7][i], nrm.f[2]), 0.0f);
                }
                if (k == n - 1)
                    stqW = col.f[3];
                for (int i = 0; i < 3; ++i)
                {
                    float acc = vadd(0.0f, c.vf[8][i]);
                    acc = vmadd(acc, l0.f[i], l[0]);
                    acc = vmadd(acc, l1.f[i], l[1]);
                    acc = vmadd(acc, l2.f[i], l[2]);
                    const float light = vmin(vmadd(acc, l3.f[i], l[3]), 1.0f);
                    const uint32_t v = ftoi0(vmul(itof0(col.u(i)), light));
                    std::memcpy(&col.f[i], &v, 4);
                }
                colours[k] = col;
            }
        }
        else
            for (uint32_t k = 0; k < n; ++k)
                colours[k] = ldq(mem, top + 1 + stride * k + 1);

        // The clipper's kicks come first (the VU kicks the strip at the end), so the strip is
        // built aside and appended.
        static const bool clipOn = !(std::getenv("RT_NATIVECLIP") && std::strcmp(std::getenv("RT_NATIVECLIP"), "0") == 0);
        static std::vector<uint8_t> strip;
        pkt.clear();
        sizes.clear();
        strip.resize(16 + 48 * n);
        uint8_t *o = strip.data();
        std::memcpy(o, hdr.f, 16);
        o += 16;
        uint32_t f0 = 0, f1 = 0;
        float cps[3][4];   // clip-space positions of the last three vertices
        for (uint32_t k = 0; k < n; ++k, o += 48)
        {
            const uint32_t base = top + 1 + stride * k;
            const Qw pos = ldq(mem, base), stq = ldq(mem, base + 2);
            float *cp = cps[k % 3], p[4];
            transform(cm, pos.f, cp);
            transform(m, pos.f, p);
            const uint32_t f = clipFlags(cp);
            const bool tested = lists ? k % 3 == 2 : k >= 2;
            bool adc = false;
            if (tested && (f0 | f1 | f) != 0)
            {
                if ((f0 & f1 & f) == 0)
                {
                    if (!clipOn)
                        return false;
                    PolyV tri[3];
                    for (int t = 0; t < 3; ++t)
                    {
                        const uint32_t kk = k - 2 + t, b = top + 1 + stride * kk;
                        std::memcpy(tri[t].q[0], cps[kk % 3], 16);
                        for (int i = 0; i < 4; ++i) tri[t].q[1][i] = itof0(colours[kk].u(i));
                        const Qw st = ldq(mem, b + 2), ps = ldq(mem, b);
                        std::memcpy(tri[t].q[2], st.f, 16);
                        std::memcpy(tri[t].q[3], ps.f, 16);
                    }
                    if (!clipTriangle(c, mem, hdr, tri, pkt, sizes))
                        return false;
                }
                adc = true;
            }
            f0 = f1;
            f1 = f;
            const float q = vdiv(1.0f, p[3]);
            vertexOut(o, stq.f, fbits(stqW), colours[k].f, p, q, adc);
        }
        pkt.insert(pkt.end(), strip.begin(), strip.end());
        sizes.push_back(static_cast<uint32_t>(strip.size()));
        // The VU's lighting pass leaves the lit colours in its memory, in place of the input ones.
        if (lit && litOut)
            for (uint32_t k = 0; k < n; ++k)
                std::memcpy(litOut + (((top + 1 + stride * k + 1) & 0x3FFu) << 4), colours[k].f, 16);
        return true;
    }

    // Dump program 1604436274 (L0038..L0160): count = header.x & vi10; per vertex XYZ, RGBA, STQ;
    // out STQ*Q (w = vf24.w, never written), RGBA, FTOI4(p*Q).
    bool PlainStrip(const vu1fast::Ctx &c, uint32_t top, const uint8_t *mem, std::vector<uint8_t> &pkt)
    {
        FloatEnv env;
        const Qw hdr = ldq(mem, top);
        uint32_t tn;
        if (!tagOk(hdr, tn))
            return false;
        const uint32_t n = hdr.u(0) & static_cast<uint32_t>(static_cast<uint16_t>(c.vi[10]));
        if (n == 0 || n != tn || (top & 0x3FFu) + 1 + 3 * n > 0x400u)
            return false;
        const float (*m)[4] = c.vf + 1;
        pkt.resize(16 + 48 * n);
        uint8_t *o = pkt.data();
        std::memcpy(o, hdr.f, 16);
        o += 16;
        for (uint32_t k = 0; k < n; ++k, o += 48)
        {
            const uint32_t base = top + 1 + 3 * k;
            const Qw pos = ldq(mem, base), col = ldq(mem, base + 1), stq = ldq(mem, base + 2);
            float p[4];
            transform(m, pos.f, p);
            const float q = vdiv(1.0f, p[3]);
            vertexOut(o, stq.f, fbits(c.vf[24][3]), col.f, p, q, false);
        }
        return true;
    }

    // Overlay group 1331843391 (L0d58..L0ee8): count = header.x & vi10; per vertex XYZ, colour,
    // STQ, normal. l = max(L.n, 0) with rows vf05..vf07; light = min(vf08 + mem8 * l.x + mem9 * l.y
    // + mem10 * l.z + light(previous vertex) * l.w, 1) (the program feeds vf26 back into itself);
    // colour.xyz = FTOI0(ITOF0(colour) * light), colour.w raw. Out STQ*Q (w = STQ.w), colour,
    // FTOI4(p*Q). vf26 ends as the last vertex's light.
    bool LitStrip(const vu1fast::Ctx &c, uint32_t top, const uint8_t *mem, std::vector<uint8_t> &pkt, WriteBack &wb)
    {
        FloatEnv env;
        const Qw hdr = ldq(mem, top);
        uint32_t tn;
        if (!tagOk(hdr, tn))
            return false;
        const uint32_t n = hdr.u(0) & static_cast<uint32_t>(static_cast<uint16_t>(c.vi[10]));
        if (n == 0 || n != tn || (top & 0x3FFu) + 1 + 4 * n > 0x400u)
            return false;
        const float (*m)[4] = c.vf + 1;
        const Qw l0 = ldq(mem, 8), l1 = ldq(mem, 9), l2 = ldq(mem, 10);
        float light[4] = {c.vf[26][0], c.vf[26][1], c.vf[26][2], c.vf[26][3]};
        pkt.resize(16 + 48 * n);
        uint8_t *o = pkt.data();
        std::memcpy(o, hdr.f, 16);
        o += 16;
        for (uint32_t k = 0; k < n; ++k, o += 48)
        {
            const uint32_t base = top + 1 + 4 * k;
            const Qw pos = ldq(mem, base), col = ldq(mem, base + 1), stq = ldq(mem, base + 2), nrm = ldq(mem, base + 3);
            float l[4];
            for (int i = 0; i < 4; ++i)
            {
                float acc = vmul(c.vf[5][i], nrm.f[0]);
                acc = vmadd(acc, c.vf[6][i], nrm.f[1]);
                l[i] = vmax(vmadd(acc, c.vf[7][i], nrm.f[2]), 0.0f);
            }
            for (int i = 0; i < 3; ++i)
            {
                float acc = vadd(0.0f, c.vf[8][i]);
                acc = vmadd(acc, l0.f[i], l[0]);
                acc = vmadd(acc, l1.f[i], l[1]);
                acc = vmadd(acc, l2.f[i], l[2]);
                light[i] = vmin(vmadd(acc, light[i], l[3]), 1.0f);
            }
            float p[4];
            transform(m, pos.f, p);
            const float q = vdiv(1.0f, p[3]);
            const uint32_t lc[4] = {ftoi0(vmul(itof0(col.u(0)), light[0])), ftoi0(vmul(itof0(col.u(1)), light[1])),
                                    ftoi0(vmul(itof0(col.u(2)), light[2])), col.u(3)};
            vertexOut(o, stq.f, stq.u(3), lc, p, q, false);
        }
        wb.vf26 = true;
        std::memcpy(wb.vf26v, light, 16);
        return true;
    }

    // Overlay group 939320750 (L3218..L36d8), per batch at TOP: header (x: particle stride, y:
    // count), constants at TOP+1..9, particles from TOP+10 (position, velocity). Each particle is
    // aged, given random drift (R), animated from a 4-frame size table (TOP + (frame & 7)), culled
    // when its alpha is zero, beyond the far plane or behind the eye, and drawn as a sprite of 4
    // corners written into one of two output buffers (TOP+0xa0 / TOP+0xb0) and kicked. The status
    // flags the program tests are those of the instruction four cycles earlier (MAC pipeline):
    // Z of the alpha (vf12.w), S of the frame countdown, S of vf10.w and of the wrapped phase.
    bool Particles(const vu1fast::Ctx &c, uint32_t top, const uint8_t *mem, std::vector<uint8_t> &pkt, std::vector<uint32_t> &sizes,
                   WriteBack &wb, uint8_t *memOut)
    {
        FloatEnv env;
        top &= 0x3FFu;
        const Qw hdr = ldq(mem, top);
        int32_t count = static_cast<int16_t>(hdr.u(1) & 0xFFFF);
        const int32_t stride = static_cast<int16_t>(hdr.u(0) & 0xFFFF);
        const uint32_t vi12 = static_cast<uint16_t>(c.vi[12]);
        if (count < 0 || count > 0x400)
            return false;
        Qw v05 = ldq(mem, top), v06 = ldq(mem, top + 1), v07 = ldq(mem, top + 2), v08 = ldq(mem, top + 3), v22 = ldq(mem, top + 4);
        const Qw v24 = ldq(mem, top + 5), v29 = ldq(mem, top + 6), v30 = ldq(mem, top + 7), v27 = ldq(mem, top + 8), v28 = ldq(mem, top + 9);
        const Qw v23 = ldq(mem, 160);
        const float (*m)[4] = c.vf + 1;    // vf1..vf4: object to screen
        const float (*cm)[4] = c.vf + 13;  // vf13..vf16: object to clip space
        // Output buffers: the tag and the qwords the program leaves alone come from VU memory.
        Qw buf[2][13];
        uint32_t bsize[2];
        for (int b = 0; b < 2; ++b)
        {
            for (uint32_t q = 0; q < 13; ++q)
                buf[b][q] = ldq(mem, top + 0xa0 + 0x10 * b + q);
            const uint64_t lo = static_cast<uint64_t>(buf[b][0].u(0)) | static_cast<uint64_t>(buf[b][0].u(1)) << 32;
            const uint32_t nloop = lo & 0x7FFF, nreg = ((buf[b][0].u(1) >> 28) & 15) ? (buf[b][0].u(1) >> 28) & 15 : 16;
            if (((lo >> 58) & 3) != 0 || !((lo >> 15) & 1) || 1 + nloop * nreg > 13)
                return false;   // only single PACKED EOP tags within the 13 qwords
            bsize[b] = 16 * (1 + nloop * nreg);
        }
        if (stride > 0 && 0xa + static_cast<uint32_t>(count) * stride + 2 > 0xa0)
            return false;   // the particles would run into the output buffers
        uint32_t r = c.r, clip = c.clip;

        v05.f[1] = itof0(v08.u(1));
        v05.f[1] = vmul(v05.f[1], v06.f[2]);
        r = 0x3F800000u | (fbits(v08.f[0]) & 0x007FFFFFu);
        v05.f[1] = vadd(v05.f[1], v22.f[0]);
        v05.f[1] = vadd(v05.f[1], v28.f[3]);
        uint32_t in = top + 0xa;
        uint32_t ob = 0;   // vi07: output buffer (0: TOP+0xa0, 1: TOP+0xb0)
        pkt.clear();
        sizes.clear();
        auto rnext = [&r]() {
            r = ((r << 1) ^ ((r >> 4) & 1u) ^ ((r >> 22) & 1u)) & 0x007FFFFFu;
            r |= 0x3F800000u;
            return bfloat(r);
        };
        for (;;)
        {
            const float v09y = vmul(v05.f[1], v06.f[0]);
            if (count == 0)
                break;
            v05.f[3] = vsub(1.0f, v09y);
            Qw v10 = ldq(mem, in), v11 = ldq(mem, in + 1);
            --count;
            in += stride;
            float v09w = vmul(v10.f[3], v07.f[1]);
            v10.f[3] = bfloat(fbits(v10.f[3]) & 0x7FFFFFFFu);
            float v12[4];
            for (int i = 0; i < 3; ++i) v12[i] = vmul(v27.f[i], v09y);
            v09w = vmul(v09w, v05.f[3]);
            float v08w = vadd(v10.f[3], v07.f[2]);
            float v23x[3];
            for (int i = 0; i < 3; ++i) v23x[i] = vmul(v28.f[i], v05.f[3]);
            v09w = vmul(v09w, v05.f[3]);
            v08w = vmul(v08w, v09y);
            for (int i = 0; i < 3; ++i) v12[i] = vadd(v23x[i], v12[i]);
            v09w = vmul(v09w, v05.f[3]);
            v08w = vadd(v08w, v07.f[0]);
            float v26[3];
            v26[0] = rnext();
            uint32_t zfl;
            v12[3] = f_mul(vn(v11.f[3]), vn(v05.f[3]), zfl);
            v26[1] = rnext();
            v26[2] = rnext();
            if (!(zfl & 1u))
            {
                // Frame: integer part of the countdown (index into the size table), then its
                // fraction by repeated subtraction, as the program does.
                const float v09x = vadd(0.0f, v09w);
                uint32_t sfl = 0;
                uint32_t iter = 0;
                do
                {
                    v09w = f_sub(vn(v09w), 1.0f, sfl);
                    if (++iter > (1u << 20))
                        return false;
                } while (!(sfl & 2u));
                const uint32_t frame = ftoi0(v09x);
                v09w = vadd(v09w, 1.0f);
                uint32_t vi06 = (frame & 0xFFFFu) & vi12;
                const uint32_t vi05 = vi06 + top;
                vi06 = (vi06 + 2) & vi12;
                const uint32_t vi11 = vi06 + top;
                float v28w = ldq(mem, vi05).f[3], v30w = ldq(mem, vi11).f[3];
                const float v29w0 = ldq(mem, vi05 + 1).f[3], v31w0 = ldq(mem, vi11 + 1).f[3];
                float v07w = vmul(v28w, v09w);
                float v22w = vmul(v30w, v09w);
                for (int i = 0; i < 3; ++i) v26[i] = vsub(v26[i], v08.f[2]);
                v28w = vsub(v28w, v07w);
                v30w = vsub(v30w, v22w);
                for (int i = 0; i < 3; ++i) v26[i] = vadd(v26[i], v26[i]);
                v07w = vmul(v29w0, v09w);
                v22w = vmul(v31w0, v09w);
                for (int i = 0; i < 3; ++i) v26[i] = vmul(v26[i], v30.f[i]);
                v28w = vadd(v28w, v07w);
                v30w = vadd(v30w, v22w);
                for (int i = 0; i < 3; ++i) v26[i] = vadd(v26[i], v29.f[i]);
                v28w = vmul(v28w, v08w);
                v30w = vmul(v30w, v08w);
                for (int i = 0; i < 3; ++i) v26[i] = vmul(v26[i], v05.f[1]);
                const float v29w = vmul(v28w, v23.f[3]);
                const float v31w = vmul(v30w, v23.f[3]);
                v28w = vmul(v28w, v23.f[2]);
                for (int i = 0; i < 3; ++i) v11.f[i] = vadd(v11.f[i], v26[i]);
                v30w = vmul(v30w, v23.f[2]);
                const float c17y = vadd(0.0f, v29w), c18y = vadd(0.0f, v31w);
                for (int i = 0; i < 3; ++i) v11.f[i] = vmul(v11.f[i], v05.f[1]);
                const float c19y = vsub(0.0f, v31w), c20y = vsub(0.0f, v29w);
                for (int i = 0; i < 3; ++i) v10.f[i] = vadd(v10.f[i], v11.f[i]);
                v28w = vmul(v28w, v24.f[1]);
                v30w = vmul(v30w, v24.f[1]);
                float v21[4];
                transform(cm, v10.f, v21);
                clip = ((clip << 6) | clipFlags(v21)) & 0xFFFFFFu;
                if (!(clip & 0x10u))
                {
                    float p[4];
                    transform(m, v10.f, p);
                    uint32_t col[4];
                    for (int i = 0; i < 4; ++i) col[i] = ftoi0(v12[i]);
                    const float c17x = vsub(0.0f, v30w);
                    const float q = vdiv(1.0f, p[3]);
                    const float c18x = vadd(0.0f, v28w), c19x = vsub(0.0f, v28w), c20x = vadd(0.0f, v30w);
                    if (!std::signbit(p[3]))
                    {
                        const float corner[4][2] = {{c17x, c17y}, {c18x, c18y}, {c19x, c19y}, {c20x, c20y}};
                        ob ^= 1u;
                        Qw *o = buf[ob];
                        for (int k = 0; k < 4; ++k)
                        {
                            float v[4];
                            for (int i = 0; i < 4; ++i)
                            {
                                const float base = i < 2 ? corner[k][i] : 0.0f;
                                v[i] = vmul(vadd(base, p[i]), q);
                            }
                            for (int i = 0; i < 2; ++i)
                                v[i] = vmin(vmax(v[i], 0.0f), 4095.0f);
                            std::memcpy(o[2 + 3 * k].f, col, 16);
                            const uint32_t xyz[4] = {ftoi4(v[0]), ftoi4(v[1]), ftoi4(v[2]), ftoi4(v[3])};
                            std::memcpy(o[3 + 3 * k].f, xyz, 16);
                        }
                        const size_t at = pkt.size();
                        pkt.resize(at + 13 * 16);
                        std::memcpy(pkt.data() + at, o, 13 * 16);
                        pkt.resize(at + bsize[ob]);
                        sizes.push_back(bsize[ob]);
                    }
                }
            }
            v05.f[1] = vsub(v05.f[1], v06.f[2]);
            uint32_t sfl;
            f_sub(vn(v05.f[1]), vn(v22.f[0]), sfl);
            if (sfl & 2u)
                v05.f[1] = vadd(v05.f[1], v06.f[1]);
        }
        // The stores of the last particle drawn into each buffer are what VU memory keeps.
        if (memOut)
            for (int b = 0; b < 2; ++b)
                for (uint32_t q = 2; q < 13; ++q)
                    if (q % 3 != 1)
                        std::memcpy(memOut + (((top + 0xa0 + 0x10 * b + q) & 0x3FFu) << 4), buf[b][q].f, 16);
        wb.r = true;
        wb.rv = r;
        wb.clip = true;
        wb.clipv = clip;
        wb.i = !sizes.empty();   // LOI 4095 runs only on the path that draws a sprite
        wb.iv = 4095.0f;
        return true;
    }

    // Overlay group 1920694623 (L3058..L3188). TOP: GIF tag; TOP+1 centre (xyz, w = size scale);
    // TOP+2 colour; TOP+3 UV rectangle (u0 v0 u1 v1). Culled when the centre is outside any clip
    // plane (FCAND of this batch's CLIPw: checked against the VU, it never sees the previous one).
    // Drawn: corners (p -/+ size) * Q, x/y clamped at 0 (and the second corner at 4095), FTOI4;
    // stored at TOP+0x96 as tag, (u1 v1 0 0), colour, corner 1, (u0 v0 0 0), colour, corner 2.
    bool Sprite(const vu1fast::Ctx &c, uint32_t top, const uint8_t *mem, std::vector<uint8_t> &pkt, WriteBack &wb, uint8_t *memOut)
    {
        FloatEnv env;
        top &= 0x3FFu;
        const Qw tag = ldq(mem, top), cen = ldq(mem, top + 1), col = ldq(mem, top + 2), uv = ldq(mem, top + 3), size = ldq(mem, 0xa3);
        const uint64_t lo = static_cast<uint64_t>(tag.u(0)) | static_cast<uint64_t>(tag.u(1)) << 32;
        const uint32_t nloop = lo & 0x7FFF, nreg = ((tag.u(1) >> 28) & 15) ? (tag.u(1) >> 28) & 15 : 16;
        if (((lo >> 58) & 3) != 0 || !((lo >> 15) & 1) || 1 + nloop * nreg > 7)
            return false;   // only a single PACKED EOP tag within the 7 qwords the program writes
        const float (*m)[4] = c.vf + 1;
        const float (*cm)[4] = c.vf + 13;
        const float v05[4] = {cen.f[0], cen.f[1], cen.f[2], vmul(1.0f, 1.0f)};
        float v20[4], v21[4];
        transform(cm, v05, v20);
        v4f acc = mul4(ld4(m[3]), 1.0f);
        acc = madd4(acc, ld4(m[2]), v05[2]);
        acc = madd4(acc, ld4(m[1]), v05[1]);
        const v4f r21 = madd4(acc, ld4(m[0]), v05[0]);
        std::memcpy(v21, &r21, 16);
        const uint32_t cf = clipFlags(v20);
        const bool culled = cf != 0;
        const uint32_t clip = ((c.clip << 6) | cf) & 0xFFFFFFu;
        const v4f v25 = mul4(ld4(size.f), cen.f[3]);
        float v24[4] = {uv.f[0], uv.f[1], vmul(uv.f[2], 0.0f), vmul(uv.f[3], 0.0f)};
        float v26[4] = {uv.f[2], uv.f[3], vmul(uv.f[0], 0.0f), vmul(uv.f[1], 0.0f)};
        const float q = vdiv(1.0f, v21[3]);
        float v22[4], v23[4];
        for (int i = 0; i < 4; ++i)
        {
            v22[i] = vsub(v21[i], v25[i]);
            v23[i] = vadd(v21[i], v25[i]);
        }
        pkt.clear();
        if (!culled)
        {
            const v4f a = mul4(ld4(v22), q), b = mul4(ld4(v23), q);
            std::memcpy(v22, &a, 16);
            std::memcpy(v23, &b, 16);
            for (int i = 0; i < 2; ++i)
            {
                v22[i] = vmax(v22[i], 0.0f);
                v23[i] = vmin(vmax(v23[i], 0.0f), 4095.0f);
            }
            const v4u x22 = ftoi4v(ld4(v22)), x23 = ftoi4v(ld4(v23));
            std::memcpy(v22, &x22, 16);
            std::memcpy(v23, &x23, 16);
            const float *out[7] = {tag.f, v26, col.f, v22, v24, col.f, v23};
            pkt.resize(7 * 16);
            for (int k = 0; k < 7; ++k)
                std::memcpy(pkt.data() + 16 * k, out[k], 16);
            if (memOut)
                for (uint32_t k = 0; k < 7; ++k)
                    std::memcpy(memOut + (((top + 0x96 + k) & 0x3FFu) << 4), out[k], 16);
            pkt.resize(16 * (1 + nloop * nreg));
            wb.i = true;
            wb.iv = 4095.0f;
        }
        wb.clip = true;
        wb.clipv = clip;
        auto setVf = [&wb](int k, const float *v) { wb.vfMask |= 1u << k; std::memcpy(wb.vf[k], v, 16); };
        setVf(5, v05);
        setVf(6, col.f);
        setVf(17, tag.f);
        setVf(20, v20);
        setVf(21, v21);
        setVf(22, v22);
        setVf(23, v23);
        setVf(24, v24);
        float v25f[4];
        std::memcpy(v25f, &v25, 16);
        setVf(25, v25f);
        setVf(26, v26);
        const float v28[4] = {c.vf[28][0], c.vf[28][1], c.vf[28][2], cen.f[3]};
        setVf(28, v28);
        wb.acc = true;
        std::memcpy(wb.accv, &acc, 16);
        wb.q = true;
        wb.qv = q;
        const int32_t vis[][2] = {{1, culled ? 1 : 0}, {2, static_cast<int32_t>(top + 0x96)}, {3, static_cast<int32_t>(top + 4)},
                                  {6, static_cast<int32_t>(top + 0x97)}, {7, 0xa0}, {8, static_cast<int32_t>(top)}, {12, 0x7fff}};
        for (const auto &v : vis)
        {
            wb.viMask |= 1u << v[0];
            wb.vi[v[0]] = v[1];
        }
        return true;
    }
    // Overlay group 1920694623 (L31a0..L31b0): XTOP vi08, XGKICK vi08. The packet runs from TOP
    // through the tag with EOP (PACKED and REGLIST: NLOOP * NREG qwords or registers; IMAGE:
    // NLOOP qwords), wrapping at the end of VU memory as the kick does.
    bool Passthrough(uint32_t top, const uint8_t *mem, std::vector<uint8_t> &pkt, WriteBack &wb)
    {
        top &= 0x3FFu;
        uint32_t q = 0;
        for (;;)
        {
            const Qw tag = ldq(mem, top + q);
            const uint64_t lo = static_cast<uint64_t>(tag.u(0)) | static_cast<uint64_t>(tag.u(1)) << 32;
            const uint32_t nloop = lo & 0x7FFF, flg = (lo >> 58) & 3, nreg = ((lo >> 60) & 15) ? (lo >> 60) & 15 : 16;
            q += 1 + (flg == 0 ? nloop * nreg : flg == 1 ? (nloop * nreg + 1) / 2 : nloop);
            if (q > 0x400u)
                return false;
            if ((lo >> 15) & 1)
                break;
        }
        pkt.resize(16 * q);
        for (uint32_t k = 0; k < q; ++k)
            std::memcpy(pkt.data() + 16 * k, mem + (((top + k) & 0x3FFu) << 4), 16);
        wb.viMask |= 1u << 8 | 1u << 12;
        wb.vi[8] = static_cast<int32_t>(top);
        wb.vi[12] = 0x7fff;
        return true;
    }
    // Dump program 1159117218 (L0388..L05c0). Header at TOP (GIF tag; x & 0x7fff = count, w & 3 ==
    // 3 = lists); per vertex XYZ, RGBA, STQ, normal (stride 4). Pass 1 (L0418, software
    // pipelined): T = ((L.n).xy + 1 + vf08.xy) * vf08.z with rows vf05..vf07, stored over STQ.xy
    // of each vertex; the pipeline also stores the incoming vf30 and the incoming vf21's T at
    // TOP-5 and TOP-1. Pass 2: as ClippedStrip (CLIPw of the last three vertices, FCOR plane
    // tests, ADC via ISW 0x8000) with STQ.w = vf28.w as it came in. Output at TOP+0x98 (the
    // header, then 3 qwords per vertex); VU mem 0xa8 = the header with y from mem[24 + w].y.
    bool EnvStrip(const vu1fast::Ctx &c, uint32_t top, const uint8_t *mem, std::vector<uint8_t> &pkt, WriteBack &wb, uint8_t *memOut)
    {
        if (!memOut)
            return false;
        FloatEnv env;
        top &= 0x3FFu;
        const Qw hdr = ldq(mem, top);
        uint32_t tn;
        if (!tagOk(hdr, tn))
            return false;
        const uint32_t n = hdr.u(0) & 0x7FFFu;
        if (n == 0 || n != tn || 1 + 4 * n > 0x98u || top + 0x99 + 3 * n > 0x400u)
            return false;   // (input overlapping the output, or a wrapping output: not modelled)
        const uint32_t vi12 = static_cast<uint16_t>(hdr.u(3));
        const bool lists = (vi12 & 3u) == 3u;
        const float (*m)[4] = c.vf + 1;
        const float (*cm)[4] = c.vf + 13;
        auto st = [memOut](uint32_t q, const float *v, int mask) {
            float *d = reinterpret_cast<float *>(memOut + ((q & 0x3FFu) << 4));
            for (int i = 0; i < 4; ++i)
                if ((mask >> i) & 1)
                    std::memcpy(&d[i], &v[i], 4);
        };

        // Pass 1 (every vertex), then its stores; T_-2 / T_-1 come from the incoming vf30 / vf21.
        static std::vector<Qw> stq;
        stq.resize(n);
        const float *v08 = c.vf[8];
        float tPrev2[4] = {c.vf[30][0], c.vf[30][1], c.vf[30][2], c.vf[30][3]};
        float a[2] = {c.vf[21][0], c.vf[21][1]};
        float v22[2], v25[2];
        float t[4] = {c.vf[30][0], c.vf[30][1], c.vf[30][2], c.vf[30][3]};
        const uint32_t base0 = top + 1;
        // VU mem 0xa8 = the header, then its y from mem[24 + w] (read after that store).
        st(0xa8, hdr.f, 15);
        const float a8y[4] = {0.0f, ldq(memOut, 24 + vi12).f[1], 0.0f, 0.0f};
        st(0xa8, a8y, 2);
        st(base0 - 6, tPrev2, 3);
        for (int32_t j = -1; j < static_cast<int32_t>(n); ++j)
        {
            // T_j from A_j (A_-1 = incoming vf21.xy), then A_{j+1} from the next normal.
            for (int i = 0; i < 2; ++i)
            {
                v22[i] = vadd(a[i], v08[i]);
                t[i] = vmul(v22[i], v08[2]);
            }
            st(base0 + 4 * j + 2, t, 3);
            if (j >= 0)
            {
                stq[j] = ldq(memOut, base0 + 4 * j + 2);
                std::memcpy(stq[j].f, t, 8);
            }
            if (j + 1 < static_cast<int32_t>(n))
            {
                const Qw nrm = ldq(memOut, base0 + 4 * (j + 1) + 3);
                for (int i = 0; i < 2; ++i)
                {
                    float acc = vmul(c.vf[5][i], nrm.f[0]);
                    acc = vmadd(acc, c.vf[6][i], nrm.f[1]);
                    v25[i] = vmadd(acc, c.vf[7][i], nrm.f[2]);
                    a[i] = vadd(v25[i], 1.0f);
                }
            }
        }

        // Pass 2.
        const float stqW = c.vf[28][3];
        pkt.resize(16 + 48 * n);
        uint8_t *o = pkt.data();
        std::memcpy(o, hdr.f, 16);
        o += 16;
        float cpm2[4] = {0, 0, 0, 1}, cpm1[4] = {0, 0, 0, 1}, cp[4] = {0, 0, 0, 1}, p[4] = {};
        uint32_t clip = c.clip;
        int32_t cm2 = -2, cm1 = -2, ck = 0, vi04 = 0, vi13 = lists ? 3 : -1;
        bool anyAdc = false;
        float q = 0.0f;
        Qw col{};
        for (uint32_t k = 0; k < n; ++k, o += 48)
        {
            const uint32_t base = base0 + 4 * k;
            const Qw pos = ldq(memOut, base);
            col = ldq(memOut, base + 1);
            std::memcpy(cpm2, cpm1, 16);
            std::memcpy(cpm1, cp, 16);
            transform(cm, pos.f, cp);
            transform(m, pos.f, p);
            const uint32_t f2 = clipFlags(cpm2), f1 = clipFlags(cpm1), f0 = clipFlags(cp);
            clip = ((clip << 18) | (f2 << 12) | (f1 << 6) | f0) & 0xFFFFFFu;
            ck = f0 != 0 ? 1 : 0;
            vi04 = cm2 + cm1 + ck;
            cm2 = cm1;
            cm1 = ck;
            bool tested = true;
            if (vi13 >= 0)
            {
                --vi13;
                if (vi13 > 0)
                    tested = false;
                else
                    vi13 = 3;
            }
            bool adc = false;
            if (tested && vi04 > 0)
            {
                if ((f2 & f1 & f0) == 0)
                    return false;   // the VU's clipper
                adc = anyAdc = true;
            }
            q = vdiv(1.0f, p[3]);
            vertexOut(o, stq[k].f, fbits(stqW), col.f, p, q, adc);
        }

        // The output at TOP+0x98 (above the input, checked at the top).
        for (uint32_t k = 0; k < 1 + 3 * n; ++k)
            st(top + 0x98 + k, reinterpret_cast<const float *>(pkt.data() + 16 * k), 15);

        // Registers as the program leaves them.
        auto setVf = [&wb](int k, const float *v) { wb.vfMask |= 1u << k; std::memcpy(wb.vf[k], v, 16); };
        const uint32_t last = base0 + 4 * (n - 1);
        const Qw next = ldq(memOut, last + 4);   // pos of a vertex n (read after the stores)
        float pn[4];
        transform(m, next.f, pn);
        setVf(19, hdr.f);
        setVf(20, next.f);
        setVf(25, pn);
        setVf(17, col.f);
        setVf(21, stq[n - 1].f);
        setVf(22, cpm2);
        setVf(23, cpm1);
        setVf(24, cp);
        const v4f p27 = mul4(ld4(p), q);
        const v4u x27 = ftoi4v(p27);
        float v27[4];
        std::memcpy(v27, &x27, 16);
        setVf(27, v27);
        const v4f s28 = mul4(ld4(stq[n - 1].f), q);
        const float v28[4] = {s28[0], s28[1], s28[2], stqW};
        setVf(28, v28);
        const float v30[4] = {t[0], t[1], c.vf[30][2], c.vf[30][3]};
        setVf(30, v30);
        float v31[4];
        for (int i = 0; i < 4; ++i) v31[i] = vmul(c.vf[31][i], 0.0f);
        setVf(31, v31);
        wb.acc = true;
        {
            const v4f a16 = mul4(ld4(cm[3]), 1.0f);
            std::memcpy(wb.accv, &a16, 16);
        }
        wb.q = true;
        wb.qv = q;
        wb.clip = true;
        wb.clipv = clip;
        const int32_t vis[][2] = {{1, ck}, {2, static_cast<int32_t>(top + 0x98)}, {3, static_cast<int32_t>(base0 + 4 * n + 1)},
                                  {4, vi04}, {5, cm2}, {6, cm1}, {7, static_cast<int32_t>(top + 0x9a + 3 * n)}, {10, 0},
                                  {11, anyAdc ? -32768 : 3}, {12, static_cast<int32_t>(vi12 & 3u)}, {13, vi13}};
        for (const auto &v : vis)
        {
            wb.viMask |= 1u << v[0];
            wb.vi[v[0]] = v[1];
        }
        return true;
    }

    // EnvStrip for a batch the renderer transforms (GPU model path): no vertices, only what the
    // program leaves behind that later batches can see. The stores at 0xa8, TOP-5 and TOP-1 are
    // made; the T values over the input STQs and the output at TOP+0x98 are not (the next unpack
    // and the kick are their only readers). The registers come from the last vertices as in
    // EnvStrip, except vi11 (the ADC marker), which only looks at the last three.
    bool EnvStripTail(const vu1fast::Ctx &c, uint32_t top, uint8_t *mem, WriteBack &wb)
    {
        FloatEnv env;
        top &= 0x3FFu;
        const Qw hdr = ldq(mem, top);
        uint32_t tn;
        if (!tagOk(hdr, tn))
            return false;
        const uint32_t n = hdr.u(0) & 0x7FFFu;
        if (n == 0 || n != tn || 1 + 4 * n > 0x98u || top + 0x99 + 3 * n > 0x400u)
            return false;
        const uint32_t vi12 = static_cast<uint16_t>(hdr.u(3));
        const bool lists = (vi12 & 3u) == 3u;
        const float (*m)[4] = c.vf + 1;
        const float (*cm)[4] = c.vf + 13;
        auto st = [mem](uint32_t q, const float *v, int mask) {
            float *d = reinterpret_cast<float *>(mem + ((q & 0x3FFu) << 4));
            for (int i = 0; i < 4; ++i)
                if ((mask >> i) & 1)
                    std::memcpy(&d[i], &v[i], 4);
        };
        const float *v08 = c.vf[8];
        const uint32_t base0 = top + 1;
        st(0xa8, hdr.f, 15);
        const float a8y[4] = {0.0f, ldq(mem, 24 + vi12).f[1], 0.0f, 0.0f};
        st(0xa8, a8y, 2);
        st(base0 - 6, c.vf[30], 3);
        // T from A (the incoming vf21 for the store at TOP-1, the last normal for the last vertex).
        auto tOf = [v08](const float *a, float *t) {
            for (int i = 0; i < 2; ++i)
                t[i] = vmul(vadd(a[i], v08[i]), v08[2]);
        };
        float t[4] = {c.vf[30][0], c.vf[30][1], c.vf[30][2], c.vf[30][3]};
        tOf(c.vf[21], t);
        st(base0 - 2, t, 3);
        const uint32_t last = base0 + 4 * (n - 1);
        {
            const Qw nrm = ldq(mem, last + 3);
            float a[2];
            for (int i = 0; i < 2; ++i)
            {
                float acc = vmul(c.vf[5][i], nrm.f[0]);
                acc = vmadd(acc, c.vf[6][i], nrm.f[1]);
                a[i] = vadd(vmadd(acc, c.vf[7][i], nrm.f[2]), 1.0f);
            }
            tOf(a, t);
        }
        Qw stqLast = ldq(mem, last + 2);
        std::memcpy(stqLast.f, t, 8);

        // Pass 2 over the last (up to) three vertices, with the clip state of the ones before.
        float cpm2[4] = {0, 0, 0, 1}, cpm1[4] = {0, 0, 0, 1}, cp[4] = {0, 0, 0, 1}, p[4] = {};
        uint32_t clip = c.clip;
        int32_t cm2 = -2, cm1 = -2, ck = 0, vi04 = 0, vi13 = lists ? 3 : -1;
        bool anyAdc = false;
        const uint32_t k0 = n > 3 ? n - 3 : 0;
        if (lists)
            for (uint32_t k = 0; k < k0; ++k)
                if (--vi13 <= 0)
                    vi13 = 3;
        if (k0 >= 1)
        {
            // The clip-test history as the loop leaves it before vertex k0 (CLIP itself is
            // shifted out by the three vertices that follow).
            float h[2][4] = {{0, 0, 0, 1}, {0, 0, 0, 1}};
            int32_t hc[2] = {-2, -2};
            for (uint32_t k = k0 > 2 ? k0 - 2 : 0; k < k0; ++k)
            {
                std::memcpy(h[0], h[1], 16);
                transform(cm, ldq(mem, base0 + 4 * k).f, h[1]);
                hc[0] = hc[1];
                hc[1] = clipFlags(h[1]) != 0 ? 1 : 0;
            }
            std::memcpy(cpm1, h[0], 16);
            std::memcpy(cp, h[1], 16);
            cm2 = hc[0];
            cm1 = hc[1];
        }
        float q = 0.0f;
        Qw col{};
        for (uint32_t k = k0; k < n; ++k)
        {
            const uint32_t base = base0 + 4 * k;
            const Qw pos = ldq(mem, base);
            col = ldq(mem, base + 1);
            std::memcpy(cpm2, cpm1, 16);
            std::memcpy(cpm1, cp, 16);
            transform(cm, pos.f, cp);
            transform(m, pos.f, p);
            const uint32_t f2 = clipFlags(cpm2), f1 = clipFlags(cpm1), f0 = clipFlags(cp);
            clip = ((clip << 18) | (f2 << 12) | (f1 << 6) | f0) & 0xFFFFFFu;
            ck = f0 != 0 ? 1 : 0;
            vi04 = cm2 + cm1 + ck;
            cm2 = cm1;
            cm1 = ck;
            bool tested = true;
            if (vi13 >= 0)
            {
                --vi13;
                if (vi13 > 0)
                    tested = false;
                else
                    vi13 = 3;
            }
            if (tested && vi04 > 0)
                anyAdc = true;
            q = vdiv(1.0f, p[3]);
        }

        auto setVf = [&wb](int k, const float *v) { wb.vfMask |= 1u << k; std::memcpy(wb.vf[k], v, 16); };
        const float stqW = c.vf[28][3];
        const Qw next = ldq(mem, last + 4);
        float pn[4];
        transform(m, next.f, pn);
        setVf(19, hdr.f);
        setVf(20, next.f);
        setVf(25, pn);
        setVf(17, col.f);
        setVf(21, stqLast.f);
        setVf(22, cpm2);
        setVf(23, cpm1);
        setVf(24, cp);
        const v4u x27 = ftoi4v(mul4(ld4(p), q));
        float v27[4];
        std::memcpy(v27, &x27, 16);
        setVf(27, v27);
        const v4f s28 = mul4(ld4(stqLast.f), q);
        const float v28[4] = {s28[0], s28[1], s28[2], stqW};
        setVf(28, v28);
        const float v30[4] = {t[0], t[1], c.vf[30][2], c.vf[30][3]};
        setVf(30, v30);
        float v31[4];
        for (int i = 0; i < 4; ++i) v31[i] = vmul(c.vf[31][i], 0.0f);
        setVf(31, v31);
        wb.acc = true;
        {
            const v4f a16 = mul4(ld4(cm[3]), 1.0f);
            std::memcpy(wb.accv, &a16, 16);
        }
        wb.q = true;
        wb.qv = q;
        wb.clip = true;
        wb.clipv = clip;
        const int32_t vis[][2] = {{1, ck}, {2, static_cast<int32_t>(top + 0x98)}, {3, static_cast<int32_t>(base0 + 4 * n + 1)},
                                  {4, vi04}, {5, cm2}, {6, cm1}, {7, static_cast<int32_t>(top + 0x9a + 3 * n)}, {10, 0},
                                  {11, anyAdc ? -32768 : 3}, {12, static_cast<int32_t>(vi12 & 3u)}, {13, vi13}};
        for (const auto &v : vis)
        {
            wb.viMask |= 1u << v[0];
            wb.vi[v[0]] = v[1];
        }
        return true;
    }

    // Setup entries: the short programs that load matrices and constants before the drawing
    // entries run (straight-line code, transcribed op for op; no pipeline effects apply).
    namespace
    {
        struct VuRegs
        {
            float vf[32][4];
            float acc[4];
            int32_t vi[16];
            uint32_t vfW = 0, viW = 0;
            explicit VuRegs(const vu1fast::Ctx &c)
            {
                std::memcpy(vf, c.vf, sizeof(vf));
                std::memcpy(acc, c.acc, sizeof(acc));
                for (int k = 0; k < 16; ++k) vi[k] = c.vi[k];
            }
            // Lane masks: bit 3 = x .. bit 0 = w. The broadcast lane is read before the write.
            void mula(int a, int b, int l, int m) { const float bc = vf[b][l]; for (int i = 0; i < 4; ++i) if ((m >> (3 - i)) & 1) acc[i] = vmul(vf[a][i], bc); }
            void madda(int a, int b, int l, int m) { const float bc = vf[b][l]; for (int i = 0; i < 4; ++i) if ((m >> (3 - i)) & 1) acc[i] = vmadd(acc[i], vf[a][i], bc); }
            void madd(int d, int a, int b, int l, int m)
            {
                const float bc = vf[b][l];
                float r[4];
                for (int i = 0; i < 4; ++i) r[i] = vmadd(acc[i], vf[a][i], bc);
                for (int i = 0; i < 4; ++i) if ((m >> (3 - i)) & 1) vf[d][i] = r[i];
                vfW |= 1u << d;
            }
            void lq(int d, int s, int off, const uint8_t *mem) { std::memcpy(vf[d], mem + (((static_cast<uint32_t>(static_cast<uint16_t>(vi[s])) + off) & 0x3FFu) << 4), 16); vfW |= 1u << d; }
            void lqi(int d, int s, const uint8_t *mem) { lq(d, s, 0, mem); vi[s] = static_cast<int16_t>(vi[s] + 1); viW |= 1u << s; }
            void sq(int s, int off, uint8_t *mem) { std::memcpy(mem + ((off & 0x3FF) << 4), vf[s], 16); }
            void seti(int d, int v) { vi[d] = static_cast<int16_t>(v); viW |= 1u << d; }
        };

        // Overlay group 3841391935 L1500 (also run from 1332105535): vf01..04 = mem[vi02..] * the
        // rows at mem[vi01..], vf13..16 the same for the second matrix; vf01..04 to mem 164..167.
        void matrixL1500(VuRegs &r, const uint8_t *mem, uint8_t *memOut)
        {
            r.lqi(17, 1, mem);  // L1500
            r.lqi(9, 2, mem);  // L1508
            r.lqi(10, 2, mem);  // L1510
            r.lqi(11, 2, mem);  // L1518
            r.lqi(12, 2, mem);  // L1520
            r.mula(9, 17, 0, 0b1111); r.lqi(18, 1, mem);  // L1528
            r.madda(10, 17, 1, 0b1111); r.lqi(19, 1, mem);  // L1530
            r.madda(11, 17, 2, 0b1111); r.lqi(20, 1, mem);  // L1538
            r.madd(1, 12, 17, 3, 0b1111);  // L1540
            r.mula(9, 18, 0, 0b1111);  // L1548
            r.madda(10, 18, 1, 0b1111);  // L1550
            r.madda(11, 18, 2, 0b1111);  // L1558
            r.madd(2, 12, 18, 3, 0b1111); r.lqi(25, 2, mem);  // L1560
            r.mula(9, 19, 0, 0b1111); r.lqi(26, 2, mem);  // L1568
            r.madda(10, 19, 1, 0b1111); r.lqi(27, 2, mem);  // L1570
            r.madda(11, 19, 2, 0b1111); r.lqi(28, 2, mem);  // L1578
            r.madd(3, 12, 19, 3, 0b1111);  // L1580
            r.mula(9, 20, 0, 0b1111);  // L1588
            r.madda(10, 20, 1, 0b1111);  // L1590
            r.madda(11, 20, 2, 0b1111);  // L1598
            r.madd(4, 12, 20, 3, 0b1111);  // L15a0
            r.mula(25, 17, 0, 0b1111);  // L15a8
            r.madda(26, 17, 1, 0b1111);  // L15b0
            r.madda(27, 17, 2, 0b1111);  // L15b8
            r.madd(13, 28, 17, 3, 0b1111);  // L15c0
            r.mula(25, 18, 0, 0b1111);  // L15c8
            r.madda(26, 18, 1, 0b1111);  // L15d0
            r.madda(27, 18, 2, 0b1111);  // L15d8
            r.madd(14, 28, 18, 3, 0b1111);  // L15e0
            r.mula(25, 19, 0, 0b1111);  // L15e8
            r.madda(26, 19, 1, 0b1111);  // L15f0
            r.madda(27, 19, 2, 0b1111); r.sq(1, 164, memOut);  // L15f8
            r.madd(15, 28, 19, 3, 0b1111); r.sq(2, 165, memOut);  // L1600
            r.mula(25, 20, 0, 0b1111); r.sq(3, 166, memOut);  // L1608
            r.madda(26, 20, 1, 0b1111); r.sq(4, 167, memOut);  // L1610
            r.madda(27, 20, 2, 0b1111);  // L1618
            r.madd(16, 28, 20, 3, 0b1111);  // L1620
        }
        // L26c0 (overlay groups 1920671007 / 1332105535): as L1500 plus light rows vf05..07 =
        // vf21..23 * vf17..19 (from mem[vi01 + 4..6]) and vf08 = mem[vi01 + 7].
        void matrixL26c0(VuRegs &r, const uint8_t *mem, uint8_t *memOut)
        {
            r.lqi(17, 1, mem);  // L26c0
            r.lqi(9, 2, mem);  // L26c8
            r.lqi(10, 2, mem);  // L26d0
            r.lqi(11, 2, mem);  // L26d8
            r.lqi(12, 2, mem);  // L26e0
            r.mula(9, 17, 0, 0b1111); r.lqi(18, 1, mem);  // L26e8
            r.madda(10, 17, 1, 0b1111); r.lqi(19, 1, mem);  // L26f0
            r.madda(11, 17, 2, 0b1111); r.lqi(20, 1, mem);  // L26f8
            r.madd(1, 12, 17, 3, 0b1111); r.lqi(21, 1, mem);  // L2700
            r.mula(9, 18, 0, 0b1111); r.lqi(22, 1, mem);  // L2708
            r.madda(10, 18, 1, 0b1111); r.lqi(23, 1, mem);  // L2710
            r.madda(11, 18, 2, 0b1111); r.lqi(8, 1, mem);  // L2718
            r.madd(2, 12, 18, 3, 0b1111); r.lqi(25, 2, mem);  // L2720
            r.mula(9, 19, 0, 0b1111); r.lqi(26, 2, mem);  // L2728
            r.madda(10, 19, 1, 0b1111); r.lqi(27, 2, mem);  // L2730
            r.madda(11, 19, 2, 0b1111); r.lqi(28, 2, mem);  // L2738
            r.madd(3, 12, 19, 3, 0b1111);  // L2740
            r.mula(9, 20, 0, 0b1111);  // L2748
            r.madda(10, 20, 1, 0b1111);  // L2750
            r.madda(11, 20, 2, 0b1111);  // L2758
            r.madd(4, 12, 20, 3, 0b1111);  // L2760
            r.mula(21, 17, 0, 0b1111);  // L2768
            r.madda(22, 17, 1, 0b1111);  // L2770
            r.madd(5, 23, 17, 2, 0b1111);  // L2778
            r.mula(21, 18, 0, 0b1111);  // L2780
            r.madda(22, 18, 1, 0b1111);  // L2788
            r.madd(6, 23, 18, 2, 0b1111);  // L2790
            r.mula(21, 19, 0, 0b1111);  // L2798
            r.madda(22, 19, 1, 0b1111);  // L27a0
            r.madd(7, 23, 19, 2, 0b1111);  // L27a8
            r.mula(25, 17, 0, 0b1111);  // L27b0
            r.madda(26, 17, 1, 0b1111);  // L27b8
            r.madda(27, 17, 2, 0b1111);  // L27c0
            r.madd(13, 28, 17, 3, 0b1111);  // L27c8
            r.mula(25, 18, 0, 0b1111);  // L27d0
            r.madda(26, 18, 1, 0b1111);  // L27d8
            r.madda(27, 18, 2, 0b1111);  // L27e0
            r.madd(14, 28, 18, 3, 0b1111);  // L27e8
            r.mula(25, 19, 0, 0b1111);  // L27f0
            r.madda(26, 19, 1, 0b1111);  // L27f8
            r.madda(27, 19, 2, 0b1111); r.sq(1, 164, memOut);  // L2800
            r.madd(15, 28, 19, 3, 0b1111); r.sq(2, 165, memOut);  // L2808
            r.mula(25, 20, 0, 0b1111); r.sq(3, 166, memOut);  // L2810
            r.madda(26, 20, 1, 0b1111); r.sq(4, 167, memOut);  // L2818
            r.madda(27, 20, 2, 0b1111);  // L2820
            r.madd(16, 28, 20, 3, 0b1111);  // L2828
        }
        // Overlay group 1585565147 L0bb8: as L26c0 with the light rows transposed (xyz only).
        void matrixL0bb8(VuRegs &r, const uint8_t *mem, uint8_t *memOut)
        {
            r.lqi(17, 1, mem);  // L0bb8
            r.lqi(9, 2, mem);  // L0bc0
            r.lqi(10, 2, mem);  // L0bc8
            r.lqi(11, 2, mem);  // L0bd0
            r.lqi(12, 2, mem);  // L0bd8
            r.mula(9, 17, 0, 0b1111); r.lqi(18, 1, mem);  // L0be0
            r.madda(10, 17, 1, 0b1111); r.lqi(19, 1, mem);  // L0be8
            r.madda(11, 17, 2, 0b1111); r.lqi(20, 1, mem);  // L0bf0
            r.madd(1, 12, 17, 3, 0b1111); r.lqi(21, 1, mem);  // L0bf8
            r.mula(9, 18, 0, 0b1111); r.lqi(22, 1, mem);  // L0c00
            r.madda(10, 18, 1, 0b1111); r.lqi(23, 1, mem);  // L0c08
            r.madda(11, 18, 2, 0b1111); r.lqi(8, 1, mem);  // L0c10
            r.madd(2, 12, 18, 3, 0b1111); r.lqi(25, 2, mem);  // L0c18
            r.mula(9, 19, 0, 0b1111); r.lqi(26, 2, mem);  // L0c20
            r.madda(10, 19, 1, 0b1111); r.lqi(27, 2, mem);  // L0c28
            r.madda(11, 19, 2, 0b1111); r.lqi(28, 2, mem);  // L0c30
            r.madd(3, 12, 19, 3, 0b1111);  // L0c38
            r.mula(9, 20, 0, 0b1111);  // L0c40
            r.madda(10, 20, 1, 0b1111);  // L0c48
            r.madda(11, 20, 2, 0b1111);  // L0c50
            r.madd(4, 12, 20, 3, 0b1111);  // L0c58
            r.mula(17, 21, 0, 0b1110);  // L0c60
            r.madda(18, 21, 1, 0b1110);  // L0c68
            r.madd(5, 19, 21, 2, 0b1110);  // L0c70
            r.mula(17, 22, 0, 0b1110);  // L0c78
            r.madda(18, 22, 1, 0b1110);  // L0c80
            r.madd(6, 19, 22, 2, 0b1110);  // L0c88
            r.mula(17, 23, 0, 0b1110);  // L0c90
            r.madda(18, 23, 1, 0b1110);  // L0c98
            r.madd(7, 19, 23, 2, 0b1110);  // L0ca0
            r.mula(25, 17, 0, 0b1111);  // L0ca8
            r.madda(26, 17, 1, 0b1111);  // L0cb0
            r.madda(27, 17, 2, 0b1111);  // L0cb8
            r.madd(13, 28, 17, 3, 0b1111);  // L0cc0
            r.mula(25, 18, 0, 0b1111);  // L0cc8
            r.madda(26, 18, 1, 0b1111);  // L0cd0
            r.madda(27, 18, 2, 0b1111);  // L0cd8
            r.madd(14, 28, 18, 3, 0b1111);  // L0ce0
            r.mula(25, 19, 0, 0b1111);  // L0ce8
            r.madda(26, 19, 1, 0b1111);  // L0cf0
            r.madda(27, 19, 2, 0b1111); r.sq(1, 164, memOut);  // L0cf8
            r.madd(15, 28, 19, 3, 0b1111); r.sq(2, 165, memOut);  // L0d00
            r.mula(25, 20, 0, 0b1111); r.sq(3, 166, memOut);  // L0d08
            r.madda(26, 20, 1, 0b1111); r.sq(4, 167, memOut);  // L0d10
            r.madda(27, 20, 2, 0b1111);  // L0d18
            r.madd(16, 28, 20, 3, 0b1111);  // L0d20
        }
    }

    bool Setup(const vu1fast::Ctx &c, uint32_t id, uint32_t pc, const uint8_t *mem, WriteBack &wb, uint8_t *memOut)
    {
        if (!memOut)
            return false;
        FloatEnv env;
        VuRegs r(c);
        // bal vi15: the link register holds the address after the delay slot / 8.
        auto call = [&](void (*fn)(VuRegs &, const uint8_t *, uint8_t *), uint32_t after) { r.seti(15, static_cast<int>(after >> 3)); fn(r, mem, memOut); };
        switch (static_cast<uint64_t>(id) << 16 | pc)
        {
        case 3841391935ull << 16 | 0x1240:   // vi01 = 0; bal L1500 (vi02 = 0xc); [E] vi14 = 0x400
        case 1332105535ull << 16 | 0x2e20:   // vi01 = 0; bal L1500 (vi02 = 0xc); [E] vi12 = 0x7fff
            r.seti(1, 0); r.seti(2, 0xc);
            call(matrixL1500, pc + 0x18);
            if (id == 3841391935u) r.seti(14, 0x400); else r.seti(12, 0x7fff);
            break;
        case 1331843391ull << 16 | 0x0ef0:   // vi01 = 0; bal L26c0 (vi02 = 0xc); [E] vi14 = 0x400
            r.seti(1, 0); r.seti(2, 0xc);
            call(matrixL26c0, 0x0f08);
            r.seti(14, 0x400);
            break;
        case 1331843391ull << 16 | 0x0d28:   // vi01 = 0; vi02 = 0xc; bal L26c0 (vi10 = 0x7fff); [E]
            r.seti(1, 0); r.seti(2, 0xc); r.seti(10, 0x7fff);
            call(matrixL26c0, 0x0d48);
            break;
        case 1585565147ull << 16 | 0x0168:   // vi01 = 0; vi02 = 0xc; bal L0bb8 (vi10 = 0x7fff); [E]
            r.seti(1, 0); r.seti(2, 0xc); r.seti(10, 0x7fff);
            call(matrixL0bb8, 0x0188);
            break;
        case 1332105535ull << 16 | 0x2ff8:   // vi01 = 0xc; vf01..04, vf13..16 = mem[vi01 + 0..7]; [E] vi12 = 0x7fff
            r.seti(1, 0xc);
            for (int k = 0; k < 4; ++k) r.lq(1 + k, 1, k, mem);
            for (int k = 0; k < 4; ++k) r.lq(13 + k, 1, 4 + k, mem);
            r.seti(12, 0x7fff);
            break;
        case 939320750ull << 16 | 0x31b8:    // vf01..04, vf13..16 = mem 12..19; [E] vi12 = 7
            for (int k = 0; k < 4; ++k) r.lq(1 + k, 0, 12 + k, mem);
            for (int k = 0; k < 4; ++k) r.lq(13 + k, 0, 16 + k, mem);
            r.seti(12, 7);
            break;
        case 3841391935ull << 16 | 0x1258:   // [E] vi14 = 0x400
        case 1920671007ull << 16 | 0x2258:
        case 1008206959ull << 16 | 0x0368:
            r.seti(14, 0x400);
            break;
        case 1331843391ull << 16 | 0x0d40:   // vi10 = 0x7fff; [E]
        case 446200663ull << 16 | 0x0020:
            r.seti(10, 0x7fff);
            break;
        case 1920694623ull << 16 | 0x3190:   // [E] vi12 = 0x7fff
            r.seti(12, 0x7fff);
            break;
        case 1332105535ull << 16 | 0x2e48:   // [E] only
        case 1920671007ull << 16 | 0x1f90:
            break;
        default:
            return false;
        }
        for (int k = 0; k < 32; ++k)
            if ((r.vfW >> k) & 1)
                std::memcpy(wb.vf[k], r.vf[k], 16);
        wb.vfMask |= r.vfW;
        for (int k = 0; k < 16; ++k)
            if ((r.viW >> k) & 1)
                wb.vi[k] = r.vi[k];
        wb.viMask |= r.viW;
        if (std::memcmp(r.acc, c.acc, 16) != 0 || r.vfW)
        {
            wb.acc = true;
            std::memcpy(wb.accv, r.acc, 16);
        }
        return true;
    }
}

namespace vu1native
{
    // Overlay group 1821510323 (an in-race effect: up to a few sprites per batch with random jitter).
    // Entry 0x31b8 loads vf01..04 from mem 12..15. Entry 0x31f8: four calls of L3768 build a 4x4
    // basis from the rows at TOP+2.. (into TOP+0x1e..0x2d) and the sprite GIF tag copies at
    // TOP+0x32 / TOP+0x46; then the loops L32a0 (outer, vf29.x) and L3418 (inner, vf29.y) place one
    // sprite each, kicked from the two buffers in turn. Every Q read comes after its DIV has
    // finished and every FMAND reads the flags of the op four cycles before it, so the program is
    // straight sequential code. memOut receives the stores and is also read (the program reads what
    // it wrote).
    bool EffectSprites(const vu1fast::Ctx &c, uint32_t pc, std::vector<uint8_t> &pkt, std::vector<uint32_t> &sizes, WriteBack &wb, uint8_t *mem)
    {
        if (!mem)
            return false;
        FloatEnv env;
        VuRegs r(c);
        float I = c.i, Q = c.q;
        uint32_t R = c.r;
        float (*vf)[4] = r.vf;
        int32_t *vi = r.vi;
        auto mark = [&](int d) { r.vfW |= 1u << d; };
        auto seti = [&](int d, int v) { r.seti(d, v); };
        auto ld = [&](int d, uint32_t qa) { std::memcpy(vf[d], mem + ((qa & 0x3FFu) << 4), 16); mark(d); };
        auto st = [&](int s, uint32_t qa) { std::memcpy(mem + ((qa & 0x3FFu) << 4), vf[s], 16); };
        auto top = [&](int off) { return static_cast<uint32_t>(static_cast<uint16_t>(vi[12]) + off); };
        // vf[d] = vf[a] + b (scalar) / vf[a] op vf[b] per lane, lanes as a "xyzw" mask string.
        auto lanes = [](const char *m, int i) { return std::strchr(m, "xyzw"[i]) != nullptr; };
        auto addS = [&](int d, int a, float b, const char *m) { float t[4]; for (int i = 0; i < 4; ++i) t[i] = vadd(vf[a][i], b); for (int i = 0; i < 4; ++i) if (lanes(m, i)) vf[d][i] = t[i]; mark(d); };
        auto subS = [&](int d, int a, float b, const char *m) { float t[4]; for (int i = 0; i < 4; ++i) t[i] = vsub(vf[a][i], b); for (int i = 0; i < 4; ++i) if (lanes(m, i)) vf[d][i] = t[i]; mark(d); };
        auto mulS = [&](int d, int a, float b, const char *m) { float t[4]; for (int i = 0; i < 4; ++i) t[i] = vmul(vf[a][i], b); for (int i = 0; i < 4; ++i) if (lanes(m, i)) vf[d][i] = t[i]; mark(d); };
        auto addV = [&](int d, int a, int b, const char *m) { float t[4]; for (int i = 0; i < 4; ++i) t[i] = vadd(vf[a][i], vf[b][i]); for (int i = 0; i < 4; ++i) if (lanes(m, i)) vf[d][i] = t[i]; mark(d); };
        auto subV = [&](int d, int a, int b, const char *m) { float t[4]; for (int i = 0; i < 4; ++i) t[i] = vsub(vf[a][i], vf[b][i]); for (int i = 0; i < 4; ++i) if (lanes(m, i)) vf[d][i] = t[i]; mark(d); };
        auto mulV = [&](int d, int a, int b, const char *m) { float t[4]; for (int i = 0; i < 4; ++i) t[i] = vmul(vf[a][i], vf[b][i]); for (int i = 0; i < 4; ++i) if (lanes(m, i)) vf[d][i] = t[i]; mark(d); };
        auto maxS = [&](int d, int a, float b, const char *m) { for (int i = 0; i < 4; ++i) if (lanes(m, i)) vf[d][i] = vmax(vf[a][i], b); mark(d); };
        auto minS = [&](int d, int a, float b, const char *m) { for (int i = 0; i < 4; ++i) if (lanes(m, i)) vf[d][i] = vmin(vf[a][i], b); mark(d); };
        auto absV = [&](int d, int a, const char *m) { for (int i = 0; i < 4; ++i) if (lanes(m, i)) vf[d][i] = std::fabs(vn(vf[a][i])); mark(d); };
        auto mov = [&](int d, int a, const char *m) { for (int i = 0; i < 4; ++i) if (lanes(m, i)) vf[d][i] = vf[a][i]; mark(d); };
        auto mr32 = [&](int d, int a) { const float t[4] = {vf[a][1], vf[a][2], vf[a][3], vf[a][0]}; std::memcpy(vf[d], t, 16); mark(d); };
        auto ftoi = [&](int d, int a, bool four) { for (int i = 0; i < 4; ++i) { const uint32_t v = four ? ftoi4(vf[a][i]) : ftoi0(vf[a][i]); std::memcpy(&vf[d][i], &v, 4); } mark(d); };
        // ACC = a*b0 + ... (MULA / MADDA chain) and vf[d] = ACC + a*b (MADD).
        auto mat = [&](int d, const int rows[4], const float b[4]) {
            float acc[4], t[4];
            for (int i = 0; i < 4; ++i) acc[i] = vmul(vf[rows[0]][i], b[0]);
            for (int i = 0; i < 4; ++i) acc[i] = vmadd(acc[i], vf[rows[1]][i], b[1]);
            for (int i = 0; i < 4; ++i) acc[i] = vmadd(acc[i], vf[rows[2]][i], b[2]);
            std::memcpy(r.acc, acc, 16);
            for (int i = 0; i < 4; ++i) t[i] = vmadd(acc[i], vf[rows[3]][i], b[3]);
            std::memcpy(vf[d], t, 16);
            mark(d);
        };
        auto rnext = [&](int d, int lane) {
            R = ((R << 1) ^ ((R >> 4) & 1u) ^ ((R >> 22) & 1u)) & 0x007FFFFFu;
            R |= 0x3F800000u;
            std::memcpy(&vf[d][lane], &R, 4);
            mark(d);
        };
        auto kick = [&](uint32_t qa) {
            uint32_t q = 0;
            for (;;)
            {
                const Qw tag = ldq(mem, qa + q);
                const uint64_t lo = static_cast<uint64_t>(tag.u(0)) | static_cast<uint64_t>(tag.u(1)) << 32;
                const uint32_t nloop = lo & 0x7FFF, flg = (lo >> 58) & 3, nreg = ((lo >> 60) & 15) ? (lo >> 60) & 15 : 16;
                q += 1 + (flg == 0 ? nloop * nreg : flg == 1 ? (nloop * nreg + 1) / 2 : nloop);
                if (q > 0x40u)
                    return false;
                if ((lo >> 15) & 1)
                    break;
            }
            const size_t at = pkt.size();
            pkt.resize(at + 16 * q);
            for (uint32_t k = 0; k < q; ++k)
                std::memcpy(pkt.data() + at + 16 * k, mem + (((qa + k) & 0x3FFu) << 4), 16);
            sizes.push_back(16 * q);
            return true;
        };
        pkt.clear();
        sizes.clear();

        if (pc == 0x31b8)
        {
            for (int k = 0; k < 4; ++k) ld(1 + k, 12 + k);
        }
        else if (pc == 0x31f8)
        {
            seti(12, c.top & 0x3FF);
            subV(8, 0, 0, "xyzw"); mov(5, 0, "xyzw");
            subV(18, 0, 0, "xyzw"); I = 63.0f;
            addS(17, 0, I, "xyzw");
            subV(7, 0, 0, "xyzw"); seti(6, vi[12] + 2);
            subV(6, 0, 0, "xyzw"); I = -1.0f;
            addS(5, 0, I, "x"); seti(2, vi[12] + 0x1e);
            subS(8, 0, I, "x"); I = 3.0f;
            addS(5, 0, I, "y"); r.lqi(9, 6, mem);
            subS(5, 0, I, "z"); r.lqi(10, 6, mem);
            subS(7, 0, I, "x"); r.lqi(11, 6, mem);
            addS(7, 0, I, "y"); r.lqi(12, 6, mem);
            addS(6, 0, I, "x");
            addS(6, 0, I, "z"); I = -6.0f;
            addS(6, 0, I, "y");
            // L3768: basis rows, sprite tag copies, UV pattern; called four times.
            auto sub = [&]() {
                const int rows[4] = {9, 10, 11, 12};
                seti(9, vi[12] + 0x14);
                mat(25, rows, vf[5]);
                ld(17, static_cast<uint16_t>(vi[9]));
                seti(9, vi[12] + 0x32);
                mov(19, 0, "xyzw");
                seti(1, vi[12] + 0x46);
                st(17, static_cast<uint16_t>(vi[1]));
                st(17, static_cast<uint16_t>(vi[9]));
                mat(26, rows, vf[6]);
                st(19, static_cast<uint16_t>(vi[9]) + 2);
                st(19, static_cast<uint16_t>(vi[1]) + 2);
                vf[19][0] = vf[18][0];
                seti(3, vi[12] + 0x13);
                mat(27, rows, vf[7]);
                R = 0x3F800000u | (fbits(vf[5][1]) & 0x007FFFFFu);
                st(19, static_cast<uint16_t>(vi[9]) + 4);
                st(19, static_cast<uint16_t>(vi[1]) + 4);
                mat(28, rows, vf[8]);
                vf[19][0] = vf[0][0];
                vf[19][1] = vf[18][1];
                const int basis[4] = {5, 6, 7, 8};
                const float b25[4] = {vf[25][0], vf[25][1], vf[25][2], vf[25][3]};
                float acc[4];
                for (int i = 0; i < 4; ++i) acc[i] = vmul(vf[5][i], b25[0]);
                for (int i = 0; i < 4; ++i) acc[i] = vmadd(acc[i], vf[6][i], b25[1]);
                for (int i = 0; i < 4; ++i) acc[i] = vmadd(acc[i], vf[7][i], b25[2]);
                r.lqi(9, 6, mem);
                for (int i = 0; i < 4; ++i) vf[21][i] = vmadd(acc[i], vf[8][i], b25[3]);
                mark(21);
                r.lqi(10, 6, mem);
                r.lqi(11, 6, mem);
                r.lqi(12, 6, mem);
                std::memcpy(r.acc, acc, 16);
                mat(22, basis, vf[26]);
                st(19, static_cast<uint16_t>(vi[9]) + 6);
                st(21, static_cast<uint16_t>(vi[2])); seti(2, vi[2] + 1);
                st(19, static_cast<uint16_t>(vi[1]) + 6);
                vf[19][0] = vf[18][0];
                mat(23, basis, vf[27]);
                st(22, static_cast<uint16_t>(vi[2])); seti(2, vi[2] + 1);
                st(19, static_cast<uint16_t>(vi[9]) + 8);
                seti(9, vi[12] + 0x32);
                mat(24, basis, vf[28]);
                st(23, static_cast<uint16_t>(vi[2])); seti(2, vi[2] + 1);
                subV(29, 29, 0, "xyzw");
                st(19, static_cast<uint16_t>(vi[1]) + 8);
                st(24, static_cast<uint16_t>(vi[2])); seti(2, vi[2] + 1);
            };
            // Delay slot of the first call: vf18.xy = FTOI4(vf17), vf31 = mem[TOP + 18].
            {
                for (int i = 0; i < 2; ++i) { const uint32_t v = ftoi4(vf[17][i]); std::memcpy(&vf[18][i], &v, 4); }
                mark(18);
                ld(31, top(18));
            }
            seti(15, 0x3270 >> 3); sub();
            mov(30, 0, "xyzw"); seti(15, 0x3280 >> 3); sub();
            mov(30, 0, "xyzw"); seti(15, 0x3290 >> 3); sub();
            subV(29, 0, 0, "xyzw");
            seti(4, vi[12] + 0x1e); seti(15, 0x32a0 >> 3); sub();

            const int setRows[2][4] = {{17, 18, 19, 20}, {21, 22, 23, 24}};
            for (;;)   // L32a0
            {
                subV(9, 0, 0, "xyzw");
                subV(5, 0, 0, "xyzw");
                subV(25, 0, 0, "xyzw");
                // Four passes: vf09 = rows * vf30, its lanes into lane p of vf05..08.
                for (int p = 0; p < 4; ++p)
                {
                    const int *rows = setRows[p & 1];
                    for (int k = 0; k < 4; ++k) r.lqi(rows[k], 4, mem);
                    mat(9, rows, vf[30]);
                    const char lane[2] = {"xyzw"[p], 0};
                    addS(5, 25, vf[9][0], lane);
                    addS(6, 25, vf[9][1], lane);
                    addS(7, 25, vf[9][2], lane);
                    addS(8, 25, vf[9][3], lane);
                    if (p == 3) { vf[19][0] = vf[29][0]; mark(19); }
                }
                {
                    const float x29 = vf[29][0];
                    for (int i = 0; i < 3; ++i) vf[30][i] = vadd(vf[0][i], x29);
                    mark(30);
                }
                mov(28, 0, "xyzw");
                addS(29, 29, vf[31][1], "x"); I = 0.5f;
                subS(19, 19, I, "x");
                mulV(30, 30, 30, "y");
                addS(21, 0, vf[0][3], "x");
                absV(19, 19, "x");
                vf[20][1] = vf[29][1]; mark(20);
                mulS(30, 30, vf[30][1], "x");
                mov(18, 0, "xyzw");
                subV(19, 21, 19, "x");
                for (;;)   // L3418
                {
                    addS(20, 0, vf[20][1], "xyzw"); I = 0.5f;
                    addS(28, 0, vf[29][1], "xyz"); ld(23, static_cast<uint16_t>(vi[3]));
                    subS(20, 20, I, "xyzw"); vf[18][3] = ldq(mem, top(0)).f[3];
                    addS(21, 0, vf[0][3], "xyzw"); rnext(17, 0);
                    rnext(17, 1);
                    mulV(28, 28, 28, "y"); rnext(17, 2);
                    absV(20, 20, "xyzw"); mr32(18, 18);
                    mulS(28, 28, vf[28][1], "x"); I = 100.0f;
                    addV(29, 29, 31, "y"); vf[18][3] = ldq(mem, top(1)).f[3];
                    subV(20, 21, 20, "xyzw");
                    mulS(17, 17, I, "xyz");
                    {
                        const int basis[4] = {5, 6, 7, 8};
                        const float b28[4] = {vf[28][0], vf[28][1], vf[28][2], vf[28][3]};
                        I = 150.0f;
                        mat(27, basis, b28);
                    }
                    mr32(18, 18);
                    subS(17, 17, I, "xyzw");
                    mulV(20, 20, 19, "x");
                    mr32(18, 18);
                    addV(27, 27, 17, "xyz"); seti(8, vi[9]);
                    {
                        const int m[4] = {1, 2, 3, 4};
                        const float b[4] = {vf[27][0], vf[27][1], vf[27][2], vf[0][3]};
                        mat(26, m, b);
                    }
                    maxS(27, 27, vf[0][2], "w"); mr32(20, 20);
                    maxS(27, 27, vf[18][0], "y");
                    I = 255.0f;
                    Q = vdiv(vf[0][3], vf[26][3]);
                    minS(27, 27, I, "w");
                    minS(27, 27, vf[18][1], "y"); I = 200000.0f;
                    addS(25, 0, I, "w");
                    vf[23][3] = vf[27][3]; mark(23);
                    mulS(26, 26, Q, "xyz"); vf[27][3] = vf[25][3];
                    addS(17, 0, vf[26][2], "xyz");
                    mulV(23, 20, 23, "w"); mov(25, 26, "xyzw");
                    subS(27, 27, vf[18][0], "y"); mov(24, 26, "xyzw");
                    const float z17 = vsub(vf[17][2], vf[0][3]);
                    vf[17][2] = z17; mov(22, 26, "xyzw");
                    mulS(17, 17, vf[31][2], "x"); seti(10, 0x20);
                    mulS(17, 17, vf[31][3], "y");
                    seti(11, std::signbit(z17) ? 0x20 : 0);
                    subV(26, 26, 17, "x"); I = 1.000100016593933f;
                    subV(26, 26, 17, "y"); Q = vdiv(vf[27][1], vf[31][0]);
                    const bool skip = vi[11] == vi[10];
                    subV(25, 25, 17, "x");
                    addV(25, 25, 17, "y");
                    if (!skip)
                    {
                        addV(24, 24, 17, "x"); I = 4095.0f;
                        subV(24, 24, 17, "y");
                        addV(22, 22, 17, "x");
                        addV(22, 22, 17, "y");
                        mulS(23, 23, Q, "xyz");
                        maxS(26, 26, vf[0][2], "x"); Q = vdiv(vf[27][3], vf[26][2]);
                        minS(26, 26, I, "y");
                        maxS(25, 25, vf[0][2], "x");
                        minS(25, 25, I, "y");
                        maxS(24, 24, vf[0][2], "x");
                        minS(24, 24, I, "y");
                        maxS(22, 22, vf[0][2], "x");
                        minS(22, 22, I, "y");
                        addS(27, 0, Q, "x");
                        minS(26, 26, I, "x");
                        maxS(26, 26, vf[0][2], "y");
                        minS(25, 25, I, "x");
                        minS(27, 27, vf[0][3], "x");
                        maxS(25, 25, vf[0][2], "y");
                        minS(24, 24, I, "x");
                        maxS(24, 24, vf[0][2], "y");
                        mulS(23, 23, vf[27][0], "w");
                        minS(22, 22, I, "x");
                        maxS(22, 22, vf[0][2], "y"); I = 40.0f;
                        maxS(23, 23, I, "xyz");
                        ftoi(26, 26, true);
                        ftoi(25, 25, true);
                        ftoi(24, 24, true);
                        ftoi(22, 22, true);
                        ftoi(23, 23, false); st(26, static_cast<uint16_t>(vi[8]) + 3);
                        st(25, static_cast<uint16_t>(vi[8]) + 5);
                        st(24, static_cast<uint16_t>(vi[8]) + 7);
                        st(22, static_cast<uint16_t>(vi[8]) + 9);
                        st(23, static_cast<uint16_t>(vi[8]) + 1);
                        if (!kick(static_cast<uint16_t>(vi[9])))
                            return false;
                        seti(8, vi[9] - 0x32);
                        seti(9, vi[12] + 0x46);
                        const bool second = vi[8] == vi[12];
                        I = 1.000100016593933f;
                        if (!second)
                            seti(9, vi[12] + 0x32);
                    }
                    // L36d8
                    const float ay = vsub(vf[29][1], I);
                    r.acc[1] = ay;
                    seti(10, 0x40);
                    vf[20][1] = vf[29][1]; mark(20);
                    seti(11, std::signbit(ay) ? 0x40 : 0);
                    if (vi[11] == vi[10])
                        continue;
                    break;
                }
                const float ax = vsub(vf[29][0], I);
                r.acc[0] = ax;
                seti(10, 0x80);
                seti(4, vi[12] + 0x1e);
                seti(11, std::signbit(ax) ? 0x80 : 0);
                vf[29][1] = vf[0][1];
                if (vi[11] != vi[10])
                    break;
            }
        }
        else
            return false;

        for (int k = 0; k < 32; ++k)
            if ((r.vfW >> k) & 1)
                std::memcpy(wb.vf[k], vf[k], 16);
        wb.vfMask |= r.vfW;
        for (int k = 0; k < 16; ++k)
            if ((r.viW >> k) & 1)
                wb.vi[k] = vi[k];
        wb.viMask |= r.viW;
        wb.acc = true;
        std::memcpy(wb.accv, r.acc, 16);
        wb.q = true; wb.qv = Q;
        wb.i = true; wb.iv = I;
        wb.r = true; wb.rv = R; wb.clipv = c.clip;
        return true;
    }
}
