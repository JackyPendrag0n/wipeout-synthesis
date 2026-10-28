// GS front end: parses GIF packets (PACKED / REGLIST / IMAGE), keeps the GS register file and
// assembles primitives, feeding a gsgpu::Recorder. Used by tools/gsreplay (capture replay) and
// usable as a standalone GS front end for the port.
#pragma once

#include "GsGpu.h"

#include <cstdint>

namespace gsgpu
{
    class Frontend
    {
    public:
        explicit Frontend(Recorder &rec) : m_rec(rec) {}
        void packet(const uint8_t *data, uint32_t size);
        void writeReg(uint32_t reg, uint64_t value);
        uint64_t primitives() const { return m_prims; }

    private:
        struct Ctx
        {
            uint64_t tex0 = 0, tex1 = 0, clamp = 0, xyoffset = 0, miptbp1 = 0, miptbp2 = 0,
                     scissor = 0, alpha = 0, test = 0, fba = 0, frame = 0, zbuf = 0;
        };
        struct V
        {
            float x, y; uint32_t z; uint8_t r, g, b, a; float q, s, t; uint16_t u, v; uint8_t f;
        };
        void packedReg(uint32_t rd, uint64_t lo, uint64_t hi);
        void vertex(uint64_t xyz, bool hasFog, uint32_t fog, bool kick, bool z32);
        void kick();
        State makeState() const;
        Vertex convert(const V &v, const V &colorFrom) const;

        Recorder &m_rec;
        Ctx m_ctx[2];
        uint64_t m_prim = 0, m_prmode = 0, m_prmodecont = 1, m_texa = 0, m_fogcol = 0, m_colclamp = 0,
                 m_pabe = 0, m_dthe = 0, m_texclut = 0, m_notHud = 0;
        uint64_t m_bitbltbuf = 0, m_trxpos = 0, m_trxreg = 0;
        uint8_t m_r = 0x80, m_g = 0x80, m_b = 0x80, m_a = 0x80;
        float m_q = 1.0f, m_s = 0, m_t = 0;
        uint16_t m_u = 0, m_v = 0;
        uint8_t m_fog = 0;
        V m_queue[3];
        int m_count = 0;
        bool m_imageActive = false;
        uint64_t m_prims = 0;
    };
}
