// Runtime support for statically recompiled VU1 micro programs (tools/vurecomp.py).
//
// Execution model (matches the PS2Recomp VU1Interpreter, which is the reference):
//  * VF/VI/ACC reads are interlocked on the hardware, so register writes are applied
//    immediately and a per-lane ready-cycle scoreboard only advances the cycle counter.
//  * Q (FDIV), P (EFU), MAC and CLIP flags are *not* interlocked: their results become
//    visible a fixed number of cycles after issue, so they are queued and committed lazily
//    when read.
//  * Upper/lower pair semantics: the lower instruction sees register values from before
//    the upper instruction of the same pair; if both write the same VF, the upper wins.
//  * A branch reading a VI written by an integer op in the immediately preceding pair sees
//    the old value.
//  * XGKICK transfers one qword every 2 cycles from issue, as the interpreter does: stores the
//    program makes after the kick reach the packet until the transfer has passed them.
//  * Float results use the VU rules: operands with exponent 0 are +-0, exponent 255 is
//    +-FLT_MAX; arithmetic runs with round-toward-zero; results that overflow clamp to
//    +-FLT_MAX and results that underflow flush to +-0.
#pragma once

#include <cfenv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace vu1fast
{
    inline uint32_t fbits(float f) { uint32_t b; std::memcpy(&b, &f, 4); return b; }
    inline float bfloat(uint32_t b) { float f; std::memcpy(&f, &b, 4); return f; }

    // normalizeOperand
    inline float vn(float v)
    {
        uint32_t b = fbits(v);
        const uint32_t e = b & 0x7F800000u;
        if (e == 0u)
            b &= 0x80000000u;
        else if (e == 0x7F800000u)
            b = (b & 0x80000000u) | 0x7F7FFFFFu;
        return bfloat(b);
    }

    // 4-lane forms (GCC/Clang vector extensions: SSE on x86-64, NEON on AArch64).
    typedef float v4f __attribute__((vector_size(16)));
    typedef int32_t v4i __attribute__((vector_size(16)));
    typedef uint32_t v4u __attribute__((vector_size(16)));
    inline v4f ld4(const float *p) { v4f v; std::memcpy(&v, p, 16); return v; }
    inline v4f vn4(v4f v)
    {
        v4u b = (v4u)v;
        const v4u e = b & 0x7F800000u, sign = b & 0x80000000u;
        const v4u zero = (v4u)(e == 0u), inf = (v4u)(e == 0x7F800000u);
        b = (b & ~zero & ~inf) | (sign & zero) | ((sign | 0x7F7FFFFFu) & inf);
        return (v4f)b;
    }

    // normalizeResult (used for Q and P)
    inline float vr(float v) { return vn(v); }

    // True when the host float result r is also the VU result (normal and below FLT_MAX).
    inline bool fastok(float r)
    {
        const uint32_t m = fbits(r) & 0x7FFFFFFFu;
        return m >= 0x00800000u && m < 0x7F7FFFFFu;
    }

    // normalizeFmacExactResult: returns lane flags (1 zero, 2 sign, 4 underflow, 8 overflow).
    inline uint32_t fixexact(float &value, long double exact)
    {
        const bool neg = std::signbit(exact);
        const long double mag = std::fabs(exact);
        uint32_t fl = neg ? 0x2u : 0u;
        uint32_t b = neg ? 0x80000000u : 0u;
        if (mag == 0.0L)
        {
            fl |= 0x1u;
            value = bfloat(b);
        }
        else if (mag > static_cast<long double>(std::numeric_limits<float>::max()))
        {
            fl |= 0x8u;
            value = bfloat(b | 0x7F7FFFFFu);
        }
        else if (mag < static_cast<long double>(std::numeric_limits<float>::min()))
        {
            fl |= 0x5u;
            value = bfloat(b);
        }
        return fl;
    }

    // FMAC lane helpers. Operands must already be normalized. fl receives the lane flags.
    // The host expressions are written exactly like the interpreter's so that compilers make
    // the same contraction decisions.
    __attribute__((noinline)) inline float slow_add(float r, float a, float b, uint32_t &fl)
    { fl = fixexact(r, static_cast<long double>(a) + static_cast<long double>(b)); return r; }
    __attribute__((noinline)) inline float slow_sub(float r, float a, float b, uint32_t &fl)
    { fl = fixexact(r, static_cast<long double>(a) - static_cast<long double>(b)); return r; }
    __attribute__((noinline)) inline float slow_mul(float r, float a, float b, uint32_t &fl)
    { fl = fixexact(r, static_cast<long double>(a) * static_cast<long double>(b)); return r; }
    __attribute__((noinline)) inline float slow_madd(float r, float c, float a, float b, uint32_t &fl)
    { fl = fixexact(r, static_cast<long double>(c) + static_cast<long double>(a) * static_cast<long double>(b)); return r; }
    __attribute__((noinline)) inline float slow_msub(float r, float c, float a, float b, uint32_t &fl)
    { fl = fixexact(r, static_cast<long double>(c) - static_cast<long double>(a) * static_cast<long double>(b)); return r; }

    inline float f_add(float a, float b, uint32_t &fl)
    {
        float r = a + b;
        if (__builtin_expect(fastok(r), 1)) { fl = (fbits(r) >> 30) & 2u; return r; }
        if (r == 0.0f && a == -b) { fl = 1u | ((fbits(r) >> 30) & 2u); return r; }
        return slow_add(r, a, b, fl);
    }
    inline float f_sub(float a, float b, uint32_t &fl)
    {
        float r = a - b;
        if (__builtin_expect(fastok(r), 1)) { fl = (fbits(r) >> 30) & 2u; return r; }
        if (r == 0.0f && a == b) { fl = 1u | ((fbits(r) >> 30) & 2u); return r; }
        return slow_sub(r, a, b, fl);
    }
    inline float f_mul(float a, float b, uint32_t &fl)
    {
        float r = a * b;
        if (__builtin_expect(fastok(r), 1)) { fl = (fbits(r) >> 30) & 2u; return r; }
        if (a == 0.0f || b == 0.0f) { fl = 1u | ((fbits(r) >> 30) & 2u); return r; }
        return slow_mul(r, a, b, fl);
    }
    inline float f_madd(float c, float a, float b, uint32_t &fl)
    {
        float r = c + a * b;
        if (__builtin_expect(fastok(r), 1)) { fl = (fbits(r) >> 30) & 2u; return r; }
        if (c == 0.0f && (a == 0.0f || b == 0.0f)) { fl = 1u | ((fbits(r) >> 30) & 2u); return r; }
        return slow_madd(r, c, a, b, fl);
    }
    inline float f_msub(float c, float a, float b, uint32_t &fl)
    {
        float r = c - a * b;
        if (__builtin_expect(fastok(r), 1)) { fl = (fbits(r) >> 30) & 2u; return r; }
        if (c == 0.0f && (a == 0.0f || b == 0.0f)) { fl = 1u | ((fbits(r) >> 30) & 2u); return r; }
        return slow_msub(r, c, a, b, fl);
    }

    // MAC bits for one lane (lane bit: x=8,y=4,z=2,w=1).
    inline uint32_t macbits(uint32_t fl, uint32_t lane)
    {
        uint32_t m = 0;
        if (fl & 1u) m |= lane;
        if (fl & 2u) m |= lane << 4;
        if (fl & 4u) m |= lane << 8;
        if (fl & 8u) m |= lane << 12;
        return m;
    }

    inline int32_t ftoi(float v, double scale)
    {
        const double s = static_cast<double>(v) * scale;
        if (s >= static_cast<double>(std::numeric_limits<int32_t>::max()))
            return std::numeric_limits<int32_t>::max();
        if (s <= static_cast<double>(std::numeric_limits<int32_t>::min()))
            return std::numeric_limits<int32_t>::min();
        return static_cast<int32_t>(s);
    }

    inline bool clipExceeds(uint32_t bits, uint32_t signMask, int32_t limit)
    {
        bits ^= signMask;
        int32_t o;
        std::memcpy(&o, &bits, 4);
        return o > limit;
    }

    float eatan(float v);
    float esin(float v);
    float eexp(float v);

    struct KickSink
    {
        virtual ~KickSink() = default;
        virtual void kick(const uint8_t *packet, uint32_t bytes) = 0;
    };

    struct Ctx
    {
        // architectural state
        float vf[32][4];
        int32_t vi[16];
        float acc[4];
        float q, p, i;
        uint32_t r, clip, mac, status;
        uint32_t top, itop;
        uint32_t pc;
        uint8_t *mem = nullptr;   // VU data memory (16 KB VU1, 4 KB VU0)
        uint32_t memMask = 0x3FFFu;
        KickSink *sink = nullptr;

        // timing
        uint32_t cyc = 0;
        uint32_t vfR[32][4];
        uint32_t viR[16];
        uint32_t accR[4];
        uint32_t kickBusy = 0;
        bool kickPending = false;
        uint32_t kickAddr = 0, kickIssue = 0, kickN = 0, kickTagEnd = 0;
        bool kickEop = false;
        uint32_t efuFree = 0;

        bool qPending = false;
        uint32_t qReady = 0;
        float qVal = 0;

        struct PEntry { uint32_t ready; float val; };
        PEntry pq[4];
        int pn = 0;

        struct CEntry { uint32_t issue, ready, val; };
        CEntry cq[8];
        int cn = 0;
        uint32_t workClip = 0;

        struct MEntry { uint32_t ready, val; };
        MEntry mq[8];
        int mn = 0;

        uint64_t pairs = 0;     // executed instruction pairs (statistics)
        std::vector<uint8_t> kickBuf;

        void resetTiming()
        {
            cyc = 0;
            std::memset(vfR, 0, sizeof(vfR));
            std::memset(viR, 0, sizeof(viR));
            std::memset(accR, 0, sizeof(accR));
            kickBusy = efuFree = 0;
            kickPending = false;
            qPending = false;
            pn = cn = mn = 0;
            workClip = clip;
        }

        float Q()
        {
            if (qPending && qReady <= cyc) { q = qVal; qPending = false; }
            return q;
        }
        uint32_t maxPReady() const
        {
            uint32_t m = 0;
            for (int j = 0; j < pn; ++j)
                if (pq[j].ready > m) m = pq[j].ready;
            return m;
        }
        float P()
        {
            commitP();
            return p;
        }
        void commitP()
        {
            int k = 0;
            for (int j = 0; j < pn; ++j)
            {
                if (pq[j].ready <= cyc) p = pq[j].val;
                else pq[k++] = pq[j];
            }
            pn = k;
        }
        uint32_t CLIP()
        {
            int k = 0;
            for (int j = 0; j < cn; ++j)
            {
                if (cq[j].ready <= cyc) clip = cq[j].val;
                else cq[k++] = cq[j];
            }
            cn = k;
            return clip;
        }
        // A MAC flag result also sets the status flags Z/S/U/O (bits 0-3) and their sticky
        // copies (bits 6-9).
        void commitMac(uint32_t m)
        {
            mac = m;
            const uint32_t cur = ((m & 0xFu) ? 1u : 0u) | ((m & 0xF0u) ? 2u : 0u) | ((m & 0xF00u) ? 4u : 0u) | ((m & 0xF000u) ? 8u : 0u);
            status = (status & 0xFF0u) | cur | (cur << 6);
        }
        uint32_t MAC()
        {
            int k = 0;
            for (int j = 0; j < mn; ++j)
            {
                if (mq[j].ready <= cyc) commitMac(mq[j].val);
                else mq[k++] = mq[j];
            }
            mn = k;
            return mac;
        }

        void queueQ(float v, uint32_t lat)
        {
            Q();   // commits the previous result (the issuing instruction stalled until it was ready)
            qPending = true;
            qReady = cyc + lat;
            qVal = vr(v);
        }
        void queueP(float v, uint32_t lat)
        {
            commitP();
            pq[pn++ & 3] = {cyc + lat, vr(v)};
            efuFree = cyc + (lat ? lat - 1u : 0u);
        }
        void queueClip(uint32_t flags)
        {
            workClip = ((workClip << 6) | (flags & 0x3Fu)) & 0xFFFFFFu;
            if (cn == 8) CLIP();
            cq[cn++] = {cyc, cyc + 4u, workClip};
        }
        void queueFcset(uint32_t v)
        {
            workClip = v & 0xFFFFFFu;
            int k = 0;
            for (int j = 0; j < cn; ++j)
                if (cq[j].issue != cyc) cq[k++] = cq[j];
            cn = k;
            cq[cn++] = {cyc, cyc + 4u, workClip};
        }
        void queueMac(uint32_t m)
        {
            if (mn == 8) MAC();
            mq[mn++] = {cyc + 4u, m};
        }

        // Commit everything (end of program).
        void flush()
        {
            if (qPending) { q = qVal; qPending = false; }
            for (int j = 0; j < pn; ++j) p = pq[j].val;
            pn = 0;
            for (int j = 0; j < cn; ++j) clip = cq[j].val;
            cn = 0;
            for (int j = 0; j < mn; ++j) commitMac(mq[j].val);
            mn = 0;
        }

        // XGKICK: the packet (GIF tags up to EOP) is read one qword every 2 cycles from issue,
        // then handed to the sink. Stores call kickTick() first, so a qword the program writes
        // after the kick is seen if the transfer has not reached it yet.
        void kickCopyOne()
        {
            const uint8_t *src = mem + ((kickAddr + 16u * kickN) & 0x3FF0u);
            if (kickN == kickTagEnd)
            {
                uint64_t tag;
                std::memcpy(&tag, src, 8);
                const uint32_t nloop = static_cast<uint32_t>(tag & 0x7FFFu);
                const uint32_t flg = static_cast<uint32_t>((tag >> 58) & 3u);
                uint32_t nreg = static_cast<uint32_t>((tag >> 60) & 0xFu);
                if (!nreg) nreg = 16;
                uint32_t q = 1;
                if (flg == 0u) q += nloop * nreg;
                else if (flg == 1u) q += (nloop * nreg + 1u) / 2u;
                else q += nloop;
                kickTagEnd = kickN + q;
                kickEop = ((tag >> 15) & 1u) != 0u || kickTagEnd > 0x1000u;
            }
            kickBuf.insert(kickBuf.end(), src, src + 16);
            ++kickN;
            if (kickN == kickTagEnd && kickEop)
            {
                kickPending = false;
                if (sink)
                    sink->kick(kickBuf.data(), static_cast<uint32_t>(kickBuf.size()));
            }
        }
        void kickTick()
        {
            while (kickPending && kickIssue + 2u * kickN < cyc)
                kickCopyOne();
        }
        void kickFlush()
        {
            while (kickPending)
                kickCopyOne();
        }
        void xgkick(uint32_t qwordAddr)
        {
            kickFlush();
            uint32_t addr = (qwordAddr * 16u) & 0x3FF0u;
            uint32_t qwords = 0;
            for (;;)
            {
                uint64_t tag;
                std::memcpy(&tag, mem + addr, 8);
                const uint32_t nloop = static_cast<uint32_t>(tag & 0x7FFFu);
                const uint32_t flg = static_cast<uint32_t>((tag >> 58) & 3u);
                uint32_t nreg = static_cast<uint32_t>((tag >> 60) & 0xFu);
                if (!nreg) nreg = 16;
                uint32_t q = 1;
                if (flg == 0u) q += nloop * nreg;
                else if (flg == 1u) q += (nloop * nreg + 1u) / 2u;
                else q += nloop;
                addr = (addr + 16u * q) & 0x3FF0u;
                qwords += q;
                if ((tag >> 15) & 1u)
                    break;
                if (qwords > 0x1000u)
                    break;
            }
            kickBusy = cyc + 2u * qwords - 1u;
            kickBuf.clear();
            kickPending = true;
            kickAddr = (qwordAddr * 16u) & 0x3FF0u;
            kickIssue = cyc;
            kickN = kickTagEnd = 0;
            kickEop = false;
        }

        uint32_t addrq(int32_t v) const { return (static_cast<uint32_t>(v) * 16u) & memMask; }
    };

    // Result of running a recompiled program fragment.
    enum class Exit : uint32_t
    {
        End = 0,       // E bit reached; ctx.pc is the resume address
        Leave = 1,     // control left this overlay group; ctx.pc is the target
        Budget = 2,    // cycle budget exhausted
    };

    using ProgramFn = Exit (*)(Ctx &c, uint32_t pc);

    struct Chunk { uint32_t vuAddr, size; uint64_t hash; };
    struct Program
    {
        const char *name;
        uint32_t id;
        const Chunk *chunks;
        uint32_t nchunks;
        ProgramFn fn;
    };

    // FNV-1a over 64-bit words (VU code is always a whole number of 8-byte pairs); must match
    // tools/vurecomp.py fnv().
    inline uint64_t fnv1a(const uint8_t *p, uint32_t n)
    {
        uint64_t h = 1469598103934665603ull;
        for (uint32_t k = 0; k + 8 <= n; k += 8)
        {
            uint64_t w;
            std::memcpy(&w, p + k, 8);
            h ^= w;
            h *= 1099511628211ull;
        }
        return h;
    }

    const Program *programs(uint32_t &count);    // generated registry (VU1)
    const Program *programs0(uint32_t &count);   // generated registry (VU0)
    using Registry = const Program *(*)(uint32_t &count);

    // Runs recompiled code starting at ctx.pc until the program ends. Returns false (with ctx
    // holding the state reached so far, ctx.pc = where to continue) when control reaches code
    // that is not covered by a resident recompiled group; the caller then continues with an
    // interpreter. residentFn(program) must say whether all chunks of a program are currently
    // loaded in VU1 micro memory.
    class Dispatcher
    {
    public:
        explicit Dispatcher(const uint8_t *vuCode, Registry registry = &programs, uint32_t codeSize = 0x4000u)
            : m_code(vuCode), m_registry(registry), m_codeSize(codeSize) {}
        void invalidate() { ++m_epoch; }
        const Program *find(uint32_t pc, const Program *after = nullptr);
        bool run(Ctx &c);
        uint64_t budgetExits = 0;

    private:
        bool resident(uint32_t index);
        const uint8_t *m_code;
        Registry m_registry;
        uint32_t m_codeSize;
        uint64_t m_epoch = 1;
        std::vector<uint64_t> m_checkedEpoch, m_residentEpoch;
    };
}
