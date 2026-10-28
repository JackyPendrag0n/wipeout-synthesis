// Debug menu (F1): level warp and cheats.
//
// The menu runs on the window thread (drawn by the overlay hook, keys read with raylib) and
// queues actions; the game thread runs them at the end of each game frame (the game's vsync
// wait, f_00291140), where calling game functions and writing game state is safe.
// While the menu is open the pad reads as released, so menu keys do not reach the game.
#include "ps2_runtime.h"
#include "runtime/ps2_capture_hook.h"
#include "raylib.h"

#include <algorithm>
#include <cmath>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

extern std::atomic<bool> g_rtSyncTrace;
namespace rufftrigger
{
    extern std::atomic<bool> g_profTrigger;
}
namespace rufftrigger
{
    extern std::atomic<uint64_t> g_gameFrames;

    namespace
    {
        PS2Runtime *g_rt = nullptr;

        uint32_t R32(const uint8_t *rdram, uint32_t a)
        {
            uint32_t v = 0;
            std::memcpy(&v, rdram + (a & 0x01FFFFFCu), 4);
            return v;
        }

        void W32(uint8_t *rdram, uint32_t a, uint32_t v) { std::memcpy(rdram + (a & 0x01FFFFFCu), &v, 4); }
        float RF(const uint8_t *rdram, uint32_t a) { float v; std::memcpy(&v, rdram + (a & 0x01FFFFFCu), 4); return v; }
        void WF(uint8_t *rdram, uint32_t a, float v) { std::memcpy(rdram + (a & 0x01FFFFFCu), &v, 4); }
        bool Valid(uint32_t a) { return a >= 0x00100000u && a < 0x02000000u && (a & 3u) == 0; }

        // Game state (see docs/debug.md): game singleton, current character.
        constexpr uint32_t kGamePtr = 0x00351594u, kLevelMgrPtr = 0x0035403Cu, kGodFlag = 0x00352210u;
        constexpr uint32_t kRenderView = 0x004EECC0u;
        uint32_t Game(const uint8_t *rdram) { const uint32_t g = R32(rdram, kGamePtr); return Valid(g) ? g : 0; }
        uint32_t Player(const uint8_t *rdram)
        {
            const uint32_t g = Game(rdram);
            if (!g) return 0;
            const uint32_t i = R32(rdram, g + 0x184);
            if (i > 1) return 0;
            const uint32_t c = R32(rdram, g + 4 * i);
            return Valid(c) ? c : 0;
        }

        // ---- host input sampled on the window thread -------------------------------------------
        struct Input { float mx = 0, mz = 0, my = 0, yaw = 0, pitch = 0; bool jump = false, fast = false; };
        std::mutex g_inMutex;
        Input g_in;

        // ---- cheat switches ------------------------------------------------------------------------
        std::atomic<bool> g_god{false}, g_moon{false}, g_noclip{false}, g_freecam{false};

        // ---- calling game functions from the game thread --------------------------------------
        constexpr uint32_t kReturnMagic = 0x0FFFFFF0u;

        // Calls guest function `fn` with integer args a0..a3 on a copy of the current context,
        // using stack below the caller's. Returns v0. Only for functions that do not block.
        uint32_t CallGuest(uint8_t *rdram, const R5900Context *ctx, PS2Runtime *rt, uint32_t fn,
                           std::initializer_list<uint32_t> args, float f12 = 0.0f)
        {
            auto first = rt->lookupFunction(fn);
            if (!first)
            {
                std::fprintf(stderr, "[debug] no function at %08x\n", fn);
                return 0;
            }
            R5900Context c = *ctx;
            const uint32_t sp = (static_cast<uint32_t>(_mm_extract_epi32(ctx->r[29], 0)) - 0x2000u) & ~0xFu;
            c.r[29] = _mm_set_epi64x(0, sp);
            c.r[31] = _mm_set_epi64x(0, kReturnMagic);
            int k = 4;
            for (uint32_t a : args)
                c.r[k++] = _mm_set_epi64x(0, static_cast<int32_t>(a));
            std::memcpy(&c.f[12], &f12, 4);
            c.pc = fn;
            first(rdram, &c, rt);
            for (int guard = 0; c.pc != kReturnMagic && c.pc != 0 && guard < 100000; ++guard)
            {
                auto next = rt->lookupFunction(c.pc);
                if (!next)
                    break;
                next(rdram, &c, rt);
            }
            return static_cast<uint32_t>(_mm_extract_epi32(c.r[2], 0));
        }

