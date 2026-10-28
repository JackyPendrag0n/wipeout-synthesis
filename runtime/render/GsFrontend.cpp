#include "GsFrontend.h"

#include <cmath>
#include <cstring>

namespace gsgpu
{
    namespace
    {
        inline uint32_t bits(uint64_t v, int lo, int n) { return static_cast<uint32_t>((v >> lo) & ((1ull << n) - 1)); }
        inline float f32(uint32_t b) { float f; std::memcpy(&f, &b, 4); return f; }
    }

    void Frontend::packet(const uint8_t *data, uint32_t size)
    {
        uint32_t pos = 0;
        while (pos + 16 <= size)
        {
            uint64_t tlo, thi;
            std::memcpy(&tlo, data + pos, 8);
            std::memcpy(&thi, data + pos + 8, 8);
            pos += 16;
            const uint32_t nloop = bits(tlo, 0, 15);
            const bool pre = bits(tlo, 46, 1);
            const uint32_t primv = bits(tlo, 47, 11);
            const uint32_t flg = bits(tlo, 58, 2);
            uint32_t nreg = bits(tlo, 60, 4);
            if (!nreg)
                nreg = 16;
            if (pre && flg == 0)
                writeReg(0x00, primv);
            if (flg == 0)
            {
                for (uint32_t l = 0; l < nloop; ++l)
                    for (uint32_t r = 0; r < nreg; ++r)
                    {
                        if (pos + 16 > size)
                            return;
                        uint64_t lo, hi;
                        std::memcpy(&lo, data + pos, 8);
                        std::memcpy(&hi, data + pos + 8, 8);
                        pos += 16;
                        packedReg(bits(thi, r * 4, 4), lo, hi);
                    }
            }
            else if (flg == 1)
            {
                const uint32_t total = nloop * nreg;
                for (uint32_t k = 0; k < total; ++k)
                {
                    if (pos + 8 > size)
                        return;
                    uint64_t v;
                    std::memcpy(&v, data + pos, 8);
                    pos += 8;
                    const uint32_t rd = bits(thi, (k % nreg) * 4, 4);
                    if (rd != 0xE && rd != 0xF)
                        writeReg(rd, v);
                }
                if (total & 1)
                    pos += 8;
            }
            else
            {
                const uint32_t n = std::min<uint32_t>(nloop * 16, size - pos);
                if (m_imageActive)
                    m_rec.transferData(data + pos, n);
                pos += nloop * 16;
            }
        }
    }

    void Frontend::packedReg(uint32_t rd, uint64_t lo, uint64_t hi)
    {
        switch (rd)
        {
        case 0x0: writeReg(0x00, lo & 0x7FF); break;
        case 0x1:
            m_r = static_cast<uint8_t>(lo); m_g = static_cast<uint8_t>(lo >> 32);
            m_b = static_cast<uint8_t>(hi); m_a = static_cast<uint8_t>(hi >> 32);
            break;
        case 0x2:
            m_s = f32(static_cast<uint32_t>(lo)); m_t = f32(static_cast<uint32_t>(lo >> 32));
            m_q = f32(static_cast<uint32_t>(hi));
            break;
        case 0x3: m_u = static_cast<uint16_t>(lo & 0x3FFF); m_v = static_cast<uint16_t>((lo >> 32) & 0x3FFF); break;
        case 0x4:   // XYZF2
        case 0xC:   // XYZF3
        {
            const uint64_t xyz = (lo & 0xFFFF) | (((lo >> 32) & 0xFFFF) << 16) | (static_cast<uint64_t>((hi >> 4) & 0xFFFFFF) << 32);
            const bool adc = (hi >> 47) & 1;
            vertex(xyz, true, bits(hi, 36, 8), rd == 0x4 && !adc, false);
            break;
        }
        case 0x5:   // XYZ2
        case 0xD:   // XYZ3
        {
            const uint64_t xyz = (lo & 0xFFFF) | (((lo >> 32) & 0xFFFF) << 16) | (static_cast<uint64_t>(hi & 0xFFFFFFFF) << 32);
            const bool adc = (hi >> 47) & 1;
            vertex(xyz, false, 0, rd == 0x5 && !adc, true);
            break;
        }
        case 0xA: m_fog = static_cast<uint8_t>(bits(hi, 36, 8)); break;
        case 0xE: writeReg(static_cast<uint32_t>(hi & 0xFF), lo); break;
        case 0xF: break;
        default: writeReg(rd, lo); break;   // 6..9: TEX0_1/2, CLAMP_1/2
        }
    }

