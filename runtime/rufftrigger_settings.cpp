// Graphics menu (F10, or L3 + R3 on a gamepad) and the frame-rate unlock.
//
// Settings: internal resolution, full screen, aspect ratio, widescreen camera, HUD shape, vsync,
// frame rate and an FPS counter. They apply while the game runs and are kept in graphics.ini
// (RT_SETTINGS=file, default next to the memory card folder: saves/graphics.ini with play.sh).
// Environment variables set by the user (RT_GPU_SCALE, RT_ASPECT, ...) win over the file at
// start; the menu can still change them afterwards.
//
// Frame rate. The game's per-frame update (f_001f7f50) ends in a frame limiter that waits for one
// guest vblank (its 60 fps mode) or two (30 fps mode), then advances game time by a fixed step:
// the timer object at 0x4C2FF0 (getter f_002fc9f8) holds that step in milliseconds at +0x1C, and
// movement code scales by it (e.g. f_001109c0: speed * step / 1000; +0x5A4 set divides it by 3,
// slow motion). Above the game's own rate:
//   * the limiter's vblank waits (f_00291140 called from 0x1f9af0/0x1f9b30/0x1f9b50) wait for
//     the host frame deadline instead, letting guest time run (other EE threads, IOP, audio);
//   * the vblank counter the limiter reads (f_001ea4d8 increments gp-0x6270) is held, so the
//     limiter waits exactly once per frame;
//   * the step at +0x1C is scaled to the real frame time (dithered whole ms, never 0, multiples
//     of 3 in slow motion) and put back to the game's own value before the limiter's mode test;
//   * every finished game frame is shown (rufftrigger_gpu.cpp GpuGameFrame) instead of only the
//     frame at each vblank;
//   * the EE is "overclocked" (fewer guest cycles charged per instruction) so 30 fps worth of
//     guest work fits in a shorter frame; if the game then falls below full speed the effective
//     frame rate steps down until it keeps up.
// Anything the game counts in frames instead of milliseconds runs faster above 30/60 fps.
#include "ps2_runtime.h"
#include "runtime/ps2_capture_hook.h"
#include "raylib.h"
#include "rufftrigger_menu_font.h"

#include "rt_platform.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace rufftrigger
{
    void SetWidescreenCamera(bool on);
    bool GpuRendererActive();
    void GpuSetScale(int scale);
    uint32_t GpuCurrentScale();
    void GpuSetHudSquash(float k);
    void GpuGameFrame();
    void GpuStopGameFrames();
}

namespace
{
    int64_t NowNs()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    const char *Env(const char *name)
    {
        const char *v = std::getenv(name);
        return v && *v ? v : nullptr;
    }

    // ---- settings ---------------------------------------------------------------------------
    const int kScales[] = {0, 1, 2, 3, 4, 6, 8};
    const int kFps[] = {0, 60, 120, 144, 165, 240};   // 0 = the game's own rate
    enum Aspect { AspectAuto, Aspect169, Aspect43, AspectStretch };

    struct Settings
    {
        int scale = 0;   // index into kScales
        bool fullscreen = false;
        int aspect = AspectAuto;
        bool widescreen = true;
        bool hud43 = true;
        bool vsync = true;
        int fps = 0;     // index into kFps
        bool showFps = false;
    };
    Settings g_set;

    std::string SettingsPath()
    {
        if (const char *p = Env("RT_SETTINGS"))
            return p;
        if (const char *mc = Env("RT_MEMCARD"))
        {
            std::filesystem::path dir = std::filesystem::path(mc).parent_path();
            if (!dir.empty())
                return (dir / "graphics.ini").string();
        }
        return "graphics.ini";
    }

    template <size_t N>
    int IndexOf(const int (&list)[N], int v, int fallback)
    {
        for (size_t i = 0; i < N; ++i)
            if (list[i] == v)
                return static_cast<int>(i);
        return fallback;
    }

