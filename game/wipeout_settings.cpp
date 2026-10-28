// WipEout Fusion PC settings menu (F10, or L3 + R3 on a gamepad), drawn over the game.
//
// Graphics: internal resolution (upscaling), window / full screen, picture shape, vsync and an
// FPS counter. They apply at once and are kept in graphics.ini next to the game data
// (RT_SETTINGS=file to move it). Environment variables set by the user (RT_GPU_SCALE, RT_ASPECT,
// ...) win over the file at start; the menu can still change them afterwards.
//
// Debug: "Unlock everything" applies the game's own unlock-all cheat (cheat 8 of the code table
// at 0x247190, handled at 0x168AD0): every team, ship, track and mode, and 50,000,000 credits.
// The writes run on the game thread, from the vsync handler (0x205758), so they use the game's
// own $gp. Save the game from its menus afterwards to keep the unlocks.
#include "ps2_runtime.h"
#include "runtime/ps2_capture_hook.h"
#include "runtime/gs/gs_backend.h"
#include "raylib.h"
#include "ps2x/iop/iop_audio.h"
#include "rufftrigger_menu_font.h"

#include "rt_platform.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <deque>
#include <array>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace gsgpu
{
    void SetDeinterlaceMode(int mode);
    void NoteRaceHud();
    void SetEdgeHud(bool on);
    void SetHudLayout(int layout);
}

namespace rufftrigger
{
    void GpuSetScale(int scale);
    uint32_t GpuCurrentScale();
}

namespace
{
    const char *Env(const char *name)
    {
        const char *v = std::getenv(name);
        return v && *v ? v : nullptr;
    }

    // ---- settings ---------------------------------------------------------------------------
    const int kScales[] = {0, 1, 2, 3, 4, 5, 6, 8};
    enum Aspect { Aspect43, Aspect169, AspectStretch, AspectCount };

    struct Settings
    {
        int scale = 0;   // index into kScales (0 = auto)
        bool fullscreen = false;
        int aspect = Aspect43;
        bool vsync = true;
        bool showFps = false;
        bool fastLoad = true;
        bool gpuModels = true;   // models transformed on the GPU (else the CPU, PS2-exact)
        bool mpWeapons = false;   // fun mod: 2-player weapons in single-player races
        bool superWeapons = false;   // fun mod: every team's super weapon for every ship
        int damage = 1;   // fun mod: damage taken, index into kDamageScale
        int uses = 0;     // fun mod: extra uses of each weapon picked up (0 = normal)
        bool noLimits = false;   // fun mod: lift the per-race limits on each weapon
        bool edgeHud = true;   // 16:9: race HUD out at the screen edges (else stretched)
        int fov = 0;   // race camera field of view, degrees added to the game's 110
        bool customWeapons = false;   // debug: weapon pads give only the weapons in weaponMask
        uint32_t weaponMask = 0x07EFFFFFu;   // bit = weapon id (all pad weapons; 20 is unused)
    };
    // Weapon pad weapons, by the game's weapon id (name table 0x2482F8). Id 20 (SHIELD_RAIDER,
    // per-race limit 0) never comes off a pad and is left out.
    struct WeaponName { int id; const char *name; };
    constexpr WeaponName kPadWeapons[] = {
        {0, "Turbo"}, {1, "Autopilot"}, {2, "Rockets"}, {3, "Mines"}, {4, "Missile"}, {5, "Quake Disruptor"},
        {6, "Gravity Bomb"}, {7, "Super Missiles"}, {8, "Orbital Laser"}, {9, "Power Swarm"}, {10, "Shield Drain"},
        {11, "Nitro Rocket"}, {12, "Penetrator"}, {13, "Flamer"}, {14, "Plasma Bolt"}, {15, "Seismic Field"},
        {16, "Grenades"}, {17, "Bio Snare"}, {18, "AG Stinger"}, {19, "Proton Cannon"}, {21, "Hunter Missile"},
        {22, "Turbo Enforcer"}, {23, "Stealth"}, {24, "Global Drain"}, {25, "Shield"}, {26, "Control Jammer"}};
    constexpr int kPadWeaponCount = static_cast<int>(std::size(kPadWeapons));
    constexpr uint32_t kAllPadWeapons = 0x07EFFFFFu;
    Settings g_set;
    constexpr int kFovMin = -30, kFovMax = 40, kFovStep = 5, kGameFov = 94;
    // The game's interlaced fields look right with Bob, bottom field first (gsgpu mode 3);
    // tested against the other PCSX2-style modes, so it is fixed rather than a setting.
    constexpr int kDeinterlaceMode = 3;

