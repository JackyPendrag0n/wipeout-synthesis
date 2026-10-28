// Wipeout Fusion (SLUS_204.62) game override, derived from the Ruff Trigger one.
// Ruff Trigger: The Vanocore Conspiracy (SLUS-213.14, NTSC-U) runtime override.
//
// Binds the statically linked Sony SDK libraries that the recompiled game would otherwise
// drive through SIF RPC servers that do not exist without a BIOS (libcdvd, libpad, libmtap),
// and wraps a few game functions to log boot milestones. Addresses were identified from the
// SIF RPC service IDs / RPC function numbers used by each routine and from libpad's
// sample state machine in f_002DC068 (see the port workspace notes).
#include "game_overrides.h"
#include "ps2_runtime.h"
#include "ps2_stubs.h"
#include "runtime/ps2_capture_hook.h"
#include "runtime/gs/gs_backend.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_vu1.h"
#include "vu1_fast.h"
#include "vu1_native_draw.h"
#include <xmmintrin.h>
#include <x86intrin.h>
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
extern "C"
{
#include <libavcodec/avcodec.h>
}

namespace vu1fast { uint32_t g_pendTarget = 0; }  // pending branch across code blocks (tools/vurecomp.py)
namespace wipeout { void InstallSettings(PS2Runtime &runtime); }