    std::map<std::string, std::string> ReadIni(const std::string &path)
    {
        std::map<std::string, std::string> kv;
        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line))
        {
            const size_t eq = line.find('=');
            if (line.empty() || line[0] == '#' || eq == std::string::npos)
                continue;
            auto trim = [](std::string s) {
                const size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
                return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
            };
            kv[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
        }
        return kv;
    }

    const char *AspectName(int a)
    {
        switch (a)
        {
        case Aspect169: return "16:9";
        case Aspect43: return "4:3";
        case AspectStretch: return "stretch";
        default: return "auto";
        }
    }

    void SaveSettings()
    {
        const std::string path = SettingsPath();
        const std::string tmp = path + ".tmp";
        {
            std::ofstream out(tmp, std::ios::trunc);
            if (!out)
                return;
            out << "# Ruff Trigger graphics settings (F10 in game changes them)\n"
                << "resolution=" << (kScales[g_set.scale] ? std::to_string(kScales[g_set.scale]) : std::string("auto")) << "\n"
                << "fullscreen=" << g_set.fullscreen << "\n"
                << "aspect=" << AspectName(g_set.aspect) << "\n"
                << "widescreen=" << g_set.widescreen << "\n"
                << "hud43=" << g_set.hud43 << "\n"
                << "vsync=" << g_set.vsync << "\n"
                << "fps=" << (kFps[g_set.fps] ? std::to_string(kFps[g_set.fps]) : std::string("original")) << "\n"
                << "showfps=" << g_set.showFps << "\n";
        }
        std::error_code ec;
        std::filesystem::rename(tmp, path, ec);
        if (ec)
            std::fprintf(stderr, "[gfx] could not save %s: %s\n", path.c_str(), ec.message().c_str());
    }

    // Before main(): the file's values become the environment the rest of the port reads at
    // start (window, renderer, widescreen), unless the user set those variables.
    struct ApplyAtStart
    {
        ApplyAtStart()
        {
            const auto kv = ReadIni(SettingsPath());
            auto get = [&](const char *k) -> const std::string * { auto it = kv.find(k); return it == kv.end() ? nullptr : &it->second; };
            auto bit = [](const std::string &v) { return v == "1" || v == "on" || v == "true" || v == "yes"; };
            if (auto v = get("resolution"))
                setenv("RT_GPU_SCALE", *v == "auto" ? "auto" : std::to_string(kScales[IndexOf(kScales, std::atoi(v->c_str()), 0)]).c_str(), 0);
            if (auto v = get("fullscreen"))
                setenv("RT_FULLSCREEN", bit(*v) ? "1" : "0", 0);
            if (auto v = get("widescreen"))
                setenv("RT_WIDESCREEN", bit(*v) ? "1" : "0", 0);
            if (auto v = get("hud43"))
                setenv("RT_WIDE_HUD", bit(*v) ? "1" : "0", 0);
            if (auto v = get("vsync"))
                setenv("RT_VSYNC", bit(*v) ? "1" : "0", 0);
            if (auto v = get("aspect"); v && *v != "auto")
                setenv("RT_ASPECT", v->c_str(), 0);
            if (auto v = get("fps"))
                g_set.fps = IndexOf(kFps, *v == "original" ? 0 : std::atoi(v->c_str()), 0);
            if (auto v = get("showfps"))
                g_set.showFps = bit(*v);
            if (const char *f = Env("RT_FPS"))   // RT_FPS=original|60|...|240
                g_set.fps = IndexOf(kFps, std::strcmp(f, "original") == 0 ? 0 : std::atoi(f), g_set.fps);
        }
    } g_applyAtStart;

    // The menu's view of what the port is actually using (environment, F9/F11 changes).
    void ReadCurrentState()
    {
        const char *sc = Env("RT_GPU_SCALE");
        g_set.scale = sc && std::strcmp(sc, "auto") != 0 ? IndexOf(kScales, std::atoi(sc), 0) : 0;
        g_set.fullscreen = Env("RT_FULLSCREEN") && std::atoi(Env("RT_FULLSCREEN")) != 0;
        g_set.widescreen = !Env("RT_WIDESCREEN") || std::atoi(Env("RT_WIDESCREEN")) != 0;
        g_set.hud43 = !Env("RT_WIDE_HUD") || std::atoi(Env("RT_WIDE_HUD")) != 0;
        g_set.vsync = !Env("RT_VSYNC") || std::atoi(Env("RT_VSYNC")) != 0;
        g_set.aspect = AspectAuto;
        if (const char *a = Env("RT_ASPECT"))
            g_set.aspect = !std::strcmp(a, "16:9") ? Aspect169 : !std::strcmp(a, "4:3") ? Aspect43 : !std::strcmp(a, "stretch") ? AspectStretch : AspectAuto;
    }

    // ---- frame rate ---------------------------------------------------------------------------
    constexpr uint32_t kTimer = 0x004C2FF0u, kStep = kTimer + 0x1Cu, kSlowMo = kTimer + 0x5A4u;
    constexpr uint32_t kVblankFlag = 0x00354444u;
    constexpr uint32_t kRaModeTestA = 0x001F9A64u, kRaModeTestB = 0x001F9A98u;
    constexpr uint32_t kRaWait60 = 0x001F9AF8u, kRaWait30 = 0x001F9B38u, kRaWait30b = 0x001F9B58u;

    PS2Runtime *g_rt = nullptr;
    PS2Runtime::RecompiledFunction g_waitInner = nullptr, g_timerOrig = nullptr, g_vbCountOrig = nullptr;
    std::atomic<int> g_targetFps{0};       // 0 = the game's own rate
    std::atomic<int> g_effectiveFps{0};    // target, lowered while the game can't keep full speed
    std::atomic<int64_t> g_lastLimiterNs{0};
    std::atomic<uint64_t> g_frameCount{0};
    std::atomic<uint32_t> g_logBase{0}, g_logDt{0}, g_logNative{0};

    // Game thread only.
    struct Pace
    {
        bool active = false, waiting = false;
        int64_t deadline = 0, lastExit = 0, nativeNs = 33366667;
        double acc = 0.0;
        uint32_t base = 20, written = 0;
    } g_pace;

    uint32_t R32(const uint8_t *rdram, uint32_t a) { uint32_t v; std::memcpy(&v, rdram + (a & 0x01FFFFFCu), 4); return v; }
    void W32(uint8_t *rdram, uint32_t a, uint32_t v) { std::memcpy(rdram + (a & 0x01FFFFFCu), &v, 4); }
    uint32_t Ra(const R5900Context *ctx) { return static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0)); }

    // f_002fc9f8 (timer getter), called by the limiter just before it tests the frame step.
    void TimerGetter(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ra = Ra(ctx);
        if (ctx->pc == 0x002FC9F8u && (ra == kRaModeTestA || ra == kRaModeTestB))
        {
            g_frameCount.fetch_add(1, std::memory_order_relaxed);
            const bool on = g_targetFps.load(std::memory_order_relaxed) > 0;
            if (on || g_pace.active)
            {
                const uint32_t cur = R32(rdram, kStep);
                if (!g_pace.active || cur != g_pace.written)
                    g_pace.base = cur;   // the game set its own step (level start, pause)
                W32(rdram, kStep, g_pace.base);
                if (!on)
                {
                    g_pace = Pace{};
                    rufftrigger::GpuStopGameFrames();
                }
            }
        }
        g_timerOrig(rdram, ctx, runtime);
    }

    // f_001ea4d8: vblank callback that counts vblanks for the limiter. Held while unlocked.
    void VblankCount(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == 0x001EA4D8u && g_pace.active)
        {
            ctx->pc = Ra(ctx);
            return;
        }
        g_vbCountOrig(rdram, ctx, runtime);
    }

    // f_00291140: wait for the next vblank.
    void FrameWait(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ra = Ra(ctx);
        const int fps = g_effectiveFps.load(std::memory_order_relaxed);
        if (ctx->pc != 0x00291140u || fps <= 0 || g_targetFps.load(std::memory_order_relaxed) <= 0 ||
            (ra != kRaWait60 && ra != kRaWait30 && ra != kRaWait30b))
        {
            g_waitInner(rdram, ctx, runtime);
            return;
        }
        const int64_t interval = 1000000000LL / fps;
        if (!g_pace.waiting)
        {
            const int64_t now = NowNs();
            g_pace.active = true;
            g_pace.waiting = true;
            g_pace.nativeNs = ra == kRaWait60 ? 16683333 : 33366667;
            g_pace.deadline = g_pace.lastExit ? g_pace.lastExit + interval : now;
            if (g_pace.deadline < now - interval)
                g_pace.deadline = now;   // fell behind: no burst of short frames
            g_lastLimiterNs.store(now, std::memory_order_relaxed);
            rufftrigger::GpuGameFrame();
        }
        // Guest time keeps running while we wait (other threads, interrupts, IOP, sound). Charge
        // enough per check that it keeps up with real time at any overclock; the scheduler holds
        // it back when it gets ahead.
        while (NowNs() < g_pace.deadline)
        {
            if (runtime->eeCheckpointDue(4096u))
            {
                ctx->pc = 0x00291140u;
                return;
            }
        }
        g_pace.waiting = false;
        const int64_t now = NowNs();
        const int64_t elapsed = std::min<int64_t>(g_pace.lastExit ? now - g_pace.lastExit : interval, g_pace.nativeNs);
        g_pace.lastExit = now;
        uint32_t step = 0;
        if (g_pace.base != 0)
        {
            g_pace.acc += static_cast<double>(g_pace.base) * static_cast<double>(elapsed) / static_cast<double>(g_pace.nativeNs);
            // Never a zero step (the game uses 0 for "paused"; code may divide by it): borrow.
            const uint32_t q = (rdram[kSlowMo & 0x01FFFFFFu] != 0) ? 3u : 1u;
            step = std::max(q, static_cast<uint32_t>(std::max(0.0, g_pace.acc) / q) * q);
            g_pace.acc = std::clamp(g_pace.acc - step, -static_cast<double>(q), static_cast<double>(g_pace.base));
        }
        else
            g_pace.acc = 0.0;
        W32(rdram, kStep, step);
        g_pace.written = step;
        g_logBase.store(g_pace.base, std::memory_order_relaxed);
        g_logDt.store(step, std::memory_order_relaxed);
        g_logNative.store(ra == kRaWait60 ? 60 : 30, std::memory_order_relaxed);
        W32(rdram, kVblankFlag, 1u);   // the game's wait returns at once
        g_waitInner(rdram, ctx, runtime);
    }

    // Window thread, every present: EE overclock and the effective frame rate.
    void PacingTick()
    {
        static int64_t t0 = 0;
        static uint64_t vs0 = 0;
        static int64_t logT = 0;
        const int target = g_targetFps.load(std::memory_order_relaxed);
        const int64_t now = NowNs();
        const bool running = target > 0 && now - g_lastLimiterNs.load(std::memory_order_relaxed) < 200000000;
        int eff = g_effectiveFps.load(std::memory_order_relaxed);
        if (target <= 0)
            eff = 0;
        else if (eff <= 0 || eff > target)
            eff = target;
        // Guest speed over the last second: below full speed the game's sound and timers lag, so
        // ask for fewer frames; raise them again while it keeps up.
        const uint64_t vs = g_rt ? g_rt->memory().gs().vsyncTick.load(std::memory_order_relaxed) : 0;
        if (t0 == 0 || !running)
        {
            t0 = now;
            vs0 = vs;
        }
        else if (now - t0 >= 1000000000LL)
        {
            const double speed = static_cast<double>(vs - vs0) / (static_cast<double>(now - t0) * 1e-9 * 59.94);
            if (speed < 0.95)
                eff = std::max(40, static_cast<int>(eff * 0.85));
            else if (speed > 0.985 && eff < target)
                eff = std::min(target, static_cast<int>(eff * 1.1) + 1);
            t0 = now;
            vs0 = vs;
        }
        g_effectiveFps.store(eff, std::memory_order_relaxed);
        const uint32_t pct = running ? static_cast<uint32_t>(std::clamp(100 * 30 / std::max(eff, 30), 10, 100)) : 0u;
        ps2x_capture::g_eeCyclePct.store(pct, std::memory_order_relaxed);
        static const bool log = Env("RT_FPSLOG") != nullptr;
        if (log && running && now - logT > 2000000000LL)
        {
            logT = now;
            std::fprintf(stderr, "[gfx] frame rate target %d effective %d | game step %u ms at %u fps -> %u ms | EE cycles %u%%\n", target, eff,
                         g_logBase.load(), g_logNative.load(), g_logDt.load(), pct);
        }
    }

    // ---- menu -----------------------------------------------------------------------------------
    PS2Runtime *g_menuRt = nullptr;
    ps2x_capture::OverlayHook g_prevOverlay = nullptr;
    bool g_open = false;
    int g_sel = 0;
    double g_openTime = 0.0, g_blockUntil = 0.0;
    Font g_font{};
    int g_fontPx = 0;
    double g_fpsT0 = 0.0;
    uint64_t g_fpsF0 = 0;
    float g_fpsShown = 0.0f;

    enum Row { RowRes, RowDisplay, RowAspect, RowWide, RowHud, RowVsync, RowFps, RowShowFps, RowDefaults, RowDone, RowCount };

    // Colours: the save icon's cream and green, dark ink outlines, a warm highlight.
    const Color kCream{246, 238, 205, 255}, kCreamDark{226, 214, 168, 255}, kInk{38, 30, 22, 255};
    const Color kGreen{58, 150, 54, 255}, kGreenDark{24, 84, 30, 255}, kGold{255, 196, 40, 255}, kRed{214, 64, 40, 255};

    void EnsureFont(int screenH)
    {
        const int want = std::clamp(screenH / 18, 24, 160);
        if (g_font.texture.id != 0 && std::abs(want - g_fontPx) * 6 < g_fontPx)
            return;
        if (g_font.texture.id != 0)
            UnloadFont(g_font);
        std::vector<int> cps;
        for (int c = 32; c < 127; ++c)
            cps.push_back(c);
        for (int c : {0x25C0, 0x25B6, 0x2191, 0x2193, 0x2022, 0x00D7})
            cps.push_back(c);
        g_font = LoadFontFromMemory(".ttf", kMenuFontTtf, static_cast<int>(sizeof(kMenuFontTtf)), want, cps.data(), static_cast<int>(cps.size()));
        SetTextureFilter(g_font.texture, TEXTURE_FILTER_BILINEAR);
        g_fontPx = want;
    }

    Vector2 Measure(const char *t, float size) { return MeasureTextEx(g_font, t, size, size * 0.02f); }

    // Bold cartoon text: dark outline, then the fill.
    void Text(const char *t, float x, float y, float size, Color fill, Color outline, float ow)
    {
        const float sp = size * 0.02f;
        if (ow > 0.0f)
        {
            for (int k = 0; k < 16; ++k)
            {
                const float a = k * 3.14159265f / 8.0f;
                DrawTextEx(g_font, t, {x + std::cos(a) * ow, y + std::sin(a) * ow}, size, sp, outline);
            }
        }
        DrawTextEx(g_font, t, {x, y}, size, sp, fill);
    }

    std::string ValueText(int row)
    {
        switch (row)
        {
        case RowRes:
        {
            const int s = kScales[g_set.scale];
            if (s == 0)
                return "Auto (" + std::to_string(rufftrigger::GpuCurrentScale() * 448) + "p)";
            return std::to_string(s) + "x  (" + std::to_string(s * 448) + "p)";
        }
        case RowDisplay: return g_set.fullscreen ? "Full screen" : "Window";
        case RowAspect:
            switch (g_set.aspect)
            {
            case Aspect169: return "16:9";
            case Aspect43: return "4:3";
            case AspectStretch: return "Stretch";
            default: return g_set.widescreen ? "Auto (16:9)" : "Auto (4:3)";
            }
        case RowWide: return g_set.widescreen ? "On" : "Off";
        case RowHud: return g_set.hud43 ? "4:3 (centred)" : "Stretched";
        case RowVsync: return g_set.vsync ? "On" : "Off";
        case RowFps: return kFps[g_set.fps] ? std::to_string(kFps[g_set.fps]) + " FPS" : "30 / 60 (Original)";
        case RowShowFps: return g_set.showFps ? "On" : "Off";
        default: return "";
        }
    }

    const char *Label(int row)
    {
        static const char *k[] = {"Resolution", "Display", "Aspect ratio", "Widescreen", "HUD", "VSync", "Frame rate", "Show FPS",
                                  "Restore defaults", "Done"};
        return k[row];
    }

    std::string Help(int row)
    {
        switch (row)
        {
        case RowRes: return "Internal rendering resolution. Auto follows the window size.";
        case RowDisplay: return "Borderless full screen or a window (F11 also switches).";
        case RowAspect: return "Shape of the picture. Auto follows the Widescreen setting.";
        case RowWide: return "16:9 camera that shows more at the sides. Applies when the camera next resets.";
        case RowHud: return "Keep menus and the HUD in their original 4:3 shape in widescreen.";
        case RowVsync: return "Sync to the display. Off can tear but shows frames as soon as they are ready.";
        case RowFps:
        {
            const int hz = GetMonitorRefreshRate(GetCurrentMonitor());
            std::string s = "Experimental above 30/60: the game's time step is scaled. Speed drops? It lowers itself.";
            if (kFps[g_set.fps] > 0 && hz > 0 && g_set.vsync && kFps[g_set.fps] > hz)
                s = "Your display runs at " + std::to_string(hz) + " Hz, so with VSync on you will see at most " + std::to_string(hz) + ".";
            return s;
        }
        case RowShowFps: return "Frame counter in the corner.";
        case RowDefaults: return "Put every setting back to the port's defaults.";
        default: return "";
        }
    }

    void ApplyAspect()
    {
        float a = 0.0f;
        switch (g_set.aspect)
        {
        case Aspect169: a = 16.0f / 9.0f; break;
        case Aspect43: a = 4.0f / 3.0f; break;
        case AspectStretch: a = -1.0f; break;
        default: a = g_set.widescreen ? 16.0f / 9.0f : 4.0f / 3.0f; break;
        }
        ps2x_capture::g_displayAspect.store(a);
    }

    void ApplyAll(bool startup)
    {
        if (!startup)
        {
            rufftrigger::GpuSetScale(kScales[g_set.scale]);
            if (IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE) != g_set.fullscreen)
                ToggleBorderlessWindowed();
            ps2x_capture::g_vsyncRequest.store(g_set.vsync ? 1 : 0);
        }
        rufftrigger::SetWidescreenCamera(g_set.widescreen);
        rufftrigger::GpuSetHudSquash(g_set.widescreen && g_set.hud43 ? 0.75f : 1.0f);
        ApplyAspect();
        g_targetFps.store(kFps[g_set.fps]);
        g_effectiveFps.store(kFps[g_set.fps]);
    }

    void Change(int row, int dir)
    {
        auto cycle = [dir](int v, int n) { return ((v + dir) % n + n) % n; };
        switch (row)
        {
        case RowRes: g_set.scale = cycle(g_set.scale, static_cast<int>(std::size(kScales))); rufftrigger::GpuSetScale(kScales[g_set.scale]); break;
        case RowDisplay:
            g_set.fullscreen = !g_set.fullscreen;
            if (IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE) != g_set.fullscreen)
                ToggleBorderlessWindowed();
            break;
        case RowAspect: g_set.aspect = cycle(g_set.aspect, 4); ApplyAspect(); break;
        case RowWide:
            g_set.widescreen = !g_set.widescreen;
            rufftrigger::SetWidescreenCamera(g_set.widescreen);
            rufftrigger::GpuSetHudSquash(g_set.widescreen && g_set.hud43 ? 0.75f : 1.0f);
            ApplyAspect();
            break;
        case RowHud:
            g_set.hud43 = !g_set.hud43;
            rufftrigger::GpuSetHudSquash(g_set.widescreen && g_set.hud43 ? 0.75f : 1.0f);
            break;
        case RowVsync: g_set.vsync = !g_set.vsync; ps2x_capture::g_vsyncRequest.store(g_set.vsync ? 1 : 0); break;
        case RowFps:
            g_set.fps = cycle(g_set.fps, static_cast<int>(std::size(kFps)));
            g_targetFps.store(kFps[g_set.fps]);
            g_effectiveFps.store(kFps[g_set.fps]);
            std::fprintf(stderr, "[gfx] frame rate: %s\n", kFps[g_set.fps] ? (std::to_string(kFps[g_set.fps]) + " fps").c_str() : "original");
            break;
        case RowShowFps: g_set.showFps = !g_set.showFps; break;
        case RowDefaults:
            g_set = Settings{};
            ApplyAll(false);
            break;
        default: return;
        }
        SaveSettings();
    }

    bool PadPressed(int b) { return IsGamepadAvailable(0) && IsGamepadButtonPressed(0, b); }
    bool PadDown(int b) { return IsGamepadAvailable(0) && IsGamepadButtonDown(0, b); }

    void HandleInput()
    {
        const bool toggle = IsKeyPressed(KEY_F10) ||
                            (PadDown(GAMEPAD_BUTTON_LEFT_THUMB) && PadPressed(GAMEPAD_BUTTON_RIGHT_THUMB)) ||
                            (PadDown(GAMEPAD_BUTTON_RIGHT_THUMB) && PadPressed(GAMEPAD_BUTTON_LEFT_THUMB));
        if (!g_open)
        {
            if (toggle)
            {
                g_open = true;
                g_openTime = GetTime();
                g_set.fullscreen = IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE);
            }
            return;
        }
        if (toggle || IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_BACKSPACE) || PadPressed(GAMEPAD_BUTTON_RIGHT_FACE_RIGHT))
        {
            g_open = false;
            g_blockUntil = GetTime() + 0.3;
            return;
        }
        if (IsKeyPressed(KEY_DOWN) || PadPressed(GAMEPAD_BUTTON_LEFT_FACE_DOWN))
            g_sel = (g_sel + 1) % RowCount;
        if (IsKeyPressed(KEY_UP) || PadPressed(GAMEPAD_BUTTON_LEFT_FACE_UP))
            g_sel = (g_sel + RowCount - 1) % RowCount;
        const int dir = (IsKeyPressed(KEY_RIGHT) || PadPressed(GAMEPAD_BUTTON_LEFT_FACE_RIGHT) ? 1 : 0) -
                        (IsKeyPressed(KEY_LEFT) || PadPressed(GAMEPAD_BUTTON_LEFT_FACE_LEFT) ? 1 : 0);
        const bool ok = IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE) || IsKeyPressed(KEY_X) || PadPressed(GAMEPAD_BUTTON_RIGHT_FACE_DOWN);
        if (g_sel == RowDone)
        {
            if (ok)
            {
                g_open = false;
                g_blockUntil = GetTime() + 0.3;
            }
            return;
        }
        if (g_sel == RowDefaults)
        {
            if (ok)
                Change(RowDefaults, 0);
            return;
        }
        if (dir)
            Change(g_sel, dir);
        else if (ok)
            Change(g_sel, 1);
    }

    void DrawFps(int screenW, int screenH)
    {
        const double t = GetTime();
        const uint64_t f = g_frameCount.load(std::memory_order_relaxed);
        if (g_fpsT0 == 0.0 || t - g_fpsT0 >= 0.5)
        {
            if (g_fpsT0 != 0.0)
                g_fpsShown = static_cast<float>((f - g_fpsF0) / (t - g_fpsT0));
            g_fpsT0 = t;
            g_fpsF0 = f;
        }
        const float size = screenH / 26.0f;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.0f FPS", g_fpsShown);
        const int target = g_effectiveFps.load(std::memory_order_relaxed);
        const Color c = target > 0 && g_fpsShown < target * 0.9f ? kGold : kCream;
        const Vector2 m = Measure(buf, size);
        Text(buf, screenW - m.x - size * 0.6f, size * 0.4f, size, c, kInk, std::max(1.5f, size * 0.08f));
    }

    void DrawMenu(int screenW, int screenH)
    {
        const float u = screenH / 720.0f;
        const double t = GetTime();
        const float open = static_cast<float>(std::min(1.0, (t - g_openTime) / 0.15));
        const float ease = 1.0f - (1.0f - open) * (1.0f - open);
        DrawRectangle(0, 0, screenW, screenH, Fade(BLACK, 0.55f * ease));

        const float pw = std::min(screenW - 40.0f * u, 760.0f * u), rowH = 41.0f * u;
        const float ph = 66.0f * u + rowH * static_cast<float>(RowCount) + 14.0f * u + 100.0f * u;
        const float px = (screenW - pw) * 0.5f, py = (screenH - ph) * 0.5f + 18.0f * u + (1.0f - ease) * 40.0f * u;
        const Rectangle panel{px, py, pw, ph};
        DrawRectangleRounded({px + 10 * u, py + 12 * u, pw, ph}, 0.08f, 12, Fade(BLACK, 0.45f));
        DrawRectangleRounded(panel, 0.08f, 12, kCream);
        DrawRectangleRoundedLinesEx(panel, 0.08f, 12, 6.0f * u, kInk);

        // Title banner.
        const char *title = "GRAPHICS";
        const float ts = 58.0f * u;
        const Vector2 tm = Measure(title, ts);
        const Rectangle banner{px + pw * 0.5f - tm.x * 0.5f - 34 * u, py - 30 * u, tm.x + 68 * u, 74 * u};
        DrawRectangleRounded({banner.x + 6 * u, banner.y + 8 * u, banner.width, banner.height}, 0.5f, 12, Fade(BLACK, 0.4f));
        DrawRectangleRounded(banner, 0.5f, 12, kGreen);
        DrawRectangleRoundedLinesEx(banner, 0.5f, 12, 5.0f * u, kInk);
        Text(title, banner.x + (banner.width - tm.x) * 0.5f, banner.y + (banner.height - tm.y) * 0.5f, ts, kCream, kInk, 3.5f * u);

        const float fs = 28.0f * u;
        float y = py + 62.0f * u;
        const float lx = px + 62 * u, vx = px + pw * 0.47f, vw = pw * 0.47f - 30 * u;
        for (int r = 0; r < RowCount; ++r, y += rowH)
        {
            const bool sel = r == g_sel;
            if (r == RowDefaults)
                y += 14 * u;
            if (sel)
            {
                const float wob = std::sin(static_cast<float>(t) * 6.0f) * 3.0f * u;
                const Rectangle hl{px + 22 * u, y - 4 * u, pw - 44 * u, rowH - 2 * u};
                DrawRectangleRounded(hl, 0.5f, 10, kGold);
                DrawRectangleRoundedLinesEx(hl, 0.5f, 10, 3.5f * u, kInk);
                Text("\xE2\x96\xB6", hl.x + 12 * u + wob, y + (rowH - fs) * 0.5f - 6 * u, fs, kRed, kInk, 2.0f * u);
            }
            if (r >= RowDefaults)
            {
                const Vector2 m = Measure(Label(r), fs);
                Text(Label(r), px + (pw - m.x) * 0.5f, y + (rowH - fs) * 0.5f - 6 * u, fs, sel ? kInk : kGreenDark, kCream, 0.0f);
                continue;
            }
            Text(Label(r), lx, y + (rowH - fs) * 0.5f - 6 * u, fs, kInk, kCream, 0.0f);
            const std::string v = ValueText(r);
            const Vector2 m = Measure(v.c_str(), fs);
            const float cx = vx + vw * 0.5f;
            const Color vc = (r == RowFps && g_set.fps > 0) ? kRed : (sel ? kCream : kGreen);
            Text(v.c_str(), cx - m.x * 0.5f, y + (rowH - fs) * 0.5f - 6 * u, fs, vc, kInk, sel ? 2.5f * u : 0.0f);
            const float as = fs * 0.8f;
            Text("\xE2\x97\x80", vx, y + (rowH - as) * 0.5f - 5 * u, as, sel ? kRed : kCreamDark, kInk, sel ? 2.0f * u : 0.0f);
            Text("\xE2\x96\xB6", vx + vw - Measure("\xE2\x96\xB6", as).x, y + (rowH - as) * 0.5f - 5 * u, as, sel ? kRed : kCreamDark, kInk, sel ? 2.0f * u : 0.0f);
        }

        // Help line and controls.
        const float hs = 21.0f * u;
        const std::string help = Help(g_sel);
        const Vector2 hm = Measure(help.c_str(), hs);
        const float hy = py + ph - 74 * u;
        DrawRectangleRounded({px + 22 * u, hy - 6 * u, pw - 44 * u, 34 * u}, 0.5f, 8, kCreamDark);
        Text(help.c_str(), px + std::max(30 * u, (pw - hm.x) * 0.5f), hy, std::min(hs, hs * (pw - 60 * u) / std::max(1.0f, hm.x)), kInk, kCream, 0.0f);
        const char *keys = "\xE2\x86\x91\xE2\x86\x93 select    \xE2\x97\x80\xE2\x96\xB6 change    Enter / \xC3\x97 ok    Esc / F10 close";
        const Vector2 km = Measure(keys, hs);
        Text(keys, px + (pw - km.x) * 0.5f, py + ph - 34 * u, hs, kGreenDark, kCream, 0.0f);
    }

    void Overlay(int screenW, int screenH)
    {
        if (g_prevOverlay)
            g_prevOverlay(screenW, screenH);
        PacingTick();
        EnsureFont(screenH);
        HandleInput();
        if (g_open)
            DrawMenu(screenW, screenH);
        if (g_set.showFps)
            DrawFps(screenW, screenH);
        // The debug menu (when installed) sets the pad block every frame; otherwise clear it here.
        if (g_open || GetTime() < g_blockUntil)
            ps2x_capture::g_padBlock.store(true);
        else if (!g_prevOverlay)
            ps2x_capture::g_padBlock.store(false);
    }
}