    std::string SettingsPath()
    {
        if (const char *p = Env("RT_SETTINGS"))
            return p;
        return "graphics.ini";   // play.sh runs in the game folder
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
        case AspectStretch: return "stretch";
        default: return "4:3";
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
            out << "# WipEout Fusion PC settings (F10 in game changes them)\n"
                << "resolution=" << (kScales[g_set.scale] ? std::to_string(kScales[g_set.scale]) : std::string("auto")) << "\n"
                << "fullscreen=" << g_set.fullscreen << "\n"
                << "aspect=" << AspectName(g_set.aspect) << "\n"
                << "vsync=" << g_set.vsync << "\n"
                << "showfps=" << g_set.showFps << "\n"
                << "fastloading=" << g_set.fastLoad << "\n"
                << "gputransform=" << g_set.gpuModels << "\n"
                << "mpweapons=" << g_set.mpWeapons << "\n"
                << "superweapons=" << g_set.superWeapons << "\n"
                << "damage=" << g_set.damage << "\n"
                << "weaponuses=" << g_set.uses + 1 << "\n"
                << "nolimits=" << g_set.noLimits << "\n"
                << "weaponset=" << g_set.customWeapons << "\n"
                << "weapons=" << std::hex << g_set.weaponMask << std::dec << "\n"
                << "racehud=" << (g_set.edgeHud ? "edges" : "stretched") << "\n"
                << "fov=" << g_set.fov << "\n";
        }
        std::error_code ec;
        std::filesystem::rename(tmp, path, ec);
        if (ec)
            std::fprintf(stderr, "[gfx] could not save %s: %s\n", path.c_str(), ec.message().c_str());
    }

    // Before main(): the file's values become the environment the rest of the port reads at
    // start (window, renderer), unless the user set those variables.
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
            if (auto v = get("vsync"))
                setenv("RT_VSYNC", bit(*v) ? "1" : "0", 0);
            setenv("RT_ASPECT", get("aspect") ? get("aspect")->c_str() : "4:3", 0);
            if (auto v = get("showfps"))
                g_set.showFps = bit(*v);
            if (auto v = get("gputransform"))
                setenv("RT_GPUMODELS", bit(*v) ? "1" : "0", 0);
            if (auto v = get("fastloading"))
                g_set.fastLoad = bit(*v);
            if (auto v = get("mpweapons"))
                g_set.mpWeapons = bit(*v);
            if (auto v = get("superweapons"))
                g_set.superWeapons = bit(*v);
            if (auto v = get("damage"))
                g_set.damage = std::clamp(std::atoi(v->c_str()), 0, 3);
            if (auto v = get("weaponuses"))
                g_set.uses = std::clamp(std::atoi(v->c_str()) - 1, 0, 2);
            if (auto v = get("nolimits"))
                g_set.noLimits = bit(*v);
            if (auto v = get("weaponset"))
                g_set.customWeapons = bit(*v);
            if (auto v = get("weapons"))
                g_set.weaponMask = static_cast<uint32_t>(std::strtoul(v->c_str(), nullptr, 16)) & kAllPadWeapons;
            if (auto v = get("racehud"))
                g_set.edgeHud = *v != "stretched";
            if (auto v = get("fov"))
                g_set.fov = std::clamp(std::atoi(v->c_str()) / 5 * 5, kFovMin, kFovMax);
        }
    } g_applyAtStart;

    // The menu's view of what the port is actually using (environment, F11 changes).
    void ReadCurrentState()
    {
        const char *sc = Env("RT_GPU_SCALE");
        g_set.scale = sc && std::strcmp(sc, "auto") != 0 ? IndexOf(kScales, std::atoi(sc), 0) : 0;
        g_set.fullscreen = Env("RT_FULLSCREEN") && std::atoi(Env("RT_FULLSCREEN")) != 0;
        g_set.vsync = !Env("RT_VSYNC") || std::atoi(Env("RT_VSYNC")) != 0;
        g_set.gpuModels = !Env("RT_GPUMODELS") || std::atoi(Env("RT_GPUMODELS")) != 0;
        g_set.aspect = Aspect43;
        if (const char *a = Env("RT_ASPECT"))
            g_set.aspect = !std::strcmp(a, "16:9") ? Aspect169 : !std::strcmp(a, "stretch") ? AspectStretch : Aspect43;
    }

    // ---- unlock everything (game thread) ------------------------------------------------------
    PS2Runtime *g_rt = nullptr;
    constexpr uint32_t kGameGp = 0x002891F0u;   // the ELF's $gp (the cheat addresses gp-0x6524)
    std::atomic<bool> g_unlockRequest{false};
    std::atomic<int> g_unlockDone{0};   // 0 = not yet, 1 = applied
    int g_unlockDelay = 0;              // RT_UNLOCKALL=N: unlock after N pad reads (testing)

    uint32_t R32(const uint8_t *rdram, uint32_t a) { uint32_t v; std::memcpy(&v, rdram + (a & 0x01FFFFFCu), 4); return v; }
    void W32(uint8_t *rdram, uint32_t a, uint32_t v) { std::memcpy(rdram + (a & 0x01FFFFFCu), &v, 4); }
    uint64_t R64(const uint8_t *rdram, uint32_t a) { uint64_t v; std::memcpy(&v, rdram + (a & 0x01FFFFF8u), 8); return v; }
    void W64(uint8_t *rdram, uint32_t a, uint64_t v) { std::memcpy(rdram + (a & 0x01FFFFF8u), &v, 8); }

    // The cheat's effect (0x168AD0..0x168B74), minus its jingle and its call to 0x140860.
    void UnlockEverything(uint8_t *rdram, uint32_t gp)
    {
        constexpr uint32_t kSave = 0x003A4968u, kState = 0x003965A0u;
        W32(rdram, kSave + 0xF68, R32(rdram, kSave + 0xF68) | 3u);
        W32(rdram, kSave + 0x8C, 50000000u);   // credits
        W32(rdram, kSave + 0xF58, 0xFFFFu);
        W64(rdram, kSave + 0xF60, 0x0000FFFFFFFFFFFFull);
        W32(rdram, kSave + 0xF6C, 0x3FFu);
        W32(rdram, kSave + 0x1C, 0x1Fu);
        W32(rdram, kSave + 0x10, 0x07EFFFFFu);
        W64(rdram, kSave, R64(rdram, kSave) | 0x0000FFFFFFFFFFFFull);
        W32(rdram, kSave + 0x14, 0x7FFu);
        W32(rdram, kState + 0x8C, 0xFFFFFFFFu);
        W32(rdram, kState + 0x20, 0u);
        W32(rdram, gp - 0x6524u, 1u);
        std::fprintf(stderr, "[debug] unlocked everything (gp=%08x)\n", gp);
    }

    // Runs where the game reads the pad (game thread, once a frame), so the writes land between frames.
    ps2x_capture::PadHook g_prevPadHook = nullptr;
    uint16_t PadHook()
    {
        if (g_unlockDelay > 0 && --g_unlockDelay == 0)
            g_unlockRequest.store(true);
        // RT_SPLIT=2|3 (tests) also sets the multiplayer split-screen option (prefs +0x80, 1 = mode 2).
        if (static const int split = std::atoi(std::getenv("RT_SPLIT") ? std::getenv("RT_SPLIT") : "0"); split >= 2)
            W32(g_rt->memory().getRDRAM(), 0x00396620u, split == 2 ? 1u : 0u);
        if (g_unlockRequest.exchange(false))
        {
            UnlockEverything(g_rt->memory().getRDRAM(), kGameGp);
            g_unlockDone.store(1);
        }
        return g_prevPadHook ? g_prevPadHook() : 0;
    }

    // ---- fast loading (game thread) -------------------------------------------------------------
    // The game's file read (0x201CB8, a3 = 0x20 for the menu movies' streaming reads, 0x4000 for
    // everything else) runs at most a few times a second in the menus and in a race, but 30-150
    // times a second while it loads; vblank pacing, not the PC, is what makes loading take its
    // PS2 time. Fast loading turns on once reads have kept up that rate (12 non-streaming reads
    // within any 30 vblanks) for a whole second, so the short loads between menu screens stay at
    // normal speed, and off 30 vblanks after the last read. Meanwhile the EE scheduler stops
    // waiting for the host clock (ps2x_capture::g_turbo) and the sound is muted (it would
    // play several times too fast).
    // The race HUD's per-frame gauge update (0x17F4F8, thrust and shield bars) runs only while
    // racing: it tells the renderer to lay the HUD out to the screen edges in widescreen.
    void WeaponTuningTick(uint8_t *rdram);
    // The lock-on reticle (0x18C058, three arcs of DATA/HUD/LOCKON_CIRCLE.HUD drawn round the
    // target) sits on the 3D picture, so the widescreen HUD layout must leave it alone. It goes
    // into the player's HUD packet (write pointer at 0x358C50 + 0x34 * player + 0x10, as
    // 0x184C18 uses it): it is bracketed there with writes to GS register 0x7E, which no GS has,
    // that the renderer reads as "not part of the HUD" (bit 0 set, centre x in bits 16-31) and
    // "HUD again" (0).
    PS2Runtime::RecompiledFunction g_reticleOrig = nullptr;
    void ReticleWrap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t gp = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[28], 0));
        const uint32_t player = R32(rdram, R32(rdram, gp - 0x3D6Cu) + 0x120u);
        const uint32_t ptrAt = 0x358C50u + 0x34u * player + 0x10u;
        auto mark = [&](uint32_t on) {
            const uint32_t at = R32(rdram, ptrAt);
            if (player > 3 || at < 0x100000u || at >= 0x2000000u)
                return;
            uint8_t *q = rdram + at;
            const uint64_t words[6] = {0x10000002ull, 0x5000000200000000ull, 0x1000000000008001ull, 0xEull, on, 0x7Eull};
            std::memcpy(q, words, sizeof words);
            W32(rdram, ptrAt, at + 0x30u);
        };
        static const bool log = std::getenv("RT_RETLOG") != nullptr;
        const float f12 = ctx->f[12], f13 = ctx->f[13];
        const uint32_t before = R32(rdram, ptrAt);
        const auto t0 = std::chrono::steady_clock::now();
        // a0: the reticle's centre x on the frame (12.4 fixed point).
        mark(1u | (static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0)) & 0xFFFFu) << 16);
        g_reticleOrig(rdram, ctx, runtime);
        mark(0);
        if (log)
            std::fprintf(stderr, "[reticle] f12 %g f13 %g player %u packet %x -> %x, %lld us\n", f12, f13, player, before, R32(rdram, ptrAt),
                         (long long)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
    }

    // ---- in-game Graphics page (Options) ------------------------------------------------------
    // Calls a recompiled game function from a hook: arguments in a0-a3, t0-t3; every GPR, hi and
    // lo come back as they were, the return value is v0.

    PS2Runtime::RecompiledFunction g_hudGaugeOrig = nullptr;
    void HudGaugeWrap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        gsgpu::NoteRaceHud();
        g_hudGaugeOrig(rdram, ctx, runtime);
        WeaponTuningTick(rdram);
        if (static const bool slog = std::getenv("RT_SHIPLOG") != nullptr; slog)
        {
            static int n = 0;
            if (++n % 30 == 0)
            {
                std::string line;
                for (uint32_t i = 0; i < 16; ++i)
                {
                    const uint32_t ship = 0x2DBD20u + i * 0xE90u;
                    uint32_t pilot, wid, ammo;
                    std::memcpy(&pilot, rdram + ship + 0xE00u, 4);
                    std::memcpy(&wid, rdram + ship + 0x40u + 0xCA4u, 4);
                    std::memcpy(&ammo, rdram + ship + 0xDF8u, 4);
                    char b[48];
                    std::snprintf(b, sizeof b, " %u:%d/%d/%d", i, pilot ? rdram[(pilot & 0x1FFFFFFu) + 0xC] : -1, static_cast<int32_t>(wid), static_cast<int32_t>(ammo));
                    line += b;
                }
                std::fprintf(stderr, "[ships]%s\n", line.c_str());
            }
        }
    }

    // ---- widescreen camera -------------------------------------------------------------------
    // Race setup (0x19E228) picks a view mode and calls 0x1EC160 with it (ra 0x19E408): 0 = full
    // screen 4:3, 1 = full screen 16:9 (the in-game Widescreen option, prefs word 0x3965CC),
    // 2 = 2-player side by side, 3 = 2-player one above the other (multiplayer prefs word
    // 0x396620, 1 = side by side). Each mode's viewports come from the table at 0x24DA60 (0x20
    // bytes each: viewport, FOV 110, aspect, far 6000); there is no widescreen version of the
    // split screens. With the 16:9 picture the 1-player camera always uses mode 1 and the
    // split-screen viewports get the same 4/3 widening (aspect 4/3 -> 16/9); with 4:3 the
    // original values come back. Stretch leaves the game's own option alone.
    // The mode also tells the renderer which race HUD layout (1 or 2 players) to widen.
    // Field of view: the race view's projection is rebuilt every frame by 0x1EDF00 (a0 = view)
    // from the view's projection distance +0x44 and width +0x3C (horizontal FOV =
    // 2 atan(w/2 / d), 94 degrees with the game's d = 240; the camera code sets d, the table
    // FOV at 0x24DA60 is not used in races). The wrapper widens it by g_set.fov degrees for
    // that call only, so the camera code and everything else keep the game's value.
    bool g_raceView = false;   // the last view setup was a race's (ViewSetupWrap)
    PS2Runtime::RecompiledFunction g_projOrig = nullptr;
    void ProjectionWrap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t view = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0)) & 0x01FFFFFFu;
        const int add = g_set.fov;
        float d = 0.0f, w = 0.0f;
        if (view && g_raceView && add)
        {
            std::memcpy(&d, rdram + view + 0x44, 4);
            std::memcpy(&w, rdram + view + 0x3C, 4);
        }
        if (!(d > 1.0f && w > 1.0f))
        {
            g_projOrig(rdram, ctx, runtime);
            return;
        }
        constexpr double kRad = 3.14159265358979 / 180.0;
        const double half = std::clamp(std::atan(0.5 * w / d) + 0.5 * add * kRad, 10.0 * kRad, 80.0 * kRad);
        const float wide = static_cast<float>(0.5 * w / std::tan(half));
        std::memcpy(rdram + view + 0x44, &wide, 4);
        g_projOrig(rdram, ctx, runtime);
        std::memcpy(rdram + view + 0x44, &d, 4);
        if (static const bool log = std::getenv("RT_FOVLOG") != nullptr; log)
            if (static int n = 0; n++ % 600 == 0)
                std::fprintf(stderr, "[fov] view %08x: distance %g -> %g (%.1f -> %.1f degrees)\n", view, d, wide,
                             2.0 * std::atan(0.5 * w / d) / kRad, 2.0 * half / kRad);
    }

    PS2Runtime::RecompiledFunction g_viewOrig = nullptr;
    constexpr uint32_t kViewSetupRet = 0x0019E408u, kSplitAspects[] = {0x0024DAB4u, 0x0024DAD4u, 0x0024DAF4u, 0x0024DB14u};
    void ViewSetupWrap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t mode = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0));
        const bool race = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0)) == kViewSetupRet;
        if (race)
            gsgpu::SetHudLayout(mode == 2 ? 2 : mode == 3 ? 1 : 0);
        if (race && g_set.aspect != AspectStretch)
        {
            const bool wide = g_set.aspect == Aspect169;
            if (mode <= 1)
                ctx->r[4] = _mm_insert_epi32(ctx->r[4], wide ? 1 : 0, 0);
            const float a = wide ? 16.0f / 9.0f : 4.0f / 3.0f;
            for (uint32_t addr : kSplitAspects)
                std::memcpy(rdram + addr, &a, 4);
        }
        g_raceView = race;
        g_viewOrig(rdram, ctx, runtime);
    }

    PS2Runtime::RecompiledFunction g_readOrig = nullptr;
    constexpr int kTurboWindow = 30, kTurboReads = 12;
    std::atomic<uint64_t> g_readVsync[kTurboReads] = {};   // vsync tick + 1 of the last reads
    std::atomic<unsigned> g_readHead{0};

    void FileReadWrap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (static_cast<uint32_t>(_mm_extract_epi32(ctx->r[7], 0)) != 0x20u)
        {
            const unsigned h = g_readHead.load(std::memory_order_relaxed);
            g_readVsync[h % kTurboReads].store(runtime->memory().gs().vsyncTick.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);
            g_readHead.store(h + 1u, std::memory_order_release);
        }
        g_readOrig(rdram, ctx, runtime);
    }

    // ---- fun mod: 2-player weapons in single player ------------------------------------------
    // The weapon pad picker (0x11D348) builds a mask of allowed weapons in $s1, sums their
    // weights from 0x3A4988 into 0x2826C8 (count in 0x285120), then calls rand (0x22BD80) and
    // picks by weight. Below game mode 12 (the 1-player modes) it clears the five multiplayer
    // weapons (mask 0x05E00000); from 12 up it adds them for human ships ($s5 != 0). With the mod
    // on, the rand call from the picker adds them back and redoes the sums before rand runs.
    constexpr uint32_t kPickerRandRet = 0x0011D528u, kWeaponMode = 0x003A4F58u, kWeaponWeights = 0x003A4988u;
    constexpr uint32_t kWeightTotal = 0x002826C8u, kWeightCount = 0x00285120u, kMpWeapons = 0x05E00000u;
    // The eight team super weapons (0x11D2C0): 7 8 9 10 11 12 15 17.
    constexpr uint32_t kSuperWeapons = 0x00029F80u, kWeaponUses = 0x00364F08u, kWeaponLimits = 0x00249A28u;
    constexpr float kDamageScale[] = {0.5f, 1.0f, 2.0f, 3.0f};
    const char *const kDamageNames[] = {"Half", "Normal", "Double", "Triple"};

    // Weapon tuning, run once per race frame on the game thread (after the HUD gauge update).
    // Ships: 16 x 0xE90 from 0x2DBD20. +0xE00 points at the pilot record, whose byte +0xC is the
    // shield (0-100). The physics block starts at +0x40: weapon id at +0x40+0xCA4 (-1 = none),
    // last weapon at +0x40+0x244. Ammo for multi-shot weapons at +0xDF8 (pickup 0x11E9F0 sets 25
    // for the proton cannon, 5 for mines, 99 for the flamer, else 0).
    // - Damage: shield losses between frames (weapons and collisions) are scaled; a ship the game
    //   has already brought to 0 is left at 0.
    // - Uses: when a held weapon is spent (id goes to -1) it comes back, up to N extra times.
    // - Limits: the per-race use limits (0x249A28, 2 for most super weapons) are raised.
    void WeaponTuningTick(uint8_t *rdram)
    {
        struct ShipState { int shield = -1; int32_t weapon = -1; int left = 0; float carry = 0.0f; };
        static ShipState st[16];
        static auto last = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        if (now - last > std::chrono::seconds(1))   // a new race (or back from a pause screen)
            for (ShipState &s : st)
                s = ShipState{};
        last = now;

        // A custom weapon set (debug) lifts them too, or "only quakes" would run dry after one.
        static int32_t savedLimits[26];
        static bool limitsRaised = false;
        const bool raise = g_set.noLimits || (g_set.customWeapons && g_set.weaponMask);
        if (raise != limitsRaised)
        {
            for (uint32_t i = 0; i < 26; ++i)
            {
                if (raise)
                    savedLimits[i] = static_cast<int32_t>(R32(rdram, kWeaponLimits + 4u * i));
                W32(rdram, kWeaponLimits + 4u * i, raise ? 1000u : static_cast<uint32_t>(savedLimits[i]));
            }
            limitsRaised = raise;
        }

        const float k = kDamageScale[std::clamp(g_set.damage, 0, 3)];
        for (uint32_t i = 0; i < 16; ++i)
        {
            const uint32_t ship = 0x2DBD20u + i * 0xE90u;
            ShipState &s = st[i];
            const uint32_t pilot = R32(rdram, ship + 0xE00u) & 0x01FFFFFFu;
            if (pilot && pilot < 0x01FFFFF0u)
            {
                uint8_t &shield = rdram[pilot + 0xCu];
                // The game's own slow drain (0x193744: every so often the shield loses the byte at
                // 0x2833E0 and the timer at pilot+0x54 restarts from 0) is not damage.
                float drainTimer;
                std::memcpy(&drainTimer, rdram + pilot + 0x54u, 4);
                const bool drain = drainTimer == 0.0f && s.shield - shield == rdram[0x2833E0u];
                if (k != 1.0f && s.shield > 0 && shield < s.shield && shield > 0 && !drain)
                {
                    const float lost = static_cast<float>(s.shield - shield) * k + s.carry;
                    const int whole = static_cast<int>(lost);
                    s.carry = lost - static_cast<float>(whole);
                    shield = static_cast<uint8_t>(std::clamp(s.shield - whole, 0, 100));
                    if (static const bool slog = std::getenv("RT_SHIPLOG") != nullptr; slog)
                        std::fprintf(stderr, "[ships] ship %u shield %d -> %d (scaled to %d)\n", i, s.shield, s.shield - (int)((lost - s.carry) / k + 0.5f), shield);
                }
                s.shield = shield;
            }
            const uint32_t wAddr = ship + 0x40u + 0xCA4u;
            int32_t w = static_cast<int32_t>(R32(rdram, wAddr));
            if (static const char *give = std::getenv("RT_GIVEWEAPON"); give && w < 0)
            {
                // Testing: RT_GIVEWEAPON=<id> refills every ship's weapon whenever it is empty.
                w = std::clamp(std::atoi(give), 0, 26);
                W32(rdram, wAddr, static_cast<uint32_t>(w));
                W32(rdram, ship + 0x40u + 0x244u, static_cast<uint32_t>(w));
            }
            if (w >= 0 && w != s.weapon)
                s.left = g_set.uses;   // a new pickup
            else if (w < 0 && s.weapon >= 0 && s.left > 0)
            {
                --s.left;
                if (static const bool slog = std::getenv("RT_SHIPLOG") != nullptr; slog)
                    std::fprintf(stderr, "[ships] ship %u weapon %d back (%d more)\n", i, s.weapon, s.left);
                W32(rdram, wAddr, static_cast<uint32_t>(s.weapon));
                W32(rdram, ship + 0x40u + 0x244u, static_cast<uint32_t>(s.weapon));
                W32(rdram, ship + 0xDF8u, s.weapon == 0x13 ? 25u : s.weapon == 3 ? 5u : s.weapon == 0xD ? 99u : 0u);
                continue;
            }
            s.weapon = w;
        }
    }
    PS2Runtime::RecompiledFunction g_randOrig = nullptr;

    uint32_t Rd32(const uint8_t *rdram, uint32_t a) { uint32_t v; std::memcpy(&v, rdram + (a & 0x01FFFFFFu), 4); return v; }
    void Wr32(uint8_t *rdram, uint32_t a, uint32_t v) { std::memcpy(rdram + (a & 0x01FFFFFFu), &v, 4); }

    // Debug weapons toggle: every pad, for every ship in every mode, picks only from the chosen
    // set. Chosen weapons the game gives no weight in this mode get the average weight.
    bool CustomWeaponPick(uint8_t *rdram, R5900Context *ctx)
    {
        const uint32_t want = g_set.weaponMask & kAllPadWeapons;
        if (!g_set.customWeapons || !want)
            return false;
        int32_t sum = 0, n = 0;
        for (int i = 0; i < 27; ++i)
            if (const int32_t w = static_cast<int32_t>(Rd32(rdram, kWeaponWeights + 4u * i)); w > 0)
                sum += w, ++n;
        const int32_t avg = n ? std::max(1, sum / n) : 3;
        int32_t total = 0, count = 0;
        for (int i = 0; i < 27; ++i)
        {
            if (!(want >> i & 1u))
                continue;
            if (static_cast<int32_t>(Rd32(rdram, kWeaponWeights + 4u * i)) <= 0)
                Wr32(rdram, kWeaponWeights + 4u * i, static_cast<uint32_t>(avg));
            total += static_cast<int32_t>(Rd32(rdram, kWeaponWeights + 4u * i));
            ++count;
        }
        ctx->r[17] = _mm_insert_epi32(ctx->r[17], static_cast<int32_t>(want), 0);
        Wr32(rdram, kWeightTotal, static_cast<uint32_t>(total));
        Wr32(rdram, kWeightCount, static_cast<uint32_t>(count));
        if (static const bool wlog = Env("RT_WEAPONLOG") != nullptr; wlog)
            std::fprintf(stderr, "[weapons] custom pad mask %08x\n", want);
        return true;
    }

    void RandWrap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const bool picker = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0)) == kPickerRandRet;
        if (picker && CustomWeaponPick(rdram, ctx))
            ;
        else if (picker && (g_set.mpWeapons || g_set.superWeapons))
        {
            uint32_t mask = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[17], 0));
            uint32_t add = 0;
            if (g_set.mpWeapons && static_cast<int32_t>(Rd32(rdram, kWeaponMode)) < 12 && _mm_extract_epi32(ctx->r[21], 0) != 0)
                add |= kMpWeapons;
            // Every team's super weapon for every ship, still within the game's per-race limits
            // (uses at 0x364F08, limits at 0x249A28).
            if (g_set.superWeapons)
                for (int i = 0; i < 27; ++i)
                    if ((kSuperWeapons >> i & 1u) && static_cast<int32_t>(Rd32(rdram, kWeaponUses + 4u * i)) < static_cast<int32_t>(Rd32(rdram, kWeaponLimits + 4u * i)))
                        add |= 1u << i;
            if ((mask & add) != add)
            {
                mask |= add;
                // Their weights may be 0 outside their own mode: give those the average weight.
                int32_t sum = 0, n = 0;
                for (int i = 0; i < 27; ++i)
                    if (!(add >> i & 1u) && (mask >> i & 1u) && static_cast<int32_t>(Rd32(rdram, kWeaponWeights + 4u * i)) > 0)
                        sum += static_cast<int32_t>(Rd32(rdram, kWeaponWeights + 4u * i)), ++n;
                const int32_t avg = n ? std::max(1, sum / n) : 3;
                int32_t total = 0, count = 0;
                for (int i = 0; i < 27; ++i)
                {
                    if (!(mask >> i & 1u))
                        continue;
                    if ((add >> i & 1u) && static_cast<int32_t>(Rd32(rdram, kWeaponWeights + 4u * i)) <= 0)
                        Wr32(rdram, kWeaponWeights + 4u * i, static_cast<uint32_t>(avg));
                    total += static_cast<int32_t>(Rd32(rdram, kWeaponWeights + 4u * i));
                    ++count;
                }
                ctx->r[17] = _mm_insert_epi32(ctx->r[17], static_cast<int32_t>(mask), 0);
                Wr32(rdram, kWeightTotal, static_cast<uint32_t>(total));
                Wr32(rdram, kWeightCount, static_cast<uint32_t>(count));
            }
            if (static const bool wlog = Env("RT_WEAPONLOG") != nullptr; wlog)
                std::fprintf(stderr, "[weapons] pad mask %08x (human %d)\n", mask, _mm_extract_epi32(ctx->r[21], 0) != 0);
        }
        // Testing: RT_FORCEWEAPON=<id> gives the human ship that weapon from every pad (ids follow
        // the name table at 0x2482F8: 2 rockets, 13 flamer, ...).
        static const char *force = Env("RT_FORCEWEAPON");
        if (force && static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0)) == kPickerRandRet && _mm_extract_epi32(ctx->r[21], 0) != 0)
        {
            const int id = std::clamp(std::atoi(force), 0, 26);
            const uint32_t w = std::max<uint32_t>(1u, Rd32(rdram, kWeaponWeights + 4u * id));
            Wr32(rdram, kWeaponWeights + 4u * id, w);
            ctx->r[17] = _mm_insert_epi32(ctx->r[17], static_cast<int32_t>(1u << id), 0);
            Wr32(rdram, kWeightTotal, w);
            Wr32(rdram, kWeightCount, 1u);
            std::fprintf(stderr, "[debug] weapon pad: forced weapon %d\n", id);
        }
        g_randOrig(rdram, ctx, runtime);
    }

    // Vblank hook (GS thread, once per vblank).
    ps2x_capture::VblankHook g_prevVblankHook = nullptr;
    void VblankHook()
    {
        if (g_prevVblankHook)
            g_prevVblankHook();
        if (!g_rt)
            return;
        // RT_FPSLOG=1: every 2 s, how fast the SPU2 produced sound against the wall clock
        // (48000/s is real time), the output backlog, and frames dropped or short reads.
        if (static const bool zs = Env("RT_ZEROSCAN") != nullptr; zs)
        {
            // RT_ZEROSCAN=1: every 10 s, EE RAM runs of 64 KB+ that stayed all zero since start.
            static std::vector<uint8_t> everUsed(0x2000000 / 4096, 0);
            static int n = 0;
            const uint8_t *ram = g_rt->memory().getRDRAM();
            for (size_t pg = 0; pg < everUsed.size(); ++pg)
                if (!everUsed[pg])
                    for (size_t k = 0; k < 4096; k += 8)
                    {
                        uint64_t w;
                        std::memcpy(&w, ram + pg * 4096 + k, 8);
                        if (w) { everUsed[pg] = 1; break; }
                    }
            if (++n % 600 == 0)
            {
                std::string line;
                for (size_t pg = 0; pg < everUsed.size();)
                {
                    size_t e = pg;
                    while (e < everUsed.size() && !everUsed[e]) ++e;
                    if (e - pg >= 16)
                        line += " " + std::to_string(pg) + "+" + std::to_string(e - pg);
                    pg = e + 1;
                }
                std::fprintf(stderr, "[zeroscan] free pages (4K):%s\n", line.c_str());
            }
        }
        if (static const char *rd = Env("RT_RAMDUMP"); rd)
        {
            // RT_RAMDUMP=s1,s2,...: EE RAM to ramdump_<s>.bin (cwd) at those seconds after start.
            static int n = 0;
            ++n;
            for (const char *q = rd; *q;)
            {
                const int at = std::atoi(q);
                if (n == at * 60)
                {
                    char name[64];
                    std::snprintf(name, sizeof name, "ramdump_%d.bin", at);
                    if (FILE *f = std::fopen(name, "wb"))
                    {
                        std::fwrite(g_rt->memory().getRDRAM(), 1, 0x2000000, f);
                        std::fclose(f);
                        std::fprintf(stderr, "[ramdump] %s\n", name);
                    }
                }
                while (*q && *q != ',') ++q;
                if (*q) ++q;
            }
        }
        if (static const bool wlog = Env("RT_WEAPONLOG") != nullptr; wlog)   // ships' weapons, once a second
        {
            static int n = 0;
            if (++n % 60 == 0)
            {
                uint8_t *ram = g_rt->memory().getRDRAM();
                std::string line;
                for (uint32_t i = 0; i < 16; ++i)
                {
                    const uint32_t wp = Rd32(ram, 0x2DBD20u + i * 0xE90u + 0xD80u);
                    line += ' ' + std::to_string(wp ? static_cast<int32_t>(Rd32(ram, wp + 0xCA4u)) : -9);
                }
                std::fprintf(stderr, "[weapons] held:%s\n", line.c_str());
            }
        }
        if (static const bool alog = Env("RT_FPSLOG") != nullptr; alog)
        {
            static auto t0 = std::chrono::steady_clock::now();
            static ps2x::iop::AudioStats a0 = ps2x::iop::audioStats();
            const auto t = std::chrono::steady_clock::now();
            const double dt = std::chrono::duration<double>(t - t0).count();
            if (dt >= 2.0)
            {
                const ps2x::iop::AudioStats a = ps2x::iop::audioStats();
                std::fprintf(stderr, "[audio] produced %.0f/s, queued %.0f ms, dropped %llu, underruns %llu\n",
                             (a.pushed - a0.pushed) / dt, a.queued * 1000.0 / ps2x::iop::kAudioRate,
                             (unsigned long long)(a.dropped - a0.dropped), (unsigned long long)(a.underruns - a0.underruns));
                t0 = t;
                a0 = a;
            }
        }
        const uint64_t now = g_rt->memory().gs().vsyncTick.load(std::memory_order_relaxed) + 1u;
        const unsigned h = g_readHead.load(std::memory_order_acquire);
        const uint64_t newest = g_readVsync[(h + kTurboReads - 1u) % kTurboReads].load(std::memory_order_relaxed);
        const uint64_t oldest = g_readVsync[h % kTurboReads].load(std::memory_order_relaxed);
        static bool loading = false;
        static uint64_t busySince = 0;   // first vblank of the current run of busy reading
        const bool busy = oldest != 0 && now < oldest + kTurboWindow;
        if (!busy)
            busySince = 0;
        else if (!busySince)
            busySince = now;
        if (!g_set.fastLoad)
            loading = false;
        else if (!loading)
            loading = busy && now >= busySince + 60u;
        else
            loading = now < newest + kTurboWindow;
        if (ps2x_capture::g_turbo.load(std::memory_order_relaxed) != loading)
        {
            ps2x_capture::g_turbo.store(loading, std::memory_order_relaxed);
            // Mute while it runs flat out (the ring takes nothing), then start again from empty.
            static bool muted = false;
            if (loading && ps2x::iop::audioOutputEnabled())
            {
                ps2x::iop::enableAudioOutput(false);
                muted = true;
            }
            else if (!loading && muted)
            {
                ps2x::iop::enableAudioOutput(true);
                muted = false;
            }
            static const bool log = Env("RT_TURBOLOG") != nullptr;
            if (log)
                std::fprintf(stderr, "[gfx] fast loading %s at vblank %llu\n", loading ? "on" : "off", (unsigned long long)now);
        }
    }

    // ---- menu -----------------------------------------------------------------------------------
    ps2x_capture::OverlayHook g_prevOverlay = nullptr;
    bool g_open = false;
    int g_sel = 0;
    int g_page = 0;   // 0 settings, 1 weapons toggle
    int g_wsel = 0;   // weapons page: 0 custom on/off, 1 all on, 2 all off, 3.. weapons, then Back
    double g_openTime = 0.0, g_blockUntil = 0.0;
    Font g_font{};
    int g_fontPx = 0;
    double g_fpsT0 = 0.0;
    uint64_t g_frames = 0, g_fpsF0 = 0, g_lastDispfb = ~0ull;
    float g_fpsShown = 0.0f;

    enum Row { RowRes, RowDisplay, RowAspect, RowRaceHud, RowFov, RowVsync, RowShowFps, RowFastLoad, RowGpuModels, RowMpWeapons, RowSuperWeapons, RowDamage, RowUses, RowLimits, RowWeapons, RowUnlock, RowDefaults, RowDone, RowCount };

    // WipEout-style colours: deep navy panel, cyan lines, yellow highlight.
    const Color kPanel{10, 16, 40, 235}, kLine{70, 170, 230, 255}, kText{225, 235, 245, 255}, kDim{120, 140, 170, 255};
    const Color kYellow{250, 220, 30, 255}, kInk{6, 10, 24, 255}, kRed{235, 70, 60, 255};

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
        for (int c : {0x25C0, 0x25B6, 0x2191, 0x2193, 0x2022, 0x00D7, 0x2713, 0x00B0})
            cps.push_back(c);
        g_font = LoadFontFromMemory(".ttf", kMenuFontTtf, static_cast<int>(sizeof(kMenuFontTtf)), want, cps.data(), static_cast<int>(cps.size()));
        SetTextureFilter(g_font.texture, TEXTURE_FILTER_BILINEAR);
        g_fontPx = want;
    }

    Vector2 Measure(const char *t, float size) { return MeasureTextEx(g_font, t, size, size * 0.02f); }

    void Text(const char *t, float x, float y, float size, Color fill, Color outline, float ow)
    {
        const float sp = size * 0.02f;
        if (ow > 0.0f)
            for (int k = 0; k < 8; ++k)
            {
                const float a = k * 3.14159265f / 4.0f;
                DrawTextEx(g_font, t, {x + std::cos(a) * ow, y + std::sin(a) * ow}, size, sp, outline);
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
            case AspectStretch: return "Stretch";
            default: return "4:3";
            }
        case RowRaceHud: return g_set.edgeHud ? "Screen edges" : "Stretched";
        case RowFov:
            if (!g_set.fov)
                return "Normal";
            return (g_set.fov > 0 ? "+" : "") + std::to_string(g_set.fov) + "\xC2\xB0  (" + std::to_string(kGameFov + g_set.fov) + "\xC2\xB0 wide)";
        case RowVsync: return g_set.vsync ? "On" : "Off";
        case RowShowFps: return g_set.showFps ? "On" : "Off";
        case RowFastLoad: return g_set.fastLoad ? "On" : "Off";
        case RowGpuModels: return !g_gsModelCapable.load() ? "CPU (no GPU support)" : g_set.gpuModels ? "GPU" : "CPU (PS2-exact)";
        case RowMpWeapons: return g_set.mpWeapons ? "On" : "Off";
        case RowSuperWeapons: return g_set.superWeapons ? "On" : "Off";
        case RowDamage: return kDamageNames[std::clamp(g_set.damage, 0, 3)];
        case RowUses: return g_set.uses ? std::to_string(g_set.uses + 1) + " per pickup" : "Normal";
        case RowLimits: return g_set.noLimits ? "Off" : "Normal";
        case RowWeapons:
        {
            if (!g_set.customWeapons || !(g_set.weaponMask & kAllPadWeapons))
                return "All (normal)";
            int n = 0;
            const char *one = "";
            for (const WeaponName &w : kPadWeapons)
                if (g_set.weaponMask >> w.id & 1u)
                    ++n, one = w.name;
            return n == 1 ? std::string("Only ") + one : std::to_string(n) + " chosen";
        }
        case RowUnlock: return g_unlockDone.load() ? "Done" : "Press \xC3\x97";
        default: return "";
        }
    }

    const char *Label(int row)
    {
        static const char *k[] = {"Resolution", "Display", "Picture shape", "Race HUD (16:9)", "Field of view", "VSync", "Show FPS", "Fast loading", "Model transform", "FUN: 2-player weapons", "FUN: all super weapons", "FUN: weapon damage", "FUN: weapon uses", "FUN: per-race limits", "Weapons toggle", "Unlock everything",
                                  "Restore defaults", "Done"};
        return k[row];
    }

    std::string Help(int row)
    {
        switch (row)
        {
        case RowRes: return "Internal rendering resolution (upscaling). Auto follows the window size.";
        case RowDisplay: return "Borderless full screen or a window (F11 also switches).";
        case RowAspect: return "4:3 is the original picture. 16:9 also switches the game's widescreen camera on (from the next race), 2-player included.";
        case RowRaceHud: return "With a 16:9 picture: the race HUD at the screen edges in its original shape, or stretched like the PS2.";
        case RowFov: return "Race camera field of view, added to the game's (about 94\xC2\xB0 across). Wider shows more track and feels faster. Changes straight away.";
        case RowVsync: return "Sync to the display. Off can tear but shows frames as soon as they are ready.";
        case RowShowFps: return "Game frames per second in the corner.";
        case RowFastLoad: return "Runs the game flat out while it loads instead of at PS2 speed.";
        case RowGpuModels: return "Where 3D models are transformed and lit. GPU is faster; CPU matches the PS2 to the pixel.";
        case RowMpWeapons: return "Hunter Missile, Turbo Enforcer, Stealth, Global Drain and Control Jammer in 1-player races too.";
        case RowSuperWeapons: return "Every team's super weapon can come from any weapon pad, for every ship (game limits per race still apply).";
        case RowDamage: return "How much shield every ship loses from hits and crashes. Applies to all ships.";
        case RowUses: return "Each weapon picked up can be fired (or absorbed) this many times. Applies to all ships.";
        case RowLimits: return "Off lifts the game's cap on how often each weapon (super weapons: 2) can appear per race.";
        case RowWeapons: return "Debug: choose exactly which weapons come off the pads, for every ship in every mode.";
        case RowUnlock: return "Debug: all teams, ships, tracks and modes, plus credits. Save in game to keep it.";
        case RowDefaults: return "Put every graphics setting back to the defaults.";
        default: return "";
        }
    }

    // Takes effect from the next model drawn; the GPU keeps its meshes either way.
    void ApplyGpuModels() { g_gsModelSupport.store(g_set.gpuModels && g_gsModelCapable.load()); }

    void ApplyAspect()
    {
        const float a = g_set.aspect == Aspect169 ? 16.0f / 9.0f : g_set.aspect == AspectStretch ? -1.0f : 4.0f / 3.0f;
        ps2x_capture::g_displayAspect.store(a);
        gsgpu::SetEdgeHud(g_set.aspect == Aspect169 && g_set.edgeHud);
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
        case RowAspect: g_set.aspect = cycle(g_set.aspect, AspectCount); ApplyAspect(); break;
        case RowRaceHud: g_set.edgeHud = !g_set.edgeHud; ApplyAspect(); break;
        case RowFov: g_set.fov = std::clamp(g_set.fov + dir * kFovStep, kFovMin, kFovMax); break;
        case RowVsync: g_set.vsync = !g_set.vsync; ps2x_capture::g_vsyncRequest.store(g_set.vsync ? 1 : 0); break;
        case RowShowFps: g_set.showFps = !g_set.showFps; break;
        case RowFastLoad: g_set.fastLoad = !g_set.fastLoad; break;
        case RowGpuModels: g_set.gpuModels = !g_set.gpuModels; ApplyGpuModels(); break;
        case RowMpWeapons: g_set.mpWeapons = !g_set.mpWeapons; break;
        case RowSuperWeapons: g_set.superWeapons = !g_set.superWeapons; break;
        case RowDamage: g_set.damage = cycle(g_set.damage, 4); break;
        case RowUses: g_set.uses = cycle(g_set.uses, 3); break;
        case RowLimits: g_set.noLimits = !g_set.noLimits; break;
        case RowWeapons:
            g_page = 1;
            g_wsel = 0;
            return;
        case RowUnlock:
            g_unlockRequest.store(true);
            return;
        case RowDefaults:
        {
            const bool fs = g_set.fullscreen;
            g_set = Settings{};
            g_set.fullscreen = fs;
            rufftrigger::GpuSetScale(kScales[g_set.scale]);
            ps2x_capture::g_vsyncRequest.store(1);
            ApplyAspect();
            ApplyGpuModels();
            break;
        }
        default: return;
        }
        SaveSettings();
    }

    bool PadPressed(int b) { return IsGamepadAvailable(0) && IsGamepadButtonPressed(0, b); }
    bool PadDown(int b) { return IsGamepadAvailable(0) && IsGamepadButtonDown(0, b); }

    // Weapons page: rows 0-2 (custom set on/off, all on, all off), then the weapons in two
    // columns of kWeaponRows, then Back.
    constexpr int kWeaponRows = (kPadWeaponCount + 1) / 2, kWeaponFirst = 3, kWeaponBack = kWeaponFirst + kPadWeaponCount;
    void HandleWeaponsInput()
    {
        const bool down = IsKeyPressed(KEY_DOWN) || PadPressed(GAMEPAD_BUTTON_LEFT_FACE_DOWN);
        const bool up = IsKeyPressed(KEY_UP) || PadPressed(GAMEPAD_BUTTON_LEFT_FACE_UP);
        const bool side = IsKeyPressed(KEY_RIGHT) || PadPressed(GAMEPAD_BUTTON_LEFT_FACE_RIGHT) ||
                          IsKeyPressed(KEY_LEFT) || PadPressed(GAMEPAD_BUTTON_LEFT_FACE_LEFT);
        const bool ok = IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE) || IsKeyPressed(KEY_X) || PadPressed(GAMEPAD_BUTTON_RIGHT_FACE_DOWN);
        int &w = g_wsel;
        const bool inGrid = w >= kWeaponFirst && w < kWeaponBack;
        const int k = w - kWeaponFirst, col = inGrid ? k / kWeaponRows : 0, row = inGrid ? k % kWeaponRows : 0;
        auto clampCell = [](int c, int r) { return kWeaponFirst + std::min(c * kWeaponRows + r, kPadWeaponCount - 1); };
        if (down)
        {
            if (w < kWeaponFirst - 1)
                ++w;
            else if (w == kWeaponFirst - 1)
                w = kWeaponFirst;
            else if (inGrid)
                w = (row + 1 < kWeaponRows && col * kWeaponRows + row + 1 < kPadWeaponCount) ? w + 1 : kWeaponBack;
            else
                w = 0;
        }
        else if (up)
        {
            if (w == 0)
                w = kWeaponBack;
            else if (w < kWeaponFirst)
                --w;
            else if (inGrid)
                w = row > 0 ? w - 1 : kWeaponFirst - 1;
            else
                w = clampCell(0, kWeaponRows - 1);
        }
        else if (side && inGrid)
            w = clampCell(1 - col, row);
        bool changed = false;
        if ((ok || (side && w == 0)) && !(side && inGrid))
        {
            if (w == 0)
                g_set.customWeapons = !g_set.customWeapons, changed = true;
            else if (w == 1)
                g_set.weaponMask = kAllPadWeapons, g_set.customWeapons = true, changed = true;
            else if (w == 2)
                g_set.weaponMask = 0, g_set.customWeapons = true, changed = true;
            else if (w == kWeaponBack)
                g_page = 0;
            else if (w >= kWeaponFirst)
            {
                g_set.weaponMask ^= 1u << kPadWeapons[w - kWeaponFirst].id;
                g_set.customWeapons = true;
                changed = true;
            }
        }
        if (changed)
            SaveSettings();
    }

    void DrawWeaponsPage(float px, float py, float pw, float ph, float u, double t)
    {
        Text("WEAPONS TOGGLE", px + 36 * u, py + 20 * u, 44.0f * u, kText, kInk, 0.0f);
        const float fs = 21.0f * u, rowH = 27.0f * u;
        float y = py + 80.0f * u;
        auto highlight = [&](float x, float yy, float w, bool sel) {
            if (!sel)
                return;
            const Rectangle hl{x, yy - 4 * u, w, rowH - 2 * u};
            DrawRectangleRec(hl, Fade(kYellow, 0.18f));
            DrawRectangleLinesEx(hl, 2.0f * u, kYellow);
        };
        const bool none = !(g_set.weaponMask & kAllPadWeapons);
        const char *top[3] = {"Custom set", "All on", "All off"};
        for (int r = 0; r < kWeaponFirst; ++r, y += rowH)
        {
            const bool sel = g_wsel == r;
            highlight(px + 22 * u, y, pw - 44 * u, sel);
            Text(top[r], px + 60 * u, y + (rowH - fs) * 0.5f - 6 * u, fs, sel ? kYellow : kText, kInk, 0.0f);
            if (r == 0)
            {
                const char *v = !g_set.customWeapons ? "Off (normal weapons)" : none ? "On (none chosen: normal)" : "On";
                Text(v, px + pw * 0.45f, y + (rowH - fs) * 0.5f - 6 * u, fs, sel ? kYellow : kLine, kInk, 0.0f);
            }
        }
        y += 10 * u;
        const float colW = (pw - 60 * u) * 0.5f;
        for (int i = 0; i < kPadWeaponCount; ++i)
        {
            const int c = i / kWeaponRows, r = i % kWeaponRows;
            const float x = px + 30 * u + c * colW, yy = y + r * rowH;
            const bool sel = g_wsel == kWeaponFirst + i, on = g_set.weaponMask >> kPadWeapons[i].id & 1u;
            highlight(x, yy, colW - 8 * u, sel);
            const Color box = !g_set.customWeapons ? kDim : on ? kYellow : kDim;
            const float bs = fs * 0.8f, by = yy + (rowH - bs) * 0.5f - 4 * u;
            DrawRectangleLinesEx({x + 12 * u, by, bs, bs}, 2.0f * u, box);
            if (on)
                DrawRectangleRec({x + 12 * u + bs * 0.22f, by + bs * 0.22f, bs * 0.56f, bs * 0.56f}, box);
            Text(kPadWeapons[i].name, x + 22 * u + bs, yy + (rowH - fs) * 0.5f - 6 * u, fs, sel ? kYellow : (on ? kText : kDim), kInk, 0.0f);
        }
        y += kWeaponRows * rowH + 10 * u;
        {
            const bool sel = g_wsel == kWeaponBack;
            highlight(px + 22 * u, y, pw - 44 * u, sel);
            const Vector2 m = Measure("Back", fs);
            Text("Back", px + (pw - m.x) * 0.5f, y + (rowH - fs) * 0.5f - 6 * u, fs, sel ? kYellow : kText, kInk, 0.0f);
        }
        const float hs = 20.0f * u;
        const char *help = "Pads give only the ticked weapons, to every ship in every mode. Per-race limits are lifted while it is on.";
        const Vector2 hm = Measure(help, hs);
        const float hy = py + ph - 76 * u;
        DrawLineEx({px + 22 * u, hy - 10 * u}, {px + pw - 22 * u, hy - 10 * u}, 1.5f * u, kDim);
        Text(help, px + std::max(30 * u, (pw - hm.x) * 0.5f), hy, std::min(hs, hs * (pw - 60 * u) / std::max(1.0f, hm.x)), kText, kInk, 0.0f);
        const char *keys = "\xE2\x86\x91\xE2\x86\x93\xE2\x97\x80\xE2\x96\xB6 move    Enter / \xC3\x97 tick    Esc back    F10 close";
        const Vector2 km = Measure(keys, hs);
        Text(keys, px + (pw - km.x) * 0.5f, py + ph - 36 * u, hs, kDim, kInk, 0.0f);
        (void)t;
    }

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
        const bool back = IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_BACKSPACE) || PadPressed(GAMEPAD_BUTTON_RIGHT_FACE_RIGHT);
        if (g_page == 1 && !toggle)
        {
            if (back)
                g_page = 0;
            else
                HandleWeaponsInput();
            return;
        }
        if (toggle || back)
        {
            g_page = 0;
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
        if (g_sel == RowDefaults || g_sel == RowUnlock || g_sel == RowWeapons)
        {
            if (ok)
                Change(g_sel, 0);
            return;
        }
        if (dir)
            Change(g_sel, dir);
        else if (ok)
            Change(g_sel, 1);
    }

    // A game frame is a new display buffer (the game flips DISPFB once per finished frame).
    void CountFrame()
    {
        if (!g_rt)
            return;
        const GSRegisters &gs = g_rt->memory().gs();
        const uint64_t d = gs.dispfb1 ^ (gs.dispfb2 << 1);
        if (d != g_lastDispfb)
        {
            g_lastDispfb = d;
            ++g_frames;
        }
    }

    void DrawFps(int screenW, int screenH)
    {
        const double t = GetTime();
        if (g_fpsT0 == 0.0 || t - g_fpsT0 >= 0.5)
        {
            if (g_fpsT0 != 0.0)
                g_fpsShown = static_cast<float>((g_frames - g_fpsF0) / (t - g_fpsT0));
            g_fpsT0 = t;
            g_fpsF0 = g_frames;
        }
        const float size = screenH / 26.0f;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.0f FPS", g_fpsShown);
        const Vector2 m = Measure(buf, size);
        Text(buf, screenW - m.x - size * 0.6f, size * 0.4f, size, g_fpsShown < 54.0f ? kYellow : kText, kInk, std::max(1.5f, size * 0.08f));
    }

    void DrawMenu(int screenW, int screenH)
    {
        const float u = screenH / 720.0f;
        const double t = GetTime();
        const float open = static_cast<float>(std::min(1.0, (t - g_openTime) / 0.15));
        const float ease = 1.0f - (1.0f - open) * (1.0f - open);
        DrawRectangle(0, 0, screenW, screenH, Fade(BLACK, 0.6f * ease));

        const float pw = std::min(screenW - 40.0f * u, 780.0f * u), rowH = std::min(42.0f, 512.0f / static_cast<float>(RowCount)) * u;
        const float ph = 80.0f * u + rowH * static_cast<float>(RowCount) + 28.0f * u + 100.0f * u;
        const float px = (screenW - pw) * 0.5f, py = (screenH - ph) * 0.5f + (1.0f - ease) * 40.0f * u;
        const Rectangle panel{px, py, pw, ph};
        DrawRectangleRec(panel, kPanel);
        DrawRectangleLinesEx(panel, 3.0f * u, kLine);
        DrawRectangle(static_cast<int>(px), static_cast<int>(py), static_cast<int>(pw), static_cast<int>(6 * u), kLine);

        if (g_page == 1)
        {
            DrawWeaponsPage(px, py, pw, ph, u, t);
            return;
        }
        const char *title = "PC SETTINGS";
        const float ts = 44.0f * u;
        Text(title, px + 36 * u, py + 20 * u, ts, kText, kInk, 0.0f);

        const float fs = 28.0f * u;
        float y = py + 80.0f * u;
        const float lx = px + 60 * u, vx = px + pw * 0.52f, vw = pw * 0.44f - 30 * u;
        for (int r = 0; r < RowCount; ++r, y += rowH)
        {
            const bool sel = r == g_sel;
            if (r == RowWeapons || r == RowDefaults)
                y += 14 * u;
            if (r == RowWeapons)
                Text("DEBUG", px + 36 * u, y - 18 * u, 18 * u, kDim, kInk, 0.0f);
            if (sel)
            {
                const Rectangle hl{px + 22 * u, y - 4 * u, pw - 44 * u, rowH - 2 * u};
                DrawRectangleRec(hl, Fade(kYellow, 0.18f));
                DrawRectangleLinesEx(hl, 2.0f * u, kYellow);
                const float wob = std::sin(static_cast<float>(t) * 6.0f) * 3.0f * u;
                Text("\xE2\x96\xB6", hl.x + 10 * u + wob, y + (rowH - fs) * 0.5f - 6 * u, fs * 0.8f, kYellow, kInk, 0.0f);
            }
            if (r >= RowDefaults)
            {
                const Vector2 m = Measure(Label(r), fs);
                Text(Label(r), px + (pw - m.x) * 0.5f, y + (rowH - fs) * 0.5f - 6 * u, fs, sel ? kYellow : kText, kInk, 0.0f);
                continue;
            }
            Text(Label(r), lx, y + (rowH - fs) * 0.5f - 6 * u, fs, sel ? kYellow : kText, kInk, 0.0f);
            const std::string v = ValueText(r);
            const Vector2 m = Measure(v.c_str(), fs);
            const float cx = vx + vw * 0.5f;
            const Color vc = r == RowUnlock && g_unlockDone.load() ? kLine : (sel ? kYellow : kLine);
            Text(v.c_str(), cx - m.x * 0.5f, y + (rowH - fs) * 0.5f - 6 * u, fs, vc, kInk, 0.0f);
            if (r == RowUnlock || r == RowWeapons)
                continue;
            const float as = fs * 0.7f;
            Text("\xE2\x97\x80", vx, y + (rowH - as) * 0.5f - 5 * u, as, sel ? kYellow : kDim, kInk, 0.0f);
            Text("\xE2\x96\xB6", vx + vw - Measure("\xE2\x96\xB6", as).x, y + (rowH - as) * 0.5f - 5 * u, as, sel ? kYellow : kDim, kInk, 0.0f);
        }

        const float hs = 20.0f * u;
        const std::string help = Help(g_sel);
        const Vector2 hm = Measure(help.c_str(), hs);
        const float hy = py + ph - 76 * u;
        DrawLineEx({px + 22 * u, hy - 10 * u}, {px + pw - 22 * u, hy - 10 * u}, 1.5f * u, kDim);
        Text(help.c_str(), px + std::max(30 * u, (pw - hm.x) * 0.5f), hy, std::min(hs, hs * (pw - 60 * u) / std::max(1.0f, hm.x)), kText, kInk, 0.0f);
        const char *keys = "\xE2\x86\x91\xE2\x86\x93 select    \xE2\x97\x80\xE2\x96\xB6 change    Enter / \xC3\x97 ok    Esc / F10 close";
        const Vector2 km = Measure(keys, hs);
        Text(keys, px + (pw - km.x) * 0.5f, py + ph - 36 * u, hs, kDim, kInk, 0.0f);
    }

    void ApplyPageChanges();   // the in-game Graphics page, below

    void Overlay(int screenW, int screenH)
    {
        if (g_prevOverlay)
            g_prevOverlay(screenW, screenH);
        CountFrame();
        EnsureFont(screenH);
        ApplyPageChanges();
        HandleInput();
        if (g_open)
            DrawMenu(screenW, screenH);
        if (g_set.showFps)
            DrawFps(screenW, screenH);
        if (g_open || GetTime() < g_blockUntil)
            ps2x_capture::g_padBlock.store(true);
        else if (!g_prevOverlay)
            ps2x_capture::g_padBlock.store(false);
    }
}