namespace rufftrigger
{
    void SetWidescreenCamera(bool on);
    void InstallSettings(PS2Runtime &runtime);
    void InstallGpuRenderer(PS2Runtime &runtime);
    void InstallAudio();
    void StartProfiler();
    void InstallMemcard(PS2Runtime &runtime);
    extern std::atomic<uint64_t> g_gameFrames;   // defined in rufftrigger_override.cpp
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
        // Wipeout Fusion has no named milestones yet; use RT_TRACE="addr[:argIndex],..."
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
            if (w.strArg == -3)
            {
                auto D = [&](int r) { const uint64_t v = static_cast<uint64_t>(_mm_extract_epi64(ctx->r[r], 0)); double d; std::memcpy(&d, &v, 8); return std::make_pair(v, d); };
                std::fprintf(stderr, "[rt] %s #%u a0=%016llx(%.17g) a1=%016llx(%.17g) a2=%016llx(%.17g) a3=%016llx(%.17g) ra=%x\n", w.what, n,
                             (unsigned long long)D(4).first, D(4).second, (unsigned long long)D(5).first, D(5).second,
                             (unsigned long long)D(6).first, D(6).second, (unsigned long long)D(7).first, D(7).second,
                             static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0)));
            }
            else if (w.strArg == -2)
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
        if (entry && g_traceExit && w.strArg == -3 && (n <= 400 || (n & (n - 1)) == 0))
        {
            const uint64_t v = static_cast<uint64_t>(_mm_extract_epi64(ctx->r[2], 0)); double d; std::memcpy(&d, &v, 8);
            std::fprintf(stderr, "[rt]   <- %s #%u v0=%016llx(%.17g)\n", w.what, n, (unsigned long long)v, d);
        }
        else if (entry && g_traceExit && (n <= 400 || (n & (n - 1)) == 0))
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
        // RT_PADFILE=path: the file holds hex button bits that stay pressed while it exists
        // (tests drive the menus by writing it).
        static const char *padFile = Env("RT_PADFILE");
        static uint64_t lastV = ~0ull;
        static uint16_t fileBits = 0;
        if (padFile && v != lastV)
        {
            lastV = v;
            fileBits = 0;
            if (std::FILE *f = std::fopen(padFile, "r"))
            {
                unsigned b = 0;
                if (std::fscanf(f, "%x", &b) == 1)
                    fileBits = static_cast<uint16_t>(b);
                std::fclose(f);
            }
        }
        return bits | fileBits;
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

    // SDK calls (sceCd*, scePad*, sceSif*, sceMc*, libc, ...) are bound by address through the
    // analyzer-generated config.toml stubs, which use the recompiler's own wrappers.
    const Binding kBindings[] = {
        {0, nullptr},
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
    // 0x002144E0 plays a boot movie (STARTUP.IPS, GOODTECH.IPS). The .IPS files are left out of the
    // packed WAD, so the player would wait forever on empty data: return as if the movie finished.
    void HleSkipBootMovie(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        setReturnS32(ctx, 0);
        ctx->pc = getRegU32(ctx, 31);
    }

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

    std::vector<std::vector<uint8_t>> *g_vvKicks = nullptr;   // RT_VU1VERIFY: interpreter kicks

    // RT_GFXCENSUS=secs: what feeds the GS. Every GIF packet is parsed and counted per source:
    // PATH1 by the VU1 program (and entry pc) that kicked it, PATH2/PATH3 by path. Vertices are
    // XYZ2/XYZ3/XYZF2/XYZF3 writes; primitive types come from PRIM fields and A+D PRIM writes.
    extern uint32_t g_vu1LastPc;
    extern const char *g_vu1LastProg;
    extern std::atomic<uint64_t> g_ndBatches, g_ndFallback;
    struct CensusRow { uint64_t packets = 0, verts = 0, image = 0, ad = 0; uint32_t prims = 0; };
    std::map<std::string, CensusRow> g_census;
    std::mutex g_censusMu;
    void Census(uint32_t kind, const uint8_t *d, uint32_t n)
    {
        char key[96];
        uint64_t magic;
        std::memcpy(&magic, d, 8);
        if (magic == 0x5854524556564E52ull)
        {
            uint64_t cnt;
            std::memcpy(&cnt, d + 8, 8);
            std::lock_guard<std::mutex> lk(g_censusMu);
            CensusRow &r = g_census["path1 native strips"];
            ++r.packets;
            r.verts += cnt & 0xFFFFFFFFu;
            return;
        }
        if (kind == ps2x_capture::GifPath1)
            std::snprintf(key, sizeof(key), "path1 %s @%#x", g_vu1LastProg ? g_vu1LastProg : "?", g_vu1LastPc);
        else
            std::snprintf(key, sizeof(key), "path%u", kind - 0x10u);
        std::lock_guard<std::mutex> lk(g_censusMu);
        CensusRow &r = g_census[key];
        ++r.packets;
        uint32_t q = 0;
        const uint32_t nq = n / 16;
        while (q < nq)
        {
            uint64_t lo, hi;
            std::memcpy(&lo, d + q * 16, 8);
            std::memcpy(&hi, d + q * 16 + 8, 8);
            ++q;
            const uint32_t nloop = lo & 0x7FFF, flg = (lo >> 58) & 3;
            uint32_t nreg = (lo >> 60) & 15;
            if (!nreg) nreg = 16;
            if ((lo >> 46) & 1)
                r.prims |= 1u << ((lo >> 47) & 7);
            if (flg == 0)
            {
                for (uint32_t l = 0; l < nloop; ++l)
                    for (uint32_t k = 0; k < nreg && q < nq; ++k, ++q)
                    {
                        const uint32_t reg = (hi >> (4 * k)) & 15;
                        if (reg == 5 || reg == 4 || reg == 0xD || reg == 0xC)
                            ++r.verts;
                        else if (reg == 0xE)
                        {
                            ++r.ad;
                            uint64_t v, a;
                            std::memcpy(&v, d + q * 16, 8);
                            std::memcpy(&a, d + q * 16 + 8, 8);
                            if ((a & 0xFF) == 0)
                                r.prims |= 1u << (v & 7);
                            else if ((a & 0xFF) == 5 || (a & 0xFF) == 4 || (a & 0xFF) == 0xD || (a & 0xFF) == 0xC)
                                ++r.verts;
                        }
                    }
            }
            else if (flg == 1)
            {
                uint32_t regs = 0;
                for (uint32_t k = 0; k < nreg; ++k)
                {
                    const uint32_t reg = (hi >> (4 * k)) & 15;
                    regs += reg == 5 || reg == 4 || reg == 0xD || reg == 0xC;
                }
                r.verts += (uint64_t)regs * nloop;
                q += (nloop * nreg + 1) / 2;
            }
            else
            {
                r.image += nloop * 16ull;
                q += nloop;
            }
        }
    }
    void CensusReport(double secs)
    {
        std::map<std::string, CensusRow> rows;
        {
            std::lock_guard<std::mutex> lk(g_censusMu);
            rows.swap(g_census);
        }
        std::vector<std::pair<std::string, CensusRow>> v(rows.begin(), rows.end());
        std::sort(v.begin(), v.end(), [](auto &a, auto &b) { return a.second.verts > b.second.verts; });
        uint64_t tv = 0;
        for (auto &[k, r] : v) tv += r.verts;
        std::fprintf(stderr, "[census] %.0f s: %.0f vertices/s; native strips: %llu batches, %llu fell back to the VU\n", secs, tv / secs,
                     (unsigned long long)g_ndBatches.exchange(0), (unsigned long long)g_ndFallback.exchange(0));
        static const char *kPrim = "PLlTSFs-";   // point line strip tri strip fan sprite
        for (auto &[k, r] : v)
        {
            char pr[9] = {};
            for (int b = 0, j = 0; b < 8; ++b) if (r.prims >> b & 1) pr[j++] = kPrim[b];
            std::fprintf(stderr, "[census]  %5.1f%% %-44s pk/s %7.0f vtx/s %8.0f img KB/s %6.0f a+d/s %7.0f prim %s\n",
                         tv ? 100.0 * r.verts / tv : 0.0, k.c_str(), r.packets / secs, r.verts / secs, r.image / secs / 1024, r.ad / secs, pr);
        }
    }

    void CaptureHook(uint32_t kind, const uint8_t *data, uint32_t size)
    {
        static const bool census = Env("RT_GFXCENSUS") != nullptr;
        if (census && kind >= ps2x_capture::GifPath1 && kind <= ps2x_capture::GifPath3)
            Census(kind, data, size);
        if (kind == ps2x_capture::GifPath1 && g_vu1CapActive)
            g_vu1CapKicks.emplace_back(data, data + size);
        if (kind == ps2x_capture::GifPath1 && g_vvKicks)
            g_vvKicks->emplace_back(data, data + size);
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
    uint32_t g_vu1LastPc = 0;
    const char *g_vu1LastProg = nullptr;
    std::vector<std::vector<uint8_t>> *g_ncKicks = nullptr;   // RT_NATIVECHECK: kicks of the checked run
    extern vu1fast::Ctx *g_vu1Ctx;
    struct GifKickSink : vu1fast::KickSink
    {
        void kick(const uint8_t *p, uint32_t n) override
        {
            static int bigLeft = Env("RT_BIGKICK") ? std::atoi(Env("RT_BIGKICK")) : 0;
            if (bigLeft > 0 && n > 0x10000u)
            {
                --bigLeft;
                uint64_t t[2];
                std::memcpy(t, p, 16);
                std::fprintf(stderr, "[bigkick] %u bytes, program pc=%#x tag=%016llx %016llx top=%#x itop=%#x vi:", n, g_vu1LastPc,
                             (unsigned long long)t[1], (unsigned long long)t[0], g_vu1Ctx->top, g_vu1Ctx->itop);
                for (int k = 0; k < 16; ++k) std::fprintf(stderr, " %x", (unsigned)(uint16_t)g_vu1Ctx->vi[k]);
                std::fprintf(stderr, "\n  @top:");
                for (int k = 0; k < 8; ++k) { uint32_t w[4]; std::memcpy(w, g_vu1Ctx->mem + (((g_vu1Ctx->top + k) & 0x3FFu) << 4), 16); std::fprintf(stderr, " [%08x %08x %08x %08x]", w[0], w[1], w[2], w[3]); }
                std::fprintf(stderr, "\n");
            }
            if (g_ncKicks)
                g_ncKicks->emplace_back(p, p + n);
            g_rt->memory().submitGifPacket(GifPathId::Path1, p, n);
        }
    };
    bool g_vu1Native = false;
    vu1fast::Ctx *g_vu1Ctx = nullptr;  // (declared above)
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
        const uint64_t h = vu1fast::fnv1a(vuCode, 0x4000u);   // whole micro memory: overlays make variants
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

    // RT_NATIVECHECK=1: native renderer milestone 1. For each batch of the strip program in
    // overlay group 3841391935 (entry 0x1410: header at TOP, then XYZ / RGBA / STQ per vertex,
    // transformed by vf1..vf4), compute the vertices natively in float and compare them with
    // the GIF packet the VU program kicks (STQ, RGBA, XYZ per vertex after the tag).
    struct NativeCheck
    {
        float m[4][4], cm[4][4];
        std::vector<uint8_t> in;
        uint32_t count = 0;
        std::vector<std::vector<uint8_t>> kicks;
        uint64_t batches = 0, skipped = 0, offscreen = 0, verts = 0, exact = 0, rgbaBad = 0, stqBad = 0, zBad = 0;
        uint64_t hist[6] = {};   // |dx|+|dy| in 1/16 px: 0, 1, 2, 3-4, 5-16, more
        double t0 = 0;
    } g_nc;

    void NativeCheckBefore(const vu1fast::Ctx &c, uint32_t top, const uint8_t *vuData)
    {
        std::memcpy(g_nc.m, c.vf + 1, sizeof(g_nc.m));
        std::memcpy(g_nc.cm, c.vf + 13, sizeof(g_nc.cm));
        uint32_t hdr[4];
        std::memcpy(hdr, vuData + ((top & 0x3FFu) << 4), 16);
        g_nc.count = hdr[0] & 0x7FFF;
        g_nc.in.resize((1 + 3 * g_nc.count) * 16);
        for (uint32_t q = 0; q < 1 + 3 * g_nc.count; ++q)
            std::memcpy(g_nc.in.data() + q * 16, vuData + (((top + q) & 0x3FFu) << 4), 16);
        g_nc.kicks.clear();
        g_ncKicks = &g_nc.kicks;
    }

    void NativeCheckAfter()
    {
        g_ncKicks = nullptr;
        const uint32_t n = g_nc.count;
        if (g_nc.kicks.empty() || g_nc.kicks[0].size() < (1 + 3 * n) * 16 || n == 0)
        {
            ++g_nc.skipped;
            return;
        }
        const uint8_t *out = g_nc.kicks[0].data();
        uint64_t tag;
        std::memcpy(&tag, out, 8);
        {
            // What the VU did with the batch: extra kicks (clipper) / ADC vertices / native clip test.
            static uint64_t st[2][2][2] = {};   // [extra kicks][any ADC][native says outside]
            static uint64_t cnt = 0;
            bool adc = false, outside = false;
            for (uint32_t k = 0; k < n && 16 + k * 48 + 48 <= g_nc.kicks[0].size(); ++k)
            {
                uint32_t w3;
                std::memcpy(&w3, out + 16 + k * 48 + 44, 4);
                adc |= (w3 >> 15) & 1;
                float pos[4];
                std::memcpy(pos, g_nc.in.data() + (1 + 3 * k) * 16, 16);
                float cp[4];
                for (int i = 0; i < 4; ++i)
                    cp[i] = ((g_nc.cm[3][i] + g_nc.cm[2][i] * pos[2]) + g_nc.cm[1][i] * pos[1]) + g_nc.cm[0][i] * pos[0];
                const float lim = std::fabs(cp[3]);
                outside |= !(std::fabs(cp[0]) < lim && std::fabs(cp[1]) < lim && std::fabs(cp[2]) < lim);
            }
            ++st[g_nc.kicks.size() > 1][adc][outside];
            // Candidate rule: ADC on vertex k (k >= 2) when vertices k-2..k share an outside plane.
            static uint64_t ruleOk[4] = {}, ruleBad[4] = {}, crossing[4] = {};
            uint32_t fl[3] = {0, 0, 0};
            bool ok = true, cross = false;
            uint32_t hw;
            std::memcpy(&hw, g_nc.in.data() + 12, 4);
            for (uint32_t k = 0; k < n && 16 + k * 48 + 48 <= g_nc.kicks[0].size(); ++k)
            {
                float pos[4], cp[4];
                std::memcpy(pos, g_nc.in.data() + (1 + 3 * k) * 16, 16);
                for (int i = 0; i < 4; ++i)
                    cp[i] = ((g_nc.cm[3][i] + g_nc.cm[2][i] * pos[2]) + g_nc.cm[1][i] * pos[1]) + g_nc.cm[0][i] * pos[0];
                const float aw = std::fabs(cp[3]);
                const uint32_t f = (cp[0] > aw) | (cp[0] < -aw) << 1 | (cp[1] > aw) << 2 | (cp[1] < -aw) << 3 | (cp[2] > aw) << 4 | (cp[2] < -aw) << 5;
                fl[0] = fl[1]; fl[1] = fl[2]; fl[2] = f;
                const bool tested = (hw & 3) == 3 ? k % 3 == 2 : k >= 2;   // mode 3: one test per triangle
                const bool rule = tested && (fl[0] & fl[1] & fl[2]) != 0;
                if (tested && !rule && (fl[0] | fl[1] | fl[2]) != 0) cross = true;
                uint32_t w3;
                std::memcpy(&w3, out + 16 + k * 48 + 44, 4);
                if (rule != (((w3 >> 15) & 1) != 0)) ok = false;
            }
            (ok ? ruleOk : ruleBad)[hw & 3]++;
            if (cross) crossing[hw & 3]++;
            if ((cnt & 0xFFFF) == 0xFFFF)
                std::fprintf(stderr, "[ncheck] adc-rule by mode: ok %llu/%llu/%llu/%llu bad %llu/%llu/%llu/%llu crossing %llu/%llu/%llu/%llu\n",
                             (unsigned long long)ruleOk[0], (unsigned long long)ruleOk[1], (unsigned long long)ruleOk[2], (unsigned long long)ruleOk[3],
                             (unsigned long long)ruleBad[0], (unsigned long long)ruleBad[1], (unsigned long long)ruleBad[2], (unsigned long long)ruleBad[3],
                             (unsigned long long)crossing[0], (unsigned long long)crossing[1], (unsigned long long)crossing[2], (unsigned long long)crossing[3]);
            if ((++cnt & 0xFFFF) == 0)
                std::fprintf(stderr, "[ncheck] [kicks>1][adc][outside]: 000=%llu 001=%llu 010=%llu 011=%llu 100=%llu 101=%llu 110=%llu 111=%llu\n",
                             (unsigned long long)st[0][0][0], (unsigned long long)st[0][0][1], (unsigned long long)st[0][1][0], (unsigned long long)st[0][1][1],
                             (unsigned long long)st[1][0][0], (unsigned long long)st[1][0][1], (unsigned long long)st[1][1][0], (unsigned long long)st[1][1][1]);
        }
        if ((tag & 0x7FFF) != n)
        {
            ++g_nc.skipped;   // the clipper rewrote the batch
            return;
        }
        ++g_nc.batches;
        const float (*m)[4] = g_nc.m;
        for (uint32_t k = 0; k < n; ++k)
        {
            float pos[4], rgba[4], stq[4];
            std::memcpy(pos, g_nc.in.data() + (1 + 3 * k) * 16, 16);
            std::memcpy(rgba, g_nc.in.data() + (2 + 3 * k) * 16, 16);
            std::memcpy(stq, g_nc.in.data() + (3 + 3 * k) * 16, 16);
            float p[4];
            for (int i = 0; i < 4; ++i)
                p[i] = ((m[3][i] + m[2][i] * pos[2]) + m[1][i] * pos[1]) + m[0][i] * pos[0];
            const float q = 1.0f / p[3];
            const uint8_t *o = out + 16 + k * 48;
            float ostq[4]; uint32_t orgba[4]; int32_t oxyz[4];
            std::memcpy(ostq, o, 16); std::memcpy(orgba, o + 16, 16); std::memcpy(oxyz, o + 32, 16);
            // Vertices outside the GS's 4096-pixel coordinate space never reach the screen
            // (the VU clips or culls them); there both sides' rounding is magnified near w = 0.
            if (oxyz[0] < 0 || oxyz[0] > 0xFFFF || oxyz[1] < 0 || oxyz[1] > 0xFFFF)
            {
                ++g_nc.offscreen;
                continue;
            }
            ++g_nc.verts;
            const int32_t nx = (int32_t)(p[0] * q * 16.0f), ny = (int32_t)(p[1] * q * 16.0f), nz = (int32_t)(p[2] * q * 16.0f);
            const uint32_t d = (uint32_t)(std::abs(nx - oxyz[0]) + std::abs(ny - oxyz[1]));
            g_nc.hist[d == 0 ? 0 : d == 1 ? 1 : d == 2 ? 2 : d <= 4 ? 3 : d <= 16 ? 4 : 5]++;
            if (std::abs(nz - oxyz[2]) > 64) ++g_nc.zBad;
            if (std::memcmp(rgba, orgba, 16) != 0) ++g_nc.rgbaBad;
            for (int i = 0; i < 3; ++i)
            {
                const float e = stq[i] * q, g = ostq[i];
                if (std::fabs(e - g) > 1e-4f * std::max(1.0f, std::fabs(g))) { ++g_nc.stqBad; break; }
            }
            static int shown = 0;
            if (d > 16 && shown < 8)
            {
                ++shown;
                std::fprintf(stderr, "[ncheck] vertex %u/%u native (%d %d %d) vu (%d %d %d) pos (%g %g %g %g)\n", k, n, nx, ny, nz,
                             oxyz[0], oxyz[1], oxyz[2], pos[0], pos[1], pos[2], pos[3]);
            }
        }
        if ((g_nc.batches & 0x3FFF) == 0)
        {
            const double v = double(std::max<uint64_t>(1, g_nc.verts));
            std::fprintf(stderr, "[ncheck] batches=%llu skipped(clipped)=%llu offscreen=%llu verts=%llu  xy err (1/16 px) 0:%.2f%% 1:%.2f%% 2:%.2f%% 3-4:%.3f%% 5-16:%.3f%% >16:%.4f%%  z>4px:%.3f%% rgba:%llu stq:%.3f%%\n",
                         (unsigned long long)g_nc.batches, (unsigned long long)g_nc.skipped, (unsigned long long)g_nc.offscreen, (unsigned long long)g_nc.verts,
                         100 * g_nc.hist[0] / v, 100 * g_nc.hist[1] / v, 100 * g_nc.hist[2] / v, 100 * g_nc.hist[3] / v,
                         100 * g_nc.hist[4] / v, 100 * g_nc.hist[5] / v, 100 * g_nc.zBad / v, (unsigned long long)g_nc.rgbaBad, 100 * g_nc.stqBad / v);
        }
    }

    // Native renderer (on by default, RT_NATIVEDRAW=0 turns it off): the drawing loops of the
    // busiest VU1 programs run here instead of on the VU (vu1_native_draw.cpp). Each computes the
    // exact GIF packet the VU program would kick; its vertices go to the GS as one native vertex
    // packet (GS::tryProcessNativeVertexPacket) through PATH1, so they keep their place among the
    // GIF paths. Batches the VU would clip, and tags of another shape, still run on the VU.
    // RT_NATIVEDRAW_MASK=<hex> picks the programs (bit = index in g_ndProgs).
    struct NdProg
    {
        uint32_t id, pc, resume;   // program, entry, pc the VU leaves for the next batch
        int kind;                  // 0 clipped strip, 1 lit clipped strip, 2 plain strip, 3 lit strip, 4 particles, 5 sprite, 6 kick as is, 7 env-mapped strip, 8 clipped strip of stride 4, 9 setup, 10 effect sprites
        const char *name;
    };
    const NdProg g_ndProgs[] = {
        {3841391935u, 0x1410u, 0x1410u, 0, "clip-strip 3841391935"},
        {1920671007u, 0x2540u, 0x2540u, 1, "lit-clip-strip 1920671007"},
        {1604436274u, 0x0038u, 0x0158u, 2, "strip 1604436274"},
        {1604436274u, 0x0158u, 0x0158u, 2, "strip 1604436274"},
        {1331843391u, 0x0d58u, 0x0ee0u, 3, "lit-strip 1331843391"},
        {1331843391u, 0x0ee0u, 0x0ee0u, 3, "lit-strip 1331843391"},
        {939320750u, 0x3218u, 0x3218u, 4, "particles 939320750"},
        {1920694623u, 0x3058u, 0x3058u, 5, "sprite 1920694623"},
        {1920694623u, 0x31a0u, 0x31a0u, 6, "kick 1920694623"},
        {1159117218u, 0x0380u, 0x05d8u, 7, "env-strip 1159117218"},
        {1159117218u, 0x05d8u, 0x05d8u, 7, "env-strip 1159117218"},
        // Second entries of the same loops (they only skip the branch into them): 0x1268 runs the
        // 0x1410 loop, 0x2268 the 0x2540 one; 0x10c0 runs a copy of the 0x1410 loop that reads
        // four qwords per vertex.
        {3841391935u, 0x1268u, 0x1410u, 0, "clip-strip 3841391935"},
        {1920671007u, 0x2268u, 0x2540u, 1, "lit-clip-strip 1920671007"},
        {3841391935u, 0x10c0u, 0x10c0u, 8, "clip-strip4 3841391935"},
        {1331843391u, 0x0f18u, 0x10c0u, 8, "clip-strip4 1331843391"},
        // Setup entries (resume = the pc after the E bit's delay slot).
        {3841391935u, 0x1240u, 0x1268u, 9, "setup 3841391935@1240"},
        {3841391935u, 0x1258u, 0x1268u, 9, "setup 3841391935@1258"},
        {1332105535u, 0x2e20u, 0x2e48u, 9, "setup 1332105535@2e20"},
        {1332105535u, 0x2e48u, 0x2e58u, 9, "setup 1332105535@2e48"},
        {1332105535u, 0x2ff8u, 0x3058u, 9, "setup 1332105535@2ff8"},
        {1331843391u, 0x0ef0u, 0x0f18u, 9, "setup 1331843391@ef0"},
        {1331843391u, 0x0d28u, 0x0d58u, 9, "setup 1331843391@d28"},
        {1331843391u, 0x0d40u, 0x0d58u, 9, "setup 1331843391@d40"},
        {1585565147u, 0x0168u, 0x0198u, 9, "setup 1585565147@168"},
        {939320750u, 0x31b8u, 0x3218u, 9, "setup 939320750@31b8"},
        {1920671007u, 0x2258u, 0x2268u, 9, "setup 1920671007@2258"},
        {1920671007u, 0x1f90u, 0x1fa8u, 9, "setup 1920671007@1f90"},
        {1008206959u, 0x0368u, 0x0380u, 9, "setup 1008206959@368"},
        {446200663u, 0x0020u, 0x0038u, 9, "setup 446200663@20"},
        {1920694623u, 0x3190u, 0x31a0u, 9, "setup 1920694623@3190"},
        {1821510323u, 0x31b8u, 0x31f8u, 10, "effect 1821510323@31b8"},
        {1821510323u, 0x31f8u, 0x31f8u, 10, "effect 1821510323@31f8"},
    };
    constexpr int kNdKinds = 11;
    constexpr int kNdProgs = sizeof(g_ndProgs) / sizeof(g_ndProgs[0]);
    std::atomic<uint64_t> g_ndBatches{0}, g_ndFallback{0}, g_ndVerts{0};
    bool g_nativeDraw = false;
    const bool g_ndCheck = std::getenv("RT_NATIVECHECK") != nullptr;   // compare instead of drawing
    uint32_t g_ndMask = ~0u;
    std::vector<uint8_t> g_ndExact, g_ndPacket;
    std::vector<uint32_t> g_ndSizes;   // the byte length of each packet in g_ndExact

    const NdProg *NdFind(uint32_t id, uint32_t pc)
    {
        for (int k = 0; k < kNdProgs; ++k)
            if (g_ndProgs[k].id == id && g_ndProgs[k].pc == pc)
                return &g_ndProgs[k];
        return nullptr;
    }

    // The exact packets the VU would kick into g_ndExact, back to back, their lengths in g_ndSizes.
    bool NdRun(const NdProg &np, const vu1fast::Ctx &c, uint32_t top, uint8_t *vuData, vu1native::WriteBack &wb, bool draw, uint8_t *checkMem = nullptr)
    {
        uint8_t *const memOut = draw ? vuData : checkMem;   // where the program's VU memory stores go
        bool ok = false;
        switch (np.kind)
        {
        case 0: return vu1native::ClippedStrip(c, top, vuData, false, g_ndExact, g_ndSizes);
        case 1: return vu1native::ClippedStrip(c, top, vuData, true, g_ndExact, g_ndSizes, memOut);
        case 8: return vu1native::ClippedStrip(c, top, vuData, false, g_ndExact, g_ndSizes, nullptr, true);
        case 2: ok = vu1native::PlainStrip(c, top, vuData, g_ndExact); break;
        case 3: ok = vu1native::LitStrip(c, top, vuData, g_ndExact, wb); break;
        case 4: return vu1native::Particles(c, top, vuData, g_ndExact, g_ndSizes, wb, memOut);
        case 5:
            ok = vu1native::Sprite(c, top, vuData, g_ndExact, wb, memOut);
            g_ndSizes.clear();
            if (!g_ndExact.empty())
                g_ndSizes.push_back((uint32_t)g_ndExact.size());
            return ok;
        case 6: ok = vu1native::Passthrough(top, vuData, g_ndExact, wb); break;
        case 7: ok = vu1native::EnvStrip(c, top, vuData, g_ndExact, wb, memOut); break;
        case 9:
        {
            // The subroutines these call live in other overlays: only when that code is loaded.
            auto codeIs = [](uint32_t at, uint32_t id) { const vu1fast::Program *p = g_vu1Disp->find(at); return p && p->id == id; };
            const uint32_t pc = np.pc;
            if ((pc == 0x1240 || pc == 0x2e20) && !codeIs(0x1500, 3841391935u))
                return false;
            if ((pc == 0x0ef0 || pc == 0x0d28) && !(codeIs(0x26c0, 1920671007u) && codeIs(0x2800, 1332105535u)))
                return false;
            if (pc == 0x2ff8 && !codeIs(0x3000, 1920694623u))
                return false;
            g_ndExact.clear();
            g_ndSizes.clear();
            return vu1native::Setup(c, np.id, pc, vuData, wb, memOut);
        }
        case 10: return vu1native::EffectSprites(c, np.pc, g_ndExact, g_ndSizes, wb, memOut);
        }
        g_ndSizes.assign(1, (uint32_t)g_ndExact.size());
        return ok;
    }

    // GPU transform (RT_GPUMODELS, on with the GPU renderer): a batch of the strip programs
    // (kinds 0, 1, 8: ClippedStrip; 2: PlainStrip; 3: LitStrip; 7: EnvStrip) is sent to the GS as a
    // model draw (GSModelPacket): the raw input vertices, keyed by the packet cache's batch id
    // (batches outside cached packets: a transient key, the mesh sent every time), and the
    // matrices and lights the program would have used. The renderer does the transform, lighting
    // and clipping in its model shader. The program's leftovers in VU memory
    // (the lit colours of kind 1, the env-mapped STQs and output of kind 7) are not written:
    // nothing reads them before the next unpack.
    std::vector<uint8_t> g_modelPkt;
    bool NdModel(const NdProg &np, const vu1fast::Ctx &c, uint32_t top, uint8_t *mem, vu1native::WriteBack &wb)
    {
        // Batches outside cached packets go as transient meshes (key bit 63, sent with each draw).
        static uint64_t transientSerial = 0;
        static const bool transientOn = !(Env("RT_GPUMODELTRANSIENT") && std::atoi(Env("RT_GPUMODELTRANSIENT")) == 0);
        uint64_t id = ps2x_capture::g_vif1BatchId;
        if (!g_gsModelSupport.load(std::memory_order_relaxed) || (!id && !transientOn))
            return false;
        const int kind = np.kind;
        static const uint32_t kinds = Env("RT_GPUMODELKINDS") ? static_cast<uint32_t>(std::strtoul(Env("RT_GPUMODELKINDS"), nullptr, 16)) : 0x18Fu;
        if ((kind != 0 && kind != 1 && kind != 8 && kind != 2 && kind != 3 && kind != 7) || !((kinds >> kind) & 1))
            return false;
        if (kind == 3 && (c.vf[5][3] != 0.0f || c.vf[6][3] != 0.0f || c.vf[7][3] != 0.0f))
            return false;   // the lit strip's carried light term (vf26 * l.w) must vanish
        uint32_t hdr[4];
        std::memcpy(hdr, mem + ((top & 0x3FFu) << 4), 16);
        const uint64_t lo = static_cast<uint64_t>(hdr[0]) | static_cast<uint64_t>(hdr[1]) << 32;
        const uint32_t regs = hdr[2] & 0xFFFu, n = static_cast<uint32_t>(lo & 0x7FFF);
        if (((lo >> 58) & 3) != 0 || ((lo >> 60) & 15) != 3 || (regs != 0x512u && regs != 0x412u) || n < 3)
            return false;
        const uint32_t prim = (lo >> 47) & 7;
        if (!((lo >> 46) & 1) || prim < 3 || prim > 5)
            return false;   // the GS primitive must be known and a triangle type
        if ((kind == 2 || kind == 3) && n != (hdr[0] & static_cast<uint32_t>(static_cast<uint16_t>(c.vi[10]))))
            return false;
        const uint32_t stride = kind == 1 || kind == 8 || kind == 3 || kind == 7 ? 4u : 3u;
        if ((top & 0x3FFu) + 1 + stride * n > 0x400u)
            return false;
        GSModelPacket m{};
        m.magic = kGsModelMagic;
        std::memcpy(m.tag, hdr, 16);
        m.key = id ? id : (1ull << 63) | ++transientSerial;
        m.n = n;
        m.stride = stride;
        m.flags = (kind == 1 || kind == 3 ? GSModelLit : 0u) | (kind == 7 ? GSModelEnv : 0u) | (regs == 0x412u ? GSModelXyzf : 0u) |
                  (kind != 2 && kind != 3 ? GSModelClip : 0u);
        m.dataBytes = n * stride * 16u;
        std::memcpy(m.m, c.vf[1], 64);
        std::memcpy(m.cm, c.vf[13], 64);
        std::memcpy(m.l, c.vf[5], 48);
        std::memcpy(m.amb, c.vf[8], 16);
        std::memcpy(m.lc, mem + 8 * 16, kind == 3 ? 48 : 64);
        if (kind == 3)
        {
            // The program leaves the last vertex's light in vf26.
            float nrm[4];
            std::memcpy(nrm, mem + (((top & 0x3FFu) + 1 + stride * (n - 1) + 3) << 4), 16);
            float l[3];
            for (int i = 0; i < 3; ++i)
                l[i] = std::max(c.vf[5][i] * nrm[0] + c.vf[6][i] * nrm[1] + c.vf[7][i] * nrm[2], 0.0f);
            for (int i = 0; i < 3; ++i)
                wb.vf26v[i] = std::min(c.vf[8][i] + m.lc[i] * l[0] + m.lc[4 + i] * l[1] + m.lc[8 + i] * l[2], 1.0f);
            wb.vf26v[3] = c.vf[26][3];
            wb.vf26 = true;
        }
        if (kind == 7)
        {
            // RT_ENVTAILCHECK=1: run the full EnvStrip on a copy of VU memory and compare its registers
            // and its stores outside the input STQs and the output with EnvStripTail's.
            static const bool check = Env("RT_ENVTAILCHECK") != nullptr;
            static std::vector<uint8_t> full, pre;
            static std::vector<uint8_t> fullPkt;
            vu1native::WriteBack fwb;
            bool fullOk = false;
            if (check)
            {
                pre.assign(mem, mem + 0x4000);
                full = pre;
                fullOk = vu1native::EnvStrip(c, top, full.data(), fullPkt, fwb, full.data());
            }
            if (!vu1native::EnvStripTail(c, top, mem, wb))
                return false;   // (EnvStripTail checks the batch first and changes nothing when it declines)
            if (check && fullOk)
            {
                static uint64_t batches = 0, regBad = 0, memBad = 0, adcBad = 0;
                ++batches;
                bool rb = wb.vfMask != fwb.vfMask || wb.viMask != fwb.viMask || wb.clipv != fwb.clipv || std::memcmp(&wb.qv, &fwb.qv, 4) ||
                          std::memcmp(wb.accv, fwb.accv, 16) || std::memcmp(wb.vf, fwb.vf, sizeof(wb.vf));
                for (int k = 0; k < 16; ++k)
                    if (k != 11 && wb.vi[k] != fwb.vi[k])
                        rb = true;
                adcBad += wb.vi[11] != fwb.vi[11];
                regBad += rb;
                const uint32_t t0 = top & 0x3FFu;
                bool mb = false;
                for (uint32_t q = 0; q < 0x400u && !mb; ++q)
                {
                    const uint32_t rel = (q - t0) & 0x3FFu;
                    const bool stqIn = rel >= 1 && rel < 1 + 4 * m.n && ((rel - 1) & 3u) == 2u;
                    const bool out = rel >= 0x98u && rel < 0x99u + 3 * m.n;
                    if (!stqIn && !out && std::memcmp(mem + 16 * q, full.data() + 16 * q, 16))
                        mb = true;
                }
                memBad += mb;
                if (rb && regBad < 4)
                    for (int k = 0; k < 32; ++k)
                        if (std::memcmp(wb.vf[k], fwb.vf[k], 16))
                            std::fprintf(stderr, "[envtail] vf%d tail %g %g %g %g full %g %g %g %g\n", k, wb.vf[k][0], wb.vf[k][1], wb.vf[k][2], wb.vf[k][3],
                                         fwb.vf[k][0], fwb.vf[k][1], fwb.vf[k][2], fwb.vf[k][3]);
                if ((batches & 0xFFFF) == 0 || (rb && regBad < 4) || (mb && memBad < 4))
                    std::fprintf(stderr, "[envtail] %llu batches: registers differ %llu, vi11 differs %llu, memory differs %llu\n",
                                 (unsigned long long)batches, (unsigned long long)regBad, (unsigned long long)adcBad, (unsigned long long)memBad);
            }
        }
        if (static const bool dbg = Env("RT_GPUMODELDBG") != nullptr; dbg && kind != 2)
        {
            // Vertices by clip-space class: inside with w > 0, inside only by |w| (w < 0), outside.
            static uint64_t cnt[4], batches = 0;
            for (uint32_t k = 0; k < n; ++k)
            {
                float pos[4], cp[4], sp[4];
                std::memcpy(pos, mem + (((top & 0x3FFu) + 1 + stride * k) << 4), 16);
                for (int i = 0; i < 4; ++i)
                {
                    cp[i] = c.vf[13][i] * pos[0] + c.vf[14][i] * pos[1] + c.vf[15][i] * pos[2] + c.vf[16][i];
                    sp[i] = c.vf[1][i] * pos[0] + c.vf[2][i] * pos[1] + c.vf[3][i] * pos[2] + c.vf[4][i];
                }
                const float aw = std::fabs(cp[3]);
                const bool in = std::fabs(cp[0]) <= aw && std::fabs(cp[1]) <= aw && std::fabs(cp[2]) <= aw;
                ++cnt[!in ? 2 : cp[3] >= 0 ? 0 : 1];
                if (sp[3] * cp[3] < 0)
                    ++cnt[3];
                static int shown = 0;
                if (in && shown < 40 && (batches % 997) == 0)
                    ++shown, std::fprintf(stderr, "[gpumodel] clip %.3f %.3f %.3f  screen %.1f %.1f\n", cp[0] / cp[3], cp[1] / cp[3], cp[2] / cp[3], sp[0] / sp[3], sp[1] / sp[3]);
            }
            if ((++batches & 0xFFFF) == 0)
                std::fprintf(stderr, "[gpumodel] vertices: in %llu, in by |w| %llu, out %llu; screen w sign differs %llu\n",
                             (unsigned long long)cnt[0], (unsigned long long)cnt[1], (unsigned long long)cnt[2], (unsigned long long)cnt[3]);
        }
        g_modelPkt.resize(sizeof(m) + m.dataBytes);
        std::memcpy(g_modelPkt.data(), &m, sizeof(m));
        std::memcpy(g_modelPkt.data() + sizeof(m), mem + (((top & 0x3FFu) + 1) << 4), m.dataBytes);
        g_rt->memory().submitGifPacket(GifPathId::Path1, g_modelPkt.data(), static_cast<uint32_t>(g_modelPkt.size()));
        g_ndVerts.fetch_add(n, std::memory_order_relaxed);
        return true;
    }

    void NdToVertexPacket(const std::vector<uint8_t> &ex, std::vector<uint8_t> &out);

    // Packets of the STQ / RGBA / XYZ(F)2 shape go to the GS as native vertex packets, others as they are.
    void NdSubmit()
    {
        size_t at = 0;
        for (const uint32_t n : g_ndSizes)
        {
            const uint8_t *p = g_ndExact.data() + at;
            at += n;
            uint32_t tw[4];
            std::memcpy(tw, p, 16);
            const uint32_t regs = tw[2] & 0xFFF;
            const uint64_t lo = (uint64_t)tw[0] | (uint64_t)tw[1] << 32;
            if (((lo >> 58) & 3) == 0 && ((lo >> 60) & 15) == 3 && (regs == 0x512 || regs == 0x412) && n == 16 + 48 * (tw[0] & 0x7FFF))
            {
                static std::vector<uint8_t> one;
                one.assign(p, p + n);
                NdToVertexPacket(one, g_ndPacket);
                g_rt->memory().submitGifPacket(GifPathId::Path1, g_ndPacket.data(), (uint32_t)g_ndPacket.size());
                g_ndVerts.fetch_add(tw[0] & 0x7FFF, std::memory_order_relaxed);
            }
            else
                g_rt->memory().submitGifPacket(GifPathId::Path1, p, n);
        }
    }

    // GIF packet -> native vertex packet: magic, count (bit 32: XYZF2), tag, then 32 bytes per
    // vertex: x, y (float pixels, from the 12.4 coordinates), z, RGBA bytes, S, T, Q, fog, ADC.
    void NdToVertexPacket(const std::vector<uint8_t> &ex, std::vector<uint8_t> &out)
    {
        uint32_t tw[4];
        std::memcpy(tw, ex.data(), 16);
        const uint32_t n = tw[0] & 0x7FFF;
        const bool xyzf = (tw[2] & 0xFFF) == 0x412;
        out.resize(32 + 32 * n);
        uint8_t *o = out.data();
        const uint64_t magic = 0x5854524556564E52ull, cnt = n | (xyzf ? 1ull << 32 : 0);
        std::memcpy(o, &magic, 8);
        std::memcpy(o + 8, &cnt, 8);
        std::memcpy(o + 16, tw, 16);
        o += 32;
        const uint8_t *b = ex.data() + 16;
        for (uint32_t k = 0; k < n; ++k, b += 48, o += 32)
        {
            uint32_t c4[4], w[4];
            std::memcpy(c4, b + 16, 16);
            std::memcpy(w, b + 32, 16);
            const float fx = (float)(w[0] & 0xFFFF) / 16.0f, fy = (float)(w[1] & 0xFFFF) / 16.0f;
            const uint32_t z = xyzf ? (w[2] >> 4) & 0xFFFFFFu : w[2];
            std::memcpy(o, &fx, 4);
            std::memcpy(o + 4, &fy, 4);
            std::memcpy(o + 8, &z, 4);
            o[12] = (uint8_t)c4[0]; o[13] = (uint8_t)c4[1]; o[14] = (uint8_t)c4[2]; o[15] = (uint8_t)c4[3];
            std::memcpy(o + 16, b, 12);
            o[28] = xyzf ? (uint8_t)(w[3] >> 4) : 0;
            o[29] = (w[3] >> 15) & 1;
            o[30] = 0; o[31] = 0;
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
        g_vu1LastPc = pc;
        const vu1fast::Program *prog = g_vu1Disp->find(pc);
        g_vu1LastProg = prog ? prog->name : "interp";
        if (!prog)
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
        vu1fast::g_pendTarget = 0;
        const uint64_t p0 = c.pairs;
        // A budget exit (64K cycles) is a time slice, not the end of the program: WipEout's
        // clipping programs can run longer, so keep going (bounded, in case of a runaway loop).
        static const char *sliceDump = Env("RT_SLICEDUMP");
        static std::vector<uint8_t> entrySnap;
        if (sliceDump)
        {
            entrySnap.assign(reinterpret_cast<const uint8_t *>(&c), reinterpret_cast<const uint8_t *>(&c) + offsetof(vu1fast::Ctx, mem));
            entrySnap.insert(entrySnap.end(), vuData, vuData + 0x4000);
            entrySnap.insert(entrySnap.end(), vuCode, vuCode + 0x4000);
        }
        const NdProg *ndp = (g_nativeDraw || g_ndCheck) ? NdFind(prog->id, pc) : nullptr;
        if (ndp && g_nativeDraw && !g_ndCheck && ((g_ndMask >> (ndp - g_ndProgs)) & 1))
        {
            vu1native::WriteBack wb;
            static const bool timing = Env("RT_NATIVETIME") != nullptr;
            const uint64_t t0 = timing ? __rdtsc() : 0;
            const bool model = NdModel(*ndp, c, top, vuData, wb);
            const bool ok = model || NdRun(*ndp, c, top, vuData, wb, true);
            if (static const bool gc = Env("RT_GPUCENSUS") != nullptr; gc)
            {
                // RT_GPUCENSUS=1: native batches by kind, inside / outside cached model packets.
                static uint64_t nb[kNdKinds][2], nv[kNdKinds][2], total = 0;
                const int in = ps2x_capture::g_vif1BatchId != 0;
                ++nb[ndp->kind][in];
                nv[ndp->kind][in] += ok && !model ? g_ndExact.size() / 48 : 0;   // GPU-transformed batches count no CPU vertices
                if ((++total & 0x3FFFF) == 0)
                {
                    std::fprintf(stderr, "[gpucensus]");
                    for (int k = 0; k < kNdKinds; ++k)
                        if (nb[k][0] | nb[k][1])
                            std::fprintf(stderr, " k%d: cached %llu/%llu v, other %llu/%llu v;", k, (unsigned long long)nb[k][1], (unsigned long long)nv[k][1],
                                         (unsigned long long)nb[k][0], (unsigned long long)nv[k][0]);
                    std::fprintf(stderr, "\n");
                }
            }
            if (timing)
            {
                // RT_NATIVETIME=1: TSC ticks per batch and per vertex of each native program.
                static uint64_t ticks[kNdKinds], batches[kNdKinds], verts[kNdKinds];
                const int k = ndp->kind;
                ticks[k] += __rdtsc() - t0;
                ++batches[k];
                verts[k] += g_ndExact.size() / 48;
                if ((batches[k] & 0xFFFF) == 0)
                {
                    std::fprintf(stderr, "[ntime] %s: %.0f ticks/batch %.1f ticks/vertex\n", ndp->name, double(ticks[k]) / batches[k], double(ticks[k]) / std::max<uint64_t>(1, verts[k]));
                    ticks[k] = batches[k] = verts[k] = 0;
                }
            }
            if (ok)
            {
                if (!model)
                    NdSubmit();
                g_ndBatches.fetch_add(1, std::memory_order_relaxed);
                if (wb.vf26)
                    std::memcpy(st.vf[26], wb.vf26v, 16);
                if (wb.r) st.r = wb.rv;
                if (wb.clip) st.clip = wb.clipv;
                if (wb.i) st.i = wb.iv;
                for (int k = 0; k < 32; ++k)
                    if ((wb.vfMask >> k) & 1)
                        std::memcpy(st.vf[k], wb.vf[k], 16);
                for (int k = 0; k < 16; ++k)
                    if ((wb.viMask >> k) & 1)
                        st.vi[k] = static_cast<int16_t>(wb.vi[k]);
                if (wb.acc) std::memcpy(st.acc, wb.accv, 16);
                if (wb.q) st.q = wb.qv;
                // The VU would end at the E bit with the next batch's entry as the resume pc.
                st.pc = ndp->resume; st.top = top; st.itop = itop;
                st.ebit = false; st.haltAfterDelaySlot = false; st.branchPending = false;
                st.stoppedByD = false; st.stoppedByT = false;
                g_vu1NativeCalls.fetch_add(1, std::memory_order_relaxed);
                return ps2x_capture::Vu1Handled;
            }
            g_ndFallback.fetch_add(1, std::memory_order_relaxed);
        }
        {
            // RT_VUCENSUS=1: the entries that still run on the (recompiled) VU, by batch count.
            static const bool vuCensus = Env("RT_VUCENSUS") != nullptr;
            if (vuCensus)
            {
                static std::map<std::pair<uint32_t, uint32_t>, uint64_t> runs;
                static uint64_t total = 0;
                ++runs[{prog->id, pc}];
                if ((++total & 0x3F) == 0)
                {
                    std::fprintf(stderr, "[vucensus] %llu VU batches:", (unsigned long long)total);
                    for (const auto &r : runs)
                        std::fprintf(stderr, " %u@%#x=%llu", r.first.first, r.first.second, (unsigned long long)r.second);
                    std::fprintf(stderr, "\n");
                }
            }
        }
        const bool checking = g_ndCheck && ndp;
        static std::vector<uint8_t> ncPacket;
        static std::vector<uint32_t> ncSizes;
        static vu1native::WriteBack ncWb;
        static std::vector<uint8_t> ncMem;
        static bool ncMemCheck = false;
        static struct { float vf[32][4]; uint16_t vi[16]; float acc[4]; } ncRegs;
        if (checking)
        {
            if (ndp->kind == 0)
                NativeCheckBefore(c, top, vuData);
            g_nc.kicks.clear();
            g_ncKicks = &g_nc.kicks;
            ncPacket.clear();
            ncWb = vu1native::WriteBack();
            // Programs whose VU memory stores are modelled: the native stores go to a copy of VU
            // memory, compared with the VU's after it runs.
            ncMemCheck = ndp->kind >= 4 && ndp->kind != 8;
            if (ncMemCheck)
                ncMem.assign(vuData, vuData + 0x4000);
            // Setup entries: every vf/vi register and ACC, not only the ones the native code reports.
            if (ndp->kind == 9 || ndp->kind == 10)
            {
                std::memcpy(ncRegs.vf, c.vf, sizeof(ncRegs.vf));
                for (int k = 0; k < 16; ++k) ncRegs.vi[k] = (uint16_t)c.vi[k];
                std::memcpy(ncRegs.acc, c.acc, 16);
            }
            if (NdRun(*ndp, c, top, vuData, ncWb, false, ncMemCheck ? ncMem.data() : nullptr))
            {
                ncPacket = g_ndExact;
                ncSizes = g_ndSizes;
                if (ncPacket.empty())
                    ncPacket.push_back(0);   // accepted, nothing drawn
            }
        }
        static const bool vuTiming = Env("RT_NATIVETIME") != nullptr && !g_nativeDraw;
        const NdProg *timedProg = vuTiming ? NdFind(prog->id, pc) : nullptr;
        const uint64_t vt0 = timedProg ? __rdtsc() : 0;
        bool complete = false;
        for (int slice = 0; slice < 256; ++slice)
        {
            const uint64_t b0 = g_vu1Disp->budgetExits;
            complete = g_vu1Disp->run(c);
            if (!complete || g_vu1Disp->budgetExits == b0)
                break;
            static bool dumped = false;
            if (sliceDump && slice == 2 && !dumped)
            {
                dumped = true;
                if (FILE *f = std::fopen(sliceDump, "wb")) { std::fwrite(entrySnap.data(), 1, entrySnap.size(), f); std::fclose(f); }
                std::fprintf(stderr, "[slice] dumped entry state (%zu bytes, ctx %zu)\n", entrySnap.size(), offsetof(vu1fast::Ctx, mem));
            }
            static int sliceLog = Env("RT_SLICELOG") ? std::atoi(Env("RT_SLICELOG")) : 0;
            if (sliceLog > 0 && slice >= 2)
            {
                --sliceLog;
                std::fprintf(stderr, "[slice] entry=%#x slice=%d pc=%#x top=%#x vi:", pc, slice, c.pc, top);
                for (int k = 0; k < 16; ++k) std::fprintf(stderr, " %x", (unsigned)(uint16_t)c.vi[k]);
                std::fprintf(stderr, "\n");
            }
            static bool once = false;
            if (slice == 255 && !once)
            {
                once = true;
                std::fprintf(stderr, "[vu1] program at %#x still running after 256 slices (pc=%#x)\n", pc, c.pc);
            }
        }
        if (timedProg)
        {
            // RT_NATIVETIME=1 RT_NATIVEDRAW=0: the same figures for the recompiled VU programs.
            static uint64_t ticks[kNdKinds], batches[kNdKinds];
            const int k = timedProg->kind;
            ticks[k] += __rdtsc() - vt0;
            if ((++batches[k] & 0xFFFF) == 0)
            {
                std::fprintf(stderr, "[ntime] VU %s: %.0f ticks/batch\n", timedProg->name, double(ticks[k]) / batches[k]);
                ticks[k] = batches[k] = 0;
            }
        }
        if (checking)
        {
            // The exact packet against the GIF packet the VU kicked, byte for byte; the register
            // the native code writes back against the VU's.
            struct Stat { uint64_t accepted, rejected, shape, vtx, bad[4], wbBad; };   // per kind   // bad: stq rgba xyz other
            static Stat stat[kNdKinds] = {};
            static int shown = 8;
            Stat &sv = stat[ndp->kind];
            if (ncPacket.empty())
                ++sv.rejected;
            else
            {
                ++sv.accepted;
                bool shapeOk = g_nc.kicks.size() == ncSizes.size();
                for (size_t j = 0, at = 0; shapeOk && j < ncSizes.size(); at += ncSizes[j++])
                    shapeOk = g_nc.kicks[j].size() >= ncSizes[j] && std::memcmp(g_nc.kicks[j].data(), ncPacket.data() + at, 16) == 0;
                if (!shapeOk)
                {
                    ++sv.shape;
                    if (shown > 0 && shown--)
                        std::fprintf(stderr, "[ncheck] %s: shape: %zu kicks (first %zu bytes), native %zu packets (first %u bytes)\n", ndp->name, g_nc.kicks.size(),
                                     g_nc.kicks.empty() ? (size_t)0 : g_nc.kicks[0].size(), ncSizes.size(), ncSizes.empty() ? 0u : ncSizes[0]);
                }
                else
                    for (size_t j = 0, at = 0; j < ncSizes.size(); at += ncSizes[j++])
                    {
                        // Per vertex qwords after the tag; fields: 0 STQ/UV, 1 RGBA, 2 XYZ (nreg 3 packets).
                        const uint32_t nq = ncSizes[j] / 16 - 1;
                        for (uint32_t qi = 0; qi < nq; ++qi)
                        {
                            const uint8_t *a = ncPacket.data() + at + 16 + 16 * qi, *b2 = g_nc.kicks[j].data() + 16 + 16 * qi;
                            const int f = qi % 3;
                            if (f == 0) ++sv.vtx;
                            if (std::memcmp(a, b2, 16) != 0)
                            {
                                ++sv.bad[f];
                                if (shown > 0 && shown--)
                                {
                                    uint32_t x[4], y[4];
                                    std::memcpy(x, a, 16);
                                    std::memcpy(y, b2, 16);
                                    std::fprintf(stderr, "[ncheck] %s: packet %zu qword %u field %d native %08x %08x %08x %08x vu %08x %08x %08x %08x\n", ndp->name, j, qi, f,
                                                 x[0], x[1], x[2], x[3], y[0], y[1], y[2], y[3]);
                                }
                            }
                        }
                    }
                if (ncWb.r && (ncWb.rv != c.r || ncWb.clipv != c.clip))
                {
                    ++sv.wbBad;
                    if (shown > 0 && shown--)
                        std::fprintf(stderr, "[ncheck] %s: r %08x clip %06x vu r %08x clip %06x\n", ndp->name, ncWb.rv, ncWb.clipv, c.r, c.clip);
                }
                if (ncWb.vf26 && std::memcmp(ncWb.vf26v, c.vf[26], 12) != 0)
                {
                    ++sv.wbBad;
                    if (shown > 0 && shown--)
                        std::fprintf(stderr, "[ncheck] %s: vf26 native %g %g %g %g vu %g %g %g %g\n", ndp->name, ncWb.vf26v[0], ncWb.vf26v[1], ncWb.vf26v[2], ncWb.vf26v[3],
                                     c.vf[26][0], c.vf[26][1], c.vf[26][2], c.vf[26][3]);
                }
                // Whole registers, CLIP, I, ACC, Q and VU memory, for the programs that report them.
                bool regBad = false;
                for (int k = 0; k < 32; ++k)
                    if (((ncWb.vfMask >> k) & 1) && std::memcmp(ncWb.vf[k], c.vf[k], 16) != 0)
                    {
                        regBad = true;
                        if (shown > 0 && shown--)
                            std::fprintf(stderr, "[ncheck] %s: vf%02d native %08x %08x %08x %08x vu %08x %08x %08x %08x\n", ndp->name, k,
                                         vu1fast::fbits(ncWb.vf[k][0]), vu1fast::fbits(ncWb.vf[k][1]), vu1fast::fbits(ncWb.vf[k][2]), vu1fast::fbits(ncWb.vf[k][3]),
                                         vu1fast::fbits(c.vf[k][0]), vu1fast::fbits(c.vf[k][1]), vu1fast::fbits(c.vf[k][2]), vu1fast::fbits(c.vf[k][3]));
                    }
                for (int k = 0; k < 16; ++k)
                    if (((ncWb.viMask >> k) & 1) && (uint16_t)ncWb.vi[k] != (uint16_t)c.vi[k])
                    {
                        regBad = true;
                        if (shown > 0 && shown--)
                            std::fprintf(stderr, "[ncheck] %s: vi%02d native %04x vu %04x\n", ndp->name, k, (unsigned)(uint16_t)ncWb.vi[k], (unsigned)(uint16_t)c.vi[k]);
                    }
                if (!ncWb.r && ncWb.clip && ncWb.clipv != c.clip)
                {
                    regBad = true;
                    if (shown > 0 && shown--)
                        std::fprintf(stderr, "[ncheck] %s: clip native %06x vu %06x\n", ndp->name, ncWb.clipv, c.clip);
                }
                if ((ncWb.i && vu1fast::fbits(ncWb.iv) != vu1fast::fbits(c.i)) || (ncWb.q && vu1fast::fbits(ncWb.qv) != vu1fast::fbits(c.q)) || (ncWb.acc && std::memcmp(ncWb.accv, c.acc, 16) != 0))
                {
                    regBad = true;
                    if (shown > 0 && shown--)
                        std::fprintf(stderr, "[ncheck] %s: i/q/acc native %08x %08x %08x vu %08x %08x %08x\n", ndp->name, vu1fast::fbits(ncWb.iv), vu1fast::fbits(ncWb.qv), vu1fast::fbits(ncWb.accv[0]),
                                     vu1fast::fbits(c.i), vu1fast::fbits(c.q), vu1fast::fbits(c.acc[0]));
                }
                if (ndp->kind == 9 || ndp->kind == 10)
                {
                    for (int k = 0; k < 32; ++k)
                        if ((ncWb.vfMask >> k) & 1) std::memcpy(ncRegs.vf[k], ncWb.vf[k], 16);
                    for (int k = 0; k < 16; ++k)
                        if ((ncWb.viMask >> k) & 1) ncRegs.vi[k] = (uint16_t)ncWb.vi[k];
                    if (ncWb.acc) std::memcpy(ncRegs.acc, ncWb.accv, 16);
                    for (int k = 1; k < 32; ++k)
                        if (std::memcmp(ncRegs.vf[k], c.vf[k], 16) != 0)
                        {
                            regBad = true;
                            if (shown > 0 && shown--) std::fprintf(stderr, "[ncheck] %s: vf%02d left changed by the VU only\n", ndp->name, k);
                        }
                    for (int k = 1; k < 16; ++k)
                        if (ncRegs.vi[k] != (uint16_t)c.vi[k])
                        {
                            regBad = true;
                            if (shown > 0 && shown--) std::fprintf(stderr, "[ncheck] %s: vi%02d native %04x vu %04x\n", ndp->name, k, ncRegs.vi[k], (unsigned)(uint16_t)c.vi[k]);
                        }
                    if (std::memcmp(ncRegs.acc, c.acc, 16) != 0)
                    {
                        regBad = true;
                        if (shown > 0 && shown--) std::fprintf(stderr, "[ncheck] %s: ACC differs\n", ndp->name);
                    }
                }
                if (ncMemCheck && std::memcmp(ncMem.data(), vuData, 0x4000) != 0)
                {
                    regBad = true;
                    uint32_t at = 0;
                    while (at < 0x4000 && ncMem[at] == vuData[at]) ++at;
                    if (shown > 0 && shown--)
                        std::fprintf(stderr, "[ncheck] %s: VU memory differs at qword %#x (top %#x)\n", ndp->name, at >> 4, top);
                }
                if (regBad)
                    ++sv.wbBad;
            }
            if (((sv.accepted + sv.rejected) & (ndp->kind == 10 ? 0x3Fu : 0x3FFFu)) == 0)
                std::fprintf(stderr, "[ncheck] %s: accepted %llu rejected %llu shape %llu | vertices %llu differ: stq %llu rgba %llu xyz %llu | registers %llu\n", ndp->name,
                             (unsigned long long)sv.accepted, (unsigned long long)sv.rejected, (unsigned long long)sv.shape, (unsigned long long)sv.vtx,
                             (unsigned long long)sv.bad[0], (unsigned long long)sv.bad[1], (unsigned long long)sv.bad[2], (unsigned long long)sv.wbBad);
            if (ndp->kind == 0)
                NativeCheckAfter();
            g_ncKicks = nullptr;
        }
        g_vu1Pairs.fetch_add(c.pairs - p0, std::memory_order_relaxed);
        {
            static long logLeft = Env("RT_VU1LOG") ? std::atol(Env("RT_VU1LOG")) : 0;
            static const double logAt = Env("RT_VU1LOG_AT") ? std::atof(Env("RT_VU1LOG_AT")) : 0.0;
            if (logLeft > 0 && std::chrono::duration<double>(std::chrono::steady_clock::now() - g_bootTime).count() >= logAt)
            {
                --logLeft;
                std::fprintf(stderr, "[vu1log] %s pc=%#x end=%#x top=%#x itop=%#x pairs=%llu @top:", kind == ps2x_capture::Vu1Mscal ? "mscal" : "mscnt",
                             pc, c.pc, top, itop, (unsigned long long)(c.pairs - p0));
                for (int k = 0; k < 4; ++k) { uint32_t w[4]; std::memcpy(w, vuData + (((top + k) & 0x3FFu) << 4), 16); std::fprintf(stderr, " [%08x %08x %08x %08x]", w[0], w[1], w[2], w[3]); }
                std::fprintf(stderr, "\n");
            }
        }
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
        vu1fast::g_pendTarget = 0;
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

    // RT_VU1VERIFY=N (after RT_VU1VERIFY_AT seconds): run the recompiled program on a copy, let
    // the interpreter run for real, and report differences in data memory, registers and kicks.
    struct VerifySink : vu1fast::KickSink
    {
        std::vector<std::vector<uint8_t>> kicks;
        void kick(const uint8_t *p, uint32_t n) override { kicks.emplace_back(p, p + n); }
    };
    struct Vu1Verify
    {
        long left = 0;
        double at = 0;
        bool active = false, nativeOk = false;
        uint32_t pc = 0, kind = 0;
        std::vector<uint8_t> mem;
        vu1fast::Ctx ctx;
        VerifySink sink;
        std::vector<std::vector<uint8_t>> interpKicks;
        std::map<uint32_t, uint64_t> bad, total;
    } g_vv;

    void Vu1VerifyBefore(uint32_t kind, uint32_t startPC, uint32_t top, uint32_t itop, VU1State &st, uint8_t *vuCode, uint8_t *vuData)
    {
        if (!g_vu1Disp)
        {
            g_vu1Ctx = new vu1fast::Ctx();
            g_vu1Disp = new vu1fast::Dispatcher(vuCode);
        }
        const uint64_t gen = g_rt->memory().getVU1CodeGeneration();
        if (gen != g_vu1Gen) { g_vu1Gen = gen; g_vu1Disp->invalidate(); }
        const uint32_t pc = (kind == ps2x_capture::Vu1Mscal ? startPC : st.pc) & 0x3FF8u;
        g_vv.pc = pc; g_vv.kind = kind;
        g_vv.active = true;
        g_vv.interpKicks.clear();
        g_vvKicks = &g_vv.interpKicks;
        g_vv.sink.kicks.clear();
        g_vv.nativeOk = false;
        if (!g_vu1Disp->find(pc))
            return;
        g_vv.mem.assign(vuData, vuData + 0x4000);
        vu1fast::Ctx &c = g_vv.ctx;
        std::memcpy(c.vf, st.vf, sizeof(c.vf));
        for (int k = 0; k < 16; ++k) c.vi[k] = st.vi[k];
        std::memcpy(c.acc, st.acc, sizeof(c.acc));
        c.q = st.q; c.p = st.p; c.i = st.i; c.r = st.r;
        c.clip = st.clip; c.mac = st.mac; c.status = st.status;
        c.pc = pc; c.top = top; c.itop = itop;
        if (kind == ps2x_capture::Vu1Mscal) { c.vf[0][0] = c.vf[0][1] = c.vf[0][2] = 0.0f; c.vf[0][3] = 1.0f; }
        c.mem = g_vv.mem.data();
        c.sink = &g_vv.sink;
        c.resetTiming();
        vu1fast::g_pendTarget = 0;
        bool ok = false;
        for (int slice = 0; slice < 256; ++slice)
        {
            const uint64_t b0 = g_vu1Disp->budgetExits;
            ok = g_vu1Disp->run(c);
            if (!ok || g_vu1Disp->budgetExits == b0) break;
        }
        g_vv.nativeOk = ok;
    }

    void Vu1VerifyAfter(VU1State &st, const uint8_t *vuData)
    {
        g_vv.active = false;
        g_vvKicks = nullptr;
        if (!g_vv.nativeOk)
            return;
        const uint32_t pc = g_vv.pc;
        ++g_vv.total[pc];
        char why[256] = "";
        const vu1fast::Ctx &c = g_vv.ctx;
        for (int k = 1; k < 16 && !why[0]; ++k)
            if ((uint16_t)c.vi[k] != (uint16_t)st.vi[k])
                std::snprintf(why, sizeof(why), "vi%02d native=%x interp=%x", k, (unsigned)(uint16_t)c.vi[k], (unsigned)(uint16_t)st.vi[k]);
        for (int k = 1; k < 32 && !why[0]; ++k)
            if (std::memcmp(c.vf[k], st.vf[k], 16) != 0)
                std::snprintf(why, sizeof(why), "vf%02d native=(%g %g %g %g) interp=(%g %g %g %g)", k, c.vf[k][0], c.vf[k][1], c.vf[k][2], c.vf[k][3],
                              st.vf[k][0], st.vf[k][1], st.vf[k][2], st.vf[k][3]);
        if (!why[0] && std::memcmp(g_vv.mem.data(), vuData, 0x4000) != 0)
        {
            uint32_t a = 0;
            while (a < 0x4000 && std::memcmp(g_vv.mem.data() + a, vuData + a, 4) == 0) a += 4;
            uint32_t nd = 0;
            for (uint32_t b = 0; b < 0x4000; b += 4) nd += std::memcmp(g_vv.mem.data() + b, vuData + b, 4) != 0;
            uint32_t wn, wi; std::memcpy(&wn, g_vv.mem.data() + a, 4); std::memcpy(&wi, vuData + a, 4);
            std::snprintf(why, sizeof(why), "data qw %#x.%u native=%08x interp=%08x (%u words differ)", a >> 4, (a >> 2) & 3, wn, wi, nd);
        }
        if (!why[0] && g_vv.sink.kicks.size() != g_vv.interpKicks.size())
            std::snprintf(why, sizeof(why), "kicks native=%zu interp=%zu", g_vv.sink.kicks.size(), g_vv.interpKicks.size());
        for (size_t k = 0; k < g_vv.sink.kicks.size() && !why[0]; ++k)
            if (g_vv.sink.kicks[k] != g_vv.interpKicks[k])
            {
                std::snprintf(why, sizeof(why), "kick %zu differs (sizes %zu/%zu)", k, g_vv.sink.kicks[k].size(), g_vv.interpKicks[k].size());
                static int dumped = 0;
                if (dumped++ < 12)
                {
                    const auto &kn = g_vv.sink.kicks[k], &ki = g_vv.interpKicks[k];
                    int shown = 0;
                    for (size_t q = 0; q + 16 <= std::min(kn.size(), ki.size()) && shown < 6; q += 16)
                        if (std::memcmp(kn.data() + q, ki.data() + q, 16) != 0)
                        {
                            uint32_t a[4], b[4];
                            std::memcpy(a, kn.data() + q, 16); std::memcpy(b, ki.data() + q, 16);
                            std::fprintf(stderr, "[vu1verify]   pc=%#x qw %zu native %08x %08x %08x %08x interp %08x %08x %08x %08x\n", g_vv.pc, q / 16,
                                         a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3]);
                            ++shown;
                        }
                }
            }
        if (why[0])
        {
            if (++g_vv.bad[pc] <= 3)
                std::fprintf(stderr, "[vu1verify] %s pc=%#x end native=%#x interp=%#x: %s\n", g_vv.kind == ps2x_capture::Vu1Mscal ? "mscal" : "mscnt",
                             pc, c.pc, st.pc, why);
        }
        if (--g_vv.left == 0)
        {
            std::fprintf(stderr, "[vu1verify] summary (pc: bad/total):");
            for (auto &[k, v] : g_vv.total)
                std::fprintf(stderr, " %#x:%llu/%llu", k, (unsigned long long)g_vv.bad[k], (unsigned long long)v);
            std::fprintf(stderr, "\n");
        }
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
        static bool vvInit = false;
        if (!vvInit)
        {
            vvInit = true;
            g_vv.left = Env("RT_VU1VERIFY") ? std::atol(Env("RT_VU1VERIFY")) : 0;
            g_vv.at = Env("RT_VU1VERIFY_AT") ? std::atof(Env("RT_VU1VERIFY_AT")) : 0.0;
        }
        if (g_vv.left > 0)
        {
            if (phase == ps2x_capture::Vu1Before &&
                std::chrono::duration<double>(std::chrono::steady_clock::now() - g_vu1Boot).count() >= g_vv.at)
            {
                Vu1VerifyBefore(kind, startPC, top, itop, st, vuCode, vuData);
                return ps2x_capture::Vu1Interpret;
            }
            if (phase != ps2x_capture::Vu1Before && g_vv.active)
            {
                Vu1VerifyAfter(st, vuData);
                return ps2x_capture::Vu1Handled;
            }
        }
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

    // RT_DMASITES=1: census of the DMA chains the game kicks, by kick site. Walks each chain at the
    // kick (tags, REF/CNT split, distinct REF sources) and parses the VIF stream (UNPACK, DIRECT, MPG,
    // MSCAL entries). Printed every 10 s. Map of the mesh submission code for the native data path.
    struct DmaSite
    {
        uint64_t kicks = 0, tags[8] = {}, tagQw[8] = {}, unpackQw = 0, directQw = 0, mpgQw = 0, vifCmds = 0;
        std::map<uint32_t, uint64_t> mscal, mpg, unpackFmt;
        std::set<uint32_t> refSrc;
    };
    std::mutex g_dsMu;
    std::map<uint64_t, DmaSite> g_dmaSites;   // key: channel << 48 | pc << 24 | ra (ra 24 bits)
    std::chrono::steady_clock::time_point g_dsLast;

    struct VifScan
    {
        DmaSite *s;
        uint32_t skip = 0, cl = 1, wl = 1, pend = 0;   // pend: words left of the current command's data
        FILE *dump = nullptr;
        void feed(const uint32_t *w, uint32_t n)
        {
            for (uint32_t i = 0; i < n;)
            {
                if (pend)
                {
                    const uint32_t k = std::min(pend, n - i);
                    pend -= k, i += k;
                    continue;
                }
                const uint32_t c = w[i++], cmd = (c >> 24) & 0x7f, num = (c >> 16) & 0xff, imm = c & 0xffff;
                ++s->vifCmds;
                if (dump && cmd)
                    std::fprintf(dump, "    vif %02x num=%u imm=%x\n", cmd, num, imm);
                if (cmd >= 0x60)
                {
                    const uint32_t vn = (cmd >> 2) & 3, vl = cmd & 3, cnt = num ? num : 256;
                    const uint32_t m = (wl <= cl) ? cnt : cl * (cnt / wl) + std::min(cnt % wl, cl);
                    pend = ((32u >> vl) * (vn + 1) * m + 31) / 32;
                    s->unpackQw += cnt;
                    ++s->unpackFmt[(cmd & 0x1f) | ((cmd & 0x10) ? 0x100 : 0) | (wl << 12) | (cl << 20)];
                }
                else switch (cmd)
                {
                case 0x01: cl = imm & 0xff, wl = (imm >> 8) & 0xff; break;
                case 0x14: case 0x15: ++s->mscal[imm * 8]; break;
                case 0x17: ++s->mscal[0xFFFFFFFFu]; break;
                case 0x20: pend = 1; if (dump && i < n) std::fprintf(dump, "      mask=%08x\n", w[i]); break;
                case 0x30: case 0x31: pend = 4; break;
                case 0x4a: { const uint32_t q = num ? num : 256; pend = q * 2; s->mpgQw += q / 2;
                             ++s->mpg[(imm * 8) | (q << 16)]; break; }
                case 0x50: case 0x51: { const uint32_t q = imm ? imm : 65536; pend = q * 4; s->directQw += q; break; }
                default: break;
                }
            }
        }
    };

    const uint32_t *DsPtr(uint32_t a)
    {
        uint8_t *ram = g_rt->memory().getRDRAM();
        if ((a & 0x80000000u) || (a >= 0x70000000u && a < 0x70004000u))
            return reinterpret_cast<const uint32_t *>(g_rt->memory().getScratchpad() + (a & 0x3ff0u));
        return reinterpret_cast<const uint32_t *>(ram + (a & 0x01fffff0u));
    }

    void DmaSiteRecord(uint32_t ch, uint32_t chcr, uint32_t madr, uint32_t qwc, uint32_t tadr, uint32_t pc, uint32_t ra)
    {
        std::lock_guard<std::mutex> lk(g_dsMu);
        DmaSite &s = g_dmaSites[(uint64_t(ch & 0xf) << 48) | (uint64_t(pc & 0xffffff) << 24) | (ra & 0xffffff)];
        ++s.kicks;
        VifScan v{&s};
        const bool vif = ch != 0xA;
        static const uint32_t dumpPc = Env("RT_DMADUMP_PC") ? std::strtoul(Env("RT_DMADUMP_PC"), nullptr, 16) : 0;
        static const double dumpAt = Env("RT_DMADUMP_AT") ? std::atof(Env("RT_DMADUMP_AT")) : 100.0;
        static bool dumped = false;
        if (!dumped && pc == dumpPc && std::chrono::duration<double>(std::chrono::steady_clock::now() - g_bootTime).count() >= dumpAt)
            dumped = true, v.dump = std::fopen("dmadump.txt", "w");
        auto data = [&](uint32_t a, uint32_t q) { if (vif && q) v.feed(DsPtr(a), q * 4); };
        if (((chcr >> 2) & 3) == 0)
        {
            ++s.tags[1], s.tagQw[1] += qwc;
            data(madr, qwc);
        }
        else
        {
            uint32_t t = tadr, asr[2] = {}, sp = 0;
            const bool tte = chcr & 0x40;
            for (int guard = 0; guard < 200000; ++guard)
            {
                const uint32_t *tag = DsPtr(t);
                const uint32_t q = tag[0] & 0xffff, id = (tag[0] >> 28) & 7, addr = tag[1] & 0x7ffffff0u | (tag[1] & 0x80000000u);
                ++s.tags[id], s.tagQw[id] += q;
                if (v.dump)
                    std::fprintf(v.dump, "tag %08x id=%u qwc=%u addr=%08x w23=%08x %08x\n", t, id, q, addr, tag[2], tag[3]);
                if (tte && vif) v.feed(tag + 2, 2);
                bool end = false;
                switch (id)
                {
                case 0: data(addr, q); s.refSrc.insert(addr); end = true; break;
                case 1: data(t + 16, q); t += 16 + q * 16; break;
                case 2: data(t + 16, q); t = addr; break;
                case 3: case 4: data(addr, q); s.refSrc.insert(addr); t += 16; break;
                case 5: data(t + 16, q); if (sp < 2) asr[sp++] = t + 16 + q * 16; t = addr; break;
                case 6: data(t + 16, q); if (sp) t = asr[--sp]; else end = true; break;
                case 7: data(t + 16, q); end = true; break;
                }
                if (end || (tag[0] & 0x80000000u && (chcr & 0x80)))
                    break;
            }
        }
        if (v.dump)
            std::fclose(v.dump), v.dump = nullptr;
        const auto now = std::chrono::steady_clock::now();
        if (now - g_dsLast < std::chrono::seconds(10))
            return;
        g_dsLast = now;
        const double tm = std::chrono::duration<double>(now - g_bootTime).count();
        std::fprintf(stderr, "[dmasites] t=%.0f\n", tm);
        static const char *tn[8] = {"refe", "cnt", "next", "ref", "refs", "call", "ret", "end"};
        for (auto &[k, d] : g_dmaSites)
        {
            std::fprintf(stderr, "[dmasites] ch%u pc=%06x ra=%06x kicks=%llu cmds=%llu unpackQw=%llu directQw=%llu mpgQw=%llu refSrc=%zu tags:",
                         unsigned(k >> 48), unsigned(k >> 24 & 0xffffff), unsigned(k & 0xffffff), (unsigned long long)d.kicks,
                         (unsigned long long)d.vifCmds, (unsigned long long)d.unpackQw, (unsigned long long)d.directQw,
                         (unsigned long long)d.mpgQw, d.refSrc.size());
            for (int i = 0; i < 8; ++i)
                if (d.tags[i])
                    std::fprintf(stderr, " %s=%llu/%lluqw", tn[i], (unsigned long long)d.tags[i], (unsigned long long)d.tagQw[i]);
            std::fprintf(stderr, "\n[dmasites]   mscal:");
            for (auto &[e, c] : d.mscal)
                std::fprintf(stderr, " %x=%llu", e, (unsigned long long)c);
            std::fprintf(stderr, "\n[dmasites]   mpg:");
            for (auto &[e, c] : d.mpg)
                std::fprintf(stderr, " %x+%u=%llu", e & 0xffff, e >> 16, (unsigned long long)c);
            std::fprintf(stderr, "\n[dmasites]   unpack(fmt|wl<<12|cl<<20):");
            for (auto &[e, c] : d.unpackFmt)
                std::fprintf(stderr, " %x=%llu", e, (unsigned long long)c);
            std::fprintf(stderr, "\n");
        }
    }

    void DmaSiteStoreHook(uint32_t vaddr, uint32_t size, uint64_t lo, uint64_t, uint32_t pc, uint32_t ra)
    {
        const uint32_t ch = vaddr & 0xffffff00u;
        if ((vaddr & 0xff) != 0 || (ch != 0x10008000u && ch != 0x10009000u && ch != 0x1000A000u) || !(lo & 0x100))
            return;
        auto &io = g_rt->memory();
        DmaSiteRecord((ch >> 12) & 0xf, uint32_t(lo), io.readIORegister(ch + 0x10), io.readIORegister(ch + 0x20) & 0xffff,
                      io.readIORegister(ch + 0x30), pc, ra);
    }

    PS2Runtime::RecompiledFunction g_dmaSendOrig[2] = {};
    template <int N>
    void DmaSendSite(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        auto R = [&](int r) { return static_cast<uint32_t>(_mm_extract_epi32(ctx->r[r], 0)); };
        uint32_t ch = R(4);
        if ((ch & 0xffff0000u) != 0x10000000u)
            ch = 0;
        const uint32_t chcr = runtime->memory().readIORegister((ch & 0xffffff00u) ? (ch & 0xffffff00u) : 0x10009000u);
        DmaSiteRecord(((ch >> 12) & 0xf) | (ch ? 0 : 0xf), N ? 0x100u | (chcr & 0x40) : 0x104u | (chcr & 0x40), R(5) & 0x7ffffff0u,
                      N ? (R(6) & 0xffff) : 0, R(5) & 0x7ffffff0u, 0x216ed0u + N * 0xd8u, R(31));
        g_dmaSendOrig[N](rdram, ctx, runtime);
    }

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
        if (const char *e = Env("RT_GFXCENSUS"))
        {
            const double secs = std::max(1.0, std::atof(e));
            std::thread([secs]() {
                for (;;)
                {
                    std::this_thread::sleep_for(std::chrono::duration<double>(secs));
                    CensusReport(secs);
                }
            }).detach();
        }
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
            if (const char *d = Env("RT_NATIVEDRAW"); !d || std::atoi(d) != 0)   // default on; RT_NATIVEDRAW=0 runs it on the VU
            {
                g_nativeDraw = true;
                if (const char *m = Env("RT_NATIVEDRAW_MASK"))
                    g_ndMask = static_cast<uint32_t>(std::strtoul(m, nullptr, 16));
                std::fprintf(stderr, "[vu1] native drawing enabled (programs %#x)\n", g_ndMask);
            }
        }
        if (const char *w = Env("RT_SPRWATCH"))
        {
            g_watchLo = static_cast<uint32_t>(std::strtoul(w, nullptr, 16));
            const char *dash = std::strchr(w, '-');
            g_watchHi = dash ? static_cast<uint32_t>(std::strtoul(dash + 1, nullptr, 16)) : g_watchLo + 16;
            ps2x_capture::g_storeHook.store(&StoreWatchHook);
        }
        if (Env("RT_DMASITES"))
        {
            ps2x_capture::g_storeHook.store(&DmaSiteStoreHook);
            g_dmaSendOrig[0] = runtime.lookupFunction(0x216ed0u);
            g_dmaSendOrig[1] = runtime.lookupFunction(0x216fa8u);
            runtime.replaceFunction(0x216ed0u, &DmaSendSite<0>);
            runtime.replaceFunction(0x216fa8u, &DmaSendSite<1>);
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
                {
                    static const char *kStatus[] = {"run", "ready", "wait", "waitsusp", "susp", "dormant"};
                    static const char *kWait[] = {"-", "sleep", "sema", "evflag", "vsync", "external", "mpeg"};
                    const EeKernelSnapshot k = runtime.eeScheduler().snapshot();
                    for (const EeThreadSnapshot &t : k.threads)
                        std::fprintf(stderr, "[watch] ee thread %d %s/%s id=%d pc=%08x ra=%08x entry=%08x prio=%d\n", t.id,
                                     kStatus[static_cast<int>(t.status)], kWait[static_cast<int>(t.waitReason)], t.waitId,
                                     t.pc, t.ra, t.entry, t.currentPriority);
                    for (const EeSemaphoreSnapshot &m : k.semaphores)
                        if (m.waiters)
                            std::fprintf(stderr, "[watch] ee sema %d count=%d/%d waiters=%u\n", m.id, m.count, m.maxCount, m.waiters);
                }
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

    // ---- IPU video frames (menu background FE.IPF, track billboards, track previews) --------
    // The .IPF players decode one intra frame at a time with the IPU: they build a toIPU DMA
    // chain over the frame's bitstream, read the flags byte with FDEC, issue IDEC (RGB32 output
    // for the front end at 0x2117F8, RGB16 for the billboard player at 0x212758) and pull the
    // macroblocks out with fromIPU DMA, up to 1023 KB at a time. The runtime has no IPU, so the
    // players' IPU helpers are replaced: the bitstream is decoded whole with FFmpeg's IPU decoder
    // (frame laid out as one 16-pixel-wide column, so its macroblocks come out in stream order),
    // converted with the IPU's colour space conversion and copied out in IPU macroblock order.
    //   RT_IPULOG=1 log each decoded frame
    struct IpuState
    {
        uint32_t src = 0, size = 0;
        bool fresh = false;
        std::vector<uint8_t> out;
        size_t outPos = 0;
        std::map<int, AVCodecContext *> decoders;   // by macroblock count
        AVPacket *pkt = nullptr;
        AVFrame *frame = nullptr;
        bool log = false;
    } g_ipu;

    void IpuDecode(uint8_t *rdram, uint32_t mbs, bool rgb16)
    {
        IpuState &s = g_ipu;
        s.out.assign(static_cast<size_t>(mbs) * (rgb16 ? 512 : 1024), 0);
        s.outPos = 0;
        if (!mbs || mbs > 8192 || !s.size || s.size > 0x200000)
            return;
        if (!s.pkt)
        {
            s.pkt = av_packet_alloc();
            s.frame = av_frame_alloc();
            s.log = Env("RT_IPULOG") != nullptr;
        }
        AVCodecContext *&dec = s.decoders[static_cast<int>(mbs)];
        if (!dec)
        {
            const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_IPU);
            if (!codec)
                return;
            dec = avcodec_alloc_context3(codec);
            dec->width = 16;
            dec->height = static_cast<int>(16 * mbs);
            if (avcodec_open2(dec, codec, nullptr) < 0)
            {
                avcodec_free_context(&dec);
                return;
            }
        }
        // The frame ends with the 0x000001B0 end code (FFmpeg wants exactly that after the last
        // macroblock); IPF frames also carry a 0x000001B1 after it.
        const uint8_t *bs = rdram + (s.src & 0x01FFFFFFu);
        uint32_t len = std::min<uint32_t>(s.size, 0x02000000u - (s.src & 0x01FFFFFFu));
        for (uint32_t i = len >= 4 ? len - 4 : 0; i > 0; --i)
            if (bs[i] == 0 && bs[i + 1] == 0 && bs[i + 2] == 1 && bs[i + 3] == 0xB0)
            {
                len = i + 4;
                break;
            }
        if (av_new_packet(s.pkt, static_cast<int>(len)) < 0)
            return;
        std::memcpy(s.pkt->data, bs, len);
        int ret = avcodec_send_packet(dec, s.pkt);
        av_packet_unref(s.pkt);
        if (ret >= 0)
            ret = avcodec_receive_frame(dec, s.frame);
        if (s.log)
            std::fprintf(stderr, "[ipu] src=%08X size=%u mbs=%u %s -> %d\n", s.src, s.size, mbs,
                         rgb16 ? "rgb16" : "rgb32", ret);
        if (ret < 0)
        {
            avcodec_flush_buffers(dec);
            return;
        }
        // IPU CSC: ITU-R BT.601 studio range; alpha 0x80 (no SETTH thresholds), RGB16 A bit 0.
        const AVFrame *f = s.frame;
        uint8_t *o = s.out.data();
        for (uint32_t mb = 0; mb < mbs; ++mb)
            for (int y = 0; y < 16; ++y)
            {
                const uint8_t *Y = f->data[0] + (mb * 16 + y) * f->linesize[0];
                const uint8_t *U = f->data[1] + (mb * 8 + y / 2) * f->linesize[1];
                const uint8_t *V = f->data[2] + (mb * 8 + y / 2) * f->linesize[2];
                for (int x = 0; x < 16; ++x)
                {
                    const int c = 298 * std::max(Y[x] - 16, 0), d = U[x / 2] - 128, e = V[x / 2] - 128;
                    const int r = std::clamp((c + 409 * e + 128) >> 8, 0, 255);
                    const int g = std::clamp((c - 100 * d - 208 * e + 128) >> 8, 0, 255);
                    const int b = std::clamp((c + 516 * d + 128) >> 8, 0, 255);
                    if (rgb16)
                    {
                        const uint16_t p = static_cast<uint16_t>((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10));
                        std::memcpy(o, &p, 2);
                        o += 2;
                    }
                    else
                    {
                        o[0] = static_cast<uint8_t>(r);
                        o[1] = static_cast<uint8_t>(g);
                        o[2] = static_cast<uint8_t>(b);
                        o[3] = 0x80;
                        o += 4;
                    }
                }
            }
        av_frame_unref(s.frame);
    }

    // (tagbuf, src, size): the toIPU chain over the frame's bitstream.
    void HleIpuSetSource(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        g_ipu.src = getRegU32(ctx, 5);
        g_ipu.size = getRegU32(ctx, 6);
        ctx->pc = getRegU32(ctx, 31);
    }

    void HleIpuStartIn(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        ctx->pc = getRegU32(ctx, 31);
    }

    // FDEC 0 / FDEC 8: returns the first byte of the bitstream (the picture flags).
    void HleIpuReadFlags(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        g_ipu.fresh = true;
        setReturnS32(ctx, g_ipu.size ? rdram[g_ipu.src & 0x01FFFFFFu] : 0);
        ctx->pc = getRegU32(ctx, 31);
    }

    // (dest, n): fromIPU n KB to dest. The caller's loop counter holds the frame's total (in KB),
    // in s3 (RGB32 player) or s2 (RGB16 player) on the first call.
    template <bool Rgb16>
    void HleIpuStartOut(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        IpuState &s = g_ipu;
        if (s.fresh)
        {
            s.fresh = false;
            const uint32_t totalKb = getRegU32(ctx, Rgb16 ? 18 : 19);
            IpuDecode(rdram, Rgb16 ? totalKb * 2 : totalKb, Rgb16);
        }
        const uint32_t dest = getRegU32(ctx, 4) & 0x01FFFFFFu;
        const size_t n = std::min<size_t>(static_cast<size_t>(getRegU32(ctx, 5)) * 1024, s.out.size() - std::min(s.outPos, s.out.size()));
        if (n && dest + n <= 0x02000000u)
            std::memcpy(rdram + dest, s.out.data() + s.outPos, n);
        s.outPos += static_cast<size_t>(getRegU32(ctx, 5)) * 1024;
        ctx->pc = getRegU32(ctx, 31);
    }

    void ApplyWipeoutFusion(PS2Runtime &runtime)
    {

        StartCapture(runtime);
        rufftrigger::InstallGpuRenderer(runtime);
        rufftrigger::InstallAudio();
        rufftrigger::StartProfiler();
        StartWatchdog(runtime);
        StartMatScan(runtime);
        std::fprintf(stderr, "[rt] host rdram=%p\n", static_cast<void *>(runtime.memory().getRDRAM()));
        // libsif / libc entry points found in SLUS_204.62 (same SDK layout as Ruff Trigger's)
        runtime.replaceFunction(0x0023B9D8, &HleGetstat);
        runtime.replaceFunction(0x0023D570, &HleSearchModuleByName);
        runtime.replaceFunction(0x0023D928, &HleSifLoadStartModule);
        runtime.replaceFunction(0x0023D908, &HleSifLoadModule);
        // libcdvd functions the analyzer did not name (so they were recompiled as guest code that
        // waits forever on cdvdfsv's S-command RPC, which the HLE IOP does not serve)
        runtime.replaceFunction(0x0021C518, &ps2_stubs::sceCdDiskReady);   // sceCdDiskReady(0) at boot
        runtime.replaceFunction(0x002144E0, &HleSkipBootMovie);
        // IPU helpers of the two .IPF players (see IpuDecode)
        runtime.replaceFunction(0x00211618, &HleIpuSetSource);
        runtime.replaceFunction(0x002116E8, &HleIpuStartIn);
        runtime.replaceFunction(0x00211728, &HleIpuReadFlags);
        runtime.replaceFunction(0x002117A0, &HleIpuStartOut<false>);
        runtime.replaceFunction(0x00212590, &HleIpuSetSource);
        runtime.replaceFunction(0x00212660, &HleIpuStartIn);
        runtime.replaceFunction(0x002126A0, &HleIpuReadFlags);
        runtime.replaceFunction(0x00212718, &HleIpuStartOut<true>);
        // Not wired yet (Ruff Trigger addresses): memcard, movies, debug menu, frame pacing.
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
            if (!b.addr)
                continue;
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
                    arg = item[colon + 1] == 'r' ? -2 : item[colon + 1] == 'd' ? -3 : std::atoi(item.c_str() + colon + 1);
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
        if (Env("RT_MEMW"))
        {
            g_memcpyOrig = runtime.lookupFunction(0x0022B088);
            g_memsetOrig = runtime.lookupFunction(0x0022B138);
            runtime.replaceFunction(0x0022B088, &MemWDbg<false>);
            runtime.replaceFunction(0x0022B138, &MemWDbg<true>);
        }
        if (Env("RT_PADFILE"))
        {
            g_padRt = &runtime;
            ps2x_capture::g_padHook.store(&PadScript);
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
        wipeout::InstallSettings(runtime);   // last: chains the pad hook installed above
        std::fprintf(stderr, "[rt] Wipeout Fusion override applied (%d extra bindings)\n", ok);
    }
}

PS2_REGISTER_GAME_OVERRIDE("WipEout Fusion (NTSC-U)", "SLUS_204.62", 0x00100008, 0,
                           ApplyWipeoutFusion)
