// Ruff Trigger: The Vanocore Conspiracy (SLUS-213.14, NTSC-U) runtime override.
//
// Binds the statically linked Sony SDK libraries that the recompiled game would otherwise
// drive through SIF RPC servers that do not exist without a BIOS (libcdvd, libpad, libmtap),
// and wraps a few game functions to log boot milestones. Addresses were identified from the
// SIF RPC service IDs / RPC function numbers used by each routine and from libpad's
// sample state machine in f_002DC068 (see the port workspace notes).
#include "game_overrides.h"
#include "ps2_runtime.h"
#include "runtime/ps2_capture_hook.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_vu1.h"
#include "vu1_fast.h"
#include <vector>
#include <set>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <cstring>
#include <string>
#include <map>
#include <mutex>
#include <cctype>
#include <array>
#include <thread>
#include <chrono>
#include <algorithm>
#include <filesystem>

namespace rufftrigger
{
    void SetWidescreenCamera(bool on);
    void InstallSettings(PS2Runtime &runtime);
    void InstallGpuRenderer(PS2Runtime &runtime);
    void InstallAudio();
    void StartProfiler();
    void InstallMemcard(PS2Runtime &runtime);
    std::atomic<uint64_t> g_gameFrames{0};
    void InstallMovies(PS2Runtime &runtime);
    void InstallDebug(PS2Runtime &runtime);
    void GpuRendererStats(uint64_t &prims, uint64_t &unsupported, uint64_t &frames);
}

namespace
{
    // getenv that treats an empty value as unset (run.sh exports every knob).
    const char *Env(const char *name)
    {
        const char *v = std::getenv(name);
        return (v && *v) ? v : nullptr;
    }

    // ---- milestone logging ---------------------------------------------------------------
    struct Wrapped
    {
        uint32_t addr;
        const char *what;
        int strArg;                       // which $aN holds a guest string to print, or -1
        PS2Runtime::RecompiledFunction original;
        std::atomic<uint32_t> calls{0};
    };

    Wrapped g_wrapped[] = {
        {0x00100F20, "main", -1, nullptr},
        {0x002AD0E0, "CPs2FileManager::AddPack", 1, nullptr},
        {0x002AD7F0, "CPs2FileManager::Open", 1, nullptr},
        {0x00293298, "LoadIopModule", 0, nullptr},
        {0x0022EEA8, "ParsedFile::LoadFromPack", 1, nullptr},
        {0x002335A8, "fsm_parse_script", -1, nullptr},
        {0x00141CD0, "LoadLevelFsm", -1, nullptr},
        {0x001C81C0, "LoadNpcDef", 1, nullptr},
        {0x00144350, "RegisterLevelActions", -1, nullptr},
        // 48 dynamic slots filled from RT_TRACE="addr[:argIndex],..." (hex)
#define RT_DYN {0, nullptr, -1, nullptr}
        RT_DYN, RT_DYN, RT_DYN, RT_DYN, RT_DYN, RT_DYN, RT_DYN, RT_DYN, RT_DYN, RT_DYN, RT_DYN, RT_DYN,
        RT_DYN, RT_DYN, RT_DYN, RT_DYN, RT_DYN, RT_DYN, RT_DYN, RT_DYN, RT_DYN, RT_DYN, RT_DYN, RT_DYN,
        RT_DYN, RT_DYN, RT_DYN, RT_DYN, RT_DYN, RT_DYN, RT_DYN, RT_DYN, RT_DYN, RT_DYN, RT_DYN, RT_DYN,
        RT_DYN, RT_DYN, RT_DYN, RT_DYN, RT_DYN, RT_DYN, RT_DYN, RT_DYN, RT_DYN, RT_DYN, RT_DYN, RT_DYN,
#undef RT_DYN
    };
    std::string g_dynNames[48];
    bool g_traceExit = false;

    std::string GuestString(uint8_t *rdram, uint32_t addr)
    {
        std::string s;
        addr &= 0x01FFFFFFu;
        while (addr < 0x02000000u && s.size() < 256)
        {
            char c = static_cast<char>(rdram[addr++]);
            if (!c)
                break;
            s.push_back(c);
        }
        return s;
    }