namespace
{
    // In-game Graphics page. The options menu's "screen alignment" item opens a second object of
    // the Preferences page's class (0xDC0 bytes; constructor 0x13BDD0, set-up 0x147B00, like the
    // one at 0x28E168), built in EE RAM the game never uses (0x13000-0x76000 stays zero) and
    // registered as that item's screen (0x64). Its nine choice rows are relabelled with the
    // graphics settings and its two sliders hidden; the rows' text comes from text ids past the
    // game's own (1151), answered by the text lookup hook from strings kept in guest RAM.
    constexpr uint32_t kGfxPage = 0x00040000u;
    constexpr uint32_t kGfxText = 0x00041000u;   // 0x40 bytes per string
    constexpr uint32_t kTextBase = 0x7000u;
    constexpr uint32_t kTextAlignItem = 93;   // "screen alignment" in the options menu
    // Preferences rows by cursor position (0xD98): two sliders, then choice widgets.
    constexpr uint32_t kSliders[] = {0x5C, 0x164};
    struct GfxRow { uint32_t widget; int row; uint32_t rest; const char *label; };
    // A choice row rests on index `rest` with every choice showing the row's value text, so a
    // left/right press shows up as the index moving off it. The game forces row 0x94C to index 0
    // in some states, so the on/off row there rests on 0 and any move toggles.
    constexpr GfxRow kGfxRows[] = {
        {0x26C, RowRes, 1, "resolution"}, {0x348, RowDisplay, 1, "display"}, {0x500, RowAspect, 1, "picture shape"},
        {0x5DC, RowRaceHud, 1, "race hud (16:9)"}, {0x6B8, RowFov, 1, "field of view"}, {0x794, RowVsync, 1, "vsync"},
        {0x870, RowShowFps, 1, "show fps"}, {0x94C, RowFastLoad, 0, "fast loading"}, {0xCBC, RowGpuModels, 1, "model transform"}};
    constexpr int kGfxRowCount = static_cast<int>(std::size(kGfxRows));
    constexpr uint32_t kFirstRow = 2, kLastRow = 10;   // cursor range on the page
    constexpr uint32_t kAutosavePref = 0x0039662Cu;   // -1: the Preferences page skips row 6

