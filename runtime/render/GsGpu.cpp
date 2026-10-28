#include <atomic>
// GPU renderer for the PS2 GS command stream. See GsGpu.h.
#include "GsGpu.h"
#include "GsMemory.h"
#include "gl_api.h"
#include "raylib.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace gl
{
    PFNGLTEXTUREBARRIERPROC TextureBarrier = nullptr;   // optional (GL 4.5 / NV_texture_barrier)
    PFNGLCLIPCONTROLPROC ClipControl = nullptr;         // optional (GL 4.5 / ARB_clip_control)
    PFNGLTEXBUFFERPROC TexBuffer = nullptr;
    PFNGLVERTEXATTRIBIPOINTERPROC VertexAttribIPointer = nullptr;
    PFNGLVERTEXATTRIBDIVISORPROC VertexAttribDivisor = nullptr;
    PFNGLMULTIDRAWELEMENTSINDIRECTPROC MultiDrawElementsIndirect = nullptr;
    PFNGLDRAWARRAYSINSTANCEDBASEINSTANCEPROC DrawArraysInstancedBaseInstance = nullptr;
#define RT_GL_DEF(type, name) type name = nullptr;
    RT_GL_FUNCS(RT_GL_DEF)
#undef RT_GL_DEF

    bool load(GetProc getProc)
    {
        bool ok = true;
#define RT_GL_LOAD(type, name)                                                        \
    name = reinterpret_cast<type>(getProc("gl" #name));                               \
    if (!name)                                                                        \
    {                                                                                 \
        std::fprintf(stderr, "[gsgpu] missing GL function gl%s\n", #name);            \
        ok = false;                                                                   \
    }
        RT_GL_FUNCS(RT_GL_LOAD)
#undef RT_GL_LOAD
        TextureBarrier = reinterpret_cast<PFNGLTEXTUREBARRIERPROC>(getProc("glTextureBarrier"));
        if (!TextureBarrier)
            TextureBarrier = reinterpret_cast<PFNGLTEXTUREBARRIERPROC>(getProc("glTextureBarrierNV"));
        ClipControl = reinterpret_cast<PFNGLCLIPCONTROLPROC>(getProc("glClipControl"));
        TexBuffer = reinterpret_cast<PFNGLTEXBUFFERPROC>(getProc("glTexBuffer"));
        VertexAttribIPointer = reinterpret_cast<PFNGLVERTEXATTRIBIPOINTERPROC>(getProc("glVertexAttribIPointer"));
        VertexAttribDivisor = reinterpret_cast<PFNGLVERTEXATTRIBDIVISORPROC>(getProc("glVertexAttribDivisor"));
        MultiDrawElementsIndirect = reinterpret_cast<PFNGLMULTIDRAWELEMENTSINDIRECTPROC>(getProc("glMultiDrawElementsIndirect"));
        DrawArraysInstancedBaseInstance = reinterpret_cast<PFNGLDRAWARRAYSINSTANCEDBASEINSTANCEPROC>(getProc("glDrawArraysInstancedBaseInstance"));
        return ok;
    }
}

namespace gsgpu
{
    namespace
    {
        std::atomic<int64_t> g_raceHudMs{INT64_MIN / 2};
        int64_t NowMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }
        std::atomic<bool> g_edgeHud{false};
        bool EdgeHudActive()
        {
            return g_edgeHud.load(std::memory_order_relaxed) && NowMs() - g_raceHudMs.load(std::memory_order_relaxed) < 500;
        }

        // Widescreen HUD layout for a 512x224 overlay shown at 16:9: every part keeps its 4:3
        // proportions (x scaled by 3/4) and is anchored to the left edge, the centre or the right
        // edge of its player's view by where it sits; the strips joining them stretch across the
        // gaps. Each row is a list of anchored spans {from, to, anchor} in view coordinates.
        // Layouts follow DATA/HUD/ARCADE_1PLAYER, _2PLAYERH (views 512x112 one above the other)
        // and _2PLAYERV (views 256x224 side by side; player 2's HUD is drawn 13 pixels left of
        // where player 1's sits in its view, so spans are looked up 13 pixels further right).
        struct Span { float lo, hi; int anchor; };   // anchor: 0 left, 1 centre, 2 right
        struct Row { const Span *spans; size_t n; };
        struct Layout { float viewW, viewH, rowSplit; Row top, bottom; float shift2; };
        constexpr Span k1Top[] = {{0, 160, 0}, {164, 367, 1}, {370, 512, 2}};
        constexpr Span k1Bottom[] = {{0, 262, 0}, {312, 512, 2}};
        constexpr Span kHTop[] = {{0, 200, 0}, {430, 512, 2}};
        constexpr Span kHBottom[] = {{0, 160, 0}, {380, 512, 2}};
        constexpr Span kVTop[] = {{0, 164, 0}, {175, 256, 2}};
        constexpr Span kVBottom[] = {{0, 100, 0}, {130, 256, 2}};
        constexpr Layout kLayouts[] = {
            {512, 224, 112, {k1Top, 3}, {k1Bottom, 2}, 0},
            {512, 112, 45, {kHTop, 2}, {kHBottom, 2}, 0},
            {256, 224, 112, {kVTop, 2}, {kVBottom, 2}, 13},
        };
        std::atomic<int> g_hudLayout{0};

        float Anchored(float x, float w, int anchor)
        {
            return anchor == 0 ? 0.75f * x : anchor == 1 ? 0.5f * w + 0.75f * (x - 0.5f * w) : w - 0.75f * (w - x);
        }
        // x is in view coordinates; the spans are looked up at x + shift.
        float MapRow(const Row &row, float w, float x, float shift)
        {
            const float sx = x + shift;
            if (x <= 0.0f || x >= w)
                return x;
            for (size_t k = 0; k < row.n; ++k)
            {
                const Span &s = row.spans[k];
                if (sx <= s.hi)
                {
                    if (sx >= s.lo || k == 0)
                        return Anchored(x, w, s.anchor);
                    const Span &p = row.spans[k - 1];
                    const float a = Anchored(p.hi - shift, w, p.anchor), b = Anchored(s.lo - shift, w, s.anchor);
                    return a + (b - a) * (sx - p.hi) / (s.lo - p.hi);
                }
            }
            return Anchored(x, w, row.spans[row.n - 1].anchor);
        }
        // Maps x of a point at (x, y), or of an element centred there; the view is the one the
        // point lies in (x outside the screen is left alone).
        float EdgeHudX(const Layout &L, float x, float y)
        {
            if (x <= 0.0f || x >= 512.0f)
                return x;
            const float ox = x >= L.viewW ? L.viewW : 0.0f, oy = y >= L.viewH ? L.viewH : 0.0f;
            return ox + MapRow(y - oy < L.rowSplit ? L.top : L.bottom, L.viewW, x - ox, ox > 0.0f ? L.shift2 : 0.0f);
        }

        // Panels map vertex by vertex, so shared edges stay joined. Textured draws (text, icons)
        // are split into runs of touching triangles on one line, a word or a strip; a run
        // narrower than a quarter of its view moves as a whole, so text keeps its shape.
        void EdgeHudMap(Vertex *v, uint32_t n, bool textured)
        {
            const Layout &L = kLayouts[std::clamp(g_hudLayout.load(std::memory_order_relaxed), 0, 2)];
            auto perVertex = [&](uint32_t a, uint32_t b) {
                for (uint32_t j = a; j < b; ++j)
                    v[j].x = EdgeHudX(L, v[j].x, v[j].y);
            };
            if (!textured)
                return perVertex(0, n);
            uint32_t start = 0;
            float rx0 = 0, rx1 = 0, ry0 = 0, ry1 = 0;
            auto flush = [&](uint32_t end) {
                if (end <= start)
                    return;
                if (rx1 - rx0 < 0.25f * L.viewW)
                {
                    const float cx = 0.5f * (rx0 + rx1), nx = EdgeHudX(L, cx, 0.5f * (ry0 + ry1));
                    for (uint32_t j = start; j < end; ++j)
                        v[j].x = nx + 0.75f * (v[j].x - cx);
                }
                else
                    perVertex(start, end);
            };
            for (uint32_t k = 0; k + 3 <= n; k += 3)
            {
                const float x0 = std::min({v[k].x, v[k + 1].x, v[k + 2].x}), x1 = std::max({v[k].x, v[k + 1].x, v[k + 2].x});
                const float y0 = std::min({v[k].y, v[k + 1].y, v[k + 2].y}), y1 = std::max({v[k].y, v[k + 1].y, v[k + 2].y});
                const bool joins = k > start && x0 <= rx1 + 4.0f && x1 >= rx0 - 4.0f && y0 <= ry1 + 2.0f && y1 >= ry0 - 2.0f;
                if (!joins)
                {
                    flush(k);
                    start = k, rx0 = x0, rx1 = x1, ry0 = y0, ry1 = y1;
                }
                else
                    rx0 = std::min(rx0, x0), rx1 = std::max(rx1, x1), ry0 = std::min(ry0, y0), ry1 = std::max(ry1, y1);
            }
            flush(n - n % 3);
        }
    }
    void SetHudLayout(int layout) { g_hudLayout.store(layout, std::memory_order_relaxed); }
    void NoteRaceHud() { g_raceHudMs.store(NowMs(), std::memory_order_relaxed); }
    void SetEdgeHud(bool on) { g_edgeHud.store(on, std::memory_order_relaxed); }

    // ------------------------------------------------------------------ Recorder
    // RT_RUNSTATS=1: why consecutive draws could not share one GPU draw (which part of the state changed).
    static uint64_t g_runSubmits = 0;   // stateIndex calls (draw submissions), for RT_RUNSTATS
    static void NoteRunBreak(const State &a, const State &b)
    {
        static const bool on = std::getenv("RT_RUNSTATS") != nullptr;
        if (!on)
            return;
        static uint64_t n = 0, why[9] = {};
        static const char *names[9] = {"prim", "texture", "blend", "test", "frame", "zbuf", "scissor", "fog", "other"};
        const bool tex = a.tme != b.tme || a.tbp0 != b.tbp0 || a.tbw != b.tbw || a.tpsm != b.tpsm || a.tw != b.tw || a.th != b.th || a.tcc != b.tcc || a.tfx != b.tfx ||
                         a.cbp != b.cbp || a.cpsm != b.cpsm || a.csm != b.csm || a.csa != b.csa || a.tex1 != b.tex1 || a.miptbp1 != b.miptbp1 ||
                         a.miptbp2 != b.miptbp2 || a.clamp != b.clamp || a.ta0 != b.ta0 || a.aem != b.aem || a.ta1 != b.ta1 || a.fst != b.fst;
        const bool d[9] = {a.type != b.type || a.iip != b.iip, tex, a.abe != b.abe || a.alpha != b.alpha || a.pabe != b.pabe, a.test != b.test || a.fba != b.fba,
                           a.fbp != b.fbp || a.fbw != b.fbw || a.fpsm != b.fpsm || a.fbmsk != b.fbmsk, a.zbp != b.zbp || a.zpsm != b.zpsm || a.zmsk != b.zmsk,
                           a.sx0 != b.sx0 || a.sx1 != b.sx1 || a.sy0 != b.sy0 || a.sy1 != b.sy1, a.fge != b.fge || a.fogr != b.fogr || a.fogg != b.fogg || a.fogb != b.fogb,
                           a.aa1 != b.aa1 || a.colclamp != b.colclamp || a.dthe != b.dthe};
        for (int k = 0; k < 9; ++k)
            why[k] += d[k];
        if ((++n & 0x3FFFF) == 0)
        {
            std::fprintf(stderr, "[runstats] %llu breaks in %llu submits:", (unsigned long long)n, (unsigned long long)g_runSubmits);
            for (int k = 0; k < 9; ++k)
                std::fprintf(stderr, " %s %.1f%%", names[k], 100.0 * why[k] / n);
            std::fprintf(stderr, "\n");
        }
    }

    uint32_t Recorder::stateIndex(const State &s)
    {
        ++g_runSubmits;
        if (!m_local.states.empty() && std::memcmp(&m_local.states.back(), &s, sizeof(State)) == 0)
            return static_cast<uint32_t>(m_local.states.size() - 1);
        if (m_lastDraw && !m_local.states.empty())
            NoteRunBreak(m_local.states.back(), s);
        m_local.states.push_back(s);
        return static_cast<uint32_t>(m_local.states.size() - 1);
    }

    void Recorder::throttle(std::unique_lock<std::mutex> &lock)
    {
        if (m_limit && m_batch.vertices.size() + m_batch.modelVertices >= m_limit)
        {
            ++m_waits;
            m_cv.wait_for(lock, std::chrono::milliseconds(250), [this] { return m_batch.vertices.size() + m_batch.modelVertices < m_limit; });
        }
    }

    void Recorder::maybePublish()
    {
        if (m_local.vertices.size() + m_local.modelVertices >= 12288 || m_local.data.size() >= (1u << 20) || m_local.cmds.size() >= 4096)
            publish();
    }

    void Recorder::publish()
    {
        if (m_local.cmds.empty())
            return;
        std::unique_lock<std::mutex> lock(m_mutex);
        throttle(lock);
        Batch &B = m_batch;
        const uint32_t vo = static_cast<uint32_t>(B.vertices.size()), so = static_cast<uint32_t>(B.states.size());
        const uint32_t dO = static_cast<uint32_t>(B.data.size()), to = static_cast<uint32_t>(B.tex0.size());
        const uint32_t fo = static_cast<uint32_t>(B.frames.size()), mo = static_cast<uint32_t>(B.models.size());
        if (B.cmds.empty())
            std::swap(B, m_local);
        else
        {
            B.cmds.reserve(B.cmds.size() + m_local.cmds.size());
            for (Cmd c : m_local.cmds)
            {
                switch (c.op)
                {
                case Op::Draw: c.a += vo; c.state += so; break;
                case Op::Clear: c.state += so; break;
                case Op::TransferData: c.a += dO; break;
                case Op::Tex0: c.a += to; break;
                case Op::Frame: c.a += fo; break;
                case Op::TransferBegin: break;
                case Op::Readback: break;
                case Op::Model: c.a += mo; c.state += so; break;
                }
                B.cmds.push_back(c);
            }
            for (ModelDraw md : m_local.models)
            {
                md.dataOff += dO;
                B.models.push_back(md);
            }
            B.modelVertices += m_local.modelVertices;
            B.states.insert(B.states.end(), m_local.states.begin(), m_local.states.end());
            B.vertices.insert(B.vertices.end(), m_local.vertices.begin(), m_local.vertices.end());
            B.data.insert(B.data.end(), m_local.data.begin(), m_local.data.end());
            B.tex0.insert(B.tex0.end(), m_local.tex0.begin(), m_local.tex0.end());
            B.frames.insert(B.frames.end(), m_local.frames.begin(), m_local.frames.end());
        }
        lock.unlock();
        m_local.clear();
        m_lastDraw = false;
    }

    void Recorder::triangle(const State &s, const Vertex v[3])
    {
        const uint32_t si = stateIndex(s);
        if (!(m_lastDraw && !m_local.cmds.empty() && m_local.cmds.back().op == Op::Draw && m_local.cmds.back().state == si))
        {
            Cmd c{};
            c.op = Op::Draw;
            c.a = static_cast<uint32_t>(m_local.vertices.size());
            c.b = 0;
            c.state = si;
            m_local.cmds.push_back(c);
        }
        m_local.vertices.insert(m_local.vertices.end(), v, v + 3);
        m_local.cmds.back().b += 3;
        m_lastDraw = true;
        maybePublish();
    }

    void Recorder::triangles(const State &s, const Vertex *v, uint32_t n)
    {
        if (n == 0)
            return;
        const uint32_t si = stateIndex(s);
        if (!(m_lastDraw && !m_local.cmds.empty() && m_local.cmds.back().op == Op::Draw && m_local.cmds.back().state == si))
        {
            Cmd c{};
            c.op = Op::Draw;
            c.a = static_cast<uint32_t>(m_local.vertices.size());
            c.b = 0;
            c.state = si;
            m_local.cmds.push_back(c);
        }
        m_local.vertices.insert(m_local.vertices.end(), v, v + n);
        m_local.cmds.back().b += n;
        m_lastDraw = true;
        maybePublish();
    }

    void Recorder::model(const State &s, const ModelDraw &m, const uint8_t *mesh)
    {
        Cmd c{};
        c.op = Op::Model;
        c.a = static_cast<uint32_t>(m_local.models.size());
        c.state = stateIndex(s);
        m_local.cmds.push_back(c);
        ModelDraw md = m;
        md.dataOff = static_cast<uint32_t>(m_local.data.size());
        if (mesh && md.dataBytes)
            m_local.data.insert(m_local.data.end(), mesh, mesh + md.dataBytes);
        else
            md.dataBytes = 0;
        m_local.models.push_back(md);
        m_local.modelVertices += md.n;
        m_lastDraw = false;
        maybePublish();
    }

    void Recorder::sprite(const State &s, const Vertex &v0, const Vertex &v1)
    {
        Vertex c[4];
        for (int k = 0; k < 4; ++k)
            c[k] = v1;
        c[0].x = v0.x; c[0].y = v0.y; c[0].s = v0.s; c[0].t = v0.t; c[0].u = v0.u; c[0].v = v0.v;
        c[1].y = v0.y; c[1].t = v0.t; c[1].v = v0.v;
        c[2].x = v0.x; c[2].s = v0.s; c[2].u = v0.u;
        const Vertex t0[3] = {c[0], c[1], c[2]};
        const Vertex t1[3] = {c[1], c[3], c[2]};
        triangle(s, t0);
        triangle(s, t1);
    }

    void Recorder::transferBegin(uint32_t dbp, uint32_t dbw, uint32_t dpsm, uint32_t dsax, uint32_t dsay, uint32_t rrw, uint32_t rrh)
    {
        Cmd c{};
        c.op = Op::TransferBegin;
        const uint32_t x[7] = {dbp, dbw, dpsm, dsax, dsay, rrw, rrh};
        std::memcpy(c.x, x, sizeof(x));
        m_local.cmds.push_back(c);
        m_lastDraw = false;
        maybePublish();
    }

    void Recorder::transferData(const uint8_t *p, uint32_t n)
    {
        Cmd c{};
        c.op = Op::TransferData;
        c.a = static_cast<uint32_t>(m_local.data.size());
        c.b = n;
        m_local.data.insert(m_local.data.end(), p, p + n);
        m_local.cmds.push_back(c);
        m_lastDraw = false;
        maybePublish();
    }

    void Recorder::tex0(const Tex0Event &e)
    {
        if (e.cld == 0)
            return;
        Cmd c{};
        c.op = Op::Tex0;
        c.a = static_cast<uint32_t>(m_local.tex0.size());
        m_local.tex0.push_back(e);
        m_local.cmds.push_back(c);
        m_lastDraw = false;
        maybePublish();
    }

    void Recorder::clear(const State &s, uint32_t rgba)
    {
        Cmd c{};
        c.op = Op::Clear;
        c.state = stateIndex(s);
        c.x[0] = rgba;
        m_local.cmds.push_back(c);
        m_lastDraw = false;
        maybePublish();
    }

    void Recorder::readback(uint32_t sbp, uint32_t sbw, uint32_t spsm, uint32_t ssax, uint32_t ssay, uint32_t rrw, uint32_t rrh)
    {
        Cmd c{};
        c.op = Op::Readback;
        const uint32_t x[7] = {sbp, sbw, spsm, ssax, ssay, rrw, rrh};
        std::memcpy(c.x, x, sizeof(x));
        m_local.cmds.push_back(c);
        m_lastDraw = false;
        maybePublish();
    }

    namespace
    {
        struct ReadbackSlot { uint32_t sbp, sbw, spsm, rrw, rrh; std::vector<uint8_t> bytes; };
        std::mutex g_readbackMu;
        std::vector<ReadbackSlot> g_readbacks;
    }

    bool ReadbackResult(uint32_t sbp, uint32_t sbw, uint32_t spsm, uint32_t rrw, uint32_t rrh, std::vector<uint8_t> &out)
    {
        std::lock_guard<std::mutex> lock(g_readbackMu);
        for (const ReadbackSlot &r : g_readbacks)
            if (r.sbp == sbp && r.sbw == sbw && r.spsm == spsm && r.rrw == rrw && r.rrh == rrh)
            {
                out = r.bytes;
                return true;
            }
        return false;
    }

    void Recorder::frame(const DisplayRegs &regs)
    {
        Cmd c{};
        c.op = Op::Frame;
        c.a = static_cast<uint32_t>(m_local.frames.size());
        m_local.frames.push_back(regs);
        m_local.cmds.push_back(c);
        m_lastDraw = false;
        publish();
    }

    void Recorder::take(Batch &out)
    {
        out.clear();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            std::swap(out, m_batch);
        }
        m_cv.notify_all();
    }

    namespace
    {
        std::atomic<uint32_t> g_meshGeneration{0};
    }
    uint32_t MeshGeneration() { return g_meshGeneration.load(std::memory_order_relaxed); }

    // ------------------------------------------------------------------ Renderer
    namespace
    {
        const char *kDrawVs = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aCol;
layout(location = 2) in vec3 aStq;
layout(location = 3) in vec2 aUv;
layout(location = 4) in float aFog;
uniform vec2 uRtSize;
uniform int uZInv;   // the depth buffer is currently stored inverted (game XORed its Z bytes)
uniform int uZ01;    // clip-space depth is [0,1] (glClipControl): Z goes to the depth buffer unrounded
uniform vec2 uHud;   // 2D overlay squash for widescreen: centre x, scale (1 = off)
out vec4 vCol;
noperspective out vec3 vStq;
noperspective out vec2 vUv;
out float vFog;
void main()
{
    float z = uZInv == 1 ? 1.0 - aPos.z : aPos.z;
    vec2 p = vec2(uHud.x + (aPos.x - uHud.x) * uHud.y, aPos.y);
    gl_Position = vec4(p / uRtSize * 2.0 - 1.0, uZ01 == 1 ? z : z * 2.0 - 1.0, 1.0);
    vCol = aCol;
    vStq = aStq;
    vUv = aUv;
    vFog = aFog;
}
)";

        // Model batches (ModelDraw): the VU1 drawing programs' transform done here. The program's
        // input vertex (position, integer colour, STQ, normal) goes through the screen matrix to
        // the values the program would have kicked (FTOI4 x/y/z of p/w, STQ/w, colour lit as the
        // program lights it) and then as the GS vertex of kDrawVs. gl_Position keeps w so the GPU
        // clips, with the program's own clip test (|c| <= w in its clip space) as clip distances.
        const char *kModelVs = R"(#version 330 core
layout(location = 0) in vec4 aPos;
layout(location = 1) in vec4 aCol;
layout(location = 2) in vec4 aStq;
layout(location = 3) in vec4 aNrm;
uniform vec2 uRtSize;
uniform int uZInv;
uniform int uZ01;
uniform vec2 uHud;
#ifdef MDI
// Batched: the parameters below come per draw from uMd, 20 texels from aDraw * 20 (see kMdTexels).
uniform samplerBuffer uMd;
layout(location = 4) in int aDraw;
#else
uniform mat4 uM;     // object -> GS screen, homogeneous
uniform mat4 uCm;    // object -> VU clip space
uniform mat4 uL;     // light directions (columns 0..2)
uniform vec4 uAmb;
uniform mat4 uLc;    // light colours (columns)
uniform vec4 uOfs;   // XYOFFSET x, y (pixels), FOG (XYZ2), depth clamp
uniform ivec4 uMf;   // 1 lit / 2 env-mapped, XYZF2, clip planes (mask: +x -x +y -y +z -z), Z format (0 Z32, 1 Z24, else Z16)
#endif
#ifdef FLAT
flat out vec4 vCol;
#else
out vec4 vColP;
#endif
out vec3 vStqP;
noperspective out vec2 vUv;
out float vFogP;
out float gl_ClipDistance[6];
void main()
{
#ifdef MDI
    int b = aDraw * 20;
    mat4 uM = mat4(texelFetch(uMd, b), texelFetch(uMd, b + 1), texelFetch(uMd, b + 2), texelFetch(uMd, b + 3));
    mat4 uCm = mat4(texelFetch(uMd, b + 4), texelFetch(uMd, b + 5), texelFetch(uMd, b + 6), texelFetch(uMd, b + 7));
    mat4 uL = mat4(texelFetch(uMd, b + 8), texelFetch(uMd, b + 9), texelFetch(uMd, b + 10), texelFetch(uMd, b + 11));
    vec4 uAmb = texelFetch(uMd, b + 12);
    mat4 uLc = mat4(texelFetch(uMd, b + 13), texelFetch(uMd, b + 14), texelFetch(uMd, b + 15), texelFetch(uMd, b + 16));
    vec4 uOfs = texelFetch(uMd, b + 17);
    ivec4 uMf = ivec4(texelFetch(uMd, b + 18));
#endif
    vec4 P = vec4(aPos.xyz, 1.0);   // the programs use w = 1
    vec4 p = uM * P;
    float q = 1.0 / p.w;
    vec3 s = p.xyz * q;
    vec2 xy = trunc(s.xy * 16.0) / 16.0 - uOfs.xy;
    float zi = uMf.y == 1 ? trunc(trunc(s.z * 16.0) / 16.0) : trunc(s.z * 16.0);
    float z;
    if (uMf.w == 0) z = zi / 4294967296.0;
    else if (uMf.w == 1) z = (zi - 16777216.0 * floor(zi / 16777216.0)) / 16777216.0;
    else z = (zi - 65536.0 * floor(zi / 65536.0)) / 65536.0;
    if (uOfs.w != 0.0) z = clamp(z, 0.0, 1.0);
    z = uZInv == 1 ? 1.0 - z : z;
    vec2 ndc = xy / uRtSize * 2.0 - 1.0;
    gl_Position = vec4(ndc, uZ01 == 1 ? z : z * 2.0 - 1.0, 1.0) * p.w;
    vec4 c = uCm * P;
    gl_ClipDistance[0] = (uMf.z & 1) != 0 ? c.w - c.x : 1.0;
    gl_ClipDistance[1] = (uMf.z & 2) != 0 ? c.w + c.x : 1.0;
    gl_ClipDistance[2] = (uMf.z & 4) != 0 ? c.w - c.y : 1.0;
    gl_ClipDistance[3] = (uMf.z & 8) != 0 ? c.w + c.y : 1.0;
    gl_ClipDistance[4] = (uMf.z & 16) != 0 ? c.w - c.z : 1.0;
    gl_ClipDistance[5] = (uMf.z & 32) != 0 ? c.w + c.z : 1.0;
    vec4 col = aCol;
    if (uMf.x == 1)
    {
        vec4 l = max(uL * vec4(aNrm.xyz, 0.0), 0.0);
        vec3 light = min(uAmb.rgb + (uLc * l).rgb, 1.0);
        col.rgb = trunc(col.rgb * light);
    }
    // The GS interpolates colour, STQ/w and fog linearly on screen. Sent multiplied by w as
    // perspective-correct varyings and multiplied by gl_FragCoord.w (1/w on screen) in the
    // fragment shader, they come out the same, and stay right where the GPU clips a triangle
    // that crosses the eye plane.
    col = col - 256.0 * floor(col / 256.0);
#ifdef FLAT
    vCol = col;
#else
    vColP = col * p.w;
#endif
    vec3 stq = aStq.xyz;
    if (uMf.x == 2)
    {
        // Environment map (program 1159117218): S, T from the normal through the rows in uL,
        // offset and scale from uAmb (its vf08).
        vec2 a = (uL * vec4(aNrm.xyz, 0.0)).xy + 1.0;
        stq.xy = (a + uAmb.xy) * uAmb.z;
    }
    vStqP = stq;
    vUv = vec2(0.0);
    vFogP = (uMf.y == 1 ? mod(trunc(p.w * q), 256.0) : uOfs.z) * p.w;
}
)";

        const char *kDrawFs = R"(#version 330 core
in vec4 vCol;
noperspective in vec3 vStq;
noperspective in vec2 vUv;
in float vFog;
layout(location = 0, index = 0) out vec4 oColor;
layout(location = 0, index = 1) out vec4 oBlend;
uniform sampler2D uTex;
uniform ivec4 uTex0;      // tme, fst, tfx, tcc
uniform ivec4 uMode;      // fge, fba, blendC (0 As, 2 FIX), pass (0 single, 1 pass-only, 2 fail-only)
uniform ivec4 uAtest;     // ate, atst, aref, -
uniform ivec4 uWrap;      // wrapU, wrapV (0 repeat, 1 clamp, 2 region clamp, 3 region repeat), lcm, mxl
uniform vec4 uRegion;     // minu, maxu, minv, maxv (texels)
uniform vec4 uTexGeom;    // TW, TH, 1/textureWidthTexels, 1/textureHeightTexels
uniform vec4 uLod;        // K, 2^L, replacement lod bias, -
uniform vec4 uFogFix;     // fog rgb (0..255), FIX
uniform vec4 uRegionRep;  // region repeat: umsk, ufix, vmsk, vfix
uniform ivec4 uTexA;      // render-target source: mode (0 as is, 1 16-bit, 2 CT24), ta0, ta1, aem
uniform sampler2D uDst;   // the render target itself (manual blending)
uniform ivec4 uBlend;     // manual blend: 0 off (fixed function), 1 wrap (COLCLAMP=0), 2 clamp; A, B, D
uniform ivec4 uBlend2;    // C (0 As, 1 Ad, 2 FIX), destination is 16-bit

float wrapc(float c, int mode, float size, float lo, float hi, float msk, float fix)
{
    if (mode == 1) return clamp(c, 0.5, size - 0.5);
    if (mode == 2) return clamp(c, lo + 0.5, hi + 0.5);
    if (mode == 3) return float((int(floor(c)) & int(msk)) | int(fix)) + fract(c);
    return c;
}

bool atest(float a)
{
    if (uAtest.x == 0) return true;
    float r = float(uAtest.z);
    int m = uAtest.y;
    if (m == 0) return false;
    if (m == 1) return true;
    if (m == 2) return a < r;
    if (m == 3) return a <= r;
    if (m == 4) return a == r;
    if (m == 5) return a >= r;
    if (m == 6) return a > r;
    return a != r;
}

void main()
{
    vec4 c = floor(vCol + 0.5);
    if (uTex0.x == 1)
    {
        vec2 st;
        float q = abs(vStq.z);
        if (uTex0.y == 1) st = vUv;
        else st = (vStq.xy / max(vStq.z, 1e-30)) * uTexGeom.xy;
        st.x = wrapc(st.x, uWrap.x, uTexGeom.x, uRegion.x, uRegion.y, uRegionRep.x, uRegionRep.y);
        st.y = wrapc(st.y, uWrap.y, uTexGeom.y, uRegion.z, uRegion.w, uRegionRep.z, uRegionRep.w);
        float lod = (uWrap.z == 1) ? uLod.x : log2(1.0 / max(q, 1e-30)) * uLod.y + uLod.x;
        lod = min(lod + uLod.z, float(uWrap.w));   // uLod.z: log2 of a replacement texture's scale
        vec4 t = floor(textureLod(uTex, st * uTexGeom.zw, lod) * 255.0 + 0.5);
        if (uTexA.x == 1)
        {
            bool hi = t.a >= 128.0;
            t.rgb = floor(t.rgb / 8.0) * 8.0;
            t.a = (uTexA.w == 1 && t.r + t.g + t.b == 0.0) ? 0.0 : float(hi ? uTexA.z : uTexA.y);
        }
        else if (uTexA.x == 2)
            t.a = (uTexA.w == 1 && t.r + t.g + t.b == 0.0) ? 0.0 : float(uTexA.y);
        int tfx = uTex0.z;
        bool tcc = uTex0.w == 1;
        if (tfx == 0)
        {
            c.rgb = min(floor(t.rgb * c.rgb / 128.0), 255.0);
            c.a = tcc ? min(floor(t.a * c.a / 128.0), 255.0) : c.a;
        }
        else if (tfx == 1)
        {
            c.rgb = t.rgb;
            c.a = tcc ? t.a : c.a;
        }
        else
        {
            c.rgb = min(floor(t.rgb * c.rgb / 128.0) + c.a, 255.0);
            c.a = (tfx == 2) ? (tcc ? min(t.a + c.a, 255.0) : c.a) : (tcc ? t.a : c.a);
        }
    }
    if (uMode.x == 1)
        c.rgb = floor((vFog * c.rgb + (255.0 - vFog) * uFogFix.rgb) / 256.0);
    bool pass = atest(c.a);
    if (uMode.w == 0 && !pass) discard;
    if (uMode.w == 1 && !pass) discard;
    if (uMode.w == 2 && pass) discard;
    float a = c.a;
    if (uMode.y == 1 && a < 128.0) a += 128.0;
    oColor = vec4(c.rgb, a) / 255.0;
    float f = (uMode.z == 2) ? uFogFix.a : c.a;
    oBlend = vec4(min(f / 128.0, 1.0));
    if (uBlend.x != 0)
    {
        // GS blend in integer steps: ((A - B) * C >> 7) + D, then wrap or clamp to 8 bits.
        vec4 d = floor(texelFetch(uDst, ivec2(gl_FragCoord.xy), 0) * 255.0 + 0.5);
        if (uBlend2.y == 1) d.rgb = floor(d.rgb / 8.0) * 8.0;
        vec3 sa = uBlend.y == 0 ? c.rgb : (uBlend.y == 1 ? d.rgb : vec3(0.0));
        vec3 sb = uBlend.z == 0 ? c.rgb : (uBlend.z == 1 ? d.rgb : vec3(0.0));
        vec3 sd = uBlend.w == 0 ? c.rgb : (uBlend.w == 1 ? d.rgb : vec3(0.0));
        float cf = uBlend2.x == 0 ? c.a : (uBlend2.x == 1 ? d.a : uFogFix.a);
        vec3 r = floor((sa - sb) * cf / 128.0) + sd;
        r = uBlend.x == 1 ? r - 256.0 * floor(r / 256.0) : clamp(r, 0.0, 255.0);
        oColor = vec4(r, a) / 255.0;
        oBlend = vec4(1.0);
    }
}
)";

        const char *kBlitVs = R"(#version 330 core
