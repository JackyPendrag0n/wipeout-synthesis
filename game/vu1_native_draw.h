// Native replacements for WipEout Fusion's VU1 drawing programs (native renderer, milestones 2-3).
// Each function computes, from the VU1 state and micro memory at program entry, the exact GIF
// packet the VU program would XGKICK (tag + STQ / RGBA / XYZ(F)2 per vertex), using the VU's
// float semantics (vu1_fast.h helpers; built with -ffp-contract=off as a vu1_* source, and run
// in the VU float environment: round toward zero, denormals flushed). It
// returns false when the batch needs something it does not model (the VU's clipper, odd tags);
// the caller then runs the program on the VU as before.
#pragma once

#include "vu1_fast.h"

#include <cstdint>
#include <vector>

namespace vu1native
{
    // Registers the VU program would leave changed that later batches read before writing.
    struct WriteBack
    {
        bool vf26 = false;
        float vf26v[4] = {};
        bool r = false, clip = false, i = false;
        uint32_t rv = 0, clipv = 0;
        float iv = 0.0f;
        // Whole registers the program leaves changed (bit k = vf k / vi k), plus ACC and Q.
        uint32_t vfMask = 0, viMask = 0;
        float vf[32][4] = {};
        int32_t vi[16] = {};
        bool acc = false, q = false;
        float accv[4] = {}, qv = 0.0f;
    };

    // Overlay group 3841391935 entries 0x1410/0x1268 (stride 3; wide = entry 0x10c0, stride 4 with the
    // fourth qword unused) and overlay group 1920671007 entries 0x2540/0x2268
    // (lit = true: stride 4 with a normal per vertex, lit in place first): transform, clip test,
    // cull (ADC) strip/list program. litOut (VU memory) receives the lit colours as the VU leaves
    // them; pass null when the VU will run the batch anyway. Triangles the VU sends through its
    // clipper (some vertex outside, no plane shared by all three) are clipped here too: their
    // packets go before the strip in pkt, one length per packet in sizes (RT_NATIVECLIP=0: such
    // batches run on the VU).
    bool ClippedStrip(const vu1fast::Ctx &c, uint32_t top, const uint8_t *mem, bool lit, std::vector<uint8_t> &pkt, std::vector<uint32_t> &sizes,
                      uint8_t *litOut = nullptr, bool wide = false);
    // Dump program 1604436274 entries 0x38 / 0x158: transform only.
    bool PlainStrip(const vu1fast::Ctx &c, uint32_t top, const uint8_t *mem, std::vector<uint8_t> &pkt);
    // Overlay group 1331843391 entries 0xd58 / 0xee0: transform + 3 directional lights.
    bool LitStrip(const vu1fast::Ctx &c, uint32_t top, const uint8_t *mem, std::vector<uint8_t> &pkt, WriteBack &wb);
    // Overlay group 939320750 entry 0x3218: particles, one sprite (4 vertices) kicked per particle.
    // pkt receives the kicked packets back to back, sizes their byte lengths. memOut (VU memory)
    // receives the program's stores to its two output buffers; pass null when the VU will run
    // the batch anyway.
    bool Particles(const vu1fast::Ctx &c, uint32_t top, const uint8_t *mem, std::vector<uint8_t> &pkt, std::vector<uint32_t> &sizes,
                   WriteBack &wb, uint8_t *memOut = nullptr);
    // Overlay group 1920694623 entry 0x3058: one sprite per batch (centre, colour, UV rectangle,
    // size from VU mem 0xa3 scaled by the centre's w), skipped when the centre is outside a
    // clip plane, written to TOP+0x96..0x9c and kicked. memOut as for Particles.
    bool Sprite(const vu1fast::Ctx &c, uint32_t top, const uint8_t *mem, std::vector<uint8_t> &pkt, WriteBack &wb, uint8_t *memOut = nullptr);
    // Overlay group 1920694623 entry 0x31a0: XGKICK of the GIF packet the EE built at TOP, as is.
    bool Passthrough(uint32_t top, const uint8_t *mem, std::vector<uint8_t> &pkt, WriteBack &wb);
    // Dump program 1159117218 entries 0x380 / 0x5d8: environment-mapped clipped strip (STQ.xy from
    // the normal, written back into the input first), cull as ClippedStrip, output at TOP+0x98.
    // memOut receives all of the program's VU memory stores (required: the input is modified).
    bool EnvStrip(const vu1fast::Ctx &c, uint32_t top, const uint8_t *mem, std::vector<uint8_t> &pkt, WriteBack &wb, uint8_t *memOut);
    // EnvStrip without the vertices, for a batch the renderer transforms itself: the program's
    // registers and its stores outside the input and output (0xa8, TOP-5, TOP-1). mem is VU memory.
    bool EnvStripTail(const vu1fast::Ctx &c, uint32_t top, uint8_t *mem, WriteBack &wb);
    // The setup entries (matrix and constant loads) of the overlay groups above, by program id and
    // entry pc; false for others. memOut receives the VU memory stores (vf01..04 to 164..167).
    bool Setup(const vu1fast::Ctx &c, uint32_t id, uint32_t pc, const uint8_t *mem, WriteBack &wb, uint8_t *memOut);
    // Overlay group 1821510323 entries 0x31b8 / 0x31f8 (in-race effect sprites). mem is VU memory,
    // read and written (required).
    bool EffectSprites(const vu1fast::Ctx &c, uint32_t pc, std::vector<uint8_t> &pkt, std::vector<uint32_t> &sizes, WriteBack &wb, uint8_t *mem);
}