    uint32_t R32g(const uint8_t *rdram, uint32_t a) { return *reinterpret_cast<const uint32_t *>(rdram + (a & 0x1FFFFFFu)); }
    void W32g(uint8_t *rdram, uint32_t a, uint32_t v) { *reinterpret_cast<uint32_t *>(rdram + (a & 0x1FFFFFFu)) = v; }
    void W16g(uint8_t *rdram, uint32_t a, uint16_t v) { *reinterpret_cast<uint16_t *>(rdram + (a & 0x1FFFFFFu)) = v; }
    uint32_t Gpr(const R5900Context *ctx, int r) { return static_cast<uint32_t>(_mm_extract_epi32(ctx->r[r], 0)); }
    void SetGpr(R5900Context *ctx, int r, uint32_t v) { ctx->r[r] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(v))); }

    void PutText(uint8_t *rdram, uint32_t id, const std::string &t)
    {
        char *d = reinterpret_cast<char *>(rdram + kGfxText + (id - kTextBase) * 0x40u);
        std::snprintf(d, 0x40, "%s", t.c_str());
    }
    uint32_t LabelId(int i) { return kTextBase + static_cast<uint32_t>(i); }
    uint32_t ValueId(int i) { return kTextBase + 16u + static_cast<uint32_t>(i); }
    constexpr uint32_t kGraphicsId = kTextBase + 32u;

    // The game's font has lower case, digits and a little punctuation (no '+' or degree sign).
    std::string GameValueText(int row)
    {
        switch (row)
        {
        case RowRes:
        {
            const int s = kScales[g_set.scale];
            return s ? std::to_string(s) + "x (" + std::to_string(s * 448) + "p)" : "auto (" + std::to_string(rufftrigger::GpuCurrentScale() * 448) + "p)";
        }
        case RowDisplay: return g_set.fullscreen ? "full screen" : "window";
        case RowAspect: return g_set.aspect == Aspect169 ? "16:9" : g_set.aspect == AspectStretch ? "stretch" : "4:3";
        case RowRaceHud: return g_set.edgeHud ? "screen edges" : "stretched";
        case RowFov: return !g_set.fov ? "normal" : std::to_string(std::abs(g_set.fov)) + (g_set.fov > 0 ? " wider" : " narrower");
        case RowVsync: return g_set.vsync ? "on" : "off";
        case RowShowFps: return g_set.showFps ? "on" : "off";
        case RowFastLoad: return g_set.fastLoad ? "on" : "off";
        case RowGpuModels: return !g_gsModelCapable.load() ? "cpu" : g_set.gpuModels ? "gpu" : "cpu (ps2-exact)";
        default: return "";
        }
    }

    // Changes made on the page are applied on the render thread (window and GL state live there).
    std::mutex g_pageMutex;
    std::vector<std::pair<int, int>> g_pageChanges;
    void ApplyPageChanges()
    {
        std::vector<std::pair<int, int>> c;
        {
            std::lock_guard<std::mutex> lock(g_pageMutex);
            c.swap(g_pageChanges);
        }
        for (const auto &[row, dir] : c)
            Change(row, dir);
    }

    void TextLookup(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        // 0x185740: text id -> string (TEXT0), 0 for -1 and ids past 0x10000.
        uint32_t id = Gpr(ctx, 4), v = 0;
        if (id == kTextAlignItem && !std::getenv("RT_NOGFXPAGE"))
            id = kGraphicsId;
        if (id >= kTextBase && id < kTextBase + 64u)
            v = kGfxText + (id - kTextBase) * 0x40u;
        else if (id != 0xFFFFFFFFu && id <= 0x10000u)
        {
            const uint32_t base = R32g(rdram, 0x0029D050u);
            v = base + R32g(rdram, base + id * 4u);
        }
        SetGpr(ctx, 2, v);
        ctx->pc = Gpr(ctx, 31);
    }

    // Guest calls from a hook run in continuation style: a native call into guest code is cut
    // short when an EE checkpoint unwinds the stack (the rest then runs from the scheduler at
    // ctx->pc), so a hook never waits for one. It sets pc and ra and returns; ra is kStubSeq, a
    // word of alignment padding the game never executes, whose handler starts the next queued
    // call or finally runs `done`. Only the EE main thread runs the front end, so one queue does.
    constexpr uint32_t kStubSeq = 0x001398CCu;
    using GuestDone = std::function<void(uint8_t *, R5900Context *, PS2Runtime *)>;
    struct GuestStep { uint32_t addr; std::vector<uint32_t> args; std::function<void(uint8_t *)> before; };
    std::deque<GuestStep> g_steps;
    GuestDone g_stepsDone;
    void SeqNext(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (!g_steps.empty())
        {
            GuestStep s = std::move(g_steps.front());
            g_steps.pop_front();
            if (s.before)
                s.before(rdram);
            for (size_t k = 0; k < s.args.size(); ++k)
                SetGpr(ctx, static_cast<int>(k < 4 ? 4 + k : 8 + (k - 4)), s.args[k]);
            SetGpr(ctx, 31, kStubSeq);
            ctx->pc = s.addr;
            return;
        }
        GuestDone d = std::move(g_stepsDone);
        g_stepsDone = nullptr;
        if (d)
            d(rdram, ctx, runtime);
    }
    void StartSeq(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, std::deque<GuestStep> steps, GuestDone done)
    {
        g_steps = std::move(steps);
        g_stepsDone = std::move(done);
        SeqNext(rdram, ctx, runtime);
    }
    // Runs a hooked function's original body with `done` after it, returning to the caller's ra.
    void CallOrigThen(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, PS2Runtime::RecompiledFunction orig, GuestDone done)
    {
        const uint32_t ra = Gpr(ctx, 31);
        g_steps.clear();
        g_stepsDone = [ra, done = std::move(done)](uint8_t *m, R5900Context *c, PS2Runtime *r) {
            done(m, c, r);
            SetGpr(c, 31, ra);
            c->pc = ra;
        };
        SetGpr(ctx, 31, kStubSeq);
        orig(rdram, ctx, runtime);
    }

    // Labels, choices and the hidden sliders; once, after the set-up.
    void DressGfxPage(uint8_t *rdram)
    {
        for (int i = 0; i < kGfxRowCount; ++i)
        {
            const GfxRow &g = kGfxRows[i];
            const uint32_t w = kGfxPage + g.widget;
            PutText(rdram, LabelId(i), g.label);
            PutText(rdram, ValueId(i), GameValueText(g.row));
            W16g(rdram, R32g(rdram, w + 0x84u) + 0x88u, static_cast<uint16_t>(LabelId(i)));   // label text element's id
            const uint32_t n = g.rest ? 3u : 2u;
            for (uint32_t k = 0; k < n; ++k)
                W32g(rdram, w + 0x48u + 12u * k, ValueId(i));
            W32g(rdram, w + 0x9Cu, n);
        }
        PutText(rdram, kGraphicsId, "graphics");
        W32g(rdram, kGfxPage + 0x1Cu, kGraphicsId);   // page title (drawn from its text id each frame)
        W32g(rdram, kGfxPage + 0xD98u, kFirstRow);
    }

    bool g_gfxFresh = false;
    void SettleGfxRows(uint8_t *rdram, bool apply)
    {
        for (int i = 0; i < kGfxRowCount; ++i)
        {
            const GfxRow &g = kGfxRows[i];
            const uint32_t w = kGfxPage + g.widget, idx = R32g(rdram, w + 0x94u);
            if (idx == g.rest)
                continue;
            if (std::getenv("RT_PAGELOG"))
                std::fprintf(stderr, "[page] row %d idx %u prev %u apply %d\n", i, idx, R32g(rdram, w + 0x98u), apply);
            if (apply)
            {
                std::lock_guard<std::mutex> lock(g_pageMutex);
                g_pageChanges.emplace_back(g.row, !g.rest || idx < g.rest ? 1 : -1);   // the widget counts down to the right
            }
            W32g(rdram, w + 0x94u, g.rest);
            W32g(rdram, w + 0xD8u, g.rest);
        }
    }

    PS2Runtime::RecompiledFunction g_registerOrig = nullptr, g_pageEnterOrig = nullptr, g_pageUpdateOrig = nullptr,
                                   g_pageUpOrig = nullptr, g_pageDownOrig = nullptr, g_pageUpRepOrig = nullptr,
                                   g_pageDownRepOrig = nullptr, g_pageAcceptOrig = nullptr, g_pageCancelOrig = nullptr;
    bool IsGfxPage(const R5900Context *ctx) { return Gpr(ctx, 4) == kGfxPage; }

    void RegisterScreenWrap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // 0x1397E8(manager, object, id); 0x1398D0 calls it for every screen, with its pages'
        // set-up argument in s2.
        if (Gpr(ctx, 6) != 0x64 || std::getenv("RT_NOGFXPAGE"))
            return g_registerOrig(rdram, ctx, runtime);
        auto saved = std::make_shared<std::array<__m128i, 32>>();
        std::memcpy(saved->data(), ctx->r, sizeof(ctx->r[0]) * 32);
        const uint32_t setupArg = Gpr(ctx, 18);
        std::memset(rdram + kGfxPage, 0, 0x1000);
        std::memset(rdram + kGfxText, 0, 0x1000);
        StartSeq(rdram, ctx, runtime,
                 {{0x13BDD0u, {kGfxPage}, nullptr},
                  {0x147B00u, {kGfxPage, setupArg}, nullptr},
                  {0x1487D0u, {kGfxPage}, DressGfxPage}},   // focus the first row
                 [saved](uint8_t *m, R5900Context *c, PS2Runtime *r) {
                     std::memcpy(c->r, saved->data(), sizeof(c->r[0]) * 32);
                     // The manager's table has 12-byte entries; +8 is the screen's object.
                     W32g(m, R32g(m, Gpr(c, 4) + 4u) + 0x64u * 12u + 8u, kGfxPage);
                     SetGpr(c, 5, kGfxPage);
                     std::fprintf(stderr, "[gfx] in-game Graphics page built (options: screen alignment)\n");
                     c->pc = 0x1397E8u;
                     g_registerOrig(m, c, r);
                 });
    }

    void PageEnterWrap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (IsGfxPage(ctx))
            g_gfxFresh = true;   // the original loads the Preferences values into the rows
        g_pageEnterOrig(rdram, ctx, runtime);
    }

    void PageUpdateWrap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (IsGfxPage(ctx))
        {
            SettleGfxRows(rdram, !g_gfxFresh);
            g_gfxFresh = false;
            for (int i = 0; i < kGfxRowCount; ++i)
                PutText(rdram, ValueId(i), GameValueText(kGfxRows[i].row));
            for (uint32_t s : kSliders)
                W32g(rdram, kGfxPage + s + 0x94u, 0);   // alpha
            W32g(rdram, kGfxPage + 0x6B8u + 0xCCu, 0x64);   // the autosave row is dimmed without a card
            W32g(rdram, kGfxPage + 0xDBCu, R32g(rdram, kGfxPage + 0x344u));   // no sound quality change
        }
        g_pageUpdateOrig(rdram, ctx, runtime);
    }

    // Up/down: keep the cursor on the choice rows (2-10) and let it onto row 6 even when the
    // Preferences page would skip it.
    void PageMove(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, PS2Runtime::RecompiledFunction orig, bool down, bool repeat)
    {
        if (!IsGfxPage(ctx) || (repeat && static_cast<int32_t>(R32g(rdram, kGfxPage + 0xD9Cu)) < 0x15))
            return orig(rdram, ctx, runtime);
        const uint32_t cur = R32g(rdram, kGfxPage + 0xD98u);
        if (std::getenv("RT_PAGELOG"))
            std::fprintf(stderr, "[page] move %s%s from %u\n", down ? "down" : "up", repeat ? " (repeat)" : "", cur);
        if (down && cur >= kLastRow)
            W32g(rdram, kGfxPage + 0xD98u, kFirstRow - 1);
        else if (!down && cur <= kFirstRow)
            W32g(rdram, kGfxPage + 0xD98u, 0);   // the original wraps 0 to 10
        const uint32_t autosave = R32g(rdram, kAutosavePref);
        W32g(rdram, kAutosavePref, 0);
        CallOrigThen(rdram, ctx, runtime, orig, [autosave](uint8_t *m, R5900Context *, PS2Runtime *) { W32g(m, kAutosavePref, autosave); });
    }
    void PageUpWrap(uint8_t *m, R5900Context *c, PS2Runtime *r) { PageMove(m, c, r, g_pageUpOrig, false, false); }
    void PageDownWrap(uint8_t *m, R5900Context *c, PS2Runtime *r) { PageMove(m, c, r, g_pageDownOrig, true, false); }
    void PageUpRepWrap(uint8_t *m, R5900Context *c, PS2Runtime *r) { PageMove(m, c, r, g_pageUpRepOrig, false, true); }
    void PageDownRepWrap(uint8_t *m, R5900Context *c, PS2Runtime *r) { PageMove(m, c, r, g_pageDownRepOrig, true, true); }

    // Accept and back both leave the page; its changes are already applied and saved.
    void PageLeave(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, bool accept)
    {
        const uint32_t ra = Gpr(ctx, 31);
        StartSeq(rdram, ctx, runtime,
                 {{0x205090u, {accept ? 0x48u : 0x42u}, nullptr},   // menu sound
                  {0x140108u, {accept ? 0x3Au : 0x3Bu, 0u}, nullptr}},   // to the options menu
                 [ra, accept](uint8_t *, R5900Context *c, PS2Runtime *) {
                     SetGpr(c, 2, accept ? 1u : 0u);
                     SetGpr(c, 31, ra);
                     c->pc = ra;
                 });
    }
    void PageAcceptWrap(uint8_t *m, R5900Context *c, PS2Runtime *r)
    {
        if (!IsGfxPage(c))
            return g_pageAcceptOrig(m, c, r);
        PageLeave(m, c, r, true);
    }
    void PageCancelWrap(uint8_t *m, R5900Context *c, PS2Runtime *r)
    {
        if (!IsGfxPage(c))
            return g_pageCancelOrig(m, c, r);
        PageLeave(m, c, r, false);
    }

    void InstallGfxPage(PS2Runtime &runtime)
    {
        auto hook = [&runtime](uint32_t addr, PS2Runtime::RecompiledFunction fn, PS2Runtime::RecompiledFunction *orig) {
            if (orig)
                *orig = runtime.lookupFunction(addr);
            runtime.replaceFunction(addr, fn);
        };
        hook(0x00185740u, &TextLookup, nullptr);
        hook(kStubSeq, &SeqNext, nullptr);
        hook(0x001397E8u, &RegisterScreenWrap, &g_registerOrig);
        hook(0x00148368u, &PageEnterWrap, &g_pageEnterOrig);
        hook(0x00148580u, &PageUpdateWrap, &g_pageUpdateOrig);
        hook(0x00148920u, &PageUpWrap, &g_pageUpOrig);
        hook(0x00148998u, &PageDownWrap, &g_pageDownOrig);
        hook(0x00148A10u, &PageUpRepWrap, &g_pageUpRepOrig);
        hook(0x00148AA0u, &PageDownRepWrap, &g_pageDownRepOrig);
        hook(0x00148C48u, &PageAcceptWrap, &g_pageAcceptOrig);
        hook(0x00148B30u, &PageCancelWrap, &g_pageCancelOrig);
    }
}