    template <size_t I>
    void Trampoline(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        Wrapped &w = g_wrapped[I];
        const uint32_t n = ++w.calls;
        // Only log the entry (ctx->pc at the function start); resumed entries mid-function
        // (thread switches) go straight to the original.
        if (ctx->pc == w.addr && (n <= 400 || (n & (n - 1)) == 0))
        {
            if (w.strArg == -2)
            {
                auto R = [&](int r) { return static_cast<uint32_t>(_mm_extract_epi32(ctx->r[r], 0)); };
                std::fprintf(stderr, "[rt] %s #%u a0=%x a1=%x a2=%x a3=%x t0=%x t1=%x t2=%x t3=%x ra=%x\n", w.what, n,
                             R(4), R(5), R(6), R(7), R(8), R(9), R(10), R(11), R(31));
            }
            else if (w.strArg >= 0)
            {
                const uint32_t a = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4 + w.strArg], 0));
                std::fprintf(stderr, "[rt] %s #%u \"%s\"\n", w.what, n, GuestString(rdram, a).c_str());
            }
            else
            {
                std::fprintf(stderr, "[rt] %s #%u\n", w.what, n);
            }
        }
        const bool entry = ctx->pc == w.addr;
        w.original(rdram, ctx, runtime);
        // RT_CHECKW=<hex addr>: report traced functions after which that word changed.
        static const uint32_t checkAddr = Env("RT_CHECKW") ? static_cast<uint32_t>(std::strtoul(Env("RT_CHECKW"), nullptr, 16)) & 0x01FFFFFCu : 0u;
        if (checkAddr)
        {
            static uint32_t last = 0;
            uint32_t now;
            std::memcpy(&now, rdram + checkAddr, 4);
            if (now != last)
            {
                std::fprintf(stderr, "[checkw] %08x: %08x -> %08x after %s (entry=%d)\n", checkAddr, last, now, w.what, entry ? 1 : 0);
                last = now;
            }
        }
        if (entry && g_traceExit && (n <= 400 || (n & (n - 1)) == 0))
            std::fprintf(stderr, "[rt]   <- %s #%u v0=0x%x\n", w.what, n,
                         static_cast<uint32_t>(_mm_extract_epi32(ctx->r[2], 0)));
    }

    template <size_t... I>
    void WrapAll(PS2Runtime &rt, std::index_sequence<I...>)
    {
        ((g_wrapped[I].addr == 0 ? (void)0 : (void)(g_wrapped[I].original = rt.lookupFunction(g_wrapped[I].addr),
          g_wrapped[I].original ? (void)rt.replaceFunction(g_wrapped[I].addr, &Trampoline<I>)
                                : (void)std::fprintf(stderr, "[rt] cannot wrap %s\n", g_wrapped[I].what))),
         ...);
    }

    // RT_MATDBG=1: log f_002c1cd8 (4x4 matrix multiply, dst = a1 * a2 rows) calls whose result
    // has a zero 3x3 part, with the operands.
    PS2Runtime::RecompiledFunction g_matMulOrig = nullptr;
    void MatMulDbg(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        auto R = [&](int r) { return static_cast<uint32_t>(_mm_extract_epi32(ctx->r[r], 0)); };
        const uint32_t a0 = R(4), a1 = R(5), a2 = R(6), ra = R(31);
        auto ptr = [&](uint32_t a) -> const float * {
            if ((a & 0xF0000000u) == 0x70000000u) return reinterpret_cast<const float *>(runtime->memory().getScratchpad() + (a & 0x3FFFu));
            return reinterpret_cast<const float *>(rdram + (a & 0x01FFFFFFu));
        };
        float A[16], B[16];
        std::memcpy(A, ptr(a1), 64);
        std::memcpy(B, ptr(a2), 64);
        g_matMulOrig(rdram, ctx, runtime);
        const float *D = ptr(a0);
        bool z = true;
        for (int k = 0; k < 12 && z; ++k) z = D[k] == 0.0f;
        static int n = 0;
        if (z && n++ < 12)
        {
            std::fprintf(stderr, "[matdbg] dst=%08x a=%08x b=%08x ra=%08x\n  A:", a0, a1, a2, ra);
            for (float f : A) std::fprintf(stderr, " %g", f);
            std::fprintf(stderr, "\n  B:");
            for (float f : B) std::fprintf(stderr, " %g", f);
            std::fprintf(stderr, "\n  D:");
            for (int k = 0; k < 16; ++k) std::fprintf(stderr, " %g", D[k]);
            std::fprintf(stderr, "\n");
        }
    }

    PS2Runtime::RecompiledFunction g_nodeMatOrig = nullptr;
    void NodeMatDbg(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t a0 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0));
        g_nodeMatOrig(rdram, ctx, runtime);
        const float *spr = reinterpret_cast<const float *>(runtime->memory().getScratchpad());
        bool z = true;
        for (int k = 0; k < 12 && z; ++k) z = spr[k] == 0.0f;
        static std::vector<uint32_t> seen;
        if (z)
        {
            uint32_t nd0;
            std::memcpy(&nd0, rdram + ((a0 + 8) & 0x01FFFFFFu), 4);
            if (std::find(seen.begin(), seen.end(), nd0) == seen.end() && seen.size() < 64)
            {
                seen.push_back(nd0);
                std::fprintf(stderr, "[nodezero] a0=%08x node=%08x ra=%08x\n", a0, nd0,
                             static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0)));
            }
        }
        static int n = 0;
        if (z && n++ < 8)
        {
            uint32_t node;
            std::memcpy(&node, rdram + ((a0 + 8) & 0x01FFFFFFu), 4);
            const float *nd = reinterpret_cast<const float *>(rdram + (node & 0x01FFFFFFu));
            const float *m300 = reinterpret_cast<const float *>(rdram + 0x45A300);
            const float *m280 = reinterpret_cast<const float *>(rdram + 0x45A280);
            std::fprintf(stderr, "[nodedbg] a0=%08x node=%08x rot:", a0, node);
            for (int k = 0x4C / 4; k < 0x88 / 4; ++k) std::fprintf(stderr, " %g", nd[k]);
            std::fprintf(stderr, "\n  45a300:");
            for (int k = 0; k < 16; ++k) std::fprintf(stderr, " %g", m300[k]);
            std::fprintf(stderr, "\n  45a280:");
            for (int k = 0; k < 16; ++k) std::fprintf(stderr, " %g", m280[k]);
            std::fprintf(stderr, "\n  spr0:");
            for (int k = 0; k < 16; ++k) std::fprintf(stderr, " %g", spr[k]);
            std::fprintf(stderr, "\n");
        }
    }

    PS2Runtime::RecompiledFunction g_nodeCtorOrig = nullptr;
    void NodeCtorDbg(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t a0 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0));
        const uint32_t ra = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0));
        g_nodeCtorOrig(rdram, ctx, runtime);
        const uint32_t lo = Env("RT_NODELO") ? std::strtoul(Env("RT_NODELO"), nullptr, 16) : 0x1725000u;
        const uint32_t hi = Env("RT_NODEHI") ? std::strtoul(Env("RT_NODEHI"), nullptr, 16) : 0x1727000u;
        const uint32_t p = a0 & 0x01FFFFFFu;
        if (p >= lo && p < hi)
        {
            float sc[3];
            std::memcpy(sc, rdram + p + 0x7C, 12);
            std::fprintf(stderr, "[nodector] node=%08x ra=%08x scale=(%g %g %g)\n", a0, ra, sc[0], sc[1], sc[2]);
        }
    }

    // RT_FINDLOG: log node name lookups (f_001e5588) with the node returned and its name.
    PS2Runtime::RecompiledFunction g_findOrig = nullptr;
    void FindDbg(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        auto R = [&](int r) { return static_cast<uint32_t>(_mm_extract_epi32(ctx->r[r], 0)); };
        const uint32_t a0 = R(4), a1 = R(5), ra = R(31);
        char want[40] = {};
        std::strncpy(want, reinterpret_cast<const char *>(rdram + (a1 & 0x01FFFFFFu)), 39);
        g_findOrig(rdram, ctx, runtime);
        const uint32_t v0 = R(2);
        char got[40] = {};
        if (v0) std::strncpy(got, reinterpret_cast<const char *>(rdram + ((v0 + 0x10) & 0x01FFFFFFu)), 39);
        uint32_t sub = 0;
        if (v0) std::memcpy(&sub, rdram + ((v0 + 0x5C) & 0x01FFFFFFu), 4);
        std::fprintf(stderr, "[find] root=%08x name='%s' ra=%08x -> %08x '%s' sub=%08x\n", a0, want, ra, v0, got, sub);
    }

    std::chrono::steady_clock::time_point g_bootTime;

    // f_00221f98 is a busy-wait (5000 x 5000 empty loop iterations, ~0.5 s on a PS2) that the
    // sound streaming setup runs between polls of the IOP. The recompiled loop is charged 32
    // EE cycles per iteration, so each call cost ~2.7 s of emulated time, and emulated time
    // cannot run ahead of real time: most of a level's loading screen. Replace it with a short
    // wait (RT_DELAY_CYCLES EE cycles, default 20,000,000 = 68 ms); the caller polls until the
    // IOP is ready either way.
    void HleBusyDelay(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        static const uint32_t cycles = Env("RT_DELAY_CYCLES") ? static_cast<uint32_t>(std::strtoul(Env("RT_DELAY_CYCLES"), nullptr, 10)) : 20000000u;
        ctx->pc = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0));
        ctx->r[2] = _mm_set_epi64x(0, 0);
        runtime->eeCheckpointDue(cycles);
    }

    // RT_PADSCRIPT="vsync:buttons[:frames],..." presses PS2 buttons (hex, e.g. 8 = Start,
    // 4000 = Cross, 40 = Down) at guest vsync counts, for repeatable benchmarks.
    struct PadStep { uint64_t at, len; uint16_t bits; };
    std::vector<PadStep> g_padScript;
    PS2Runtime *g_padRt = nullptr;
    uint16_t PadScript()
    {
        if (!g_padRt)
            return 0;
        const uint64_t v = g_padRt->memory().gs().vsyncTick.load(std::memory_order_relaxed);
        uint16_t bits = 0;
        for (const PadStep &p : g_padScript)
            if (v >= p.at && v < p.at + p.len)
                bits |= p.bits;
        return bits;
    }

    // RT_FPSLOG: count the game's end-of-frame calls (f_00291140) for the pacing log.
    PS2Runtime::RecompiledFunction g_frameOrig = nullptr;
    void FrameCount(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == 0x00291140u)
            rufftrigger::g_gameFrames.fetch_add(1, std::memory_order_relaxed);
        g_frameOrig(rdram, ctx, runtime);
    }

    // Widescreen (RT_WIDESCREEN, default on): f_001e9970 builds a camera's projection * screen
    // matrix from the viewport size, the FOV (+0x20) and an aspect factor it stores at +0x24
    // (1.25 or 1.3671875 by video mode). The horizontal projection term only enters row 0 of the
    // combined matrix, so scaling that row by 3/4 turns the 4:3 view into 16:9 (same vertical
    // FOV, wider horizontal one). The aspect factor is widened as well for code that derives
    // view-frustum culling from it.
    // The graphics menu switches it at run time (1 = the original 4:3 camera); a camera picks
    // the change up the next time the game rebuilds its projection.
    PS2Runtime::RecompiledFunction g_projOrig = nullptr;
    std::atomic<float> g_wideXSetting{0.75f};
    float g_wideX = 0.75f;
    void ProjWide(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t cam = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0));
        g_wideX = g_wideXSetting.load(std::memory_order_relaxed);
        g_projOrig(rdram, ctx, runtime);
        if (g_wideX == 1.0f)
            return;
        uint32_t vp = 0;
        std::memcpy(&vp, rdram + ((cam + 8u) & 0x01FFFFFFu), 4);
        if (!vp)
            return;
        auto scaleF = [&](uint8_t *p, float k) { float f; std::memcpy(&f, p, 4); f *= k; std::memcpy(p, &f, 4); };
        uint8_t *v = rdram + (vp & 0x01FFFFFFu);
        for (uint32_t off : {0x74u, 0x78u, 0x7Cu})
            scaleF(v + off, g_wideX);
        scaleF(v + 0x24, 1.0f / g_wideX);
        uint8_t *spr = runtime->memory().getScratchpad();
        for (uint32_t off = 0x40u; off < 0x50u; off += 4u)
            scaleF(spr + off, g_wideX);
    }

    // The per-frame scene setup f_0025c610 builds a second projection (effects, light shafts)
    // from its own aspect field (+0x28, computed from +0x18/+0x1C when negative): widen it for
    // the duration of the call.
    PS2Runtime::RecompiledFunction g_sceneOrig = nullptr;
    void SceneWide(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t obj = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0)) & 0x01FFFFFFu;
        g_wideX = g_wideXSetting.load(std::memory_order_relaxed);
        if (g_wideX == 1.0f)
        {
            g_sceneOrig(rdram, ctx, runtime);
            return;
        }
        float aspect, w, h;
        std::memcpy(&aspect, rdram + obj + 0x28, 4);
        std::memcpy(&w, rdram + obj + 0x18, 4);
        std::memcpy(&h, rdram + obj + 0x1C, 4);
        if (aspect < 0.0f && h != 0.0f)
            aspect = w / h;
        const float wide = aspect / g_wideX;
        std::memcpy(rdram + obj + 0x28, &wide, 4);
        g_sceneOrig(rdram, ctx, runtime);
        std::memcpy(rdram + obj + 0x28, &aspect, 4);
    }

    // RT_TEXLOG_AT=<secs>: log texture-cache requests (f_002962f8) for a while after that time.
    PS2Runtime::RecompiledFunction g_texReqOrig = nullptr;
    void TexReqDbg(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        auto R = [&](int r) { return static_cast<uint32_t>(_mm_extract_epi32(ctx->r[r], 0)); };
        const uint32_t a0 = R(4), a1 = R(5), ra = R(31);
        auto W = [&](uint32_t a) { uint32_t v = 0; std::memcpy(&v, rdram + (a & 0x01FFFFFCu), 4); return v; };
        const uint32_t free0 = W(0x3544D8), off0 = W(0x353BC0);
        g_texReqOrig(rdram, ctx, runtime);
        static const double at = std::atof(Env("RT_TEXLOG_AT"));
        static int n = 0;
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_bootTime).count();
        if (t >= at && n < 300)
        {
            ++n;
            std::fprintf(stderr, "[texreq] a0=%08x a1=%08x ra=%08x f14=%u f2c=%08x f1c=%u free %d->%d off %u->%u ret=%08x idx=%u wp=%08x/%08x\n", a0, a1, ra,
                         W(a0 + 0x14), W(a0 + 0x2C), rdram[(a0 + 0x1C) & 0x01FFFFFFu], (int)free0, (int)W(0x3544D8), off0, W(0x353BC0), R(2), W(0x353BA4), W(0x3544C8), W(0x3544CC));
        }
    }

    // RT_MEMW=<hex addr>: log memcpy/memset calls that cover that address (with $ra).
    PS2Runtime::RecompiledFunction g_memcpyOrig = nullptr, g_memsetOrig = nullptr;
    template <bool IsSet>
    void MemWDbg(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static const uint32_t w = static_cast<uint32_t>(std::strtoul(Env("RT_MEMW"), nullptr, 16)) & 0x01FFFFFFu;
        auto R = [&](int r) { return static_cast<uint32_t>(_mm_extract_epi32(ctx->r[r], 0)); };
        const uint32_t dst = R(4) & 0x01FFFFFFu, a1 = R(5), n = R(6), ra = R(31);
        const bool hit = w >= dst && w < dst + n;
        uint32_t before = 0;
        if (hit) std::memcpy(&before, rdram + w, 4);
        (IsSet ? g_memsetOrig : g_memcpyOrig)(rdram, ctx, runtime);
        if (hit)
        {
            uint32_t after;
            std::memcpy(&after, rdram + w, 4);
            std::fprintf(stderr, "[memw] %s dst=%08x %s=%08x n=%u ra=%08x: %08x -> %08x\n", IsSet ? "memset" : "memcpy", dst,
                         IsSet ? "c" : "src", a1, n, ra, before, after);
        }
    }

    // ---- SDK bindings ---------------------------------------------------------------------
    struct Binding
    {
        uint32_t addr;
        const char *handler;
    };

    const Binding kBindings[] = {
        // libcdvd (cdvdfsv SIDs 0x80000592..0x8000059C)
        {0x002BD178, "sceCdInit"},
        {0x002BD678, "sceCdDiskReady"},
        {0x002BCC50, "sceCdSearchFile"},
        {0x002BDBE0, "sceCdRead"},          // N-cmd 1
        {0x002BDDC0, "sceCdReadIOPm"},      // N-cmd 13
        {0x002BD8C8, "sceCdReadChain"},     // N-cmd 15
        {0x002BDF18, "sceCdSeek"},          // N-cmd 5
        {0x002BDFE8, "sceCdStandby"},       // N-cmd 6
        {0x002BE098, "sceCdStop"},          // N-cmd 7
        {0x002BE148, "sceCdPause"},         // N-cmd 8
        {0x002BCE60, "sceCdSync"},          // "N cmd wait"
        {0x002BCF00, "sceCdSyncS"},         // "S cmd wait"
        {0x002BE1F8, "sceCdGetDiskType"},   // S-cmd 3
        {0x002BE2B8, "sceCdGetError"},      // S-cmd 4
        {0x002BE350, "sceCdStatus"},        // S-cmd 12, "status called"
        {0x002BE408, "sceCdMmode"},         // S-cmd 0x22
        {0x002BE500, "sceCdTrayReq"},       // S-cmd 5
        {0x002BE5F0, "sceCdStInit"},        // stream sub-cmd 5
        {0x002BE620, "sceCdStStart"},       // stream sub-cmd 1
        {0x002BE658, "sceCdStSeekF"},       // stream sub-cmd 9
        {0x002BE688, "sceCdStSeek"},        // stream sub-cmd 4
        {0x002BE6B8, "sceCdStStop"},        // stream sub-cmd 3
        {0x002BE6F0, "sceCdStRead"},
        {0x002BE920, "sceCdStStat"},
        // libpad (padman SIDs 0x80000100/0x80000101)
        {0x002DCA80, "scePadInit"},
        {0x002DCED8, "scePadPortOpen"},
        {0x002DD3A0, "scePadGetState"},
        {0x002DD328, "scePadRead"},
        {0x002DD7C8, "scePadInfoMode"},
        {0x002DD900, "scePadSetMainMode"},
        {0x002DD508, "scePadGetReqState"},
        {0x002DD598, "scePadInfoAct"},
        {0x002DDA80, "scePadSetActAlign"},
        {0x002DDCB8, "scePadInfoPressMode"},
        {0x002DDD38, "scePadEnterPressMode"},
        {0x002DD9B8, "scePadSetActDirect"},
        // libsif IOP reboot (no BIOS UDNL: let the runtime reset its IOP model)
        {0x002F6558, "sceSifRebootIop"},    // builds "rom0:UDNL <img>"
        {0x002F6508, "sceSifSyncIop"},
        // libsif command/RPC layer -> runtime SIF model routed to ps2xIOP
        {0x002EFC88, "sceSifInitCmd"},
        {0x002F03E8, "sceSifInitRpc"},      // registers handlers for cmds 8/9/A/C
        {0x002F0B90, "sceSifBindRpc"},      // sends cmd 0x80000009
        {0x002F0D70, "sceSifCallRpc"},      // sends cmd 0x8000000A
        {0x002F0F70, "sceSifCheckStatRpc"},
        {0x002F57B8, "sceSifLoadFileReset"},
        {0x002F50D8, "sceSifInitIopHeap"},  // binds 0x80000003
        {0x002F1F48, "sceFsReset"},
        {0x002DFEE8, "printf"},            // newlib printf (stdout -> fileio RPC)
        // libc file I/O (fileio RPC SID 0x80000001 has no server; route to the runtime VFS).
        // Identified by the SceStdio*Sema names each wrapper creates.
        {0x002F21B0, "sceOpen"},
        {0x002F2440, "sceClose"},
        {0x002F25B8, "sceLseek"},
        {0x002F27F8, "sceRead"},
        {0x002F2A68, "sceWrite"},
        {0x002F2D28, "sceIoctl"},
        // libmtap: no MTAPMAN.IRX on the disc -> report "no multitap"
        {0x002DC658, "ret1"},               // sceMtapInit
        {0x002DC778, "ret0"},               // sceMtapPortOpen
        {0x002DC7E8, "ret0"},               // sceMtapPortClose
        {0x002DC938, "ret0"},               // sceMtapGetConnection
    };

    // ---- IOP module loading -------------------------------------------------------------
    // There is no LOADFILE RPC server (SID 0x80000006) without a BIOS, so the three entry
    // points that use it are implemented on top of PS2Runtime::loadIopModule():
    //   0x002F6028 sceSifLoadModule(path, argLen, args)
    //   0x002F6048 sceSifLoadStartModule(path, argLen, args, &result)
    //   0x002F5C90 sceSifSearchModuleByName(name)
    std::mutex g_modMutex;
    std::map<std::string, int32_t> g_loadedModules;   // "sio2man" -> module id

    std::string ModuleKey(std::string path)
    {
        size_t cut = path.find_last_of("\\/:");
        if (cut != std::string::npos) path.erase(0, cut + 1);
        if (size_t semi = path.find(';'); semi != std::string::npos) path.resize(semi);
        if (size_t dot = path.find('.'); dot != std::string::npos) path.resize(dot);
        for (char &c : path) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return path;
    }

    void SetV0Return(R5900Context *ctx, int32_t v)
    {
        ctx->r[2] = _mm_set_epi64x(0, v);
        ctx->pc = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0));
    }

    void HleLoadModuleCommon(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, bool hasResult)
    {
        auto R = [&](int r) { return static_cast<uint32_t>(_mm_extract_epi32(ctx->r[r], 0)); };
        std::string path = GuestString(rdram, R(4));
        for (size_t i = 0; (i = path.find("\\\\", i)) != std::string::npos;) path.erase(i, 1);   // "\\" -> "\"
        const uint32_t argLen = R(5), argAddr = R(6) & 0x01FFFFFFu;
        const auto res = runtime->loadIopModule(path, argLen ? rdram + argAddr : nullptr, argLen);
        if (hasResult && R(7))
        {
            const int32_t sr = res.startResult;
            std::memcpy(rdram + (R(7) & 0x01FFFFFFu), &sr, 4);
        }
        if (res.moduleId > 0)
        {
            std::lock_guard<std::mutex> lock(g_modMutex);
            g_loadedModules[ModuleKey(path)] = res.moduleId;
        }
        std::fprintf(stderr, "[rt] LoadModule(\"%s\") -> id=%d start=%d\n", path.c_str(), res.moduleId, res.startResult);
        SetV0Return(ctx, res.moduleId > 0 ? res.moduleId : -1);
    }

    void HleSifLoadModule(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt) { HleLoadModuleCommon(rdram, ctx, rt, false); }
    void HleSifLoadStartModule(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt) { HleLoadModuleCommon(rdram, ctx, rt, true); }

    void HleSearchModuleByName(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        const std::string name = GuestString(rdram, static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0)));
        int32_t id = -1;
        {
            std::lock_guard<std::mutex> lock(g_modMutex);
            if (auto it = g_loadedModules.find(ModuleKey(name)); it != g_loadedModules.end()) id = it->second;
        }
        std::fprintf(stderr, "[rt] sceSifSearchModuleByName(\"%s\") -> %d\n", name.c_str(), id);
        SetV0Return(ctx, id);
    }

    // sceGetstat (0x002F3D98): no runtime handler. Fill iox_stat_t {mode, attr, size, ...}
    // from the host file in the CD root (cdrom0:\\A\\B;1 -> <cdRoot>/A/B).
    void HleGetstat(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        const uint32_t a0 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0));
        const uint32_t a1 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[5], 0)) & 0x01FFFFFFu;
        std::string path = GuestString(rdram, a0);
        int32_t ret = -2;   // ENOENT-ish
        size_t colon = path.find(':');
        if (colon != std::string::npos)
        {
            std::string rel = path.substr(colon + 1);
            if (rel.size() > 2 && rel[rel.size() - 2] == ';') rel.resize(rel.size() - 2);
            for (char &c : rel) if (c == '\\') c = '/';
            while (!rel.empty() && rel[0] == '/') rel.erase(0, 1);
            std::error_code ec;
            const auto host = PS2Runtime::getIoPaths().cdRoot / rel;
            const auto size = std::filesystem::file_size(host, ec);
            if (!ec)
            {
                std::memset(rdram + a1, 0, 0x40);
                const uint32_t mode = 0x2000u | 0x0124u;   // FIO_S_IFREG | r--r--r--
                const uint32_t sz = static_cast<uint32_t>(size);
                std::memcpy(rdram + a1 + 0, &mode, 4);
                std::memcpy(rdram + a1 + 8, &sz, 4);
                ret = 0;
            }
        }
        std::fprintf(stderr, "[rt] sceGetstat(\"%s\") -> %d\n", path.c_str(), ret);
        ctx->r[2] = _mm_set_epi64x(0, ret);
        ctx->pc = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0));
    }

    // ---- graphics traffic statistics + capture ----------------------------------------------
    // RT_CAPTURE=<file> RT_CAPTURE_AT=<seconds after boot> RT_CAPTURE_FRAMES=<vsyncs>
    // File: "RTCAP001" then records {u32 type, u32 size, u64 vsyncTick, payload}.
    //   0x10 VIF1 stream, 0x11..0x13 GIF PATH1..3,
    //   0x20 VU1 micro memory, 0x21 VU1 data memory, 0x22 GS VRAM, 0x23 GS privileged regs
    //   (snapshots written when the capture starts and again when it ends).
    PS2Runtime *g_rt = nullptr;
    std::atomic<uint64_t> g_capBytes[4], g_capPackets[4];
    std::FILE *g_capFile = nullptr;
    double g_capAt = -1;
    uint64_t g_capFrames = 0, g_capStartTick = 0;
    int g_capState = 0;   // 0 idle, 1 recording, 2 done

    void CapWrite(uint32_t type, const void *data, uint32_t size, uint64_t tick)
    {
        std::fwrite(&type, 4, 1, g_capFile);
        std::fwrite(&size, 4, 1, g_capFile);
        std::fwrite(&tick, 8, 1, g_capFile);
        std::fwrite(data, 1, size, g_capFile);
    }

    void CapSnapshots(uint64_t tick)
    {
        PS2Memory &m = g_rt->memory();
        CapWrite(0x20, m.getVU1Code(), PS2_VU1_CODE_SIZE, tick);
        CapWrite(0x21, m.getVU1Data(), PS2_VU1_DATA_SIZE, tick);
        CapWrite(0x22, m.getGSVRAM(), PS2_GS_VRAM_SIZE, tick);
        const GSRegisters &g = m.gs();
        uint64_t regs[20] = {g.pmode, g.smode1, g.smode2, g.srfsh, g.synch1, g.synch2, g.syncv,
                             g.dispfb1, g.display1, g.dispfb2, g.display2, g.extbuf, g.extdata,
                             g.extwrite, g.bgcolor, g.csr.load(), g.vsyncTick.load(), g.imr, g.busdir, g.siglblid};
        CapWrite(0x23, regs, sizeof(regs), tick);
        CapWrite(0x24, m.getScratchpad(), PS2_SCRATCHPAD_SIZE, tick);
    }

    // ---- VU1 call corpus (reference data for the recompiled VU1 programs) -------------------
    // RT_VU1CAP=<file> RT_VU1CAP_AT=<secs> RT_VU1CAP_N=<calls>
    // File "VU1CAP01", then per call: u32 kind, u32 pcBefore, u32 top, u32 itop, State before,
    // u32 hasCode [16 KB code], 16 KB data before, State after, 16 KB data after,
    // u32 nKicks, {u32 size, bytes}...
    struct Vu1CapState
    {
        float vf[32][4];
        int32_t vi[16];
        float acc[4];
        float q, p, i;
        uint32_t r, clip, mac, status, pc;
    };
    std::FILE *g_vu1Cap = nullptr;
    double g_vu1CapAt = 0;
    uint64_t g_vu1CapLeft = 0;
    bool g_vu1CapActive = false;
    std::vector<uint8_t> g_vu1CapCode, g_vu1CapRecord;
    std::vector<std::vector<uint8_t>> g_vu1CapKicks;
    std::chrono::steady_clock::time_point g_vu1Boot = std::chrono::steady_clock::now();

    void Vu1CapState_(const VU1State &st, Vu1CapState &o)
    {
        std::memcpy(o.vf, st.vf, sizeof(o.vf));
        std::memcpy(o.vi, st.vi, sizeof(o.vi));
        std::memcpy(o.acc, st.acc, sizeof(o.acc));
        o.q = st.q; o.p = st.p; o.i = st.i; o.r = st.r; o.clip = st.clip; o.mac = st.mac;
        o.status = st.status; o.pc = st.pc;
    }

    void Put(std::vector<uint8_t> &v, const void *p, size_t n)
    {
        const uint8_t *b = static_cast<const uint8_t *>(p);
        v.insert(v.end(), b, b + n);
    }

    uint32_t Vu1Hook(uint32_t phase, uint32_t kind, uint32_t startPC, uint32_t top, uint32_t itop,
                     void *state, uint8_t *vuCode, uint8_t *vuData);

    void CaptureHook(uint32_t kind, const uint8_t *data, uint32_t size)
    {
        if (kind == ps2x_capture::GifPath1 && g_vu1CapActive)
            g_vu1CapKicks.emplace_back(data, data + size);
        const uint32_t k = (kind - 0x10u) & 3u;
        g_capBytes[k].fetch_add(size, std::memory_order_relaxed);
        g_capPackets[k].fetch_add(1, std::memory_order_relaxed);
        if (!g_capFile || g_capState == 2)
            return;
        const uint64_t tick = g_rt->eeScheduler().currentVSyncTick();
        if (g_capState == 0)
        {
            const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_bootTime).count();
            if (t < g_capAt)
                return;
            g_capState = 1;
            g_capStartTick = tick;
            std::fwrite("RTCAP001", 1, 8, g_capFile);
            CapSnapshots(tick);
            std::fprintf(stderr, "[cap] recording from vsync %llu\n", (unsigned long long)tick);
        }
        if (tick >= g_capStartTick + g_capFrames)
        {
            CapSnapshots(tick);
            std::fclose(g_capFile);
            g_capFile = nullptr;
            g_capState = 2;
            std::fprintf(stderr, "[cap] done at vsync %llu\n", (unsigned long long)tick);
            return;
        }
        CapWrite(kind, data, size, tick);
    }

    // RT_VU1NATIVE=1: run VU1 micro programs through the statically recompiled code
    // (src/vu1/gen, verified bit-exact against the interpreter by tests/test_vu1fast).
    struct GifKickSink : vu1fast::KickSink
    {
        void kick(const uint8_t *p, uint32_t n) override
        {
            g_rt->memory().submitGifPacket(GifPathId::Path1, p, n);
        }
    };
    bool g_vu1Native = false;
    vu1fast::Ctx *g_vu1Ctx = nullptr;
    vu1fast::Dispatcher *g_vu1Disp = nullptr;
    GifKickSink g_vu1Sink;
    uint64_t g_vu1Gen = ~0ull;
    std::atomic<uint64_t> g_vu1NativeCalls{0}, g_vu1InterpCalls{0}, g_vu1PartialCalls{0}, g_vu1Pairs{0};
    uint64_t g_vu1CapEvery = 1, g_vu1CapCounter = 0;

    // RT_VU1MISS=<dir>: save the micro memory for entry points without recompiled code
    // (input for tools/vurecomp.py --dumps).
    void Vu1Miss(uint32_t pc, const uint8_t *vuCode)
    {
        static const char *missDir = Env("RT_VU1MISS");
        if (!missDir)
            return;
        static std::set<std::pair<uint32_t, uint64_t>> seen;
        const uint64_t h = vu1fast::fnv1a(vuCode + pc, std::min<uint32_t>(512u, 0x4000u - pc));
        if (seen.size() < 400 && seen.insert({pc, h}).second)
        {
            char path[512];
            std::snprintf(path, sizeof(path), "%s/miss_%04x_%016llx.bin", missDir, pc, static_cast<unsigned long long>(h));
            if (std::FILE *f = std::fopen(path, "wb"))
            {
                std::fwrite(&pc, 4, 1, f);
                std::fwrite(vuCode, 1, 0x4000, f);
                std::fclose(f);
            }
            std::fprintf(stderr, "[vu1] no native code for pc=%#x (%s)\n", pc, path);
        }
    }

    uint32_t RunVu1Native(uint32_t kind, uint32_t startPC, uint32_t top, uint32_t itop, VU1State &st, uint8_t *vuCode, uint8_t *vuData)
    {
        if (!g_vu1Disp)
        {
            g_vu1Ctx = new vu1fast::Ctx();
            g_vu1Disp = new vu1fast::Dispatcher(vuCode);
        }
        const uint64_t gen = g_rt->memory().getVU1CodeGeneration();
        if (gen != g_vu1Gen)
        {
            g_vu1Gen = gen;
            g_vu1Disp->invalidate();
        }
        const uint32_t pc = (kind == ps2x_capture::Vu1Mscal ? startPC : st.pc) & 0x3FF8u;
        if (!g_vu1Disp->find(pc))
        {
            g_vu1InterpCalls.fetch_add(1, std::memory_order_relaxed);
            Vu1Miss(pc, vuCode);
            return ps2x_capture::Vu1Interpret;
        }
        vu1fast::Ctx &c = *g_vu1Ctx;
        std::memcpy(c.vf, st.vf, sizeof(c.vf));
        for (int k = 0; k < 16; ++k)
            c.vi[k] = st.vi[k];
        std::memcpy(c.acc, st.acc, sizeof(c.acc));
        c.q = st.q; c.p = st.p; c.i = st.i; c.r = st.r;
        c.clip = st.clip; c.mac = st.mac; c.status = st.status;
        c.pc = pc; c.top = top; c.itop = itop;
        if (kind == ps2x_capture::Vu1Mscal)
        {
            c.vf[0][0] = c.vf[0][1] = c.vf[0][2] = 0.0f;
            c.vf[0][3] = 1.0f;
        }
        c.mem = vuData;
        c.sink = &g_vu1Sink;
        c.resetTiming();
        const uint64_t p0 = c.pairs;
        const bool complete = g_vu1Disp->run(c);
        g_vu1Pairs.fetch_add(c.pairs - p0, std::memory_order_relaxed);
        std::memcpy(st.vf, c.vf, sizeof(c.vf));
        for (int k = 0; k < 16; ++k)
            st.vi[k] = static_cast<int16_t>(c.vi[k]);
        std::memcpy(st.acc, c.acc, sizeof(c.acc));
        st.q = c.q; st.p = c.p; st.i = c.i; st.r = c.r;
        st.clip = c.clip; st.mac = c.mac;
        st.pc = c.pc; st.top = top; st.itop = itop;
        st.ebit = false; st.haltAfterDelaySlot = false; st.branchPending = false;
        st.stoppedByD = false; st.stoppedByT = false;
        if (complete)
        {
            g_vu1NativeCalls.fetch_add(1, std::memory_order_relaxed);
            return ps2x_capture::Vu1Handled;
        }
        g_vu1PartialCalls.fetch_add(1, std::memory_order_relaxed);
        Vu1Miss(c.pc & 0x3FF8u, vuCode);
        return ps2x_capture::Vu1Continue;
    }

    // Native VU0 micro programs (tools/vurecomp.py --vu0). RT_VU0CHECK=1 also runs the interpreter
    // on a copy and reports differences.
    std::atomic<uint64_t> g_vu0Calls{0}, g_vu0Native{0};
    vu1fast::Dispatcher *g_vu0Disp = nullptr;
    vu1fast::Ctx *g_vu0Ctx = nullptr;
    std::atomic<uint64_t> g_vu0Mismatch{0};
    uint32_t RunVu0Native(uint32_t pc, uint32_t itop, VU1State &st, uint8_t *vuCode, uint8_t *vuData)
    {
        if (!g_vu0Disp)
        {
            g_vu0Ctx = new vu1fast::Ctx();
            g_vu0Disp = new vu1fast::Dispatcher(vuCode, &vu1fast::programs0, 0x1000u);
        }
        // VU0 code memory has no generation counter: re-check residency only when it changed.
        static uint8_t lastCode[0x1000];
        if (std::memcmp(lastCode, vuCode, 0x1000) != 0)
        {
            std::memcpy(lastCode, vuCode, 0x1000);
            g_vu0Disp->invalidate();
        }
        if (!g_vu0Disp->find(pc))
            return ps2x_capture::Vu1Interpret;
        static const bool check = Env("RT_VU0CHECK") && std::atoi(Env("RT_VU0CHECK")) != 0;
        static uint8_t dataCopy[0x1000];
        static VU1State before;
        std::memcpy(dataCopy, vuData, 0x1000);
        before = st;
        vu1fast::Ctx &c = *g_vu0Ctx;
        std::memcpy(c.vf, st.vf, sizeof(c.vf));
        for (int k = 0; k < 16; ++k)
            c.vi[k] = st.vi[k];
        std::memcpy(c.acc, st.acc, sizeof(c.acc));
        c.q = st.q; c.p = st.p; c.i = st.i; c.r = st.r;
        c.clip = st.clip; c.mac = st.mac; c.status = st.status;
        c.pc = pc; c.top = 0; c.itop = itop;
        c.mem = vuData;
        c.memMask = 0x0FFFu;
        c.sink = nullptr;
        c.resetTiming();
        bool complete = false;
        for (int guard = 0; guard < 64; ++guard)
        {
            complete = g_vu0Disp->run(c);
            if (!complete || g_vu0Disp->budgetExits == 0)
                break;
            g_vu0Disp->budgetExits = 0;   // long program: keep going
        }
        if (!complete)
        {
            // Left recompiled code mid-program: undo and let the interpreter run it all.
            std::memcpy(vuData, dataCopy, 0x1000);
            st = before;
            return ps2x_capture::Vu1Interpret;
        }
        std::memcpy(st.vf, c.vf, sizeof(c.vf));
        for (int k = 0; k < 16; ++k)
            st.vi[k] = static_cast<int16_t>(c.vi[k]);
        std::memcpy(st.acc, c.acc, sizeof(c.acc));
        st.q = c.q; st.p = c.p; st.i = c.i; st.r = c.r;
        st.clip = c.clip; st.mac = c.mac; st.status = c.status;
        st.pc = c.pc;
        st.vf[0][0] = st.vf[0][1] = st.vf[0][2] = 0.0f;
        st.vf[0][3] = 1.0f;
        st.vi[0] = 0;
        g_vu0Native.fetch_add(1, std::memory_order_relaxed);
        static long logLeft = Env("RT_VU0LOG") ? std::atol(Env("RT_VU0LOG")) : 0;
        static const double logAt = Env("RT_VU0LOG_AT") ? std::atof(Env("RT_VU0LOG_AT")) : 0.0;
        if (logLeft > 0 && (logAt <= 0.0 || std::chrono::duration<double>(std::chrono::steady_clock::now() - g_bootTime).count() >= logAt))
        {
            --logLeft;
            std::fprintf(stderr, "[vu0log] pc=%#x vi1=%d in vf1=(%g %g %g %g) vf2=(%g %g %g %g) -> vf1=(%g %g %g %g) vf2=(%g %g %g %g)\n",
                         pc, before.vi[1], before.vf[1][0], before.vf[1][1], before.vf[1][2], before.vf[1][3],
                         before.vf[2][0], before.vf[2][1], before.vf[2][2], before.vf[2][3],
                         st.vf[1][0], st.vf[1][1], st.vf[1][2], st.vf[1][3], st.vf[2][0], st.vf[2][1], st.vf[2][2], st.vf[2][3]);
            if (before.vi[1] == 0 || before.vi[1] == 255)
            {
                std::fprintf(stderr, "  vi:");
                for (int k = 0; k < 16; ++k) std::fprintf(stderr, " %d", before.vi[k]);
                const uint32_t b4 = static_cast<uint16_t>(before.vi[4]) & 0xFFu;
                auto q = [&](const uint8_t *m, uint32_t a) { const int32_t *w = reinterpret_cast<const int32_t *>(m + ((a & 0xFFu) << 4)); std::fprintf(stderr, " [%d %d %d %d]", w[0], w[1], w[2], w[3]); };
                std::fprintf(stderr, "\n  in@vi4:"); for (int k = 0; k < 4; ++k) q(dataCopy, b4 + k);
                std::fprintf(stderr, "\n  out@vi4:"); for (int k = 0; k < 4; ++k) q(vuData, b4 + k);
                const uint32_t bi = static_cast<uint32_t>(reinterpret_cast<const int32_t *>(dataCopy + (b4 << 4))[3]) & 0xFFu;
                std::fprintf(stderr, "\n  bone@%u:", bi);
                for (int k = 0; k < 3; ++k) { const float *f = reinterpret_cast<const float *>(dataCopy + (((bi + k) & 0xFFu) << 4)); std::fprintf(stderr, " (%g %g %g %g)", f[0], f[1], f[2], f[3]); }
                std::fprintf(stderr, "\n");
            }
        }
        if (check)
        {
            static VU1Interpreter interp(VU1Interpreter::Unit::VU0);
            static std::vector<uint8_t> nativeData;
            nativeData.assign(vuData, vuData + 0x1000);
            std::vector<uint8_t> ref(dataCopy, dataCopy + 0x1000);
            interp.reset();
            interp.state() = before;
            interp.execute(vuCode, 0x1000, ref.data(), 0x1000, g_rt->gs(), &g_rt->memory(), pc, 0u, itop, 1u << 20);
            const VU1State &r = interp.state();
            bool same = std::memcmp(r.vf, st.vf, sizeof(st.vf)) == 0 && std::memcmp(r.acc, st.acc, sizeof(st.acc)) == 0 &&
                        r.q == st.q && r.mac == st.mac && r.clip == st.clip && (r.status & 0xFFF) == (st.status & 0xFFF) &&
                        ref == nativeData;
            for (int k = 1; k < 16 && same; ++k)
                same = static_cast<int16_t>(r.vi[k]) == static_cast<int16_t>(st.vi[k]);
            if (!same)
            {
                const uint64_t n = g_vu0Mismatch.fetch_add(1);
                if (n < 20)
                {
                    std::fprintf(stderr, "[vu0check] mismatch #%llu pc=%#x vi1=%d:", (unsigned long long)n, pc, before.vi[1]);
                    for (int k = 0; k < 32; ++k)
                        if (std::memcmp(r.vf[k], st.vf[k], 16))
                            std::fprintf(stderr, " vf%d(%g,%g,%g,%g vs %g,%g,%g,%g)", k, r.vf[k][0], r.vf[k][1], r.vf[k][2], r.vf[k][3],
                                         st.vf[k][0], st.vf[k][1], st.vf[k][2], st.vf[k][3]);
                    for (int k = 1; k < 16; ++k)
                        if (static_cast<int16_t>(r.vi[k]) != static_cast<int16_t>(st.vi[k]))
                            std::fprintf(stderr, " vi%d(%d vs %d)", k, r.vi[k], st.vi[k]);
                    if (r.mac != st.mac) std::fprintf(stderr, " mac(%x vs %x)", r.mac, st.mac);
                    if (r.clip != st.clip) std::fprintf(stderr, " clip(%x vs %x)", r.clip, st.clip);
                    if ((r.status & 0xFFF) != (st.status & 0xFFF)) std::fprintf(stderr, " status(%x vs %x)", r.status, st.status);
                    if (r.q != st.q) std::fprintf(stderr, " q(%g vs %g)", r.q, st.q);
                    if (ref != nativeData) std::fprintf(stderr, " data");
                    std::fprintf(stderr, "\n");
                }
            }
        }
        return ps2x_capture::Vu1Handled;
    }

    // VU0 micro programs (VCALLMS). RT_VU0DUMP=<dir> saves the 4 KB micro memory for each new
    // (entry, code) pair, the input for tools/vurecomp.py --vu0.
    uint32_t Vu0Hook(uint32_t startPC, uint32_t itop, VU1State &st, uint8_t *vuCode, uint8_t *vuData)
    {
        (void)itop; (void)st; (void)vuData;
        g_vu0Calls.fetch_add(1, std::memory_order_relaxed);
        static const char *dumpDir = Env("RT_VU0DUMP");
        if (dumpDir)
        {
            static std::set<std::pair<uint32_t, uint64_t>> seen;
            static std::map<uint32_t, uint64_t> perPc;
            const uint32_t pc = startPC & 0xFF8u;
            ++perPc[pc];
            const uint64_t h = vu1fast::fnv1a(vuCode, 0x1000);
            if (seen.size() < 200 && seen.insert({pc, h}).second)
            {
                char path[512];
                std::snprintf(path, sizeof(path), "%s/vu0_%03x_%016llx.bin", dumpDir, pc, static_cast<unsigned long long>(h));
                if (std::FILE *f = std::fopen(path, "wb"))
                {
                    std::fwrite(&pc, 4, 1, f);
                    std::fwrite(vuCode, 1, 0x1000, f);
                    std::fclose(f);
                }
                std::fprintf(stderr, "[vu0] new program pc=%#x code=%016llx\n", pc, static_cast<unsigned long long>(h));
            }
            static uint64_t n = 0;
            if ((++n & ((1u << 20) - 1)) == 0)
            {
                std::fprintf(stderr, "[vu0] calls by entry:");
                for (auto &[k, v] : perPc)
                    std::fprintf(stderr, " %#x=%llu", k, static_cast<unsigned long long>(v));
                std::fprintf(stderr, "\n");
            }
        }
        static const bool vu0Native = !Env("RT_VU0NATIVE") || std::atoi(Env("RT_VU0NATIVE")) != 0;
        if (!g_vu1Native || !vu0Native)
            return ps2x_capture::Vu1Interpret;
        return RunVu0Native(startPC & 0xFF8u, itop, st, vuCode, vuData);
    }

    uint32_t Vu1Hook(uint32_t phase, uint32_t kind, uint32_t startPC, uint32_t top, uint32_t itop,
                     void *state, uint8_t *vuCode, uint8_t *vuData)
    {
        VU1State &st = *static_cast<VU1State *>(state);
        if (kind == ps2x_capture::Vu0Callms)
            return Vu0Hook(startPC, itop, st, vuCode, vuData);
        // While a capture is armed but its start time has not come, keep running natively.
        static bool capStarted = false;
        if (g_vu1Cap && !capStarted && phase == ps2x_capture::Vu1Before &&
            std::chrono::duration<double>(std::chrono::steady_clock::now() - g_vu1Boot).count() >= g_vu1CapAt)
            capStarted = true;
        if (g_vu1Native && (!g_vu1Cap || !capStarted) && phase == ps2x_capture::Vu1Before)
            return RunVu1Native(kind, startPC, top, itop, st, vuCode, vuData);
        if (g_vu1Cap && g_vu1CapLeft)
        {
            if (phase == ps2x_capture::Vu1Before)
            {
                const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_vu1Boot).count();
                if (t >= g_vu1CapAt && (g_vu1CapCounter++ % g_vu1CapEvery) == 0)
                {
                    g_vu1CapActive = true;
                    g_vu1CapKicks.clear();
                    g_vu1CapRecord.clear();
                    const uint32_t pc = kind == ps2x_capture::Vu1Mscal ? startPC : st.pc;
                    const uint32_t hdr[4] = {kind, pc, top, itop};
                    Put(g_vu1CapRecord, hdr, sizeof(hdr));
                    Vu1CapState s0;
                    Vu1CapState_(st, s0);
                    Put(g_vu1CapRecord, &s0, sizeof(s0));
                    const uint32_t hasCode = (g_vu1CapCode.size() != PS2_VU1_CODE_SIZE ||
                                              std::memcmp(g_vu1CapCode.data(), vuCode, PS2_VU1_CODE_SIZE) != 0) ? 1u : 0u;
                    Put(g_vu1CapRecord, &hasCode, 4);
                    if (hasCode)
                    {
                        g_vu1CapCode.assign(vuCode, vuCode + PS2_VU1_CODE_SIZE);
                        Put(g_vu1CapRecord, vuCode, PS2_VU1_CODE_SIZE);
                    }
                    Put(g_vu1CapRecord, vuData, PS2_VU1_DATA_SIZE);
                }
            }
            else if (g_vu1CapActive)
            {
                g_vu1CapActive = false;
                Vu1CapState s1;
                Vu1CapState_(st, s1);
                Put(g_vu1CapRecord, &s1, sizeof(s1));
                Put(g_vu1CapRecord, vuData, PS2_VU1_DATA_SIZE);
                const uint32_t n = static_cast<uint32_t>(g_vu1CapKicks.size());
                Put(g_vu1CapRecord, &n, 4);
                for (const auto &k : g_vu1CapKicks)
                {
                    const uint32_t sz = static_cast<uint32_t>(k.size());
                    Put(g_vu1CapRecord, &sz, 4);
                    Put(g_vu1CapRecord, k.data(), sz);
                }
                std::fwrite(g_vu1CapRecord.data(), 1, g_vu1CapRecord.size(), g_vu1Cap);
                if (--g_vu1CapLeft == 0)
                {
                    std::fclose(g_vu1Cap);
                    g_vu1Cap = nullptr;
                    std::fprintf(stderr, "[vu1cap] done\n");
                }
            }
        }
        return ps2x_capture::Vu1Interpret;
    }

    // RT_SPRWATCH=lo-hi (hex guest addresses): log slow-path stores overlapping [lo,hi) with the
    // storing function's pc and $ra; '!' marks values containing exponent-255 floats.
    uint32_t g_watchLo = 0, g_watchHi = 0;
    std::atomic<uint64_t> g_watchHits{0};

    void StoreWatchHook(uint32_t vaddr, uint32_t size, uint64_t lo, uint64_t hi, uint32_t pc, uint32_t ra)
    {
        if (vaddr + size <= g_watchLo || vaddr >= g_watchHi)
            return;
        const uint64_t n = g_watchHits.fetch_add(1, std::memory_order_relaxed);
        if (n > 4000 && (n & 1023) != 0)
            return;
        bool bad = false;
        const uint64_t words[2] = {lo, hi};
        for (uint32_t i = 0; i < size / 4u; ++i)
        {
            const uint32_t w = static_cast<uint32_t>(words[i / 2] >> (32 * (i & 1)));
            bad |= (w & 0x7F800000u) == 0x7F800000u;
        }
        const uint64_t tick = g_rt ? g_rt->eeScheduler().currentVSyncTick() : 0;
        std::fprintf(stderr, "[sprw] #%llu v%llu %08x/%u %016llx%016llx pc=%08x ra=%08x%s\n",
                     (unsigned long long)n, (unsigned long long)tick, vaddr, size,
                     (unsigned long long)hi, (unsigned long long)lo, pc, ra, bad ? " !" : "");
    }

    void StartCapture(PS2Runtime &runtime)
    {
        if (const char *f = Env("RT_VU1CAP"); f && (g_vu1Cap = std::fopen(f, "wb")) != nullptr)
        {
            std::fwrite("VU1CAP01", 1, 8, g_vu1Cap);
            g_vu1CapAt = Env("RT_VU1CAP_AT") ? std::atof(Env("RT_VU1CAP_AT")) : 30.0;
            g_vu1CapLeft = Env("RT_VU1CAP_N") ? std::strtoull(Env("RT_VU1CAP_N"), nullptr, 10) : 2000;
            g_vu1CapEvery = Env("RT_VU1CAP_EVERY") ? std::max<uint64_t>(1, std::strtoull(Env("RT_VU1CAP_EVERY"), nullptr, 10)) : 1;
            ps2x_capture::g_vu1Hook.store(&Vu1Hook);
        }
        if (const char *n = Env("RT_VU1NATIVE"); !n || std::atoi(n) != 0)   // default on; RT_VU1NATIVE=0 interprets
        {
            g_vu1Native = true;
            ps2x_capture::g_vu1Hook.store(&Vu1Hook);
            std::fprintf(stderr, "[vu1] recompiled VU1 programs enabled\n");
        }
        if (const char *w = Env("RT_SPRWATCH"))
        {
            g_watchLo = static_cast<uint32_t>(std::strtoul(w, nullptr, 16));
            const char *dash = std::strchr(w, '-');
            g_watchHi = dash ? static_cast<uint32_t>(std::strtoul(dash + 1, nullptr, 16)) : g_watchLo + 16;
            ps2x_capture::g_storeHook.store(&StoreWatchHook);
        }
        g_rt = &runtime;
        g_bootTime = std::chrono::steady_clock::now();
        if (const char *f = Env("RT_CAPTURE"))
        {
            g_capFile = std::fopen(f, "wb");
            g_capAt = Env("RT_CAPTURE_AT") ? std::atof(Env("RT_CAPTURE_AT")) : 30.0;
            g_capFrames = Env("RT_CAPTURE_FRAMES") ? std::strtoull(Env("RT_CAPTURE_FRAMES"), nullptr, 10) : 4;
        }
        ps2x_capture::g_hook.store(&CaptureHook);
    }

    // RT_PEEK=spec[,spec...] printed by the watchdog. spec = [*]ADDR[+OFF]:NWORDS (hex);
    // a leading '*' reads a u32 pointer at ADDR first, then dumps NWORDS words at ptr+OFF.
    void PrintPeeks(PS2Runtime &runtime)
    {
        static const char *env = Env("RT_PEEK");
        if (!env)
            return;
        uint8_t *ram = runtime.memory().getRDRAM();
        auto rd = [&](uint32_t a) -> uint32_t {
            uint32_t v = 0;
            if ((a & 0xF0000000u) == 0x70000000u)
                std::memcpy(&v, runtime.memory().getScratchpad() + (a & 0x3FFFu), 4);
            else
                std::memcpy(&v, ram + (a & 0x1FFFFFCu), 4);
            return v;
        };
        std::string spec(env);
        size_t pos = 0;
        while (pos < spec.size())
        {
            size_t end = spec.find(',', pos);
            std::string item = spec.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            pos = end == std::string::npos ? spec.size() : end + 1;
            const bool deref = !item.empty() && item[0] == '*';
            const char *c = item.c_str() + (deref ? 1 : 0);
            char *e = nullptr;
            uint32_t addr = static_cast<uint32_t>(std::strtoul(c, &e, 16));
            uint32_t off = 0, n = 4;
            if (*e == '+')
                off = static_cast<uint32_t>(std::strtoul(e + 1, &e, 16));
            if (*e == ':')
                n = static_cast<uint32_t>(std::strtoul(e + 1, &e, 16));
            const uint32_t base = (deref ? rd(addr) : addr) + off;
            std::string line;
            char buf[48];
            for (uint32_t i = 0; i < n; ++i)
            {
                const uint32_t w = rd(base + 4 * i);
                float f;
                std::memcpy(&f, &w, 4);
                std::snprintf(buf, sizeof(buf), " %08x(%.4g)", w, f);
                line += buf;
            }
            std::fprintf(stderr, "[peek] %s @%08x:%s\n", item.c_str(), base, line.c_str());
        }
    }

    // RT_WATCH=<seconds>: periodically print the IOP emulator state.
    // RT_MATSCAN=<seconds>[,<repeat seconds>]: scan EE RAM for 4x4 matrices whose 3x3 part is
    // zero while the translation row looks like a world position (debugging collapsed models).
    void StartMatScan(PS2Runtime &runtime)
    {
        const char *env = Env("RT_MATSCAN");
        if (!env)
            return;
        const double at = std::atof(env);
        const char *comma = std::strchr(env, ',');
        const double every = comma ? std::atof(comma + 1) : 0.0;
        std::thread([&runtime, at, every]() {
            std::this_thread::sleep_for(std::chrono::duration<double>(at));
            for (;;)
            {
                const uint8_t *ram = runtime.memory().getRDRAM();
                int found = 0;
                for (uint32_t a = 0x100000; a + 64 <= 0x2000000 && found < 40; a += 16)
                {
                    float m[16];
                    std::memcpy(m, ram + a, 64);
                    bool zero3 = true;
                    for (int k = 0; k < 12 && zero3; ++k)
                        zero3 = m[k] == 0.0f && (k % 4 != 3 || true);
                    if (!zero3 || m[15] != 1.0f || std::fabs(m[12]) + std::fabs(m[13]) + std::fabs(m[14]) < 100.0f ||
                        !std::isfinite(m[12]) || std::fabs(m[12]) > 1e6f)
                        continue;
                    // skip runs of zero memory before: require the qword before not all zero? keep simple
                    std::fprintf(stderr, "[matscan] %08x t=(%g %g %g)\n", a, m[12], m[13], m[14]);
                    ++found;
                }
                std::fprintf(stderr, "[matscan] %d candidates\n", found);
                if (every <= 0.0)
                    return;
                std::this_thread::sleep_for(std::chrono::duration<double>(every));
            }
        }).detach();
    }

    void StartWatchdog(PS2Runtime &runtime)
    {
        const char *env = Env("RT_WATCH");
        if (!env)
            return;
        const int secs = std::max(1, std::atoi(env));
        std::thread([&runtime, secs]() {
            uint64_t lastInsn = 0;
            for (;;)
            {
                std::this_thread::sleep_for(std::chrono::seconds(secs));
                if (runtime.isStopRequested())
                    return;
                const ps2x::iop::DebugSnapshot s = runtime.iopDebugSnapshot();
                std::fprintf(stderr, "[watch] iop cycles=%llu insn=%llu (+%llu) modules=%u threads=%u rpcServers=%u\n",
                             (unsigned long long)s.emulatorCycles, (unsigned long long)s.emulatorInstructions,
                             (unsigned long long)(s.emulatorInstructions - lastInsn), s.emulatorLoadedModules,
                             s.emulatorThreads, s.emulatorRpcServers);
                lastInsn = s.emulatorInstructions;
                std::string counts;
                for (const Wrapped &w : g_wrapped)
                    if (w.addr && w.original && w.calls.load())
                        counts += std::string(" ") + w.what + "=" + std::to_string(w.calls.load());
                std::fprintf(stderr, "[watch] calls:%s\n", counts.c_str());
                std::fprintf(stderr, "[watch] gfx vsync=%llu vif1=%lluKB/%llu gif1=%lluKB/%llu gif2=%lluKB/%llu gif3=%lluKB/%llu\n",
                             (unsigned long long)runtime.eeScheduler().currentVSyncTick(),
                             (unsigned long long)(g_capBytes[0] >> 10), (unsigned long long)g_capPackets[0].load(),
                             (unsigned long long)(g_capBytes[1] >> 10), (unsigned long long)g_capPackets[1].load(),
                             (unsigned long long)(g_capBytes[2] >> 10), (unsigned long long)g_capPackets[2].load(),
                             (unsigned long long)(g_capBytes[3] >> 10), (unsigned long long)g_capPackets[3].load());
                if (g_vu1Native)
                    std::fprintf(stderr, "[watch] vu1 native=%llu partial=%llu interp=%llu pairs=%llu\n",
                                 (unsigned long long)g_vu1NativeCalls.load(), (unsigned long long)g_vu1PartialCalls.load(),
                                 (unsigned long long)g_vu1InterpCalls.load(), (unsigned long long)g_vu1Pairs.load());
                if (g_vu1Native)
                    std::fprintf(stderr, "[watch] vu0 calls=%llu native=%llu mismatches=%llu\n",
                                 (unsigned long long)g_vu0Calls.load(), (unsigned long long)g_vu0Native.load(),
                                 (unsigned long long)g_vu0Mismatch.load());
                {
                    uint64_t prims = 0, unsup = 0, frames = 0;
                    rufftrigger::GpuRendererStats(prims, unsup, frames);
                    if (frames)
                        std::fprintf(stderr, "[watch] gpu prims=%llu unsupported=%llu hostFrames=%llu\n",
                                     (unsigned long long)prims, (unsigned long long)unsup, (unsigned long long)frames);
                }
                PrintPeeks(runtime);
                if (Env("RT_WATCH_EE"))
                {
                    const EeKernelSnapshot ks = runtime.eeScheduler().snapshot();
                    for (const EeThreadSnapshot &t : ks.threads)
                        std::fprintf(stderr, "[watch] ee thread %d pc=%08x ra=%08x entry=%08x prio=%d status=%d wait=%d/%d\n",
                                     t.id, t.pc, t.ra, t.entry, t.currentPriority, static_cast<int>(t.status),
                                     static_cast<int>(t.waitReason), t.waitId);
                    for (const EeSemaphoreSnapshot &sm : ks.semaphores)
                        std::fprintf(stderr, "[watch] ee sema %d count=%d max=%d\n", sm.id, sm.count, sm.maxCount);
                }
                for (size_t i = 0; i < s.diagnostics.size() && i < 12; ++i)
                    std::fprintf(stderr, "[watch]   %s\n", s.diagnostics[s.diagnostics.size() - 1 - i].c_str());
            }
        }).detach();
    }

    // ps2_game_overrides::bindAddressHandler() installs the raw handler, but the recompiler's
    // own stub wrappers set ctx->pc = $ra *before* calling it. Handlers that block (RPC waits,
    // semaphores, CD sync) park the guest thread, and on resume it restarts at ctx->pc - i.e.
    // at the start of the bound function, re-issuing the call forever. Wrap every binding so
    // it behaves like a generated stub.
    constexpr size_t kMaxBound = 96;
    PS2Runtime::RecompiledFunction g_boundHandlers[kMaxBound] = {};

    template <size_t I>
    void BoundStub(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ctx->pc = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0));
        g_boundHandlers[I](rdram, ctx, runtime);
    }

    template <size_t... I>
    constexpr std::array<PS2Runtime::RecompiledFunction, sizeof...(I)> MakeBoundStubs(std::index_sequence<I...>)
    {
        return {&BoundStub<I>...};
    }
    const auto kBoundStubs = MakeBoundStubs(std::make_index_sequence<kMaxBound>{});

    bool BindWithReturn(PS2Runtime &runtime, uint32_t addr, const char *handler, size_t slot)
    {
        if (slot >= kMaxBound || !ps2_game_overrides::bindAddressHandler(runtime, addr, handler))
            return false;
        g_boundHandlers[slot] = runtime.lookupFunction(addr);   // the raw handler just installed
        return runtime.replaceFunction(addr, kBoundStubs[slot]);
    }

    void ApplyRuffTrigger(PS2Runtime &runtime)
    {
        StartCapture(runtime);
        rufftrigger::InstallGpuRenderer(runtime);
        rufftrigger::InstallAudio();
        rufftrigger::StartProfiler();
        StartWatchdog(runtime);
        StartMatScan(runtime);
        std::fprintf(stderr, "[rt] host rdram=%p\n", static_cast<void *>(runtime.memory().getRDRAM()));
        runtime.replaceFunction(0x002F3D98, &HleGetstat);
        runtime.replaceFunction(0x002F5C90, &HleSearchModuleByName);
        runtime.replaceFunction(0x002F6048, &HleSifLoadStartModule);
        runtime.replaceFunction(0x002F6028, &HleSifLoadModule);
        rufftrigger::InstallMemcard(runtime);
        if (!Env("RT_DELAY_CYCLES") || std::atoi(Env("RT_DELAY_CYCLES")) != 0)
            runtime.replaceFunction(0x00221F98u, &HleBusyDelay);
        rufftrigger::InstallMovies(runtime);
        rufftrigger::InstallDebug(runtime);
        rufftrigger::InstallSettings(runtime);
        if (const char *iso = Env("RT_ISO"))
        {
            PS2Runtime::IoPaths paths = PS2Runtime::getIoPaths();
            paths.cdImage = iso;
            PS2Runtime::setIoPaths(paths);
            std::fprintf(stderr, "[rt] CD image: %s\n", iso);
        }
        int ok = 0;
        size_t slot = 0;
        for (const Binding &b : kBindings)
        {
            if (BindWithReturn(runtime, b.addr, b.handler, slot++))
                ++ok;
            else
                std::fprintf(stderr, "[rt] bind FAILED %s@0x%08X\n", b.handler, b.addr);
        }
        if (const char *env = Env("RT_TRACE"))
        {
            size_t slot = 0;
            while (g_wrapped[slot].addr) ++slot;
            std::string list = env;
            size_t pos = 0, k = 0;
            while (pos < list.size() && slot < std::size(g_wrapped) && k < 48)
            {
                size_t end = list.find(',', pos);
                std::string item = list.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
                pos = end == std::string::npos ? list.size() : end + 1;
                if (item.empty()) continue;
                int arg = -1;
                size_t colon = item.find(':');
                if (colon != std::string::npos)
                {
                    arg = item[colon + 1] == 'r' ? -2 : std::atoi(item.c_str() + colon + 1);
                    item.resize(colon);
                }
                g_dynNames[k] = "f_" + item;
                g_wrapped[slot].addr = static_cast<uint32_t>(std::strtoul(item.c_str(), nullptr, 16));
                g_wrapped[slot].what = g_dynNames[k].c_str();
                g_wrapped[slot].strArg = arg;
                ++slot; ++k;
            }
        }
        g_traceExit = Env("RT_TRACE_EXIT") != nullptr;
        WrapAll(runtime, std::make_index_sequence<std::size(g_wrapped)>{});
        if (Env("RT_MATDBG") && (g_matMulOrig = runtime.lookupFunction(0x002C1CD8u)) != nullptr)
            runtime.replaceFunction(0x002C1CD8u, &MatMulDbg);
        if (Env("RT_MEMW"))
        {
            if ((g_memcpyOrig = runtime.lookupFunction(0x002DE284u)) != nullptr)
                runtime.replaceFunction(0x002DE284u, &MemWDbg<false>);
            if ((g_memsetOrig = runtime.lookupFunction(0x002DE42Cu)) != nullptr)
                runtime.replaceFunction(0x002DE42Cu, &MemWDbg<true>);
        }
        if ((g_projOrig = runtime.lookupFunction(0x001E9970u)) != nullptr)
        {
            const char *w = Env("RT_WIDESCREEN");
            const bool wide = !w || std::atoi(w) != 0;
            g_wideXSetting.store(wide ? 0.75f : 1.0f);
            runtime.replaceFunction(0x001E9970u, &ProjWide);
            if ((g_sceneOrig = runtime.lookupFunction(0x0025C610u)) != nullptr)
                runtime.replaceFunction(0x0025C610u, &SceneWide);
            if (wide)
                std::fprintf(stderr, "[rt] widescreen camera (16:9)\n");
        }
        if (const char *ps = Env("RT_PADSCRIPT"); ps && *ps)
        {
            std::string list = ps;
            for (size_t pos = 0; pos < list.size();)
            {
                size_t end = list.find(',', pos);
                if (end == std::string::npos) end = list.size();
                unsigned long long at = 0, len = 10;
                unsigned bits = 0;
                if (std::sscanf(list.substr(pos, end - pos).c_str(), "%llu:%x:%llu", &at, &bits, &len) >= 2)
                    g_padScript.push_back({at, len, static_cast<uint16_t>(bits)});
                pos = end + 1;
            }
            g_padRt = &runtime;
            ps2x_capture::g_padHook.store(&PadScript);
        }
        if (Env("RT_TEXLOG_AT") && (g_texReqOrig = runtime.lookupFunction(0x002962F8u)) != nullptr)
            runtime.replaceFunction(0x002962F8u, &TexReqDbg);
        if (Env("RT_FINDLOG") && (g_findOrig = runtime.lookupFunction(0x001E5588u)) != nullptr)
            runtime.replaceFunction(0x001E5588u, &FindDbg);
        if (Env("RT_MATDBG") && (g_nodeCtorOrig = runtime.lookupFunction(0x001D6B80u)) != nullptr)
            runtime.replaceFunction(0x001D6B80u, &NodeCtorDbg);
        if (Env("RT_MATDBG") && (g_nodeMatOrig = runtime.lookupFunction(0x001E9218u)) != nullptr)
            runtime.replaceFunction(0x001E9218u, &NodeMatDbg);
        std::fprintf(stderr, "[rt] Ruff Trigger override: %d/%zu SDK bindings\n", ok, std::size(kBindings));
    }
}

namespace rufftrigger
{
    void SetWidescreenCamera(bool on) { g_wideXSetting.store(on ? 0.75f : 1.0f); }
}

PS2_REGISTER_GAME_OVERRIDE("Ruff Trigger: The Vanocore Conspiracy (NTSC-U)", "SLUS_213.14", 0x00100008, 0,
                           ApplyRuffTrigger)