    void Frontend::writeReg(uint32_t reg, uint64_t v)
    {
        switch (reg)
        {
        case 0x00: m_prim = v; m_count = 0; break;
        case 0x01:
            m_r = static_cast<uint8_t>(v); m_g = static_cast<uint8_t>(v >> 8); m_b = static_cast<uint8_t>(v >> 16);
            m_a = static_cast<uint8_t>(v >> 24); m_q = f32(static_cast<uint32_t>(v >> 32));
            break;
        case 0x02: m_s = f32(static_cast<uint32_t>(v)); m_t = f32(static_cast<uint32_t>(v >> 32)); break;
        case 0x03: m_u = static_cast<uint16_t>(v & 0x3FFF); m_v = static_cast<uint16_t>((v >> 16) & 0x3FFF); break;
        case 0x04: vertex((v & 0xFFFFFFFFull) | ((v >> 32 & 0xFFFFFF) << 32), true, bits(v, 56, 8), true, false); break;
        case 0x05: vertex(v, false, 0, true, true); break;
        case 0x0C: vertex((v & 0xFFFFFFFFull) | ((v >> 32 & 0xFFFFFF) << 32), true, bits(v, 56, 8), false, false); break;
        case 0x0D: vertex(v, false, 0, false, true); break;
        case 0x0A: m_fog = static_cast<uint8_t>(v >> 56); break;
        case 0x06: case 0x07:
        {
            Ctx &c = m_ctx[reg - 0x06];
            c.tex0 = v;
            Tex0Event e{};
            e.cbp = static_cast<uint16_t>(bits(v, 37, 14)); e.psm = static_cast<uint8_t>(bits(v, 20, 6));
            e.cpsm = static_cast<uint8_t>(bits(v, 51, 4)); e.csm = static_cast<uint8_t>(bits(v, 55, 1));
            e.csa = static_cast<uint8_t>(bits(v, 56, 5)); e.cld = static_cast<uint8_t>(bits(v, 61, 3));
            e.cbw = static_cast<uint8_t>(bits(m_texclut, 0, 6)); e.cou = static_cast<uint8_t>(bits(m_texclut, 6, 6));
            e.cov = static_cast<uint16_t>(bits(m_texclut, 12, 10));
            m_rec.tex0(e);
            break;
        }
        case 0x16: case 0x17:   // TEX2: psm and clut fields only
        {
            Ctx &c = m_ctx[reg - 0x16];
            const uint64_t mask = (0x3Full << 20) | (0x7FFFFFFull << 37);
            c.tex0 = (c.tex0 & ~mask) | (v & mask);
            writeReg(0x06 + (reg - 0x16), c.tex0);
            break;
        }
        case 0x08: case 0x09: m_ctx[reg - 0x08].clamp = v; break;
        case 0x14: case 0x15: m_ctx[reg - 0x14].tex1 = v; break;
        case 0x18: case 0x19: m_ctx[reg - 0x18].xyoffset = v; break;
        case 0x1A: m_prmodecont = v; break;
        case 0x1B: m_prmode = v; break;
        case 0x1C: m_texclut = v; break;
        case 0x34: case 0x35: m_ctx[reg - 0x34].miptbp1 = v; break;
        case 0x36: case 0x37: m_ctx[reg - 0x36].miptbp2 = v; break;
        case 0x3B: m_texa = v; break;
        case 0x3D: m_fogcol = v; break;
        case 0x40: case 0x41: m_ctx[reg - 0x40].scissor = v; break;
        case 0x42: case 0x43: m_ctx[reg - 0x42].alpha = v; break;
        case 0x45: m_dthe = v; break;
        case 0x7E: m_notHud = v; break;   // not a GS register: the port marks draws that are not HUD
        case 0x46: m_colclamp = v; break;
        case 0x47: case 0x48: m_ctx[reg - 0x47].test = v; break;
        case 0x49: m_pabe = v; break;
        case 0x4A: case 0x4B: m_ctx[reg - 0x4A].fba = v; break;
        case 0x4C: case 0x4D: m_ctx[reg - 0x4C].frame = v; break;
        case 0x4E: case 0x4F: m_ctx[reg - 0x4E].zbuf = v; break;
        case 0x50: m_bitbltbuf = v; break;
        case 0x51: m_trxpos = v; break;
        case 0x52: m_trxreg = v; break;
        case 0x53:
        {
            const uint32_t dir = bits(v, 0, 2);
            m_imageActive = dir == 0;
            if (dir == 0)
                m_rec.transferBegin(bits(m_bitbltbuf, 32, 14), bits(m_bitbltbuf, 48, 6), bits(m_bitbltbuf, 56, 6),
                                    bits(m_trxpos, 32, 11), bits(m_trxpos, 48, 11), bits(m_trxreg, 0, 12), bits(m_trxreg, 32, 12));
            break;
        }
        default: break;
        }
    }