        // ---- actions queued for the game thread ------------------------------------------------
        using Action = std::function<void(uint8_t *, R5900Context *, PS2Runtime *)>;
        std::mutex g_actMutex;
        std::vector<Action> g_actions;

        void Queue(Action a)
        {
            std::lock_guard<std::mutex> lock(g_actMutex);
            g_actions.push_back(std::move(a));
        }

        // ---- levels (RUFF.INI) -------------------------------------------------------------------
        struct Level { int num; std::string name; };
        std::vector<Level> g_levels;

        void LoadLevelList()
        {
            namespace fs = std::filesystem;
            const fs::path root = PS2Runtime::getIoPaths().cdRoot;
            std::error_code ec;
            fs::path ini;
            for (auto it = fs::directory_iterator(root, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
            {
                std::string n = it->path().filename().string();
                for (char &ch : n) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                if (n == "RUFF.INI")
                    ini = it->path();
            }
            std::ifstream f(ini);
            std::string line;
            int num = -1;
            while (std::getline(f, line))
            {
                auto value = [&](const char *key) -> std::string {
                    const size_t k = line.find(key);
                    if (k == std::string::npos) return {};
                    const size_t eq = line.find('=', k);
                    if (eq == std::string::npos) return {};
                    std::string v = line.substr(eq + 1);
                    v.erase(0, v.find_first_not_of(" \t"));
                    while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
                    return v;
                };
                if (line.find("$LEVELNUM") != std::string::npos)
                    num = std::atoi(value("$LEVELNUM").c_str());
                else if (line.find("$LEVELDESCRIPTION") != std::string::npos && num >= 0)
                {
                    std::string d = value("$LEVELDESCRIPTION");
                    std::replace(d.begin(), d.end(), '_', ' ');
                    g_levels.push_back({num, d});
                    num = -1;
                }
            }
            std::fprintf(stderr, "[debug] %zu levels\n", g_levels.size());
        }

        // ---- menu --------------------------------------------------------------------------------
        enum Item { kWarp, kGod, kMoon, kNoclip, kFreecam, kGiveAll, kMaxRank, kCount };
        bool g_open = false;
        int g_sel = 0;
        int g_level = 1;
        std::string g_toast;
        double g_toastUntil = 0;

        void Toast(const std::string &s)
        {
            g_toast = s;
            g_toastUntil = GetTime() + 3.0;
            std::fprintf(stderr, "[debug] %s\n", s.c_str());
        }

        // A warp leaves the old level (and its player) running through the summary screen until
        // the new level loads: 1 = warp requested, 2 = old player gone, 0 = new level running.
        std::atomic<int> g_warpState{0};

        void WarpTo(int levelIndex)
        {
            if (levelIndex < 0 || levelIndex >= static_cast<int>(g_levels.size()))
                return;
            const Level L = g_levels[levelIndex];
            Queue([L](uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt) {
                const uint32_t mgr = R32(rdram, 0x0035403Cu);
                if (!mgr)
                {
                    std::fprintf(stderr, "[debug] warp: no level manager yet\n");
                    return;
                }
                CallGuest(rdram, ctx, rt, 0x00170440u, {mgr, static_cast<uint32_t>(static_cast<int8_t>(L.num))});
                g_warpState.store(1);
            });
            Toast("Warp to " + std::to_string(L.num) + ": " + L.name);
        }

        void SampleInput()
        {
            Input in;
            auto key = [](int k) { return IsKeyDown(k) ? 1.0f : 0.0f; };
            in.mx = key(KEY_D) - key(KEY_A);
            in.mz = key(KEY_W) - key(KEY_S);
            in.my = key(KEY_E) - key(KEY_Q);
            in.yaw = key(KEY_L) - key(KEY_J);
            in.pitch = key(KEY_I) - key(KEY_K);
            in.jump = IsKeyDown(KEY_SPACE) || IsKeyDown(KEY_X);
            in.fast = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
            if (IsGamepadAvailable(0))
            {
                auto dz = [](float v) { return std::fabs(v) < 0.2f ? 0.0f : v; };
                in.mx += dz(GetGamepadAxisMovement(0, GAMEPAD_AXIS_LEFT_X));
                in.mz -= dz(GetGamepadAxisMovement(0, GAMEPAD_AXIS_LEFT_Y));
                in.yaw += dz(GetGamepadAxisMovement(0, GAMEPAD_AXIS_RIGHT_X));
                in.pitch -= dz(GetGamepadAxisMovement(0, GAMEPAD_AXIS_RIGHT_Y));
                in.my += (IsGamepadButtonDown(0, GAMEPAD_BUTTON_RIGHT_TRIGGER_1) ? 1.0f : 0.0f) - (IsGamepadButtonDown(0, GAMEPAD_BUTTON_LEFT_TRIGGER_1) ? 1.0f : 0.0f);
                in.jump = in.jump || IsGamepadButtonDown(0, GAMEPAD_BUTTON_RIGHT_FACE_DOWN);
                in.fast = in.fast || IsGamepadButtonDown(0, GAMEPAD_BUTTON_LEFT_TRIGGER_2);
            }
            std::lock_guard<std::mutex> lock(g_inMutex);
            g_in = in;
        }

        // Weapons (items 1-10) and werewolf powers (12-17) for both characters: the game's
        // setter for the current one, direct inventory writes for the other, and the weapon
        // objects' availability field cleared.
        void GiveAll()
        {
            // Given between a warp and the new level, the weapons go to the old level's player and
            // the new level is built with all of them, including effects it never loads: the game
            // then builds graphics data from bad pointers and freezes.
            if (g_warpState.load() != 0)
            {
                Toast("Give all: wait until the new level has started");
                return;
            }
            Queue([](uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt) {
                if (g_warpState.load() != 0)
                {
                    std::fprintf(stderr, "[debug] give all: level is changing, not given\n");
                    return;
                }
                const uint32_t g = Game(rdram);
                const uint32_t p = Player(rdram);
                if (!g || !p)
                {
                    std::fprintf(stderr, "[debug] give all: not in a level\n");
                    return;
                }
                static const int items[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 13, 14, 15, 16, 17};
                for (const uint32_t slotOff : {0x17Cu, 0x180u})
                {
                    const uint32_t slot = R32(rdram, g + slotOff);
                    if (slot > 8) continue;
                    for (int i : items)
                        W32(rdram, g + 0x390 + slot * 0x16C + i * 4, 1);
                }
                for (int i : items)
                    CallGuest(rdram, ctx, rt, 0x00109F90u, {g, static_cast<uint32_t>(i), 1u, 0u});
                const uint32_t wm = R32(rdram, p + 0x3FC);
                if (Valid(wm))
                    for (int i = 0; i < 18; ++i)
                    {
                        const uint32_t w = R32(rdram, wm + 0xC + i * 4);
                        static const bool log = std::getenv("RT_DEBUGLOG") != nullptr;
                        if (log)
                            std::fprintf(stderr, "[debug] weapon slot %d obj %08x +50=%08x +54=%08x +58=%08x\n", i, w,
                                         Valid(w) ? R32(rdram, w + 0x50) : 0u, Valid(w) ? R32(rdram, w + 0x54) : 0u, Valid(w) ? R32(rdram, w + 0x58) : 0u);
                        static const bool noClear = std::getenv("RT_GIVEALL_NOCLEAR") != nullptr;
                        if (Valid(w) && !noClear)
                            W32(rdram, w + 0x50, 0);
                    }
            });
            Toast("All weapons and werewolf powers given");
        }

        // Rank: the singleton at 0x351598 holds the rank index (0-10); the game's update
        // (f_0010a5a0) raises it while the player's points (player+0x6D0) reach the next
        // threshold in the RANK_MEDAL table at 0x4c6110. Give the points for the top rank, update.
        void MaxRank()
        {
            Queue([](uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt) {
                const uint32_t p = Player(rdram);
                if (!p)
                {
                    std::fprintf(stderr, "[debug] max rank: not in a level\n");
                    return;
                }
                const uint32_t rank = CallGuest(rdram, ctx, rt, 0x0010A490u, {});
                if (!Valid(rank))
                    return;
                const int32_t top = static_cast<int32_t>(R32(rdram, 0x004C6110u + 10 * 4));
                if (static_cast<int32_t>(R32(rdram, p + 0x6D0)) < top)
                    W32(rdram, p + 0x6D0, static_cast<uint32_t>(top));
                CallGuest(rdram, ctx, rt, 0x0010A5A0u, {rank, 0u, 0u});
                std::fprintf(stderr, "[debug] rank now %u, points %d\n", R32(rdram, rank),
                             static_cast<int32_t>(R32(rdram, p + 0x6D0)));
            });
            Toast("Max rank");
        }

        void HandleKeys()
        {
            SampleInput();
            if (IsKeyPressed(KEY_F1))
                g_open = !g_open;
            if (IsKeyPressed(KEY_F2)) { g_noclip = !g_noclip; Toast(std::string("No clip ") + (g_noclip ? "on" : "off")); }
            if (IsKeyPressed(KEY_F3)) { g_freecam = !g_freecam; Toast(std::string("Free camera ") + (g_freecam ? "on" : "off")); }
            if (IsKeyPressed(KEY_F4)) { g_moon = !g_moon; Toast(std::string("Moon jump ") + (g_moon ? "on" : "off")); }
            if (IsKeyPressed(KEY_F5)) { g_god = !g_god; Toast(std::string("Invincible ") + (g_god ? "on" : "off")); }
            ps2x_capture::g_padBlock.store(g_open || g_freecam);
            if (!g_open)
                return;
            if (IsKeyPressed(KEY_DOWN)) g_sel = (g_sel + 1) % kCount;
            if (IsKeyPressed(KEY_UP)) g_sel = (g_sel + kCount - 1) % kCount;
            const int dir = (IsKeyPressed(KEY_RIGHT) ? 1 : 0) - (IsKeyPressed(KEY_LEFT) ? 1 : 0);
            const bool go = IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE) || IsKeyPressed(KEY_X);
            const int nl = std::max<int>(1, static_cast<int>(g_levels.size()));
            switch (g_sel)
            {
            case kWarp:
                if (dir) g_level = (g_level + dir + nl) % nl;
                if (go) { WarpTo(g_level); g_open = false; }
                break;
            case kGod: if (go || dir) { g_god = !g_god; Toast(std::string("Invincible ") + (g_god ? "on" : "off")); } break;
            case kMoon: if (go || dir) { g_moon = !g_moon; Toast(std::string("Moon jump ") + (g_moon ? "on (hold jump)" : "off")); } break;
            case kNoclip: if (go || dir) { g_noclip = !g_noclip; Toast(std::string("No clip ") + (g_noclip ? "on (WASD, Q/E up/down, Shift fast)" : "off")); } break;
            case kFreecam: if (go || dir) { g_freecam = !g_freecam; Toast(std::string("Free camera ") + (g_freecam ? "on (WASD move, IJKL look, Q/E, Shift)" : "off")); } break;
            case kGiveAll: if (go) { GiveAll(); } break;
            case kMaxRank: if (go) { MaxRank(); } break;
            }
            ps2x_capture::g_padBlock.store(g_open || g_freecam);
        }

        void DrawMenu(int screenW, int screenH)
        {
            const int fs = std::max(16, screenH / 40);
            if (g_open)
            {
                const int w = fs * 28, lines = kCount + 3;
                const int x = 20, y = 20, h = fs * lines + fs;
                DrawRectangle(x, y, w, h, Fade(BLACK, 0.75f));
                DrawRectangleLines(x, y, w, h, GOLD);
                DrawText("DEBUG  (F1 close, arrows, Enter)", x + fs / 2, y + fs / 2, fs, GOLD);
                auto row = [&](int i, const std::string &label) {
                    const int ry = y + fs / 2 + (i + 2) * fs;
                    DrawText(((g_sel == i ? "> " : "  ") + label).c_str(), x + fs / 2, ry, fs, g_sel == i ? YELLOW : RAYWHITE);
                };
                std::string lv = g_levels.empty() ? "(no level list)" :
                    "< " + std::to_string(g_levels[g_level].num) + ": " + g_levels[g_level].name + " >";
                row(kWarp, "Level warp " + lv);
                auto onoff = [](bool b) { return std::string(b ? "[ON]" : "[off]"); };
                row(kGod, "Invincible " + onoff(g_god));
                row(kMoon, "Moon jump " + onoff(g_moon));
                row(kNoclip, "No clip " + onoff(g_noclip));
                row(kFreecam, "Free camera " + onoff(g_freecam));
                row(kGiveAll, "Give all weapons + werewolf powers");
                row(kMaxRank, "Max rank");
            }
            if (!g_toast.empty() && GetTime() < g_toastUntil)
                DrawText(g_toast.c_str(), 20, screenH - fs * 2, fs, YELLOW);
            (void)screenW;
        }

        void Overlay(int screenW, int screenH)
        {
            HandleKeys();
            DrawMenu(screenW, screenH);
        }

        // ---- per-frame cheats (game thread) ------------------------------------------------------
        bool g_noclipActive = false, g_godActive = false;
        uint32_t g_noclipChar = 0;
        float g_camPos[3], g_camYaw = 0, g_camPitch = 0;
        bool g_camInit = false, g_camRestore = false;
        float g_camSaveEye[3], g_camSaveTgt[3];
        uint32_t g_camEyeAddr = 0;

        void Cheats(uint8_t *rdram)
        {
            Input in;
            {
                std::lock_guard<std::mutex> lock(g_inMutex);
                in = g_in;
            }
            const uint32_t p = Player(rdram);
            // Invincible: the game's flag that its damage handler and weapon checks test.
            if (g_god != g_godActive)
            {
                W32(rdram, kGodFlag, g_god ? 1u : 0u);
                g_godActive = g_god;
            }
            else if (g_god && R32(rdram, kGodFlag) == 0)
                W32(rdram, kGodFlag, 1u);   // cleared on level load
            if (!p)
                return;
            static const bool log = std::getenv("RT_DEBUGLOG") != nullptr;
            static uint32_t n = 0;
            if (log && ++n % 60 == 0)
                std::fprintf(stderr, "[debug] player %08x pos %.1f %.1f %.1f vy %.1f god=%u cam %.1f %.1f %.1f\n", p, RF(rdram, p + 0xC0),
                             RF(rdram, p + 0xC4), RF(rdram, p + 0xC8), RF(rdram, p + 0xEC), R32(rdram, kGodFlag),
                             RF(rdram, kRenderView + 0x68), RF(rdram, kRenderView + 0x6C), RF(rdram, kRenderView + 0x70));
            if (g_moon && in.jump && !g_noclip)
            {
                WF(rdram, p + 0xEC, 700.0f);
                WF(rdram, p + 0xF8, 700.0f);
            }
            // No clip: skip the character's collision/gravity step and move it directly,
            // relative to the camera's heading.
            if (g_noclip && !g_freecam)
            {
                if (!g_noclipActive || g_noclipChar != p)
                {
                    W32(rdram, p + 0x1C4, 1);
                    g_noclipActive = true;
                    g_noclipChar = p;
                }
                for (uint32_t o = 0xE8; o <= 0xFC; o += 4)
                    WF(rdram, p + o, 0.0f);
                float fx = RF(rdram, p + 0xC0) - RF(rdram, kRenderView + 0x68);
                float fz = RF(rdram, p + 0xC8) - RF(rdram, kRenderView + 0x70);
                const float l = std::sqrt(fx * fx + fz * fz);
                if (l > 1e-3f) { fx /= l; fz /= l; } else { fx = 0; fz = 1; }
                const float rx = fz, rz = -fx;
                const float sp = in.fast ? 40.0f : 12.0f;
                const float dx = (fx * in.mz + rx * in.mx) * sp, dz = (fz * in.mz + rz * in.mx) * sp, dy = in.my * sp;
                for (uint32_t base : {0xC0u, 0xCCu, 0xD8u})
                {
                    WF(rdram, p + base, RF(rdram, p + base) + dx);
                    WF(rdram, p + base + 4, RF(rdram, p + base + 4) + dy);
                    WF(rdram, p + base + 8, RF(rdram, p + base + 8) + dz);
                }
            }
            else if (g_noclipActive)
            {
                if (Valid(g_noclipChar))
                {
                    W32(rdram, g_noclipChar + 0x1C4, 0);
                    for (uint32_t o = 0xE8; o <= 0xFC; o += 4)
                        WF(rdram, g_noclipChar + o, 0.0f);
                }
                g_noclipActive = false;
            }
            // Free camera: position/orientation integrated here, applied in the LookAt wrapper.
            if (g_freecam && g_camInit)
            {
                g_camYaw += in.yaw * 0.04f;
                g_camPitch = std::clamp(g_camPitch + in.pitch * 0.03f, -1.5f, 1.5f);
                const float cy = std::cos(g_camYaw), sy = std::sin(g_camYaw), cp = std::cos(g_camPitch), spi = std::sin(g_camPitch);
                const float f[3] = {sy * cp, spi, cy * cp}, r[3] = {cy, 0, -sy};
                const float v = in.fast ? 60.0f : 15.0f;
                for (int k = 0; k < 3; ++k)
                    g_camPos[k] += (f[k] * in.mz + r[k] * in.mx) * v + (k == 1 ? in.my * v : 0.0f);
            }
            if (!g_freecam)
                g_camInit = false;
        }

        // Camera LookAt (f_001d3c20(eye*, target*, out 3x3*, ?, roll)): while the free
        // camera is on, substitute its eye and target for the duration of the call.
        PS2Runtime::RecompiledFunction g_lookAtOrig = nullptr;
        void LookAt(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t eye = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0)) & 0x01FFFFFFu;
            const uint32_t tgt = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[5], 0)) & 0x01FFFFFFu;
            const uint32_t view = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[7], 0)) & 0x01FFFFFFu;
            static const bool lookLog = std::getenv("RT_LOOKLOG") != nullptr;
            static int lookN = 0;
            if (lookLog && ctx->pc == 0x001D3C20u && lookN++ % 200 == 0)
                std::fprintf(stderr, "[debug] lookat view=%08x eye=%08x ra=%08x\n", view, eye, static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0)));
            if (!g_freecam || ctx->pc != 0x001D3C20u || !Valid(eye) || !Valid(tgt))
            {
                // Leaving the free camera: put back the game's camera from when it started.
                if (!g_freecam && g_camRestore && ctx->pc == 0x001D3C20u && eye == g_camEyeAddr && Valid(eye) && Valid(tgt))
                {
                    std::memcpy(rdram + eye, g_camSaveEye, 12);
                    std::memcpy(rdram + tgt, g_camSaveTgt, 12);
                    g_camRestore = false;
                }
                g_lookAtOrig(rdram, ctx, runtime);
                return;
            }
            float e[3], t[3], se[3], st[3];
            std::memcpy(se, rdram + eye, 12);
            std::memcpy(st, rdram + tgt, 12);
            if (!g_camInit)
            {
                std::memcpy(g_camSaveEye, se, 12);
                std::memcpy(g_camSaveTgt, st, 12);
                g_camEyeAddr = eye;
                g_camRestore = true;
                std::memcpy(g_camPos, se, 12);
                const float d[3] = {st[0] - se[0], st[1] - se[1], st[2] - se[2]};
                g_camYaw = std::atan2(d[0], d[2]);
                g_camPitch = std::atan2(d[1], std::sqrt(d[0] * d[0] + d[2] * d[2]));
                g_camInit = true;
            }
            const float cp = std::cos(g_camPitch);
            std::memcpy(e, g_camPos, 12);
            t[0] = e[0] + std::sin(g_camYaw) * cp * 100.0f;
            t[1] = e[1] + std::sin(g_camPitch) * 100.0f;
            t[2] = e[2] + std::cos(g_camYaw) * cp * 100.0f;
            std::memcpy(rdram + eye, e, 12);
            std::memcpy(rdram + tgt, t, 12);
            // The caller keeps using eye/target after the call (e.g. as the view position), so
            // they stay replaced.
            g_lookAtOrig(rdram, ctx, runtime);
        }

        // ---- game-thread hook at the end of each game frame -------------------------------------
        PS2Runtime::RecompiledFunction g_frameEndOrig = nullptr;
        // RT_DEBUGTEST=<level>: scripted test for headless runs. Warps to the level once in a
        // game, presses Cross until the new level runs, gives all weapons, then repeatedly taps
        // D-pad down and fires (hold R1, press Square).
        std::atomic<uint16_t> g_testPad{0};
        uint16_t TestPad() { return g_testPad.load(std::memory_order_relaxed); }
        int g_testLevel = -1, g_testState = 0;
        uint32_t g_testFrames = 0, g_testCycle = 0;
        void TestStep(uint8_t *rdram)
        {
            const bool player = Player(rdram) != 0;
            ++g_testFrames;
            uint16_t pad = 0;
            switch (g_testState)
            {
            case 0:
                if (!player) { g_testFrames = 0; break; }
                if (g_testFrames > 90 && g_testLevel == 0)
                {
                    g_testState = 3; g_testFrames = 0;   // RT_DEBUGTEST=0: stay in the current level
                    break;
                }
                if (g_testFrames > 90)
                {
                    for (size_t i = 0; i < g_levels.size(); ++i)
                        if (g_levels[i].num == g_testLevel) WarpTo(static_cast<int>(i));
                    g_testState = 1; g_testFrames = 0;
                }
                break;
            case 1:   // wait for the level to end, pressing Cross through the summary
                if (!player) { g_testState = 2; g_testFrames = 0; break; }
                if (g_testFrames > 120 && (g_testFrames % 60) < 20) pad = 0x4000;
                if (g_testFrames % 300 == 0) std::fprintf(stderr, "[test] waiting, player %08x\n", Player(rdram));
                break;
            case 2:
                if (player && g_testFrames > 60) { g_testState = 3; g_testFrames = 0; std::fprintf(stderr, "[test] in new level\n"); break; }
                if (!player && (g_testFrames % 90) < 15) pad = 0x4000;
                break;
            case 3:
                if (g_testFrames > 120) { GiveAll(); g_testState = 4; g_testFrames = 0; }
                break;
            case 4:
            {
                const uint32_t f = g_testFrames % 240;
                if (f == 0) std::fprintf(stderr, "[test] fire cycle %u\n", ++g_testCycle);
                if (g_testCycle == 3) g_profTrigger.store(true);
                static const int traceAt = std::getenv("RT_DEBUGTEST_TRACE") ? std::atoi(std::getenv("RT_DEBUGTEST_TRACE")) : 0;
                if (traceAt && static_cast<int>(g_testCycle) == traceAt) g_rtSyncTrace.store(true);
                static const bool up = std::getenv("RT_DEBUGTEST_UP") != nullptr;
                if (f < 12 && (!up || g_testCycle == 1)) pad = up ? 0x0010 : 0x0040;   // D-pad down (or up once)
                else if (f >= 60 && f < 200) pad = 0x0800 | ((f >= 100 && f < 180) ? 0x8000 : 0);   // R1 (+ Square)
                break;
            }
            }
            g_testPad.store(pad, std::memory_order_relaxed);
        }

        void FrameEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc == 0x00291140u)
            {
                g_gameFrames.fetch_add(1, std::memory_order_relaxed);
                std::vector<Action> todo;
                {
                    std::lock_guard<std::mutex> lock(g_actMutex);
                    todo.swap(g_actions);
                }
                for (auto &a : todo)
                    a(rdram, ctx, runtime);
                if (const int w = g_warpState.load())
                {
                    static uint32_t waited = 0;
                    const bool player = Player(rdram) != 0;
                    if (w == 1 && !player)
                        g_warpState.store(2);
                    else if (w == 2 && player)
                        g_warpState.store(0);
                    else if (w == 1 && ++waited > 36000)   // the warp never left the level (minutes)
                        g_warpState.store(0);
                    if (g_warpState.load() != 1)
                        waited = 0;
                }
                Cheats(rdram);
                if (g_testLevel >= 0)
                    TestStep(rdram);
            }
            g_frameEndOrig(rdram, ctx, runtime);
        }
    }

    void InstallDebug(PS2Runtime &runtime)
    {
        const char *e = std::getenv("RT_DEBUG");
        if (e && *e && std::atoi(e) == 0)
            return;
        g_rt = &runtime;
        LoadLevelList();
        g_frameEndOrig = runtime.lookupFunction(0x00291140u);
        if (g_frameEndOrig)
            runtime.replaceFunction(0x00291140u, &FrameEnd);
        g_lookAtOrig = runtime.lookupFunction(0x001D3C20u);
        if (g_lookAtOrig)
            runtime.replaceFunction(0x001D3C20u, &LookAt);
        ps2x_capture::g_overlayHook.store(&Overlay);
        // RT_RAMDUMP=<file>@<seconds>: write guest RAM to <file> that many seconds after start.
        if (const char *d = std::getenv("RT_RAMDUMP"); d && std::strchr(d, '@'))
        {
            const std::string spec = d;
            const std::string file = spec.substr(0, spec.find('@'));
            const double secs = std::atof(spec.substr(spec.find('@') + 1).c_str());
            PS2Runtime *rt = &runtime;
            std::thread([rt, file, secs] {
                std::this_thread::sleep_for(std::chrono::duration<double>(secs));
                if (std::FILE *f = std::fopen(file.c_str(), "wb"))
                {
                    std::fwrite(rt->memory().getRDRAM(), 1, PS2_RAM_SIZE, f);
                    std::fclose(f);
                    std::fprintf(stderr, "[debug] RAM dumped to %s\n", file.c_str());
                }
            }).detach();
        }
        if (const char *t = std::getenv("RT_DEBUGTEST"); t && *t)
        {
            g_testLevel = std::atoi(t);
            ps2x_capture::g_padHook.store(&TestPad);
        }
        std::fprintf(stderr, "[debug] F1 debug menu; F2 no clip, F3 free camera, F4 moon jump, F5 invincible\n");
    }
}