namespace
{
    // RT_CALLCENSUS=lo-hi (hex guest addresses): counts calls of every recompiled function that
    // starts in the range and writes the ones called in each RT_CALLCENSUS_SECS (default 2) second
    // window to callcensus.txt (cwd), one line per window: "t=<s> addr:count ...". For finding the
    // code behind a screen: compare windows taken on different screens.
    std::vector<PS2Runtime::RecompiledFunction> g_censusOrig;
    std::vector<uint32_t> g_censusCount;
    uint32_t g_censusLo = 0;
    FILE *g_censusFile = nullptr;
    double g_censusNext = 0.0, g_censusStart = 0.0;
    double CensusNow() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
    void CensusWrap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t slot = (ctx->pc - g_censusLo) >> 2;
        ++g_censusCount[slot];
        static const double secs = Env("RT_CALLCENSUS_SECS") ? std::atof(Env("RT_CALLCENSUS_SECS")) : 2.0;
        const double now = CensusNow();
        if (now >= g_censusNext && g_censusFile)
        {
            g_censusNext = now + secs;
            std::fprintf(g_censusFile, "t=%.1f", now - g_censusStart);
            for (size_t k = 0; k < g_censusCount.size(); ++k)
                if (g_censusCount[k])
                {
                    std::fprintf(g_censusFile, " %x:%u", g_censusLo + 4u * static_cast<uint32_t>(k), g_censusCount[k]);
                    g_censusCount[k] = 0;
                }
            std::fprintf(g_censusFile, "\n");
            std::fflush(g_censusFile);
        }
        g_censusOrig[slot](rdram, ctx, runtime);
    }
    // RT_ARGLOG=addr[,addr...] (hex): logs a0..a3, ra and the first 16 words at a0 of the first
    // RT_ARGLOG_N (default 8) calls of each function, to stderr.
    std::map<uint32_t, std::pair<PS2Runtime::RecompiledFunction, int>> g_argLog;
    void ArgLogWrap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        auto &e = g_argLog[ctx->pc];
        static const int maxN = Env("RT_ARGLOG_N") ? std::atoi(Env("RT_ARGLOG_N")) : 8;
        if (e.second++ < maxN)
        {
            auto r = [ctx](int k) { return static_cast<uint32_t>(_mm_extract_epi32(ctx->r[k], 0)); };
            std::fprintf(stderr, "[arglog] %x a0=%x a1=%x a2=%x a3=%x t0=%x t1=%x t2=%x t3=%x ra=%x |", ctx->pc, r(4), r(5), r(6), r(7), r(8), r(9), r(10), r(11), r(31));
            const uint32_t a0 = r(4) & 0x1FFFFFFu;
            if (a0 + 64 <= 0x2000000u)
                for (int k = 0; k < 16; ++k)
                {
                    uint32_t w;
                    std::memcpy(&w, rdram + a0 + 4 * k, 4);
                    std::fprintf(stderr, " %x", w);
                }
            std::fprintf(stderr, "\n");
        }
        e.first(rdram, ctx, runtime);
    }
    void InstallArgLog(PS2Runtime &runtime, const char *spec)
    {
        for (const char *p = spec; *p;)
        {
            char *end = nullptr;
            const uint32_t a = static_cast<uint32_t>(std::strtoul(p, &end, 16));
            if (end == p)
                break;
            g_argLog[a] = {runtime.lookupFunction(a), 0};
            runtime.replaceFunction(a, &ArgLogWrap);
            p = *end ? end + 1 : end;
        }
    }
    void InstallCensus(PS2Runtime &runtime, const char *spec)
    {
        uint32_t lo = 0, hi = 0;
        if (std::sscanf(spec, "%x-%x", &lo, &hi) != 2 || hi <= lo)
            return;
        lo = std::max(lo & ~3u, g_ps2RecompiledFunctionTableBase);
        hi = std::min(hi, g_ps2RecompiledFunctionTableBase + 4u * g_ps2RecompiledFunctionTableSlotCount);
        g_censusLo = lo;
        g_censusOrig.assign((hi - lo) / 4, nullptr);
        g_censusCount.assign((hi - lo) / 4, 0);
        PS2Runtime::RecompiledFunction prev = nullptr;
        size_t n = 0;
        for (uint32_t a = g_ps2RecompiledFunctionTableBase; a < hi; a += 4)
        {
            PS2Runtime::RecompiledFunction f = g_ps2RecompiledFunctionTable[(a - g_ps2RecompiledFunctionTableBase) >> 2];
            if (!f)
                continue;
            if (a >= lo && f != prev)   // a function's first entry (its labels share the pointer)
            {
                g_censusOrig[(a - lo) >> 2] = f;
                runtime.replaceFunction(a, &CensusWrap);
                ++n;
            }
            prev = f;
        }
        g_censusFile = std::fopen("callcensus.txt", "w");
        g_censusStart = CensusNow();
        std::fprintf(stderr, "[census] %zu functions in %x-%x\n", n, lo, hi);
    }
}