    State Frontend::makeState() const
    {
        const uint64_t p = (m_prmodecont & 1) ? m_prim : ((m_prim & 7) | (m_prmode & 0x7F8));
        const Ctx &c = m_ctx[bits(p, 9, 1)];
        State s{};
        std::memset(&s, 0, sizeof(s));
        s.type = static_cast<uint8_t>(bits(p, 0, 3)); s.iip = bits(p, 3, 1); s.tme = bits(p, 4, 1); s.fge = bits(p, 5, 1);
        s.abe = bits(p, 6, 1); s.aa1 = bits(p, 7, 1); s.fst = bits(p, 8, 1);
        s.fbp = static_cast<uint16_t>(bits(c.frame, 0, 9)); s.fbw = static_cast<uint8_t>(bits(c.frame, 16, 6));
        s.fpsm = static_cast<uint8_t>(bits(c.frame, 24, 6)); s.fbmsk = bits(c.frame, 32, 32);
        s.zbp = static_cast<uint16_t>(bits(c.zbuf, 0, 9)); s.zpsm = static_cast<uint8_t>(bits(c.zbuf, 24, 4) | 0x30);
        s.zmsk = bits(c.zbuf, 32, 1);
        if (s.tme)
        {
            s.tbp0 = static_cast<uint16_t>(bits(c.tex0, 0, 14)); s.tbw = static_cast<uint8_t>(bits(c.tex0, 14, 6));
            s.tpsm = static_cast<uint8_t>(bits(c.tex0, 20, 6)); s.tw = static_cast<uint8_t>(bits(c.tex0, 26, 4));
            s.th = static_cast<uint8_t>(bits(c.tex0, 30, 4)); s.tcc = bits(c.tex0, 34, 1); s.tfx = static_cast<uint8_t>(bits(c.tex0, 35, 2));
            s.cbp = static_cast<uint16_t>(bits(c.tex0, 37, 14)); s.cpsm = static_cast<uint8_t>(bits(c.tex0, 51, 4));
            s.csm = bits(c.tex0, 55, 1); s.csa = static_cast<uint8_t>(bits(c.tex0, 56, 5));
            s.tex1 = c.tex1; s.miptbp1 = c.miptbp1; s.miptbp2 = c.miptbp2; s.clamp = c.clamp;
            s.ta0 = static_cast<uint8_t>(bits(m_texa, 0, 8)); s.aem = bits(m_texa, 15, 1); s.ta1 = static_cast<uint8_t>(bits(m_texa, 32, 8));
        }
        s.alpha = s.abe ? c.alpha : 0;
        s.test = c.test;
        s.fba = c.fba;
        s.sx0 = static_cast<uint16_t>(bits(c.scissor, 0, 11)); s.sx1 = static_cast<uint16_t>(bits(c.scissor, 16, 11));
        s.sy0 = static_cast<uint16_t>(bits(c.scissor, 32, 11)); s.sy1 = static_cast<uint16_t>(bits(c.scissor, 48, 11));
        if (s.fge)
        {
            s.fogr = static_cast<uint8_t>(m_fogcol); s.fogg = static_cast<uint8_t>(m_fogcol >> 8); s.fogb = static_cast<uint8_t>(m_fogcol >> 16);
        }
        s.colclamp = m_colclamp & 1;
        s.pabe = m_pabe & 1;
        s.dthe = m_dthe & 1;
        s.notHud = m_notHud & 1;
        s.notHudX = s.notHud ? static_cast<uint16_t>(m_notHud >> 16) : 0;
        return s;
    }