layout(location = 0) in vec2 aPos;
out vec2 vUv;
uniform vec4 uSrc;   // u0, v0, u1, v1
void main()
{
    vec2 p = aPos * 0.5 + 0.5;
    vUv = mix(uSrc.xy, uSrc.zw, p);
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

        const char *kBlitFs = R"(#version 330 core
in vec2 vUv;
out vec4 oColor;
uniform sampler2D uTex;
uniform int uOpaque;
void main()
{
    vec4 c = texture(uTex, vUv);
    oColor = uOpaque == 1 ? vec4(c.rgb, 1.0) : c;
}
)";

        const char *kDeintFs = R"(#version 330 core
in vec2 vUv;
out vec4 oColor;
uniform sampler2D uCur;
uniform sampler2D uPrev;
uniform int uMode;
uniform int uField;
uniform float uLines;   // source lines in the picture
void main()
{
    vec2 uv = vUv;
    if (uMode == 1)
        oColor = vec4(mix(texture(uCur, uv).rgb, texture(uPrev, uv).rgb, 0.5), 1.0);
    else if (uMode == 2 || uMode == 3)
    {
        // A CRT draws the second field of a frame half a line lower.
        bool lower = (uField == 1) == (uMode == 2);
        uv.y += (lower ? -0.25 : 0.25) / uLines;
        oColor = vec4(texture(uCur, uv).rgb, 1.0);
    }
    else if (uMode == 4 || uMode == 5)
    {
        // Each source line becomes two: the top field's, then the bottom field's.
        bool firstHalf = mod(floor(uv.y * uLines * 2.0), 2.0) < 1.0;
        bool curIsTop = (uField == 0) == (uMode == 4);
        oColor = vec4((firstHalf == curIsTop ? texture(uCur, uv) : texture(uPrev, uv)).rgb, 1.0);
    }
    else
        oColor = vec4(texture(uCur, uv).rgb, 1.0);
}
)";

        GLuint compile(GLenum type, const char *src)
        {
            GLuint s = gl::CreateShader(type);
            gl::ShaderSource(s, 1, &src, nullptr);
            gl::CompileShader(s);
            GLint ok = 0;
            gl::GetShaderiv(s, GL_COMPILE_STATUS, &ok);
            if (!ok)
            {
                char log[4096];
                gl::GetShaderInfoLog(s, sizeof(log), nullptr, log);
                std::fprintf(stderr, "[gsgpu] shader compile failed:\n%s\n", log);
            }
            return s;
        }

        GLuint link(const char *vs, const char *fs, bool dualSource)
        {
            GLuint p = gl::CreateProgram();
            gl::AttachShader(p, compile(GL_VERTEX_SHADER, vs));
            gl::AttachShader(p, compile(GL_FRAGMENT_SHADER, fs));
            if (dualSource)
            {
                gl::BindFragDataLocationIndexed(p, 0, 0, "oColor");
                gl::BindFragDataLocationIndexed(p, 0, 1, "oBlend");
            }
            gl::LinkProgram(p);
            GLint ok = 0;
            gl::GetProgramiv(p, GL_LINK_STATUS, &ok);
            if (!ok)
            {
                char log[4096];
                gl::GetProgramInfoLog(p, sizeof(log), nullptr, log);
                std::fprintf(stderr, "[gsgpu] program link failed:\n%s\n", log);
            }
            return p;
        }

        uint64_t hashBytes(const uint8_t *p, size_t n, uint64_t h = 0x9E3779B97F4A7C15ull)
        {
            size_t k = 0;
            for (; k + 8 <= n; k += 8)
            {
                uint64_t v;
                std::memcpy(&v, p + k, 8);
                h = (h ^ v) * 0x100000001B3ull;
                h ^= h >> 29;
            }
            for (; k < n; ++k)
                h = (h ^ p[k]) * 0x100000001B3ull;
            return h;
        }

        inline uint32_t bits(uint64_t v, int lo, int n) { return static_cast<uint32_t>((v >> lo) & ((1ull << n) - 1)); }
    }

    struct Renderer::Impl
    {
        uint32_t scale = 1;
        std::vector<uint8_t> vram;
        gsmem::Transfer xfer;
        uint64_t seq = 1;

        // CLUT buffer (GS internal), as 32-bit entries.
        uint32_t clut[256] = {};
        bool clutPending = false;
        Tex0Event clutEv{};
        uint64_t clutHash = 0;
        uint32_t cbp0 = ~0u, cbp1 = ~0u;

        // Colour formats a target can hold: 0 CT32/CT24, 1 CT16, 2 CT16S. A target mirrors its
        // GS memory: VRAM uploads refresh it, and pages drawn on the GPU are marked dirty and
        // written back to the VRAM shadow when something reads that memory another way.
        static int formatClass(uint32_t psm)
        {
            switch (psm)
            {
            case gsmem::CT32: case gsmem::CT24: return 0;
            case gsmem::CT16: return 1;
            case gsmem::CT16S: return 2;
            default: return -1;
            }
        }
        static uint32_t classPsm(int cls) { return cls == 1 ? gsmem::CT16 : cls == 2 ? gsmem::CT16S : gsmem::CT32; }

        struct Target
        {
            uint16_t fbp; uint8_t fbw; uint8_t cls = 0;
            uint32_t w, h;          // GS pixels
            GLuint tex = 0;
            uint64_t lastWrite = 0, overwritten = 0;
            uint32_t maxY = 0;
            std::vector<uint8_t> dirty;   // per page: drawn on the GPU since the last write-back
            bool anyDirty = false;
            std::vector<uint8_t> stale;   // per page: GS memory written since the GPU copy was made
            bool anyStale = false;
            bool hudSeen = false;         // edge HUD: this frame's 2D overlay has started here
            uint32_t rowsPerPage() const { return cls ? 64u : 32u; }
            uint32_t pages() const { return (h / rowsPerPage()) * std::max<uint32_t>(1u, fbw); }
        };
        std::vector<Target> targets;
        struct Depth { uint16_t zbp; uint32_t w, h; GLuint tex = 0; bool inverted = false; };   // inverted: memory holds 0xFFFFFF - Z
        std::vector<Depth> depths;
        bool clip01 = false;   // glClipControl available: draws use a [0,1] clip depth
        std::unordered_map<uint64_t, GLuint> fbos;
        std::unordered_set<uint32_t> knownZ;   // ZBP values used by draws (colour draws there are Z writes)

        struct TexEntry
        {
            uint64_t key;
            uint64_t contentHash;
            uint32_t begin, end;
            GLuint tex = 0;
            bool valid = true;
            uint64_t lastUse = 0;
            uint32_t levels = 1;
            float hdBias = 0.0f;      // log2(replacement width / GS width); 0 = the game's own texture
            uint32_t hdLevels = 0;
            GLuint hdTex = 0;         // the replacement (texPackGl owns it), else 0
        };
        std::unordered_map<uint64_t, TexEntry> texCache;
        float lastHdBias = 0.0f;      // of the texture getTexture() returned last
        uint32_t lastHdLevels = 0;

        // Texture packs. Every decoded texture is named by a hash of its level-0 RGBA pixels (so
        // the same picture gets the same name wherever the game puts it in GS memory and whatever
        // palette produced it). RT_TEXDUMP=<dir> writes each one once as <name>.png;
        // RT_TEXPACK=<dir> (default ./textures) replaces it with <name>.png from there, at any
        // size (a power-of-two multiple of the original looks best), with its own mipmaps.
        std::string texDumpDir, texPackDir;
        std::unordered_set<uint64_t> texDumped, texPackIds;
        bool texPackInit = false;
        struct HdTex { GLuint tex; int w, h; uint32_t levels; };
        std::unordered_map<uint64_t, HdTex> texPackGl;
        uint64_t texReplaced = 0;
        void initTexPacks()
        {
            texPackInit = true;
            if (const char *d = std::getenv("RT_TEXDUMP"); d && *d)
            {
                texDumpDir = d;
                std::error_code ec;
                std::filesystem::create_directories(texDumpDir, ec);
            }
            const char *p = std::getenv("RT_TEXPACK");
            texPackDir = p && *p ? p : "textures";
            std::error_code ec;
            if (!std::filesystem::is_directory(texPackDir, ec))
                return;
            for (const auto &f : std::filesystem::directory_iterator(texPackDir, ec))
            {
                const std::string n = f.path().filename().string();
                if (n.size() == 20 && n.compare(16, 4, ".png") == 0)
                    texPackIds.insert(std::strtoull(n.substr(0, 16).c_str(), nullptr, 16));
            }
            std::fprintf(stderr, "[gsgpu] texture pack %s: %zu textures\n", texPackDir.c_str(), texPackIds.size());
        }
        static std::string texName(uint64_t id)
        {
            char b[24];
            std::snprintf(b, sizeof(b), "%016llx.png", static_cast<unsigned long long>(id));
            return b;
        }
        // Level 0 of the texture just decoded into scratch (w x h RGBA): dump it, or upload its
        // replacement. True when a replacement was uploaded (with its mipmaps).
        bool texPackLevel0(TexEntry &e, uint32_t w, uint32_t h)
        {
            if (!texPackInit)
                initTexPacks();
            if (texDumpDir.empty() && texPackIds.empty())
                return false;
            const uint64_t id = hashBytes(scratch.data(), static_cast<size_t>(w) * h * 4);
            if (!texDumpDir.empty() && texDumped.insert(id).second)
            {
                Image img{scratch.data(), static_cast<int>(w), static_cast<int>(h), 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
                ExportImage(img, (texDumpDir + "/" + texName(id)).c_str());
            }
            if (!texPackIds.count(id))
                return false;
            // One GL texture per replacement, shared by every cache entry that decodes to the same
            // picture (the game re-uploads some textures every frame).
            auto it = texPackGl.find(id);
            if (it == texPackGl.end())
            {
                Image img = LoadImage((texPackDir + "/" + texName(id)).c_str());
                if (!img.data || img.width <= 0 || img.height <= 0)
                {
                    texPackIds.erase(id);
                    return false;
                }
                ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
                HdTex t{};
                t.w = img.width;
                t.h = img.height;
                gl::GenTextures(1, &t.tex);
                gl::BindTexture(GL_TEXTURE_2D, t.tex);
                gl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, img.width, img.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, img.data);
                gl::GenerateMipmap(GL_TEXTURE_2D);
                t.levels = 1u + static_cast<uint32_t>(std::log2(static_cast<float>(std::max(img.width, img.height))));
                gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
                gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, static_cast<int>(t.levels) - 1);
                gl::BindTexture(GL_TEXTURE_2D, e.tex);
                UnloadImage(img);
                it = texPackGl.emplace(id, t).first;
            }
            e.hdTex = it->second.tex;
            e.hdBias = std::log2(static_cast<float>(it->second.w) / static_cast<float>(w));
            e.hdLevels = it->second.levels;
            ++texReplaced;
            return true;
        }

        GLuint drawProg = 0, blitProg = 0, vao = 0, vbo = 0, quadVao = 0, quadVbo = 0;
        GLuint displayTex = 0, displayFbo = 0, blitFbo = 0;
        GLuint fieldTex[2] = {}, fieldFbo[2] = {}, deintProg = 0;
        int fieldIdx = 0;
        GLint dCur = -1, dPrev = -1, dMode = -1, dField = -1, dLines = -1, dSrc = -1;
        uint32_t displayW = 0, displayH = 0;
        bool frameValid = false;   // displayTex holds the picture of the last Frame mark
        float hudSquash = 1.0f;
        std::vector<Vertex> hudVerts;
        struct Uniforms
        {
            GLint rtSize, zinv, z01, hud, tex, tex0, mode, atest, wrap, region, texGeom, lod, fogFix, regionRep, texA, dst, blend, blend2;
            GLint m, cm, l, amb, lc, ofs, mf;   // model programs only
        } u{}, um[2]{};
        void getUniforms(GLuint p, Uniforms &U)
        {
            U.rtSize = gl::GetUniformLocation(p, "uRtSize");
            U.zinv = gl::GetUniformLocation(p, "uZInv");
            U.z01 = gl::GetUniformLocation(p, "uZ01");
            U.hud = gl::GetUniformLocation(p, "uHud");
            U.tex = gl::GetUniformLocation(p, "uTex");
            U.tex0 = gl::GetUniformLocation(p, "uTex0");
            U.mode = gl::GetUniformLocation(p, "uMode");
            U.atest = gl::GetUniformLocation(p, "uAtest");
            U.wrap = gl::GetUniformLocation(p, "uWrap");
            U.region = gl::GetUniformLocation(p, "uRegion");
            U.texGeom = gl::GetUniformLocation(p, "uTexGeom");
            U.lod = gl::GetUniformLocation(p, "uLod");
            U.fogFix = gl::GetUniformLocation(p, "uFogFix");
            U.regionRep = gl::GetUniformLocation(p, "uRegionRep");
            U.texA = gl::GetUniformLocation(p, "uTexA");
            U.dst = gl::GetUniformLocation(p, "uDst");
            U.blend = gl::GetUniformLocation(p, "uBlend");
            U.blend2 = gl::GetUniformLocation(p, "uBlend2");
            U.m = gl::GetUniformLocation(p, "uM");
            U.cm = gl::GetUniformLocation(p, "uCm");
            U.l = gl::GetUniformLocation(p, "uL");
            U.amb = gl::GetUniformLocation(p, "uAmb");
            U.lc = gl::GetUniformLocation(p, "uLc");
            U.ofs = gl::GetUniformLocation(p, "uOfs");
            U.mf = gl::GetUniformLocation(p, "uMf");
        }
        // Model batches: smooth / flat programs, and the meshes in one buffer (bump allocated,
        // dropped whole when full: MeshGeneration tells the recording side to send them again).
        // Transient meshes (key bit 63: batches not from a cached packet, sent with every draw) go
        // to a ring at the end of the buffer instead, kTransientBytes per replay at most.
        GLuint modelProg[2] = {}, meshVao = 0, meshVbo = 0;
        static constexpr size_t kMeshBytes = 64u << 20, kTransientBytes = 8u << 20,
                                kCachedBytes = (kMeshBytes - kTransientBytes) / 192u * 192u;   // the ring starts on a vertex of every stride
        size_t meshTop = 0, transientTop = 0, transientUsed = 0;
        std::unordered_map<uint64_t, uint32_t> meshAt;
        uint64_t modelDraws = 0, modelMissing = 0;
        bool modelsOk = true;
        // Batched model draws (GL 4.3, RT_GPUMODELMDI=0 off): every model of a replayed batch puts
        // its parameters into a buffer texture (kMdTexels RGBA32F texels each); a run of model
        // commands in one GS state is one MultiDrawElementsIndirect, the draw index arriving as an
        // instanced attribute through baseInstance. Strips and fans are drawn as triangle lists
        // through fixed index patterns (mdPattern: list, strip, fan), so they batch with lists;
        // the last vertex of each triangle stays the provoking one.
        static constexpr int kMdTexels = 20;
        static constexpr uint32_t kMdMaxVerts = 1024;
        struct DrawIndirect { GLuint count, instances, firstIndex; GLint baseVertex; GLuint baseInstance; };
        GLuint mdElems = 0;
        uint32_t mdPattern[3] = {};
        bool mdi = false;
        GLuint mdProg[2] = {}, mdTbo = 0, mdTex = 0, mdIdxVbo = 0, mdIndirect = 0;
        Uniforms umd[2]{};
        GLint mdLoc[2] = {-1, -1};
        size_t mdIdxCap = 0, mdTboCap = 0;
        std::vector<float> mdData;
        std::vector<uint32_t> mdFirst;   // per model of the batch: first vertex in the mesh buffer, or ~0u
        std::vector<DrawIndirect> mdCmds;
        uint64_t mdRuns = 0, mdBatched = 0;
        // Mesh offsets are multiples of 192 bytes, so a mesh starts on a whole vertex for both strides.
        static size_t meshAlign(size_t x) { return (x + 191u) / 192u * 192u; }
        // The mesh of model md: in the buffer already, or uploaded now from the batch's data.
        // ~0u when it is not there and was not sent. reset: set when the buffer had to be dropped.
        uint32_t meshOffset(const Batch &b, const ModelDraw &md, bool &reset)
        {
            if (md.key >> 63)
            {
                if (md.dataBytes == 0 || transientUsed + md.dataBytes + 192u > kTransientBytes)
                    return ~0u;
                if (transientTop + md.dataBytes > kTransientBytes)
                {
                    transientUsed += kTransientBytes - transientTop;   // the skipped tail counts as used
                    transientTop = 0;
                    if (transientUsed + md.dataBytes + 192u > kTransientBytes)
                        return ~0u;
                }
                const uint32_t at = static_cast<uint32_t>(kCachedBytes + transientTop);
                gl::BindBuffer(GL_ARRAY_BUFFER, meshVbo);
                gl::BufferSubData(GL_ARRAY_BUFFER, static_cast<GLintptr>(at), md.dataBytes, b.data.data() + md.dataOff);
                transientTop = meshAlign(transientTop + md.dataBytes);
                transientUsed += md.dataBytes + 192u;
                return at;
            }
            auto it = meshAt.find(md.key);
            if (it != meshAt.end())
                return it->second;
            if (md.dataBytes == 0)
                return ~0u;
            if (meshTop + md.dataBytes > kCachedBytes)
            {
                meshAt.clear();
                meshTop = 0;
                g_meshGeneration.fetch_add(1, std::memory_order_relaxed);
                reset = true;
            }
            gl::BindBuffer(GL_ARRAY_BUFFER, meshVbo);
            gl::BufferSubData(GL_ARRAY_BUFFER, static_cast<GLintptr>(meshTop), md.dataBytes, b.data.data() + md.dataOff);
            const uint32_t at = static_cast<uint32_t>(meshTop);
            meshAt.emplace(md.key, at);
            meshTop = meshAlign(meshTop + md.dataBytes);
            return at;
        }
        GLint bSrc = -1, bTex = -1, bOpaque = -1;
        std::vector<uint8_t> scratch, readback;
        uint64_t m_syncs = 0, m_targetUploads = 0, m_readbacks = 0;

        // --- targets ------------------------------------------------------
        int findTarget(uint32_t fbp, uint32_t fbw, int cls = 0) const
        {
            for (size_t k = 0; k < targets.size(); ++k)
                if (targets[k].fbp == fbp && targets[k].fbw == fbw && targets[k].cls == cls)
                    return static_cast<int>(k);
            return -1;
        }

        void uploadTargetRect(Target &t, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h)
        {
            // Decode the target's memory into a temporary texture and draw it scaled into the
            // target. 16-bit pixels keep their alpha bit as 0x80.
            ++m_targetUploads;
            scratch.resize(static_cast<size_t>(w) * h * 4);
            const uint32_t psm = classPsm(t.cls);
            for (uint32_t y = 0; y < h; ++y)
                for (uint32_t x = 0; x < w; ++x)
                {
                    uint32_t v = gsmem::readPixel(vram.data(), psm, t.fbp * 32u, t.fbw, x0 + x, y0 + y);
                    if (t.cls)
                        v = ((v & 31u) << 3) | (((v >> 5) & 31u) << 11) | (((v >> 10) & 31u) << 19) | ((v & 0x8000u) ? 0x80000000u : 0u);
                    std::memcpy(&scratch[(static_cast<size_t>(y) * w + x) * 4], &v, 4);
                }
            GLuint tmp = 0;
            gl::GenTextures(1, &tmp);
            gl::BindTexture(GL_TEXTURE_2D, tmp);
            gl::PixelStorei(GL_UNPACK_ALIGNMENT, 1);
            gl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, scratch.data());
            gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            gl::BindFramebuffer(GL_FRAMEBUFFER, fboFor(&t - targets.data(), -1));
            gl::Viewport(x0 * scale, y0 * scale, w * scale, h * scale);
            gl::Disable(GL_SCISSOR_TEST);
            gl::Disable(GL_DEPTH_TEST);
            gl::Disable(GL_BLEND);
            gl::ColorMask(1, 1, 1, 1);
            gl::UseProgram(blitProg);
            gl::Uniform4f(bSrc, 0, 0, 1, 1);
            gl::Uniform1i(bOpaque, 0);
            gl::Uniform1i(bTex, 0);
            gl::ActiveTexture(GL_TEXTURE0);
            gl::BindTexture(GL_TEXTURE_2D, tmp);
            gl::BindVertexArray(quadVao);
            gl::DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            gl::DeleteTextures(1, &tmp);
        }

        // Re-upload the pages of a target whose GS memory was written since (runs of adjacent
        // pages in a page row go up as one rectangle).
        void refreshTarget(int k)
        {
            if (k < 0)
                return;
            Target &t = targets[k];
            if (!t.anyStale)
                return;
            const uint32_t fbw = std::max<uint32_t>(1u, t.fbw), rpp = t.rowsPerPage(), n = t.pages();
            for (uint32_t p = 0; p < n;)
            {
                if (!t.stale[p])
                {
                    ++p;
                    continue;
                }
                uint32_t q = p;
                while (q + 1 < n && t.stale[q + 1] && (q + 1) % fbw != 0)
                    ++q;
                uploadTargetRect(t, (p % fbw) * 64u, (p / fbw) * rpp, (q - p + 1) * 64u, rpp);
                for (uint32_t r = p; r <= q; ++r)
                    t.stale[r] = 0;
                p = q + 1;
            }
            t.anyStale = false;
        }

        int getTarget(uint32_t fbp, uint32_t fbw, int cls, uint32_t needH)
        {
            int k = findTarget(fbp, fbw, cls);
            const uint32_t h = std::max<uint32_t>(512u, (needH + 63u) & ~63u);
            if (k >= 0 && targets[k].h >= needH)
            {
                refreshTarget(k);
                return k;
            }
            Target t{};
            t.fbp = static_cast<uint16_t>(fbp);
            t.fbw = static_cast<uint8_t>(fbw);
            t.cls = static_cast<uint8_t>(cls);
            t.w = std::max<uint32_t>(1u, fbw) * 64u;
            t.h = h;
            t.dirty.assign(t.pages(), 0);
            t.stale.assign(t.pages(), 0);
            gl::GenTextures(1, &t.tex);
            gl::BindTexture(GL_TEXTURE_2D, t.tex);
            gl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, t.w * scale, t.h * scale, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
            if (k >= 0)
            {
                // Grow: keep the old contents.
                Target old = targets[k];
                targets[k] = t;
                GLuint src = 0;
                gl::GenFramebuffers(1, &src);
                gl::BindFramebuffer(GL_READ_FRAMEBUFFER, src);
                gl::FramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, old.tex, 0);
                dropFbos(old.tex);
                gl::BindFramebuffer(GL_DRAW_FRAMEBUFFER, fboFor(k, -1));
                gl::BlitFramebuffer(0, 0, old.w * scale, old.h * scale, 0, 0, old.w * scale, old.h * scale, GL_COLOR_BUFFER_BIT, GL_NEAREST);
                gl::DeleteFramebuffers(1, &src);
                gl::DeleteTextures(1, &old.tex);
                targets[k].lastWrite = old.lastWrite;
                targets[k].maxY = old.maxY;
                std::copy(old.dirty.begin(), old.dirty.end(), targets[k].dirty.begin());
                targets[k].anyDirty = old.anyDirty;
                std::copy(old.stale.begin(), old.stale.end(), targets[k].stale.begin());
                targets[k].anyStale = old.anyStale;
                refreshTarget(k);
                return k;
            }
            targets.push_back(t);
            k = static_cast<int>(targets.size() - 1);
            uploadTargetRect(targets[k], 0, 0, targets[k].w, targets[k].h);
            return k;
        }

        // A Z address inside an existing depth buffer of the same width (e.g. its second half)
        // resolves to that buffer at a row offset. Returns -1 if none.
        int aliasDepth(uint32_t zbp, uint32_t w, uint32_t &yoff) const
        {
            const uint32_t ppr = std::max<uint32_t>(1u, w / 64u);
            for (size_t k = 0; k < depths.size(); ++k)
            {
                const Depth &d = depths[k];
                const uint32_t pages = (d.h / 32u) * std::max<uint32_t>(1u, d.w / 64u);
                if (d.w == w && zbp >= d.zbp && zbp < d.zbp + pages && (zbp - d.zbp) % ppr == 0)
                {
                    yoff = (zbp - d.zbp) / ppr * 32u;
                    return static_cast<int>(k);
                }
            }
            return -1;
        }

        // GS -> host transfer of a Z buffer region (PSMZ32/24/16/16S): the depth buffer at that
        // address, one sample per GS pixel (the first of its scale x scale block), converted
        // back to the Z value the GS memory would hold. Other formats read as zeros.
        void readbackRegion(const uint32_t *x)
        {
            const uint32_t sbp = x[0], sbw = std::max<uint32_t>(1u, x[1]), spsm = x[2], sx = x[3], sy = x[4], w = x[5], h = x[6];
            const uint32_t zf = spsm & 0xFu;
            const bool isZ = (spsm & 0x30u) == 0x30u;
            const uint32_t bytesPer = !isZ ? 4u : zf == 0 ? 4u : zf == 1 ? 3u : 2u;
            std::vector<uint8_t> bytes(static_cast<size_t>(w) * h * bytesPer, 0);
            uint32_t yoff = 0;
            int zi = isZ && (sbp % 32u) == 0 ? aliasDepth(sbp / 32u, sbw * 64u, yoff) : -1;
            if (zi >= 0 && w > 0 && h > 0)
            {
                const Depth &D = depths[zi];
                const uint32_t S = scale, y0 = sy + yoff;
                if (sx + w <= D.w && y0 + h <= D.h)
                {
                    std::vector<float> z(static_cast<size_t>(w) * S * h * S);
                    gl::BindFramebuffer(GL_FRAMEBUFFER, fboFor(-1, zi));
                    gl::PixelStorei(GL_PACK_ALIGNMENT, 4);
                    gl::ReadPixels(static_cast<GLint>(sx * S), static_cast<GLint>(y0 * S), static_cast<GLsizei>(w * S), static_cast<GLsizei>(h * S),
                                   GL_DEPTH_COMPONENT, GL_FLOAT, z.data());
                    const double range = zf == 0 ? 4294967296.0 : zf == 1 ? 16777216.0 : 65536.0;
                    uint8_t *o = bytes.data();
                    for (uint32_t j = 0; j < h; ++j)
                        for (uint32_t i = 0; i < w; ++i, o += bytesPer)
                        {
                            double d = z[(static_cast<size_t>(j) * S) * (w * S) + i * S];
                            if (D.inverted)
                                d = 1.0 - d;
                            const double v = std::floor(std::clamp(d, 0.0, 1.0) * range + 0.5);
                            const uint32_t u = static_cast<uint32_t>(std::min(v, range - 1.0));
                            for (uint32_t k = 0; k < bytesPer; ++k)
                                o[k] = static_cast<uint8_t>(u >> (8 * k));
                        }
                    ++m_readbacks;
                }
            }
            std::lock_guard<std::mutex> lock(g_readbackMu);
            for (ReadbackSlot &r : g_readbacks)
                if (r.sbp == sbp && r.sbw == x[1] && r.spsm == spsm && r.rrw == w && r.rrh == h)
                {
                    r.bytes.swap(bytes);
                    return;
                }
            g_readbacks.push_back({sbp, x[1], spsm, w, h, std::move(bytes)});
            if (g_readbacks.size() > 16)
                g_readbacks.erase(g_readbacks.begin());
        }

        int getDepth(uint32_t zbp, uint32_t w, uint32_t h)
        {
            for (size_t k = 0; k < depths.size(); ++k)
                if (depths[k].zbp == zbp && depths[k].w >= w && depths[k].h >= h)
                    return static_cast<int>(k);
            Depth d{};
            d.zbp = static_cast<uint16_t>(zbp);
            d.w = w;
            d.h = h;
            gl::GenTextures(1, &d.tex);
            gl::BindTexture(GL_TEXTURE_2D, d.tex);
            gl::TexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, w * scale, h * scale, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
            gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            depths.push_back(d);
            const int k = static_cast<int>(depths.size() - 1);
            // New depth buffers start cleared to 0 (GEQUAL passes for everything).
            gl::BindFramebuffer(GL_FRAMEBUFFER, fboFor(-1, k));
            gl::Disable(GL_SCISSOR_TEST);
            gl::DepthMask(1);
            gl::ClearDepth(0.0);
            gl::Clear(GL_DEPTH_BUFFER_BIT);
            return k;
        }

        void dropFbos(GLuint colorTex)
        {
            (void)colorTex;
            for (auto &kv : fbos)
                gl::DeleteFramebuffers(1, &kv.second);
            fbos.clear();
        }

        GLuint fboFor(long t, int z)
        {
            const uint64_t key = (static_cast<uint64_t>(t + 1) << 32) | static_cast<uint32_t>(z + 1);
            auto it = fbos.find(key);
            if (it != fbos.end())
                return it->second;
            GLuint f = 0;
            gl::GenFramebuffers(1, &f);
            gl::BindFramebuffer(GL_FRAMEBUFFER, f);
            if (t >= 0)
                gl::FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, targets[t].tex, 0);
            if (z >= 0)
                gl::FramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depths[z].tex, 0);
            const GLenum db = t >= 0 ? GL_COLOR_ATTACHMENT0 : GL_NONE;
            gl::DrawBuffers(1, &db);
            const GLenum st = gl::CheckFramebufferStatus(GL_FRAMEBUFFER);
            if (st != GL_FRAMEBUFFER_COMPLETE)
                std::fprintf(stderr, "[gsgpu] incomplete framebuffer %#x (target %ld depth %d)\n", st, t, z);
            fbos[key] = f;
            return f;
        }

        // --- VRAM writes ----------------------------------------------------
        void invalidateTextures(uint32_t begin, uint32_t end)
        {
            for (auto &kv : texCache)
                if (kv.second.valid && kv.second.begin < end && begin < kv.second.end)
                    kv.second.valid = false;
        }

        void onVramWrite(uint32_t begin, uint32_t end, int except = -1)
        {
            invalidateTextures(begin, end);
            // Frame buffer pages overwritten in VRAM: refresh those pages of the targets.
            for (size_t k = 0; k < targets.size(); ++k)
            {
                if (static_cast<int>(k) == except)
                    continue;
                Target &t = targets[k];
                const uint32_t tb = t.fbp * 8192u;
                const uint32_t te = tb + t.pages() * 8192u;
                if (!(tb < end && begin < te))
                    continue;
                t.overwritten = seq;
                const uint32_t p0 = (std::max(begin, tb) - tb) / 8192u;
                const uint32_t p1 = (std::min(end, te) - tb + 8191u) / 8192u;
                const uint32_t rpp = t.rowsPerPage(), fbw = std::max<uint32_t>(1u, t.fbw);
                (void)rpp;
                (void)fbw;
                static const bool eager = std::getenv("GSGPU_EAGER") != nullptr;   // old behaviour, for comparison
                for (uint32_t p = p0; p < p1; ++p)
                {
                    // Brought up to date when the target is next used (refreshTarget): the
                    // game streams many textures through memory that also backs targets.
                    if (eager)
                        uploadTargetRect(t, (p % fbw) * 64u, (p / fbw) * rpp, 64u, rpp);
                    else
                    {
                        t.stale[p] = 1;
                        t.anyStale = true;
                    }
                    t.dirty[p] = 0;
                }
            }
        }

        void markDirty(Target &t, int x0, int y0, int x1, int y1)   // inclusive-exclusive GS pixels
        {
            x0 = std::max(x0, 0); y0 = std::max(y0, 0);
            x1 = std::min<int>(x1, static_cast<int>(t.w)); y1 = std::min<int>(y1, static_cast<int>(t.h));
            if (x1 <= x0 || y1 <= y0)
                return;
            const uint32_t rpp = t.rowsPerPage(), fbw = std::max<uint32_t>(1u, t.fbw);
            for (uint32_t py = static_cast<uint32_t>(y0) / rpp; py <= static_cast<uint32_t>(y1 - 1) / rpp; ++py)
                for (uint32_t px = static_cast<uint32_t>(x0) / 64u; px <= static_cast<uint32_t>(x1 - 1) / 64u; ++px)
                    t.dirty[py * fbw + px] = 1;
            t.anyDirty = true;
        }

        // Write GPU-drawn pages overlapping [begin, end) back into the VRAM shadow, oldest target
        // first so the most recent drawing wins where targets overlap.
        void syncToVram(uint32_t begin, uint32_t end)
        {
            std::vector<int> order;
            for (size_t k = 0; k < targets.size(); ++k)
            {
                const Target &t = targets[k];
                const uint32_t tb = t.fbp * 8192u, te = tb + t.pages() * 8192u;
                if (t.anyDirty && tb < end && begin < te)
                    order.push_back(static_cast<int>(k));
            }
            if (order.empty())
                return;
            std::sort(order.begin(), order.end(), [&](int a, int b) { return targets[a].lastWrite < targets[b].lastWrite; });
            for (const int k : order)
            {
                Target &t = targets[k];
                const uint32_t tb = t.fbp * 8192u;
                const uint32_t p0 = (std::max(begin, tb) - tb) / 8192u;
                const uint32_t p1 = std::min<uint32_t>(t.pages(), (std::min(end, tb + t.pages() * 8192u) - tb + 8191u) / 8192u);
                const uint32_t rpp = t.rowsPerPage(), fbw = std::max<uint32_t>(1u, t.fbw);
                uint32_t rx0 = ~0u, ry0 = ~0u, rx1 = 0, ry1 = 0;
                for (uint32_t p = p0; p < p1; ++p)
                    if (t.dirty[p])
                    {
                        rx0 = std::min(rx0, (p % fbw) * 64u); rx1 = std::max(rx1, (p % fbw) * 64u + 64u);
                        ry0 = std::min(ry0, (p / fbw) * rpp); ry1 = std::max(ry1, (p / fbw) * rpp + rpp);
                    }
                if (rx1 <= rx0)
                    continue;
                ++m_syncs;
                const uint32_t S = scale, rw = (rx1 - rx0) * S, rh = (ry1 - ry0) * S;
                readback.resize(static_cast<size_t>(rw) * rh * 4);
                gl::BindFramebuffer(GL_FRAMEBUFFER, fboFor(k, -1));
                gl::PixelStorei(GL_PACK_ALIGNMENT, 1);
                gl::ReadPixels(rx0 * S, ry0 * S, rw, rh, GL_RGBA, GL_UNSIGNED_BYTE, readback.data());
                const uint32_t psm = classPsm(t.cls);
                for (uint32_t p = p0; p < p1; ++p)
                {
                    if (!t.dirty[p])
                        continue;
                    t.dirty[p] = 0;
                    const uint32_t px = (p % fbw) * 64u, py = (p / fbw) * rpp;
                    for (uint32_t y = py; y < py + rpp; ++y)
                        for (uint32_t x = px; x < px + 64u; ++x)
                        {
                            uint32_t v;
                            std::memcpy(&v, &readback[((static_cast<size_t>(y - ry0) * S) * rw + (x - rx0) * S) * 4], 4);
                            if (t.cls)
                                v = ((v >> 3) & 31u) | (((v >> 11) & 31u) << 5) | (((v >> 19) & 31u) << 10) | ((v >> 31) << 15);
                            gsmem::writePixel(vram.data(), psm, t.fbp * 32u, t.fbw, x, y, v);
                        }
                    // Other targets over this memory now see the drawing too.
                    const uint32_t pb = tb + p * 8192u;
                    for (size_t j = 0; j < targets.size(); ++j)
                    {
                        if (static_cast<int>(j) == k)
                            continue;
                        Target &u = targets[j];
                        const uint32_t ub = u.fbp * 8192u, ue = ub + u.pages() * 8192u;
                        if (pb < ue && ub < pb + 8192u && u.lastWrite <= t.lastWrite)
                        {
                            const uint32_t q = (pb - ub) / 8192u;
                            u.stale[q] = 1;
                            u.anyStale = true;
                            u.dirty[q] = 0;
                        }
                    }
                    invalidateTextures(pb, pb + 8192u);
                }
                t.anyDirty = std::any_of(t.dirty.begin(), t.dirty.end(), [](uint8_t d) { return d != 0; });
            }
        }

        void loadClut(const Tex0Event &e)
        {
            // The GS loads a CLUT only for indexed textures. Wipe'out sets CLD=1 with CBP=0 on its
            // 32-bit textures too; loading would read the frame buffer at 0 back from the GPU (a
            // full pipeline stall) several times a frame.
            if (gsmem::bitsPerPixel(e.psm) > 8)
                return;
            bool load = false;
            switch (e.cld)
            {
            case 1: load = true; break;
            case 2: load = true; cbp0 = e.cbp; break;
            case 3: load = true; cbp1 = e.cbp; break;
            case 4: load = e.cbp != cbp0; cbp0 = e.cbp; break;
            case 5: load = e.cbp != cbp1; cbp1 = e.cbp; break;
            default: break;
            }
            if (!load)
                return;
            // Deferred to the first indexed texture that needs it: the game also loads a dummy
            // CLUT from CBP=0 (its frame buffer) to reset the CLUT cache before the real one, and
            // loading that would read the frame buffer back from the GPU. A full 256-entry load
            // replaces a pending one; anything smaller is applied first.
            if (clutPending && !(gsmem::bitsPerPixel(e.psm) == 8))
                applyClut(clutEv);
            clutEv = e;
            clutPending = true;
        }

        static void clutExtent(const Tex0Event &e, uint32_t &cb, uint32_t &ce)
        {
            if (e.csm == 0)
                gsmem::extent(e.cpsm, e.cbp, 1, 16, 16, cb, ce);
            else
                gsmem::extent(e.cpsm, e.cbp, e.cbw ? e.cbw : 1u, e.cou * 16u + 256u, e.cov + 1u, cb, ce);
            ce = std::min<uint32_t>(ce, gsmem::kVramSize);
        }

        // Apply a pending CLUT load before its source memory changes or a texture uses it.
        void flushClut(uint32_t begin = 0, uint32_t end = gsmem::kVramSize)
        {
            if (!clutPending)
                return;
            uint32_t cb = 0, ce = 0;
            clutExtent(clutEv, cb, ce);
            if (cb < end && begin < ce)
                applyClut(clutEv);
        }

        void applyClut(const Tex0Event &e)
        {
            clutPending = false;
            {
                uint32_t cb = 0, ce = 0;
                clutExtent(e, cb, ce);
                syncToVram(cb, ce);
            }
            const bool four = gsmem::bitsPerPixel(e.psm) == 4;
            const bool c16 = e.cpsm == gsmem::CT16 || e.cpsm == gsmem::CT16S;
            const uint32_t n = four ? 16u : 256u;
            const uint32_t base = four ? (e.csa & (c16 ? 0x1Fu : 0x0Fu)) * 16u : 0u;
            for (uint32_t i = 0; i < n; ++i)
            {
                uint32_t sx, sy, sw = 1;
                if (e.csm == 0)
                {
                    const uint32_t s = gsmem::csm1Index(i);
                    sx = s & 15u;
                    sy = s >> 4;
                }
                else
                {
                    sw = e.cbw ? e.cbw : 1u;
                    sx = e.cou * 16u + i;
                    sy = e.cov;
                }
                const uint32_t raw = gsmem::readPixel(vram.data(), e.cpsm, e.cbp, sw, sx, sy);
                clut[(base + i) & 255u] = c16 ? (raw | 0x80000000u) : raw;   // CT16 flagged for expansion
            }
            clutHash = hashBytes(reinterpret_cast<const uint8_t *>(clut), sizeof(clut));
        }

        // --- textures ---------------------------------------------------------
        static uint32_t expand16(uint32_t c, const State &s)
        {
            const uint32_t r = (c & 31u) << 3, g = ((c >> 5) & 31u) << 3, b = ((c >> 10) & 31u) << 3;
            uint32_t a = (c & 0x8000u) ? s.ta1 : s.ta0;
            if (s.aem && (c & 0x7FFFu) == 0)
                a = 0;
            return r | (g << 8) | (b << 16) | (a << 24);
        }

        uint32_t texel(const State &s, uint32_t psm, uint32_t tbp, uint32_t tbw, uint32_t x, uint32_t y) const
        {
            const uint32_t raw = gsmem::readPixel(vram.data(), psm, tbp, tbw, x, y);
            switch (psm)
            {
            case gsmem::CT32: return raw;
            case gsmem::CT24:
            {
                uint32_t a = s.ta0;
                if (s.aem && (raw & 0xFFFFFFu) == 0) a = 0;
                return (raw & 0xFFFFFFu) | (a << 24);
            }
            case gsmem::CT16: case gsmem::CT16S: return expand16(raw, s);
            default:
            {
                uint32_t idx = raw;
                if (gsmem::bitsPerPixel(psm) == 4) idx += (s.csa & 15u) * 16u;
                const uint32_t c = clut[idx & 255u];
                if (s.cpsm == gsmem::CT16 || s.cpsm == gsmem::CT16S) return expand16(c & 0xFFFFu, s);
                if (s.cpsm == gsmem::CT24)
                {
                    uint32_t a = s.ta0;
                    if (s.aem && (c & 0xFFFFFFu) == 0) a = 0;
                    return (c & 0xFFFFFFu) | (a << 24);
                }
                return c;
            }
            }
        }

        void levelAddr(const State &s, uint32_t level, uint32_t &tbp, uint32_t &tbw) const
        {
            if (level == 0) { tbp = s.tbp0; tbw = s.tbw; return; }
            const uint64_t m = level <= 3 ? s.miptbp1 : s.miptbp2;
            const int k = static_cast<int>((level - 1) % 3);
            tbp = bits(m, k * 20, 14);
            tbw = bits(m, k * 20 + 14, 6);
        }

        GLuint getTexture(const State &s, uint32_t &levelsOut)
        {
            const uint32_t tw = 1u << std::min<uint32_t>(s.tw, 10u), th = 1u << std::min<uint32_t>(s.th, 10u);
            const uint32_t mmin = bits(s.tex1, 6, 3);
            const bool mip = mmin >= 2 && mmin <= 5;
            uint32_t levels = mip ? std::min<uint32_t>(bits(s.tex1, 2, 3), 6u) + 1u : 1u;
            levels = std::min<uint32_t>(levels, 1u + static_cast<uint32_t>(std::log2(std::max(tw, th))));
            const bool indexed = gsmem::bitsPerPixel(s.tpsm) <= 8;
            if (indexed)
                flushClut();
            uint64_t key = s.tbp0 | (uint64_t(s.tbw) << 14) | (uint64_t(s.tpsm) << 20) | (uint64_t(s.tw) << 26) |
                           (uint64_t(s.th) << 30) | (uint64_t(levels) << 34) | (uint64_t(s.cpsm) << 38) | (uint64_t(s.csa) << 42);
            key = key * 0x9E3779B97F4A7C15ull ^ (levels > 1 ? (s.miptbp1 * 31 + s.miptbp2) : 0);
            key = key * 0x9E3779B97F4A7C15ull ^ (indexed ? clutHash : 0);
            if (!indexed && gsmem::bitsPerPixel(s.tpsm) < 32)
                key = key * 0x9E3779B97F4A7C15ull ^ (s.ta0 | (s.ta1 << 8) | (s.aem << 16));
            TexEntry &e = texCache[key];
            levelsOut = levels;
            // Extent of all levels (for invalidation and content hashing).
            uint32_t begin = ~0u, end = 0;
            for (uint32_t l = 0; l < levels; ++l)
            {
                uint32_t tbp, tbw, b0, e0;
                levelAddr(s, l, tbp, tbw);
                gsmem::extent(s.tpsm, tbp, tbw, std::max(1u, tw >> l), std::max(1u, th >> l), b0, e0);
                begin = std::min(begin, b0);
                end = std::max(end, e0);
            }
            end = std::min<uint32_t>(end, gsmem::kVramSize);
            if (begin >= end)
                begin = 0;
            {
                static const bool syncLog = std::getenv("GSGPU_SYNCLOG") != nullptr;
                const uint64_t before = m_syncs;
                syncToVram(begin, end);
                static int logged = 0;
                if (syncLog && m_syncs != before && logged++ < 200)
                {
                    std::fprintf(stderr, "[gsgpu] readback for texture tbp=%#x tbw=%u psm=%#x %ux%u (bytes %#x-%#x):", s.tbp0, s.tbw, s.tpsm, tw, th, begin, end);
                    for (const Target &t : targets)
                        if (t.fbp * 8192u < end && begin < t.fbp * 8192u + t.pages() * 8192u)
                            std::fprintf(stderr, " target fbp=%#x fbw=%u cls=%d", t.fbp, t.fbw, t.cls);
                    std::fprintf(stderr, "\n");
                }
            }
            if (e.tex && e.valid)
            {
                e.lastUse = seq;
                lastHdBias = e.hdBias;
                lastHdLevels = e.hdLevels;
                return e.hdTex ? e.hdTex : e.tex;
            }
            const uint64_t content = hashBytes(vram.data() + begin, end - begin);
            if (e.tex && content == e.contentHash)
            {
                e.valid = true;
                e.begin = begin;
                e.end = end;
                e.lastUse = seq;
                lastHdBias = e.hdBias;
                lastHdLevels = e.hdLevels;
                return e.hdTex ? e.hdTex : e.tex;
            }
            if (!e.tex)
                gl::GenTextures(1, &e.tex);
            e.key = key;
            e.contentHash = content;
            e.begin = begin;
            e.end = end;
            e.valid = true;
            e.lastUse = seq;
            e.levels = levels;
            gl::BindTexture(GL_TEXTURE_2D, e.tex);
            gl::PixelStorei(GL_UNPACK_ALIGNMENT, 1);
            for (uint32_t l = 0; l < levels; ++l)
            {
                uint32_t tbp, tbw;
                levelAddr(s, l, tbp, tbw);
                const uint32_t w = std::max(1u, tw >> l), h = std::max(1u, th >> l);
                scratch.resize(static_cast<size_t>(w) * h * 4);
                uint32_t *out = reinterpret_cast<uint32_t *>(scratch.data());
                for (uint32_t y = 0; y < h; ++y)
                    for (uint32_t x = 0; x < w; ++x)
                        out[y * w + x] = texel(s, s.tpsm, tbp, tbw, x, y);
                if (l == 0)
                {
                    e.hdBias = 0.0f;
                    e.hdLevels = 0;
                    e.hdTex = 0;
                    if (texPackLevel0(e, w, h))
                        break;
                }
                gl::TexImage2D(GL_TEXTURE_2D, l, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, scratch.data());
            }
            lastHdBias = e.hdBias;
            lastHdLevels = e.hdLevels;
            if (e.hdTex)
                return e.hdTex;
            gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
            gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, static_cast<int>(levels) - 1);
            return e.tex;
        }

        void evictTextures()
        {
            if (texCache.size() < 512)
                return;
            for (auto it = texCache.begin(); it != texCache.end();)
            {
                if (it->second.lastUse + 600 < seq)
                {
                    gl::DeleteTextures(1, &it->second.tex);
                    it = texCache.erase(it);
                }
                else
                    ++it;
            }
        }
    };

    Renderer::Renderer() : m(std::make_unique<Impl>()) {}
    Renderer::~Renderer() = default;
    uint32_t Renderer::scale() const { return m->scale; }

    void Renderer::setHudSquash(float k) { m->hudSquash = k > 0.0f ? k : 1.0f; }

    void Renderer::setScale(uint32_t scale)
    {
        Impl &I = *m;
        scale = std::max<uint32_t>(1u, scale);
        if (scale == I.scale)
            return;
        I.syncToVram(0, gsmem::kVramSize);
        for (auto &t : I.targets)
            if (t.tex)
                gl::DeleteTextures(1, &t.tex);
        I.targets.clear();
        for (auto &d : I.depths)
            if (d.tex)
                gl::DeleteTextures(1, &d.tex);
        I.depths.clear();
        for (auto &kv : I.fbos)
            gl::DeleteFramebuffers(1, &kv.second);
        I.fbos.clear();
        if (I.displayTex)
            gl::DeleteTextures(1, &I.displayTex);
        I.displayTex = 0;
        gl::DeleteTextures(2, I.fieldTex);
        I.fieldTex[0] = I.fieldTex[1] = 0;
        I.displayW = I.displayH = 0;
        I.frameValid = false;
        I.scale = scale;
    }

    bool Renderer::lastFrame(uint32_t &tex, uint32_t &w, uint32_t &h) const
    {
        const Impl &I = *m;
        if (!I.frameValid || !I.displayTex)
            return false;
        tex = I.displayTex;
        w = I.displayW;
        h = I.displayH;
        return true;
    }

    bool Renderer::init(void *(*getProc)(const char *), const uint8_t *initialVram, uint32_t scale)
    {
        if (!gl::load(getProc))
            return false;
        m->scale = std::max<uint32_t>(1u, scale);
        m->vram.assign(initialVram, initialVram + gsmem::kVramSize);
        m->drawProg = link(kDrawVs, kDrawFs, true);
        m->blitProg = link(kBlitVs, kBlitFs, false);
        m->deintProg = link(kBlitVs, kDeintFs, false);
        Impl &I = *m;
        I.dCur = gl::GetUniformLocation(I.deintProg, "uCur");
        I.dPrev = gl::GetUniformLocation(I.deintProg, "uPrev");
        I.dMode = gl::GetUniformLocation(I.deintProg, "uMode");
        I.dField = gl::GetUniformLocation(I.deintProg, "uField");
        I.dLines = gl::GetUniformLocation(I.deintProg, "uLines");
        I.dSrc = gl::GetUniformLocation(I.deintProg, "uSrc");
        GLuint p = I.drawProg;
        I.getUniforms(p, I.u);
        GLint glMajor = 0, glMinor = 0;
        gl::GetIntegerv(GL_MAJOR_VERSION, &glMajor);
        gl::GetIntegerv(GL_MINOR_VERSION, &glMinor);
        const char *mdiEnv = std::getenv("RT_GPUMODELMDI");
        I.mdi = (glMajor > 4 || (glMajor == 4 && glMinor >= 3)) && gl::TexBuffer && gl::VertexAttribIPointer && gl::VertexAttribDivisor &&
                gl::MultiDrawElementsIndirect && gl::DrawArraysInstancedBaseInstance && !(mdiEnv && std::atoi(mdiEnv) == 0);
        for (int f = 0; f < (I.mdi ? 4 : 2); ++f)
        {
            std::string vs = kModelVs, fs = kDrawFs;
            vs.insert(vs.find('\n') + 1, std::string((f & 1) ? "#define FLAT\n" : "") + (f >= 2 ? "#define MDI\n" : ""));
            auto sub = [&fs](const char *a, const std::string &b) { fs.replace(fs.find(a), std::strlen(a), b); };
            sub("in vec4 vCol;", (f & 1) ? "flat in vec4 vCol;" : "in vec4 vColP;\nvec4 vCol;");
            sub("noperspective in vec3 vStq;", "in vec3 vStqP;\nvec3 vStq;");
            sub("in float vFog;", "in float vFogP;\nfloat vFog;");
            sub("void main()\n{\n", std::string("void main()\n{\n    vStq = vStqP * gl_FragCoord.w;\n    vFog = vFogP * gl_FragCoord.w;\n") +
                                       ((f & 1) ? "" : "    vCol = vColP * gl_FragCoord.w;\n"));
            const GLuint prog = link(vs.c_str(), fs.c_str(), true);
            GLint linked = 0;
            gl::GetProgramiv(prog, GL_LINK_STATUS, &linked);
            if (f < 2)
            {
                I.modelProg[f] = prog;
                I.getUniforms(prog, I.um[f]);
                I.modelsOk = I.modelsOk && linked;
            }
            else
            {
                I.mdProg[f - 2] = prog;
                I.getUniforms(prog, I.umd[f - 2]);
                I.mdLoc[f - 2] = gl::GetUniformLocation(prog, "uMd");
                I.mdi = I.mdi && linked;
            }
        }
        if (I.mdi)
        {
            gl::GenBuffers(1, &I.mdTbo);
            gl::GenTextures(1, &I.mdTex);
            gl::GenBuffers(1, &I.mdIdxVbo);
            gl::GenBuffers(1, &I.mdIndirect);
            gl::GenBuffers(1, &I.mdElems);
        }
        std::fprintf(stderr, "[gsgpu] model draws: %s (OpenGL %d.%d)\n", I.mdi ? "batched (multi-draw indirect)" : "one draw each", glMajor, glMinor);
        // The game keeps small values in a 32-bit Z buffer (a few thousand to a few hundred thousand
        // out of 2^32). Through the default [-1,1] clip depth, z * 2 - 1 lands next to -1 where a
        // float step is ~128 Z units, so coplanar track layers and decals z-fight. With a [0,1] clip
        // depth the value reaches the 32-bit float depth buffer with its full relative precision.
        // Only replay() switches it on; the host's own drawing (raylib) expects the default.
        I.clip01 = gl::ClipControl != nullptr;
        std::fprintf(stderr, "[gsgpu] depth: %s\n", I.clip01 ? "[0,1] clip control" : "default [-1,1] clip (reduced Z precision)");
        I.bSrc = gl::GetUniformLocation(I.blitProg, "uSrc");
        I.bTex = gl::GetUniformLocation(I.blitProg, "uTex");
        I.bOpaque = gl::GetUniformLocation(I.blitProg, "uOpaque");

        gl::GenVertexArrays(1, &I.vao);
        gl::BindVertexArray(I.vao);
        gl::GenBuffers(1, &I.vbo);
        gl::BindBuffer(GL_ARRAY_BUFFER, I.vbo);
        const GLsizei stride = sizeof(Vertex);
        gl::VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void *>(offsetof(Vertex, x)));
        gl::VertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void *>(offsetof(Vertex, r)));
        gl::VertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void *>(offsetof(Vertex, s)));
        gl::VertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void *>(offsetof(Vertex, u)));
        gl::VertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void *>(offsetof(Vertex, fog)));
        for (int k = 0; k < 5; ++k)
            gl::EnableVertexAttribArray(k);

        gl::GenVertexArrays(1, &I.meshVao);
        gl::BindVertexArray(I.meshVao);
        gl::GenBuffers(1, &I.meshVbo);
        gl::BindBuffer(GL_ARRAY_BUFFER, I.meshVbo);
        gl::BufferData(GL_ARRAY_BUFFER, Impl::kMeshBytes, nullptr, GL_STATIC_DRAW);
        for (int k = 0; k < 4; ++k)
            gl::EnableVertexAttribArray(k);
        if (I.mdi)
        {
            // Attribute 4: the draw index (0, 1, 2, ... one per instance, offset by baseInstance).
            gl::BindBuffer(GL_ARRAY_BUFFER, I.mdIdxVbo);
            gl::VertexAttribIPointer(4, 1, GL_INT, 4, nullptr);
            gl::VertexAttribDivisor(4, 1);
            gl::EnableVertexAttribArray(4);
            // The index patterns for up to kMdMaxVerts vertices: list 0 1 2 3 ..., strip
            // (k, k+1, k+2), fan (0, k+1, k+2).
            std::vector<uint32_t> idx;
            const uint32_t N = Impl::kMdMaxVerts;
            I.mdPattern[0] = 0;
            for (uint32_t k = 0; k < N; ++k)
                idx.push_back(k);
            I.mdPattern[1] = static_cast<uint32_t>(idx.size());
            for (uint32_t k = 0; k + 2 < N; ++k)
                idx.insert(idx.end(), {k, k + 1, k + 2});
            I.mdPattern[2] = static_cast<uint32_t>(idx.size());
            for (uint32_t k = 0; k + 2 < N; ++k)
                idx.insert(idx.end(), {0u, k + 1, k + 2});
            gl::BindBuffer(GL_ELEMENT_ARRAY_BUFFER, I.mdElems);   // (VAO state of meshVao)
            gl::BufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(idx.size() * 4), idx.data(), GL_STATIC_DRAW);
        }

        gl::GenVertexArrays(1, &I.quadVao);
        gl::BindVertexArray(I.quadVao);
        gl::GenBuffers(1, &I.quadVbo);
        gl::BindBuffer(GL_ARRAY_BUFFER, I.quadVbo);
        const float quad[8] = {-1, -1, 1, -1, -1, 1, 1, 1};
        gl::BufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
        gl::VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8, nullptr);
        gl::EnableVertexAttribArray(0);
        gl::BindVertexArray(0);
        std::fprintf(stderr, "[gsgpu] OpenGL %s, renderer %s, internal scale %ux\n",
                     reinterpret_cast<const char *>(gl::GetString(GL_VERSION)),
                     reinterpret_cast<const char *>(gl::GetString(GL_RENDERER)), m->scale);
        return true;
    }

    bool Renderer::modelsOk() const { return m->modelsOk; }

    const Stats &Renderer::stats() const
    {
        m_stats.readbacks = m->m_syncs;
        m_stats.targetUploads = m->m_targetUploads;
        return m_stats;
    }

    void Renderer::replay(const Batch &b)
    {
        Impl &I = *m;
        if (b.cmds.empty())
            return;
        struct ClipScope
        {
            bool on;
            explicit ClipScope(bool o) : on(o) { if (on) gl::ClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE); }
            ~ClipScope() { if (on) gl::ClipControl(GL_LOWER_LEFT, GL_NEGATIVE_ONE_TO_ONE); }
        } clipScope(I.clip01);
        gl::Disable(GL_CULL_FACE);
        I.transientUsed = 0;
        if (I.mdi && !b.models.empty())
        {
            // Batched model draws: every mesh in place first (once more from the start if the mesh
            // buffer had to be dropped half way), then the parameters of every model.
            const size_t nm = b.models.size();
            for (int attempt = 0; attempt < 2; ++attempt)
            {
                I.mdFirst.assign(nm, ~0u);
                bool reset = false;
                for (const Cmd &c : b.cmds)
                    if (c.op == Op::Model)
                    {
                        const ModelDraw &md = b.models[c.a];
                        const uint32_t off = I.meshOffset(b, md, reset);
                        I.mdFirst[c.a] = off == ~0u || off % (md.stride * 16u) ? ~0u : off / (md.stride * 16u);
                    }
                if (!reset)
                    break;
            }
            static const bool zclamp = std::getenv("RT_GPUMODELZCLAMP") != nullptr;
            static const int planes = std::getenv("RT_GPUMODELPLANES") ? static_cast<int>(std::strtol(std::getenv("RT_GPUMODELPLANES"), nullptr, 16)) : 0x3F;
            I.mdData.resize(nm * Impl::kMdTexels * 4);
            for (size_t k = 0; k < nm; ++k)
            {
                const ModelDraw &md = b.models[k];
                float *d = I.mdData.data() + k * Impl::kMdTexels * 4;
                std::memcpy(d, md.m, 64);
                std::memcpy(d + 16, md.cm, 64);
                std::memcpy(d + 32, md.l, 64);
                std::memcpy(d + 48, md.amb, 16);
                std::memcpy(d + 52, md.lc, 64);
                const float ofs[4] = {md.ofx, md.ofy, md.fog, zclamp ? 1.0f : 0.0f};
                std::memcpy(d + 68, ofs, 16);
                const float mf[4] = {(md.flags & 1u) ? 1.0f : (md.flags & 16u) ? 2.0f : 0.0f, static_cast<float>((md.flags >> 1) & 1u),
                                     static_cast<float>((md.flags & 4u) ? planes : 0), static_cast<float>(md.zpsm & 0xFu)};
                std::memcpy(d + 72, mf, 16);
            }
            gl::BindBuffer(GL_TEXTURE_BUFFER, I.mdTbo);
            const size_t bytes = I.mdData.size() * sizeof(float);
            if (bytes > I.mdTboCap)
            {
                I.mdTboCap = std::max(bytes, I.mdTboCap * 2);
                gl::BufferData(GL_TEXTURE_BUFFER, static_cast<GLsizeiptr>(I.mdTboCap), nullptr, GL_STREAM_DRAW);
            }
            else
                gl::BufferData(GL_TEXTURE_BUFFER, static_cast<GLsizeiptr>(I.mdTboCap), nullptr, GL_STREAM_DRAW);   // orphan
            gl::BufferSubData(GL_TEXTURE_BUFFER, 0, static_cast<GLsizeiptr>(bytes), I.mdData.data());
            gl::ActiveTexture(GL_TEXTURE2);
            gl::BindTexture(GL_TEXTURE_BUFFER, I.mdTex);
            gl::TexBuffer(GL_TEXTURE_BUFFER, GL_RGBA32F, I.mdTbo);
            gl::ActiveTexture(GL_TEXTURE0);
            if (nm > I.mdIdxCap)
            {
                I.mdIdxCap = std::max<size_t>({nm, I.mdIdxCap * 2, 4096});
                std::vector<int32_t> iota(I.mdIdxCap);
                for (size_t k = 0; k < iota.size(); ++k)
                    iota[k] = static_cast<int32_t>(k);
                gl::BindBuffer(GL_ARRAY_BUFFER, I.mdIdxVbo);
                gl::BufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(iota.size() * 4), iota.data(), GL_STATIC_DRAW);
            }
        }
        gl::BindVertexArray(I.vao);
        gl::BindBuffer(GL_ARRAY_BUFFER, I.vbo);
        gl::BufferData(GL_ARRAY_BUFFER, b.vertices.size() * sizeof(Vertex), b.vertices.data(), GL_STREAM_DRAW);
        const uint32_t S = I.scale;
        const bool edgeHud = I.hudSquash < 1.0f && EdgeHudActive();
        // Only the last Frame mark's picture can be shown; compositing earlier ones is wasted.
        size_t lastMark = b.cmds.size();
        for (size_t k = b.cmds.size(); k-- > 0;)
            if (b.cmds[k].op == Op::Frame)
            {
                lastMark = k;
                break;
            }

        for (size_t ci = 0, nextCi = 1; ci < b.cmds.size(); ci = nextCi, nextCi = ci + 1)
        {
            const Cmd &c = b.cmds[ci];
            ++I.seq;
            switch (c.op)
            {
            case Op::TransferBegin:
                I.xfer.begin(c.x[0], c.x[1], c.x[2], c.x[3], c.x[4], c.x[5], c.x[6]);
                break;
            case Op::TransferData:
            {
                uint32_t beg = 0, end = 0;
                gsmem::extent(I.xfer.dpsm, I.xfer.dbp, I.xfer.dbw, I.xfer.dsax + I.xfer.rrw, I.xfer.dsay + I.xfer.rrh, beg, end);
                I.flushClut(beg, end);
                I.xfer.data(I.vram.data(), b.data.data() + c.a, c.b);
                ++m_stats.uploads;
                I.onVramWrite(beg, std::min<uint32_t>(end, gsmem::kVramSize));
                gl::BindVertexArray(I.vao);
                gl::BindBuffer(GL_ARRAY_BUFFER, I.vbo);
                break;
            }
            case Op::Tex0:
                I.loadClut(b.tex0[c.a]);
                break;
            case Op::Readback:
                I.readbackRegion(c.x);
                break;
            case Op::Frame:
            {
                if (static_cast<size_t>(&c - b.cmds.data()) != lastMark)
                    break;
                // End of a guest frame: keep its picture. Every Draw/Clear binds its own GL
                // state, so compositing here does not disturb the rest of the replay.
                uint32_t tex = 0, w = 0, h = 0;
                I.frameValid = present(b.frames[c.a], tex, w, h);
                break;
            }
            case Op::Clear:
            {
                const State &s = b.states[c.state];
                const int cls = Impl::formatClass(s.fpsm);
                const int t = I.getTarget(s.fbp, s.fbw, cls < 0 ? 0 : cls, s.sy1 + 1u);
                gl::BindFramebuffer(GL_FRAMEBUFFER, I.fboFor(t, -1));
                gl::Disable(GL_SCISSOR_TEST);
                gl::ColorMask(1, 1, 1, 1);
                const uint32_t v = c.x[0];
                gl::ClearColor((v & 255) / 255.0f, ((v >> 8) & 255) / 255.0f, ((v >> 16) & 255) / 255.0f, (v >> 24) / 255.0f);
                gl::Clear(GL_COLOR_BUFFER_BIT);
                I.targets[t].lastWrite = I.seq;
                I.markDirty(I.targets[t], 0, 0, I.targets[t].w, s.sy1 + 1);
                break;
            }
            case Op::Draw:
            case Op::Model:
            {
                const State &s = b.states[c.state];
                const ModelDraw *md = c.op == Op::Model ? &b.models[c.a] : nullptr;
                ++m_stats.runs;
                m_stats.vertices += md ? md->n : c.b;
                if (s.fbw == 0)
                    break;
                uint32_t meshOff = 0;
                size_t members = 1;   // batched: model commands drawn together with this one
                if (md && I.mdi)
                {
                    if (I.mdFirst[c.a] == ~0u)
                    {
                        ++I.modelMissing;
                        break;
                    }
                    // The run of model commands this one can be drawn with: same state, primitive,
                    // vertex layout and shading.
                    size_t end = ci + 1;
                    while (end < b.cmds.size())
                    {
                        const Cmd &g = b.cmds[end];
                        if (g.op != Op::Model || g.state != c.state || I.mdFirst[g.a] == ~0u)
                            break;
                        const ModelDraw &gm = b.models[g.a];
                        if (gm.stride != md->stride || ((gm.flags ^ md->flags) & 8u))
                            break;
                        m_stats.vertices += gm.n;
                        ++m_stats.runs;
                        ++end;
                    }
                    members = end - ci;
                    if (static const bool st = std::getenv("RT_MDISTATS") != nullptr; st)
                    {
                        // RT_MDISTATS=1: why runs of model draws end (next command not a model, other
                        // state, mesh missing, primitive, stride, shading), and their mean length.
                        static uint64_t why[6], runs = 0, drawn = 0;
                        ++runs;
                        drawn += members;
                        if (end < b.cmds.size())
                        {
                            const Cmd &g = b.cmds[end];
                            const ModelDraw *gm = g.op == Op::Model ? &b.models[g.a] : nullptr;
                            ++why[!gm ? 0 : g.state != c.state ? 1 : I.mdFirst[g.a] == ~0u ? 2 : gm->stride != md->stride ? 4 : 5];
                        }
                        if ((runs & 0x3FFFF) == 0)
                            std::fprintf(stderr, "[mdi] %llu runs, %.2f models each; ends: other cmd %llu, state %llu, missing %llu, prim %llu, stride %llu, shading %llu\n",
                                         (unsigned long long)runs, double(drawn) / runs, (unsigned long long)why[0], (unsigned long long)why[1],
                                         (unsigned long long)why[2], (unsigned long long)why[3], (unsigned long long)why[4], (unsigned long long)why[5]);
                    }
                    nextCi = end;
                    I.seq += members - 1;
                    if (I.knownZ.count(s.fbp))
                    {
                        ++m_stats.unsupported;
                        break;
                    }
                }
                else if (md)
                {
                    // The mesh: in the buffer already, or sent with this draw.
                    bool reset = false;
                    meshOff = I.meshOffset(b, *md, reset);
                    if (meshOff == ~0u)
                    {
                        ++I.modelMissing;
                        break;
                    }
                    if (I.knownZ.count(s.fbp))
                    {
                        ++m_stats.unsupported;
                        break;
                    }
                }
                // Drawing into a Z buffer's memory as a colour buffer (the game clears Z this way):
                // write the colour value as depth into the matching depth buffer.
                if (bits(s.test, 16, 1))
                    I.knownZ.insert(s.zbp);
                if (I.knownZ.count(s.fbp) && (s.abe || s.fbmsk))
                {
                    // Blended or masked colour draws over a Z buffer. Ruff Trigger inverts its Z
                    // bytes (Cs=255, (Cs - Cd) * 128/128, alpha masked) before drawing shadows
                    // with reversed depth tests, and back again afterwards: track that as a flag
                    // on the depth buffer instead of rewriting it.
                    const uint32_t A = bits(s.alpha, 0, 2), B = bits(s.alpha, 2, 2), C = bits(s.alpha, 4, 2), D = bits(s.alpha, 6, 2);
                    const Vertex *v = &b.vertices[c.a];
                    bool white = !s.tme && c.b > 0;
                    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
                    for (uint32_t k = 0; k < c.b && white; ++k)
                    {
                        white = v[k].r == 255 && v[k].g == 255 && v[k].b == 255;
                        x0 = std::min(x0, v[k].x); x1 = std::max(x1, v[k].x);
                        y0 = std::min(y0, v[k].y); y1 = std::max(y1, v[k].y);
                    }
                    if (white && s.abe && A == 0 && B == 1 && C == 2 && D == 2 && bits(s.alpha, 32, 8) == 128 &&
                        (s.fbmsk & 0xFFFFFFu) == 0 && x0 <= 0.5f && y0 <= 0.5f)
                    {
                        uint32_t yoff = 0;
                        int zi = I.aliasDepth(s.fbp, std::max<uint32_t>(1u, s.fbw) * 64u, yoff);
                        if (zi < 0)
                            zi = I.getDepth(s.fbp, std::max<uint32_t>(1u, s.fbw) * 64u, std::max<uint32_t>(512u, s.sy1 + 1u));
                        I.depths[zi].inverted = !I.depths[zi].inverted;
                        ++m_stats.depthInverts;
                    }
                    else
                        ++m_stats.unsupported;
                    break;
                }
                if (I.knownZ.count(s.fbp))
                {
                    const int zi = I.getDepth(s.fbp, std::max<uint32_t>(1u, s.fbw) * 64u, std::max<uint32_t>(512u, s.sy1 + 1u));
                    {
                        const Vertex *v = &b.vertices[c.a];
                        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
                        bool uniform = !s.tme;
                        for (uint32_t k = 0; k < c.b; ++k)
                        {
                            x0 = std::min(x0, v[k].x); x1 = std::max(x1, v[k].x);
                            y0 = std::min(y0, v[k].y); y1 = std::max(y1, v[k].y);
                            uniform = uniform && v[k].r == v[0].r && v[k].g == v[0].g && v[k].b == v[0].b && v[k].a == v[0].a;
                        }
                        if (!uniform)
                        {
                            ++m_stats.unsupported;
                            break;
                        }
                        const uint32_t zv = static_cast<uint32_t>(v[0].r) | (static_cast<uint32_t>(v[0].g) << 8) |
                                            (static_cast<uint32_t>(v[0].b) << 16) | (static_cast<uint32_t>(v[0].a) << 24);
                        const uint32_t zfmt = s.fpsm & 0xF;   // CT32 -> Z32, CT24 -> Z24, CT16 -> Z16
                        const double depth = zfmt == 0 ? (zv / 4294967296.0) : zfmt == 1 ? ((zv & 0xFFFFFFu) / 16777216.0)
                                                                                         : ((zv & 0xFFFFu) / 65536.0);
                        const Impl::Depth &D = I.depths[zi];
                        const double depthStored = D.inverted ? 1.0 - depth : depth;
                        const int ix0 = std::max(0, static_cast<int>(std::floor(std::max<float>(x0, s.sx0))));
                        const int iy0 = std::max(0, static_cast<int>(std::floor(std::max<float>(y0, s.sy0))));
                        const int ix1 = std::min<int>(static_cast<int>(D.w), static_cast<int>(std::ceil(std::min<float>(x1, s.sx1 + 1.0f))));
                        const int iy1 = std::min<int>(static_cast<int>(D.h), static_cast<int>(std::ceil(std::min<float>(y1, s.sy1 + 1.0f))));
                        if (ix1 > ix0 && iy1 > iy0)
                        {
                            gl::BindFramebuffer(GL_FRAMEBUFFER, I.fboFor(-1, zi));
                            gl::Enable(GL_SCISSOR_TEST);
                            gl::Scissor(ix0 * S, iy0 * S, (ix1 - ix0) * S, (iy1 - iy0) * S);
                            gl::DepthMask(1);
                            gl::ClearDepth(depthStored);
                            gl::Clear(GL_DEPTH_BUFFER_BIT);
                            ++m_stats.depthFills;
                        }
                        // The same pass may also write Z through its own ZBUF (the game clears the
                        // second half of its Z buffer this way).
                        const bool zte2 = bits(s.test, 16, 1), zalways = bits(s.test, 17, 2) == 1;
                        if (zte2 && zalways && !s.zmsk && s.zbp != s.fbp)
                        {
                            uint32_t yoff = 0;
                            int zj = I.aliasDepth(s.zbp, D.w, yoff);
                            if (zj < 0)
                                zj = I.getDepth(s.zbp, D.w, D.h), yoff = 0;
                            const Impl::Depth &D2 = I.depths[zj];
                            const int jy0 = iy0 + static_cast<int>(yoff), jy1 = std::min<int>(static_cast<int>(D2.h), iy1 + static_cast<int>(yoff));
                            if (ix1 > ix0 && jy1 > jy0)
                            {
                                gl::BindFramebuffer(GL_FRAMEBUFFER, I.fboFor(-1, zj));
                                gl::Scissor(ix0 * S, jy0 * S, (ix1 - ix0) * S, (jy1 - jy0) * S);
                                gl::ClearDepth(D2.inverted ? 1.0 - v[0].z : v[0].z);
                                gl::Clear(GL_DEPTH_BUFFER_BIT);
                                ++m_stats.depthFills;
                            }
                        }
                        break;
                    }
                }
                const int fcls = Impl::formatClass(s.fpsm);
                if (fcls < 0)
                {
                    ++m_stats.unsupported;
                    break;
                }
                const int t = I.getTarget(s.fbp, s.fbw, fcls, s.sy1 + 1u);
                Impl::Target &T = I.targets[t];
                const bool zte = bits(s.test, 16, 1);
                static const bool dbgNoZ = std::getenv("GSGPU_DBG_NOZ_CC0") != nullptr;
                const uint32_t ztst = (dbgNoZ && !s.colclamp && s.abe) ? 1u : bits(s.test, 17, 2);
                const bool useDepth = zte && !(ztst == 1 && s.zmsk);
                int z = -1;
                if (useDepth)
                {
                    uint32_t yoff = 0;
                    z = I.aliasDepth(s.zbp, T.w, yoff);
                    if (z >= 0 && yoff != 0)
                        ++m_stats.unsupported;   // colour and depth at different offsets
                    if (z < 0 || yoff != 0)
                        z = I.getDepth(s.zbp, T.w, T.h);
                }

                // Texture.
                uint32_t levels = 1;
                GLuint tex = 0;
                float texW = 1, texH = 1;
                const uint32_t tw = 1u << std::min<uint32_t>(s.tw, 10u), th = 1u << std::min<uint32_t>(s.th, 10u);
                bool fromTarget = false;
                bool hd = false;   // a texture pack replacement: smooth filtering, its own mipmaps
                int texA = 0;
                if (s.tme)
                {
                    const int tcls = Impl::formatClass(s.tpsm);
                    if (tcls >= 0 && (s.tbp0 & 31u) == 0)
                    {
                        const int src = I.findTarget(s.tbp0 / 32u, s.tbw, tcls);
                        texA = tcls ? 1 : (s.tpsm == gsmem::CT24 ? 2 : 0);
                        if (src >= 0 && I.targets[src].lastWrite > I.targets[src].overwritten)
                        {
                            I.refreshTarget(src);
                            tex = I.targets[src].tex;
                            texW = static_cast<float>(I.targets[src].w);
                            texH = static_cast<float>(I.targets[src].h);
                            fromTarget = true;
                            ++m_stats.rtSamples;
                            if (src == t)
                                ++m_stats.unsupported;   // feedback loop: undefined in GL
                        }
                    }
                    if (!fromTarget)
                    {
                        tex = I.getTexture(s, levels);
                        hd = I.lastHdLevels != 0;
                        texW = static_cast<float>(tw);
                        texH = static_cast<float>(th);
                    }
                }

                gl::BindFramebuffer(GL_FRAMEBUFFER, I.fboFor(t, z));
                gl::Viewport(0, 0, T.w * S, T.h * S);
                const Impl::Uniforms &U = md ? (I.mdi ? I.umd : I.um)[(md->flags & 8u) ? 1 : 0] : I.u;
                if (md && I.mdi)
                {
                    const uint32_t st = md->stride * 16u;
                    const int f = (md->flags & 8u) ? 1 : 0;
                    gl::BindVertexArray(I.meshVao);
                    gl::BindBuffer(GL_ARRAY_BUFFER, I.meshVbo);
                    gl::VertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, st, nullptr);
                    gl::VertexAttribPointer(1, 4, GL_INT, GL_FALSE, st, reinterpret_cast<const void *>(16));
                    gl::VertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, st, reinterpret_cast<const void *>(32));
                    gl::VertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, st, reinterpret_cast<const void *>(md->stride >= 4u ? 48 : 0));
                    gl::UseProgram(I.mdProg[f]);
                    gl::Uniform1i(I.mdLoc[f], 2);
                    I.mdCmds.clear();
                    for (size_t k = ci; k < ci + members; ++k)
                    {
                        const uint32_t a = b.cmds[k].a;
                        const ModelDraw &gm = b.models[a];
                        const uint32_t n = std::min(gm.n, Impl::kMdMaxVerts), mode = std::min<uint32_t>(gm.mode, 2u);
                        const uint32_t count = mode == 0 ? n / 3u * 3u : n >= 3 ? 3u * (n - 2) : 0u;
                        I.mdCmds.push_back({count, 1u, I.mdPattern[mode], static_cast<GLint>(I.mdFirst[a]), a});
                    }
                    gl::BindBuffer(GL_DRAW_INDIRECT_BUFFER, I.mdIndirect);
                    gl::BufferData(GL_DRAW_INDIRECT_BUFFER, static_cast<GLsizeiptr>(I.mdCmds.size() * sizeof(Impl::DrawIndirect)), I.mdCmds.data(), GL_STREAM_DRAW);
                    I.modelDraws += members;
                    ++I.mdRuns;
                    I.mdBatched += members;
                }
                else if (md)
                {
                    const uint32_t st = md->stride * 16u;
                    const char *base = reinterpret_cast<const char *>(static_cast<uintptr_t>(meshOff));
                    gl::BindVertexArray(I.meshVao);
                    gl::BindBuffer(GL_ARRAY_BUFFER, I.meshVbo);
                    gl::VertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, st, base);
                    gl::VertexAttribPointer(1, 4, GL_INT, GL_FALSE, st, base + 16);
                    gl::VertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, st, base + 32);
                    gl::VertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, st, base + (md->stride >= 4u ? 48 : 0));
                    gl::UseProgram(I.modelProg[(md->flags & 8u) ? 1 : 0]);
                    gl::UniformMatrix4fv(U.m, 1, GL_FALSE, md->m);
                    gl::UniformMatrix4fv(U.cm, 1, GL_FALSE, md->cm);
                    gl::UniformMatrix4fv(U.l, 1, GL_FALSE, md->l);
                    gl::Uniform4f(U.amb, md->amb[0], md->amb[1], md->amb[2], md->amb[3]);
                    gl::UniformMatrix4fv(U.lc, 1, GL_FALSE, md->lc);
                    gl::Uniform4f(U.ofs, md->ofx, md->ofy, md->fog, std::getenv("RT_GPUMODELZCLAMP") ? 1.0f : 0.0f);
                    static const int planes = std::getenv("RT_GPUMODELPLANES") ? static_cast<int>(std::strtol(std::getenv("RT_GPUMODELPLANES"), nullptr, 16)) : 0x3F;
                    gl::Uniform4i(U.mf, (md->flags & 1u) ? 1 : (md->flags & 16u) ? 2 : 0, (md->flags >> 1) & 1u, (md->flags & 4u) ? planes : 0, static_cast<int>(md->zpsm & 0xFu));
                    ++I.modelDraws;
                }
                else
                {
                    gl::BindVertexArray(I.vao);
                    gl::UseProgram(I.drawProg);
                }
                gl::Uniform2f(U.rtSize, static_cast<float>(T.w), static_cast<float>(T.h));
                gl::Uniform1i(U.zinv, useDepth && z >= 0 && I.depths[z].inverted ? 1 : 0);
                gl::Uniform1i(U.z01, I.clip01 ? 1 : 0);
                {
                    // Widescreen: the game's 2D overlay (HUD, menus, text: UV-addressed, no depth
                    // test, smaller than the screen) is drawn into the main frame buffers for a 4:3
                    // picture; squash it about the centre so it keeps its shape when the frame is
                    // shown at 16:9. Full-screen passes and offscreen effect targets are left alone.
                    float hudScale = 1.0f, hudX = T.w * 0.5f;
                    if (md)
                    {
                        if (edgeHud && T.w == 512 && T.cls == 0)
                            T.hudSeen = false;   // 3D: ends the overlay
                    }
                    else if (edgeHud && T.w == 512 && T.cls == 0 && !s.notHud)
                    {
                        // Racing (see NoteRaceHud): the HUD is pushed out to the screen edges.
                        // Its untextured panels start the overlay; textured draws after them in
                        // the same frame are its text and icons. 3D draws end it.
                        const Vertex *v = &b.vertices[c.a];
                        bool flat = true;
                        for (uint32_t k = 0; k < c.b && flat; ++k)
                            flat = s.fst || v[k].q == 1.0f;
                        bool edge = false;
                        if (!flat)
                            T.hudSeen = false;
                        else if (!s.tme && !zte)
                            edge = T.hudSeen = true;
                        else if (s.tme && T.hudSeen)
                            edge = true;
                        if (edge)
                        {
                            I.hudVerts.assign(v, v + c.b);
                            EdgeHudMap(I.hudVerts.data(), c.b, s.tme);
                            static const bool hudLog = std::getenv("RT_HUDLOG") != nullptr;
                            if (hudLog)
                            {
                                std::string line;
                                for (uint32_t k = 0; k < c.b && k < 60; k += 3)
                                {
                                    char t[64];
                                    std::snprintf(t, sizeof t, " %.0f,%.0f>%.0f", v[k].x, v[k].y, I.hudVerts[k].x);
                                    line += t;
                                }
                                std::fprintf(stderr, "[hud] n=%u tme=%d%s\n", c.b, (int)s.tme, line.c_str());
                            }
                            gl::BindBuffer(GL_ARRAY_BUFFER, I.vbo);
                            gl::BufferSubData(GL_ARRAY_BUFFER, static_cast<GLintptr>(c.a) * sizeof(Vertex), c.b * sizeof(Vertex), I.hudVerts.data());
                            ++m_stats.hudDraws;
                        }
                    }
                    else if (edgeHud && T.w == 512 && T.cls == 0)
                    {
                        // Marked as not HUD (the lock-on reticle round a ship): kept in shape about
                        // its own centre instead.
                        hudScale = I.hudSquash;
                        hudX = s.notHudX / 16.0f;
                    }
                    else if (I.hudSquash < 1.0f && s.tme && (!zte || ztst == 1) && T.fbw >= 8 && T.cls == 0)
                    {
                        const Vertex *v = &b.vertices[c.a];
                        float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
                        bool flat = true;   // UV-addressed, or STQ with q == 1 everywhere (no perspective)
                        for (uint32_t k = 0; k < c.b; ++k)
                        {
                            flat = flat && (s.fst || v[k].q == 1.0f);
                            x0 = std::min(x0, v[k].x); x1 = std::max(x1, v[k].x);
                            y0 = std::min(y0, v[k].y); y1 = std::max(y1, v[k].y);
                        }
                        if (flat && x1 - x0 < 0.7f * T.w && !(y1 - y0 >= 0.9f * T.h))
                            hudScale = I.hudSquash, ++m_stats.hudDraws;
                    }
                    gl::Uniform2f(U.hud, hudX, hudScale);
                }
                gl::Uniform1i(U.tex, 0);
                gl::ActiveTexture(GL_TEXTURE0);
                gl::BindTexture(GL_TEXTURE_2D, tex);
                if (s.tme)
                {
                    const uint32_t wms = bits(s.clamp, 0, 2), wmt = bits(s.clamp, 2, 2);
                    const uint32_t mmag = bits(s.tex1, 5, 1), mmin = bits(s.tex1, 6, 3);
                    static const GLenum kMin[8] = {GL_NEAREST, GL_LINEAR, GL_NEAREST_MIPMAP_NEAREST, GL_NEAREST_MIPMAP_LINEAR,
                                                   GL_LINEAR_MIPMAP_NEAREST, GL_LINEAR_MIPMAP_LINEAR, GL_LINEAR, GL_LINEAR};
                    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mmag || hd ? GL_LINEAR : GL_NEAREST);
                    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, hd ? GL_LINEAR_MIPMAP_LINEAR : fromTarget ? (mmin & 1 ? GL_LINEAR : GL_NEAREST) : kMin[mmin]);
                    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, (!fromTarget && wms == 0) ? GL_REPEAT : GL_CLAMP_TO_EDGE);
                    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, (!fromTarget && wmt == 0) ? GL_REPEAT : GL_CLAMP_TO_EDGE);
                    gl::Uniform4i(U.tex0, 1, s.fst, s.tfx, s.tcc);
                    gl::Uniform4i(U.texA, fromTarget ? texA : 0, s.ta0, s.ta1, s.aem);
                    gl::Uniform4i(U.wrap, fromTarget && wms == 0 ? 1 : static_cast<int>(wms), fromTarget && wmt == 0 ? 1 : static_cast<int>(wmt),
                                  static_cast<int>(bits(s.tex1, 0, 1)), hd ? static_cast<int>(I.lastHdLevels) - 1 : static_cast<int>(levels - 1));
                    gl::Uniform4f(U.region, static_cast<float>(bits(s.clamp, 4, 10)), static_cast<float>(bits(s.clamp, 14, 10)),
                                  static_cast<float>(bits(s.clamp, 24, 10)), static_cast<float>(bits(s.clamp, 34, 10)));
                    gl::Uniform4f(U.regionRep, static_cast<float>(bits(s.clamp, 4, 10)), static_cast<float>(bits(s.clamp, 14, 10)),
                                  static_cast<float>(bits(s.clamp, 24, 10)), static_cast<float>(bits(s.clamp, 34, 10)));
                    gl::Uniform4f(U.texGeom, static_cast<float>(tw), static_cast<float>(th), 1.0f / texW, 1.0f / texH);
                    int k = static_cast<int>(bits(s.tex1, 32, 12));
                    if (k & 0x800) k -= 0x1000;
                    gl::Uniform4f(U.lod, k / 16.0f, static_cast<float>(1u << bits(s.tex1, 19, 2)), hd ? I.lastHdBias : 0.0f, 0);
                }
                else
                    gl::Uniform4i(U.tex0, 0, 0, 0, 0);

                // Alpha blending.
                const uint32_t A = bits(s.alpha, 0, 2), B = bits(s.alpha, 2, 2), C = bits(s.alpha, 4, 2), D = bits(s.alpha, 6, 2);
                const float fix = static_cast<float>(bits(s.alpha, 32, 8));
                gl::Uniform4f(U.fogFix, s.fogr, s.fogg, s.fogb, fix);
                bool blend = s.abe;
                // COLCLAMP=0 (wrap) and Ad factors are not expressible with GL blending: blend in the
                // shader, reading the target with a texture barrier before every triangle.
                const bool manual = blend && gl::TextureBarrier && (!s.colclamp || C == 1) && !(A == B && D == 1);
                GLenum eq = GL_FUNC_ADD, sf = GL_ONE, df = GL_ZERO;
                gl::Uniform4i(U.blend, manual ? (s.colclamp ? 2 : 1) : 0, static_cast<int>(A), static_cast<int>(B), static_cast<int>(D));
                gl::Uniform4i(U.blend2, static_cast<int>(C), T.cls ? 1 : 0, 0, 0);
                if (manual)
                {
                    gl::Uniform1i(U.dst, 1);
                    gl::ActiveTexture(GL_TEXTURE1);
                    gl::BindTexture(GL_TEXTURE_2D, T.tex);
                    gl::ActiveTexture(GL_TEXTURE0);
                    ++m_stats.manualBlend;
                    blend = false;
                }
                if (blend)
                {
                    if (C == 1)
                        ++m_stats.unsupported;   // Ad as blend factor
                    if (A == B) { sf = D == 0 ? GL_ONE : GL_ZERO; df = D == 1 ? GL_ONE : GL_ZERO; }
                    else if (A == 0 && B == 1 && D == 1) { sf = GL_SRC1_ALPHA; df = GL_ONE_MINUS_SRC1_ALPHA; }
                    else if (A == 0 && B == 2 && D == 1) { sf = GL_SRC1_ALPHA; df = GL_ONE; }
                    else if (A == 0 && B == 2 && D == 2) { sf = GL_SRC1_ALPHA; df = GL_ZERO; }
                    else if (A == 1 && B == 0 && D == 0) { sf = GL_ONE_MINUS_SRC1_ALPHA; df = GL_SRC1_ALPHA; }
                    else if (A == 1 && B == 2 && D == 0) { sf = GL_ONE; df = GL_SRC1_ALPHA; }
                    else if (A == 1 && B == 2 && D == 2) { sf = GL_ZERO; df = GL_SRC1_ALPHA; }
                    else if (A == 2 && B == 0 && D == 1) { eq = GL_FUNC_REVERSE_SUBTRACT; sf = GL_SRC1_ALPHA; df = GL_ONE; }
                    else if (A == 2 && B == 1 && D == 0) { eq = GL_FUNC_SUBTRACT; sf = GL_ONE; df = GL_SRC1_ALPHA; }
                    else { ++m_stats.unsupported; sf = GL_SRC1_ALPHA; df = GL_ONE_MINUS_SRC1_ALPHA; }
                }
                if (blend)
                {
                    gl::Enable(GL_BLEND);
                    gl::BlendEquationSeparate(eq, GL_FUNC_ADD);
                    gl::BlendFuncSeparate(sf, df, GL_ONE, GL_ZERO);
                }
                else
                    gl::Disable(GL_BLEND);

                // Scissor.
                gl::Enable(GL_SCISSOR_TEST);
                const uint32_t sx1 = std::min<uint32_t>(s.sx1, T.w - 1), sy1 = std::min<uint32_t>(s.sy1, T.h - 1);
                if (s.sx0 > sx1 || s.sy0 > sy1)
                    break;
                gl::Scissor(s.sx0 * S, s.sy0 * S, (sx1 - s.sx0 + 1) * S, (sy1 - s.sy0 + 1) * S);
                T.maxY = std::max<uint32_t>(T.maxY, sy1);

                // Depth.
                static const GLenum kZ[4] = {GL_NEVER, GL_ALWAYS, GL_GEQUAL, GL_GREATER};
                if (useDepth)
                {
                    gl::Enable(GL_DEPTH_TEST);
                    static const GLenum kZInv[4] = {GL_NEVER, GL_ALWAYS, GL_LEQUAL, GL_LESS};
                    gl::DepthFunc(z >= 0 && I.depths[z].inverted ? kZInv[ztst] : kZ[ztst]);
                }
                else
                    gl::Disable(GL_DEPTH_TEST);

                const uint32_t fbmsk = s.fbmsk;
                const bool mr = (fbmsk & 0xFFu) != 0xFFu, mg = ((fbmsk >> 8) & 0xFFu) != 0xFFu, mb = ((fbmsk >> 16) & 0xFFu) != 0xFFu;
                const bool ma = (fbmsk >> 24) == 0u;
                const bool ate = bits(s.test, 0, 1), afailMode = true;
                (void)afailMode;
                const uint32_t atst = bits(s.test, 1, 3), aref = bits(s.test, 4, 8), afail = bits(s.test, 12, 2);
                gl::Uniform4i(U.atest, ate && atst != 1 ? 1 : 0, static_cast<int>(atst), static_cast<int>(aref), 0);

                auto drawPass = [&](int pass, bool colorOn, bool alphaOn, bool depthOn) {
                    gl::Uniform4i(U.mode, s.fge, bits(s.fba, 0, 1), C == 2 ? 2 : 0, pass);
                    gl::ColorMask(colorOn && mr, colorOn && mg, colorOn && mb, colorOn && alphaOn && ma);
                    gl::DepthMask(useDepth && depthOn && !s.zmsk);
                    if (md && I.mdi)
                    {
                        static const GLenum kMode[3] = {GL_TRIANGLES, GL_TRIANGLE_STRIP, GL_TRIANGLE_FAN};
                        gl::Enable(GL_DEPTH_CLAMP);
                        for (int k = 0; k < 6; ++k)
                            gl::Enable(GL_CLIP_DISTANCE0 + k);
                        if (manual)
                            for (const Impl::DrawIndirect &d : I.mdCmds)
                            {
                                const ModelDraw &gm = b.models[d.baseInstance];
                                const GLenum mode = kMode[std::min<uint32_t>(gm.mode, 2u)];
                                if (gm.mode == 2u)
                                {
                                    gl::DrawArraysInstancedBaseInstance(mode, d.baseVertex, static_cast<GLsizei>(gm.n), 1, d.baseInstance);
                                    continue;
                                }
                                for (uint32_t k = 0; k + 3 <= gm.n; k += gm.mode == 0u ? 3u : 1u)
                                {
                                    gl::TextureBarrier();
                                    gl::DrawArraysInstancedBaseInstance(mode, d.baseVertex + static_cast<GLint>(k), 3, 1, d.baseInstance);
                                }
                            }
                        else
                            gl::MultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT, nullptr, static_cast<GLsizei>(I.mdCmds.size()), 0);
                        gl::Disable(GL_DEPTH_CLAMP);
                        for (int k = 0; k < 6; ++k)
                            gl::Disable(GL_CLIP_DISTANCE0 + k);
                    }
                    else if (md)
                    {
                        static const GLenum kMode[3] = {GL_TRIANGLES, GL_TRIANGLE_STRIP, GL_TRIANGLE_FAN};
                        const GLenum mode = kMode[std::min<uint32_t>(md->mode, 2u)];
                        gl::Enable(GL_DEPTH_CLAMP);
                        for (int k = 0; k < 6; ++k)
                            gl::Enable(GL_CLIP_DISTANCE0 + k);
                        if (manual && md->mode != 2u)
                            for (uint32_t k = 0; k + 3 <= md->n; k += md->mode == 0u ? 3u : 1u)
                            {
                                gl::TextureBarrier();
                                gl::DrawArrays(mode, static_cast<GLint>(k), 3);
                            }
                        else
                            gl::DrawArrays(mode, 0, static_cast<GLsizei>(md->n));
                        gl::Disable(GL_DEPTH_CLAMP);
                        for (int k = 0; k < 6; ++k)
                            gl::Disable(GL_CLIP_DISTANCE0 + k);
                    }
                    else if (manual)
                        for (uint32_t k = 0; k + 3 <= c.b; k += 3)
                        {
                            gl::TextureBarrier();
                            gl::DrawArrays(GL_TRIANGLES, c.a + k, 3);
                        }
                    else
                        gl::DrawArrays(GL_TRIANGLES, c.a, c.b);
                    ++m_stats.draws;
                };
                if (!ate || atst == 1)
                    drawPass(3, true, true, true);
                else if (afail == 0)
                    drawPass(0, true, true, true);
                else
                {
                    const bool fbC = afail == 1 || afail == 3, fbA = afail == 1, zW = afail == 2;
                    if (atst == 0)
                    {
                        gl::Uniform4i(U.atest, 0, 0, 0, 0);
                        drawPass(3, fbC, fbA, zW);
                    }
                    else
                    {
                        drawPass(1, true, true, true);
                        drawPass(2, fbC, fbA, zW);
                    }
                }
                T.lastWrite = I.seq;
                if (md)
                    I.markDirty(T, s.sx0, s.sy0, sx1 + 1, sy1 + 1);
                else
                {
                    const Vertex *v = &b.vertices[c.a];
                    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
                    for (uint32_t k = 0; k < c.b; ++k)
                    {
                        x0 = std::min(x0, v[k].x); x1 = std::max(x1, v[k].x);
                        y0 = std::min(y0, v[k].y); y1 = std::max(y1, v[k].y);
                    }
                    I.markDirty(T, std::max<int>(static_cast<int>(std::floor(x0)), s.sx0), std::max<int>(static_cast<int>(std::floor(y0)), s.sy0),
                                std::min<int>(static_cast<int>(std::ceil(x1)) + 1, sx1 + 1), std::min<int>(static_cast<int>(std::ceil(y1)) + 1, sy1 + 1));
                }
                break;
            }
            }
        }
        I.evictTextures();
    }

    namespace
    {
        std::atomic<int> g_deinterlace{3};   // Bob, bottom field first
    }
    void SetDeinterlaceMode(int mode) { g_deinterlace.store(std::clamp(mode, 0, 5), std::memory_order_relaxed); }

    bool Renderer::present(const DisplayRegs &r, uint32_t &tex, uint32_t &w, uint32_t &h)
    {
        Impl &I = *m;
        const bool en1 = r.pmode & 1, en2 = (r.pmode >> 1) & 1;
        if (!en1 && !en2)
            return false;
        const uint64_t dispfb = en1 ? r.dispfb1 : r.dispfb2;
        const uint64_t display = en1 ? r.display1 : r.display2;
        const uint32_t fbp = bits(dispfb, 0, 9), fbw = bits(dispfb, 9, 6);
        const int dcls = Impl::formatClass(bits(dispfb, 15, 5));
        const uint32_t dbx = bits(dispfb, 32, 11), dby = bits(dispfb, 43, 11);
        const uint32_t magh = bits(display, 23, 4) + 1, dw = bits(display, 32, 12) + 1;
        // Interlaced field mode (SMODE2 INT=1, FFMD=1): DH counts display lines, the buffer holds half of them.
        const uint32_t dh = (bits(display, 44, 11) + 1) >> ((r.smode2 & 3) == 3 ? 1 : 0);
        const int t = I.findTarget(fbp, fbw, dcls < 0 ? 0 : dcls);
        if (t < 0)
            return false;
        I.refreshTarget(t);
        const Impl::Target &T = I.targets[t];
        uint32_t outW = std::min<uint32_t>(dw / magh, T.w - std::min(dbx, T.w - 1));
        uint32_t outH = std::min<uint32_t>(dh, T.maxY + 1 - std::min(dby, T.maxY));
        if (outH > 448 && T.maxY + 1 <= 448)
            outH = T.maxY + 1;
        if (!outW || !outH)
            return false;
        const uint32_t S = I.scale;
        if (I.displayW != outW * S || I.displayH != outH * S || !I.displayTex)
        {
            auto makeTex = [&](GLuint &t, GLuint &fbo) {
                if (t)
                    gl::DeleteTextures(1, &t);
                if (!fbo)
                    gl::GenFramebuffers(1, &fbo);
                gl::GenTextures(1, &t);
                gl::BindTexture(GL_TEXTURE_2D, t);
                gl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, outW * S, outH * S, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                gl::BindFramebuffer(GL_FRAMEBUFFER, fbo);
                gl::FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
                gl::ClearColor(0, 0, 0, 1);
                gl::Clear(GL_COLOR_BUFFER_BIT);
            };
            makeTex(I.displayTex, I.displayFbo);
            makeTex(I.fieldTex[0], I.fieldFbo[0]);
            makeTex(I.fieldTex[1], I.fieldFbo[1]);
            I.displayW = outW * S;
            I.displayH = outH * S;
        }
        // The field picture, then the deinterlaced one from it and the previous field.
        const int cur = I.fieldIdx;
        I.fieldIdx ^= 1;
        gl::BindFramebuffer(GL_FRAMEBUFFER, I.fieldFbo[cur]);
        gl::Viewport(0, 0, I.displayW, I.displayH);
        gl::Disable(GL_SCISSOR_TEST);
        gl::Disable(GL_DEPTH_TEST);
        gl::Disable(GL_BLEND);
        gl::Disable(GL_CULL_FACE);
        gl::ColorMask(1, 1, 1, 1);
        gl::UseProgram(I.blitProg);
        gl::Uniform4f(I.bSrc, static_cast<float>(dbx) / T.w, static_cast<float>(dby) / T.h,
                      static_cast<float>(dbx + outW) / T.w, static_cast<float>(dby + outH) / T.h);
        gl::Uniform1i(I.bOpaque, 1);
        gl::Uniform1i(I.bTex, 0);
        gl::ActiveTexture(GL_TEXTURE0);
        gl::BindTexture(GL_TEXTURE_2D, T.tex);
        gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        gl::BindVertexArray(I.quadVao);
        gl::DrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        gl::BindFramebuffer(GL_FRAMEBUFFER, I.displayFbo);
        gl::UseProgram(I.deintProg);
        gl::Uniform4f(I.dSrc, 0.0f, 0.0f, 1.0f, 1.0f);
        gl::Uniform1i(I.dCur, 0);
        gl::Uniform1i(I.dPrev, 1);
        gl::Uniform1i(I.dMode, g_deinterlace.load(std::memory_order_relaxed));
        gl::Uniform1i(I.dField, static_cast<int>(r.field & 1u));
        gl::Uniform1f(I.dLines, static_cast<float>(outH));
        gl::ActiveTexture(GL_TEXTURE1);
        gl::BindTexture(GL_TEXTURE_2D, I.fieldTex[cur ^ 1]);
        gl::ActiveTexture(GL_TEXTURE0);
        gl::BindTexture(GL_TEXTURE_2D, I.fieldTex[cur]);
        gl::DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        tex = I.displayTex;
        w = I.displayW;
        h = I.displayH;
        return true;
    }

    void Renderer::restoreHostState(int screenW, int screenH)
    {
        gl::BindFramebuffer(GL_FRAMEBUFFER, 0);
        gl::Viewport(0, 0, screenW, screenH);
        gl::Disable(GL_SCISSOR_TEST);
        gl::Disable(GL_DEPTH_TEST);
        gl::DepthMask(1);
        gl::ColorMask(1, 1, 1, 1);
        gl::Enable(GL_BLEND);
        gl::BlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
        gl::BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        gl::Enable(GL_CULL_FACE);
        gl::CullFace(GL_BACK);
        gl::UseProgram(0);
        gl::BindVertexArray(0);
        gl::ActiveTexture(GL_TEXTURE0);
        gl::BindTexture(GL_TEXTURE_2D, 0);
        gl::PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    }

    void Renderer::describe(std::FILE *out) const
    {
        for (const auto &t : m->targets)
            std::fprintf(out, "[gsgpu] target fbp=%u fbw=%u fmt=%u %ux%u maxY=%u lastWrite=%llu overwritten=%llu\n", t.fbp, t.fbw, t.cls, t.w, t.h,
                         t.maxY, (unsigned long long)t.lastWrite, (unsigned long long)t.overwritten);
        for (const auto &d : m->depths)
            std::fprintf(out, "[gsgpu] depth zbp=%u %ux%u\n", d.zbp, d.w, d.h);
        std::fprintf(out, "[gsgpu] textures=%zu fbos=%zu depthFills=%llu writebacks=%llu\n", m->texCache.size(), m->fbos.size(),
                     (unsigned long long)m_stats.depthFills, (unsigned long long)m->m_syncs);
    }

    bool Renderer::dumpTarget(uint32_t fbp, uint32_t fbw, std::vector<uint8_t> &rgba, uint32_t &w, uint32_t &h)
    {
        Impl &I = *m;
        int t = I.findTarget(fbp, fbw);
        for (int cls = 1; t < 0 && cls <= 2; ++cls)
            t = I.findTarget(fbp, fbw, cls);
        if (t < 0)
            return false;
        I.refreshTarget(t);
        const Impl::Target &T = I.targets[t];
        w = T.w * I.scale;
        h = T.h * I.scale;
        rgba.resize(static_cast<size_t>(w) * h * 4);
        gl::BindFramebuffer(GL_FRAMEBUFFER, I.fboFor(t, -1));
        gl::PixelStorei(GL_PACK_ALIGNMENT, 1);
        gl::ReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        return true;
    }
}