namespace wipeout
{
    void InstallSettings(PS2Runtime &runtime)
    {
        g_rt = &runtime;
        ReadCurrentState();
        ApplyAspect();
        gsgpu::SetDeinterlaceMode(kDeinterlaceMode);
        if (!g_set.vsync)
            ps2x_capture::g_vsyncRequest.store(0);
        // Chain after the override's own pad hook (RT_PADFILE / RT_PADSCRIPT), so call this last.
        g_prevPadHook = ps2x_capture::g_padHook.load();
        ps2x_capture::g_padHook.store(&PadHook);
        if (const char *o = Env("RT_GFXMENU_OPEN"))   // start with the menu open (testing)
        {
            g_open = true;
            g_sel = std::clamp(std::atoi(o), 0, RowCount - 1);
            if (const char *w = Env("RT_WEAPONPAGE"))   // ... on the weapons page, row n
                g_sel = RowWeapons, g_page = 1, g_wsel = std::clamp(std::atoi(w), 0, kWeaponBack);
        }
        g_readOrig = runtime.lookupFunction(0x00201CB8u);
        runtime.replaceFunction(0x00201CB8u, &FileReadWrap);
        g_viewOrig = runtime.lookupFunction(0x001EC160u);
        runtime.replaceFunction(0x001EC160u, &ViewSetupWrap);
        g_projOrig = runtime.lookupFunction(0x001EDF00u);
        runtime.replaceFunction(0x001EDF00u, &ProjectionWrap);
        InstallGfxPage(runtime);
        g_reticleOrig = runtime.lookupFunction(0x0018C058u);
        runtime.replaceFunction(0x0018C058u, &ReticleWrap);
        g_hudGaugeOrig = runtime.lookupFunction(0x0017F4F8u);
        runtime.replaceFunction(0x0017F4F8u, &HudGaugeWrap);
        g_randOrig = runtime.lookupFunction(0x0022BD80u);
        runtime.replaceFunction(0x0022BD80u, &RandWrap);
        g_prevVblankHook = ps2x_capture::g_vblankHook.load();
        ps2x_capture::g_vblankHook.store(&VblankHook);
        if (const char *u = Env("RT_UNLOCKALL"))   // testing: unlock after N pad reads (default 1)
            g_unlockDelay = std::max(1, std::atoi(u));
        g_prevOverlay = ps2x_capture::g_overlayHook.load();
        ps2x_capture::g_overlayHook.store(&Overlay);
        std::fprintf(stderr, "[gfx] F10 (gamepad: L3 + R3) opens the PC settings menu; settings in %s\n", SettingsPath().c_str());
        if (const char *c = Env("RT_CALLCENSUS"))
            InstallCensus(runtime, c);
        if (const char *a = Env("RT_ARGLOG"))
            InstallArgLog(runtime, a);
    }
}
