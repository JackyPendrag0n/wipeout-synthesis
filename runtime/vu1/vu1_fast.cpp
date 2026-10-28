#if defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#endif
// Runtime pieces of the VU1 recompiler support (see vu1_fast.h).
#include "vu1_fast.h"
#include <cstdio>
#include <cstdlib>
#include <unordered_set>

namespace vu1fast
{
    // EFU polynomial approximations, as in the PS2Recomp VU interpreter.
    float eatan(float value)
    {
        constexpr float coefficients[] = {0.999999344348907f, -0.333298563957214f, 0.199465364217758f,
                                          -0.13085337519646f, 0.096420042216778f, -0.055909886956215f,
                                          0.021861229091883f, -0.004054057877511f};
        constexpr float quarterPi = 0.785398185253143f;
        const float squared = value * value;
        float polynomial = coefficients[7];
        for (int index = 6; index >= 0; --index)
            polynomial = coefficients[index] + squared * polynomial;
        return quarterPi + value * polynomial;
    }

    float esin(float value)
    {
        constexpr float coefficients[] = {1.0f, -0.166666567325592f, 0.008333025500178f,
                                          -0.000198074136279f, 0.000002601886990f};
        const float squared = value * value;
        float polynomial = coefficients[4];
        for (int index = 3; index >= 0; --index)
            polynomial = coefficients[index] + squared * polynomial;
        return value * polynomial;
    }

    float eexp(float value)
    {
        constexpr float coefficients[] = {0.249998688697815f, 0.031257584691048f, 0.002591371303424f,
                                          0.000171562001924f, 0.000005430199963f, 0.000000690600018f};
        float polynomial = coefficients[5];
        for (int index = 4; index >= 0; --index)
            polynomial = coefficients[index] + value * polynomial;
        polynomial = 1.0f + value * polynomial;
        polynomial *= polynomial;
        polynomial *= polynomial;
        return polynomial != 0.0f ? 1.0f / polynomial : std::numeric_limits<float>::max();
    }

    bool Dispatcher::resident(uint32_t index)
    {
        uint32_t count = 0;
        const Program *progs = m_registry(count);
        if (m_checkedEpoch.size() != count)
        {
            m_checkedEpoch.assign(count, 0);
            m_residentEpoch.assign(count, 0);
        }
        if (m_checkedEpoch[index] != m_epoch)
        {
            m_checkedEpoch[index] = m_epoch;
            bool ok = true;
            const Program &p = progs[index];
            for (uint32_t k = 0; k < p.nchunks && ok; ++k)
            {
                const Chunk &ch = p.chunks[k];
                ok = ch.vuAddr + ch.size <= m_codeSize && fnv1a(m_code + ch.vuAddr, ch.size) == ch.hash;
            }
            m_residentEpoch[index] = ok ? m_epoch : 0;
        }
        return m_residentEpoch[index] == m_epoch;
    }

    const Program *Dispatcher::find(uint32_t pc, const Program *after)
    {
        uint32_t count = 0;
        const Program *progs = m_registry(count);
        for (uint32_t index = after ? (uint32_t)(after - progs) + 1u : 0u; index < count; ++index)
        {
            const Program &p = progs[index];
            bool covers = false;
            for (uint32_t k = 0; k < p.nchunks && !covers; ++k)
                covers = pc >= p.chunks[k].vuAddr && pc + 8u <= p.chunks[k].vuAddr + p.chunks[k].size;
            if (covers && resident(index))
                return &p;
        }
        return nullptr;
    }

    // RT_VU1ENTRIES=file appends every distinct (program, entry pc) the dispatcher starts.
    // tools/vurecomp.py reads such a list to keep only those pcs (plus branch targets) as
    // entry labels, which lets the compiler optimize straight through the code in between.
    static void logEntry(const Program &p, uint32_t pc)
    {
        static FILE *f = [] { const char *e = std::getenv("RT_VU1ENTRIES"); return e ? std::fopen(e, "a") : nullptr; }();
        if (!f)
            return;
        static std::unordered_set<uint64_t> seen;
        if (seen.insert((uint64_t)p.id << 32 | pc).second)
        {
            std::fprintf(f, "%u %#x\n", p.id, pc);
            std::fflush(f);
        }
    }

    bool Dispatcher::run(Ctx &c)
    {
        const int previousRounding = std::fegetround();
        std::fesetround(FE_TOWARDZERO);
#if defined(__x86_64__) || defined(__i386__)
        // Denormal inputs read as zero and denormal results flush to zero, as on the VU: the
        // generated fast paths then need no explicit input normalization (inputs that are
        // Inf/NaN still produce non-finite results, which take the exact path).
        const unsigned int previousCsr = _mm_getcsr();
        _mm_setcsr(previousCsr | 0x8040u);
#endif
        bool ok = true;
        for (;;)
        {
            const Program *p = find(c.pc);
            if (!p)
            {
                ok = false;
                break;
            }
            const uint32_t entry = c.pc;
            logEntry(*p, entry);
            Exit e = p->fn(c, c.pc);
            // A program built with only its recorded entry points leaves at once on any other
            // entry; its variant with every pair as an entry follows it in the registry.
            while (e == Exit::Leave && c.pc == entry && (p = find(entry, p)) != nullptr)
                e = p->fn(c, c.pc);
            if (e == Exit::Leave && c.pc == entry)
            {
                ok = false;
                break;
            }
            if (e == Exit::End)
                break;
            if (e == Exit::Budget)
            {
                ++budgetExits;
                break;
            }
        }
        c.kickFlush();
#if defined(__x86_64__) || defined(__i386__)
        _mm_setcsr(previousCsr);
#endif
        std::fesetround(previousRounding);
        return ok;
    }
}