namespace rufftrigger
{
    void InstallSettings(PS2Runtime &runtime)
    {
        g_rt = &runtime;
        g_menuRt = &runtime;
        ReadCurrentState();
        ApplyAll(true);
        // RT_VSYNC=0 at start presents on a 60 Hz timer; the menu's "off" presents uncapped.
        if (!g_set.vsync)
            ps2x_capture::g_vsyncRequest.store(0);
        if (const char *e = Env("RT_FPS_UNLOCK"); e && std::atoi(e) == 0)
            g_targetFps.store(0);
        else if (PS2Runtime::RecompiledFunction w = runtime.lookupFunction(0x00291140u),
                 tg = runtime.lookupFunction(0x002FC9F8u), vb = runtime.lookupFunction(0x001EA4D8u);
                 w && tg && vb)
        {
            g_waitInner = w;
            g_timerOrig = tg;
            g_vbCountOrig = vb;
            runtime.replaceFunction(0x00291140u, &FrameWait);
            runtime.replaceFunction(0x002FC9F8u, &TimerGetter);
            runtime.replaceFunction(0x001EA4D8u, &VblankCount);
        }
        else
            g_targetFps.store(0);
        if (const char *o = Env("RT_GFXMENU_OPEN"))   // start with the menu open (testing)
        {
            g_open = true;
            g_sel = std::clamp(std::atoi(o), 0, RowCount - 1);
        }
        g_prevOverlay = ps2x_capture::g_overlayHook.load();
        ps2x_capture::g_overlayHook.store(&Overlay);
        std::fprintf(stderr, "[gfx] F10 (gamepad: L3 + R3) opens the graphics menu; settings in %s; frame rate %s\n", SettingsPath().c_str(),
                     kFps[g_set.fps] ? (std::to_string(kFps[g_set.fps]) + " fps").c_str() : "original");
    }
}