    Vertex Frontend::convert(const V &v, const V &col) const
    {
        const uint64_t p = (m_prmodecont & 1) ? m_prim : ((m_prim & 7) | (m_prmode & 0x7F8));
        const Ctx &c = m_ctx[bits(p, 9, 1)];
        Vertex o;
        o.x = v.x - bits(c.xyoffset, 0, 16) / 16.0f;
        o.y = v.y - bits(c.xyoffset, 32, 16) / 16.0f;
        switch (bits(c.zbuf, 24, 4))
        {
        case 0: o.z = static_cast<float>(v.z / 4294967296.0); break;
        case 1: o.z = static_cast<float>((v.z & 0xFFFFFFu) / 16777216.0); break;
        default: o.z = static_cast<float>((v.z & 0xFFFFu) / 65536.0); break;
        }
        o.r = col.r; o.g = col.g; o.b = col.b; o.a = col.a;
        o.s = v.s; o.t = v.t; o.q = v.q;
        o.u = v.u / 16.0f; o.v = v.v / 16.0f;
        o.fog = v.f;
        return o;
    }

    void Frontend::vertex(uint64_t xyz, bool hasFog, uint32_t fog, bool doKick, bool z32)
    {
        V v;
        v.x = static_cast<float>(xyz & 0xFFFF) / 16.0f;
        v.y = static_cast<float>((xyz >> 16) & 0xFFFF) / 16.0f;
        v.z = z32 ? static_cast<uint32_t>(xyz >> 32) : static_cast<uint32_t>((xyz >> 32) & 0xFFFFFF);
        v.r = m_r; v.g = m_g; v.b = m_b; v.a = m_a; v.q = m_q; v.s = m_s; v.t = m_t; v.u = m_u; v.v = m_v;
        v.f = hasFog ? static_cast<uint8_t>(fog) : m_fog;
        if (hasFog)
            m_fog = static_cast<uint8_t>(fog);
        const uint32_t type = bits(m_prim, 0, 3);
        const int need = type == 0 ? 1 : (type <= 2 || type == 6) ? 2 : 3;
        if (m_count < 3)
            m_queue[m_count++] = v;
        else
        {
            m_queue[0] = m_queue[1];
            m_queue[1] = m_queue[2];
            m_queue[2] = v;
        }
        if (m_count >= need && doKick)
            kick();
        if (m_count >= need)
        {
            switch (type)
            {
            case 2: m_queue[0] = m_queue[1]; m_count = 1; break;          // line strip
            case 4: m_queue[0] = m_queue[1]; m_queue[1] = m_queue[2]; m_count = 2; break;
            case 5: m_queue[1] = m_queue[2]; m_count = 2; break;          // fan keeps vertex 0
            default: m_count = 0; break;
            }
        }
    }

    void Frontend::kick()
    {
        ++m_prims;
        const State s = makeState();
        const uint32_t type = s.type;
        if (type >= 3 && type <= 5)
        {
            const bool flat = !s.iip;
            Vertex v[3];
            for (int k = 0; k < 3; ++k)
                v[k] = convert(m_queue[k], flat ? m_queue[2] : m_queue[k]);
            m_rec.triangle(s, v);
        }
        else if (type == 6)
            m_rec.sprite(s, convert(m_queue[0], m_queue[1]), convert(m_queue[1], m_queue[1]));
    }
}
