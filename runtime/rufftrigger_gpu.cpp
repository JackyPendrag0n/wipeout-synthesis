// PS2Recomp integration of the GPU GS renderer (src/render/GsGpu.*).
//
// The GPU renderer (default; RT_GPU=0 selects the runtime's CPU rasterizer) replaces the CPU
// rasterizer with GpuBackend: the GS frontend (game thread) hands every primitive/transfer/CLUT
// load to a gsgpu::Recorder, and the window thread replays the recorded stream with OpenGL each
// host frame and displays the result (present hook).
// RT_GPU_SCALE=n fixes the internal resolution multiplier; the default "auto" follows the window
// (the smallest multiple of the 448-line PS2 picture that covers the window height, max 8).
// RT_GPU_DUMP=secs writes the display and 3D frame buffers to gpu_*.ppm at that time.
#include <ctime>
#include "GsGpu.h"
#include "GsMemory.h"

#include "ps2_runtime.h"
#include "runtime/gs/gs_backend.h"
#include "runtime/ps2_capture_hook.h"
#include "raylib.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" void *glfwGetProcAddress(const char *procname);

// raylib (built for OpenGL 3.3) asks GLFW for a 3.3 core context, and some drivers (NVIDIA) then
// give exactly 3.3, which leaves out the batched model draws (4.3). The link wraps
// glfwCreateWindow: ask for 4.6, then 4.3, and fall back to raylib's own request.
// RT_GL33=1 keeps raylib's request.
// Only where the linker can wrap symbols (GNU ld: Linux, MinGW); CMake defines RT_WRAP_GLFW_CREATE.
#if defined(RT_WRAP_GLFW_CREATE)
struct GLFWwindow;
struct GLFWmonitor;
extern "C" GLFWwindow *__real_glfwCreateWindow(int w, int h, const char *title, GLFWmonitor *mon, GLFWwindow *share);
extern "C" void glfwWindowHint(int hint, int value);
extern "C" GLFWwindow *__wrap_glfwCreateWindow(int w, int h, const char *title, GLFWmonitor *mon, GLFWwindow *share)
{
    constexpr int kMajor = 0x00022002, kMinor = 0x00022003;   // GLFW_CONTEXT_VERSION_MAJOR / _MINOR
    const char *keep = std::getenv("RT_GL33");
    if (!(keep && std::atoi(keep) != 0))
        for (const int minor : {6, 3})
        {
            glfwWindowHint(kMajor, 4);
            glfwWindowHint(kMinor, minor);
            if (GLFWwindow *win = __real_glfwCreateWindow(w, h, title, mon, share))
                return win;
        }
    glfwWindowHint(kMajor, 3);
    glfwWindowHint(kMinor, 3);
    return __real_glfwCreateWindow(w, h, title, mon, share);
}
#endif

extern std::atomic<long long> g_rtGameThreadClock;
extern std::atomic<int> g_rtGameThreadCpu;
namespace rufftrigger
{
    extern std::atomic<uint64_t> g_gameFrames;
    bool MovieOverlayGet(uint64_t &seq, std::vector<uint8_t> &rgba, int &w, int &h, float &aspect);
}

namespace
{
    class GpuBackend;
    GpuBackend *g_backend = nullptr;
    PS2Runtime *g_runtime = nullptr;
    gsgpu::Renderer *g_renderer = nullptr;
    gsgpu::Batch g_batch;
    uint32_t g_scale = 2;
    bool g_autoScale = true;
    // Graphics menu requests, applied on the window thread at the next present.
    std::atomic<int> g_scaleRequest{-1};      // 0 = follow the window, n = fixed multiplier
    std::atomic<float> g_hudSquash{-1.0f};    // pending setHudSquash value
    float g_hudSquashNow = 1.0f;
    bool g_hudSquashSet = false;
    // While the game runs above its own frame rate (graphics menu), each game frame is shown when
    // the game finishes it rather than at the guest vblank (see GpuGameFrame).
    std::atomic<int64_t> g_gameFrameMarkNs{0};

    uint32_t AutoScale(int screenH)
    {
        const int s = (std::max(1, screenH) + 447) / 448;
        return static_cast<uint32_t>(std::clamp(s, 1, 8));
    }
    double g_dumpAt = -1;
    std::chrono::steady_clock::time_point g_start;
    uint64_t g_frames = 0;

    class GpuBackend final : public GSRasterBackend
    {
    public:
        gsgpu::Recorder recorder;
        uint8_t *vram = nullptr;
        gsmem::Transfer xfer;
        std::atomic<uint64_t> prims{0}, unsupported{0};
        static bool envSet(const char *n) { const char *v = std::getenv(n); return v && *v; }
        bool shadowVram = envSet("RT_CAPTURE") || envSet("RT_GPU_SHADOW");

        void Initialize(uint8_t *v, uint32_t) override { vram = v; }
        void Reset() override {}

        static gsgpu::State convert(const GSDrawState &d)
        {
            gsgpu::State s{};
            std::memset(&s, 0, sizeof(s));
            const GSContext &c = d.context;
            // Every primitive reaches the renderer as triangles with its colours already resolved
            // (flat shading copies the provoking vertex's colour), so the primitive type and IIP
            // are left out: draws that differ only there merge into one GPU draw.
            static const bool keepPrim = std::getenv("RT_GSMERGE") && std::strcmp(std::getenv("RT_GSMERGE"), "0") == 0;
            if (keepPrim)
            {
                s.type = d.prim.type;
                s.iip = d.prim.iip;
            }
            s.tme = d.prim.tme; s.fge = d.prim.fge;
            s.abe = d.prim.abe; s.aa1 = d.prim.aa1; s.fst = d.prim.fst;
            s.fbp = static_cast<uint16_t>(c.frame.fbp); s.fbw = static_cast<uint8_t>(c.frame.fbw);
            s.fpsm = c.frame.psm; s.fbmsk = c.frame.fbmsk;
            s.zbp = static_cast<uint16_t>(c.zbuf.zbp); s.zpsm = c.zbuf.psm; s.zmsk = c.zbuf.zmask;
            if (s.tme)
            {
                s.tbp0 = static_cast<uint16_t>(c.tex0.tbp0); s.tbw = c.tex0.tbw; s.tpsm = c.tex0.psm;
                s.tw = c.tex0.tw; s.th = c.tex0.th; s.tcc = c.tex0.tcc; s.tfx = c.tex0.tfx;
                s.cbp = static_cast<uint16_t>(c.tex0.cbp); s.cpsm = c.tex0.cpsm; s.csm = c.tex0.csm; s.csa = c.tex0.csa;
                s.tex1 = c.tex1; s.miptbp1 = c.miptbp1; s.miptbp2 = c.miptbp2; s.clamp = c.clamp;
                s.ta0 = d.texa.ta0; s.aem = d.texa.aem; s.ta1 = d.texa.ta1;
            }
            s.alpha = d.prim.abe ? c.alpha : 0;
            s.test = c.test;
            s.fba = c.fba;
            s.sx0 = c.scissor.x0; s.sx1 = c.scissor.x1; s.sy0 = c.scissor.y0; s.sy1 = c.scissor.y1;
            if (d.prim.fge)
            {
                s.fogr = d.fogR; s.fogg = d.fogG; s.fogb = d.fogB;
            }
            s.colclamp = static_cast<uint8_t>(d.colclamp & 1);
            s.pabe = d.pabe;
            s.dthe = static_cast<uint8_t>(d.dthe & 1);
            s.notHud = static_cast<uint8_t>(d.portMark & 1);
            s.notHudX = s.notHud ? static_cast<uint16_t>(d.portMark >> 16) : 0;
            return s;
        }

        static gsgpu::Vertex vert(const GSVertex &v, const GSContext &c, uint32_t zpsm, const GSVertex &colorFrom)
        {
            gsgpu::Vertex o;
            o.x = v.x - c.xyoffset.ofx / 16.0f;
            o.y = v.y - c.xyoffset.ofy / 16.0f;
            switch (zpsm & 0xF)
            {
            case 0: o.z = static_cast<float>(v.z / 4294967296.0); break;
            case 1: o.z = static_cast<float>(std::fmod(v.z, 16777216.0) / 16777216.0); break;
            default: o.z = static_cast<float>(std::fmod(v.z, 65536.0) / 65536.0); break;
            }
            o.r = colorFrom.r; o.g = colorFrom.g; o.b = colorFrom.b; o.a = colorFrom.a;
            o.s = v.s; o.t = v.t; o.q = v.q;
            o.u = v.u / 16.0f; o.v = v.v / 16.0f;
            o.fog = v.fog;
            return o;
        }

        void Submit(const GSPrimitiveBatch &b) override
        {
            prims.fetch_add(1, std::memory_order_relaxed);
            const gsgpu::State s = convert(b.state);
            const GSContext &c = b.state.context;
            const uint32_t zpsm = c.zbuf.psm;
            switch (b.state.prim.type)
            {
            case GS_PRIM_TRIANGLE:
            case GS_PRIM_TRISTRIP:
            case GS_PRIM_TRIFAN:
            {
                if (b.vertexCount < 3)
                    return;
                const bool flat = !b.state.prim.iip;
                gsgpu::Vertex v[3];
                for (int k = 0; k < 3; ++k)
                    v[k] = vert(b.vertices[k], c, zpsm, flat ? b.vertices[2] : b.vertices[k]);
                if (listCapture)
                    listCapture->insert(listCapture->end(), v, v + 3);
                recorder.triangle(s, v);
                break;
            }
            case GS_PRIM_SPRITE:
            {
                if (b.vertexCount < 2)
                    return;
                const gsgpu::Vertex v0 = vert(b.vertices[0], c, zpsm, b.vertices[1]);
                const gsgpu::Vertex v1 = vert(b.vertices[1], c, zpsm, b.vertices[1]);
                recorder.sprite(s, v0, v1);
                break;
            }
            case GS_PRIM_LINE:
            case GS_PRIM_LINESTRIP:
            {
                // Drawn as a quad one pixel wide around the segment.
                if (b.vertexCount < 2)
                    return;
                const bool flat = !b.state.prim.iip;
                const gsgpu::Vertex a = vert(b.vertices[0], c, zpsm, flat ? b.vertices[1] : b.vertices[0]);
                const gsgpu::Vertex e = vert(b.vertices[1], c, zpsm, b.vertices[1]);
                const float dx = e.x - a.x, dy = e.y - a.y;
                const float len = std::sqrt(dx * dx + dy * dy);
                if (len <= 0.0f)
                    return;
                const float nx = -dy / len * 0.5f, ny = dx / len * 0.5f;
                gsgpu::Vertex q[4] = {a, a, e, e};
                q[0].x += nx; q[0].y += ny; q[1].x -= nx; q[1].y -= ny;
                q[2].x += nx; q[2].y += ny; q[3].x -= nx; q[3].y -= ny;
                gsgpu::Vertex t0[3] = {q[0], q[1], q[2]}, t1[3] = {q[1], q[3], q[2]};
                recorder.triangle(s, t0);
                recorder.triangle(s, t1);
                break;
            }
            case GS_PRIM_POINT:
            {
                if (b.vertexCount < 1)
                    return;
                gsgpu::Vertex v0 = vert(b.vertices[0], c, zpsm, b.vertices[0]), v1 = v0;
                v1.x += 1.0f; v1.y += 1.0f;
                recorder.sprite(s, v0, v1);
                break;
            }
            default:
            {
                unsupported.fetch_add(1, std::memory_order_relaxed);
                break;
            }
            }
        }

        // Model batches (GPU transform): the draw state and the program's matrices; the mesh bytes
        // go along the first time a key is seen (and again after the GL side dropped its meshes).
        std::unordered_map<uint64_t, uint32_t> meshSent;   // key -> mesh generation it was sent in
        bool SubmitModel(const GSDrawState &st, const GSModelPacket &m, const uint8_t *data, uint8_t fog) override
        {
            const uint32_t type = st.prim.type;
            if (type != GS_PRIM_TRIANGLE && type != GS_PRIM_TRISTRIP && type != GS_PRIM_TRIFAN)
                return false;
            prims.fetch_add(m.n, std::memory_order_relaxed);
            const gsgpu::State s = convert(st);
            const GSContext &c = st.context;
            gsgpu::ModelDraw md{};
            md.key = m.key;
            md.n = m.n;
            md.stride = m.stride;
            md.mode = type == GS_PRIM_TRIANGLE ? 0u : type == GS_PRIM_TRISTRIP ? 1u : 2u;
            md.flags = (m.flags & GSModelLit ? 1u : 0u) | (m.flags & GSModelXyzf ? 2u : 0u) | (m.flags & GSModelClip ? 4u : 0u) |
                       (st.prim.iip ? 0u : 8u) | (m.flags & GSModelEnv ? 16u : 0u);
            static const bool noClip = envSet("RT_GPUMODELNOCLIP");
            if (noClip)
                md.flags &= ~4u;
            md.zpsm = c.zbuf.psm;
            md.ofx = c.xyoffset.ofx / 16.0f;
            md.ofy = c.xyoffset.ofy / 16.0f;
            md.fog = fog;
            std::memcpy(md.m, m.m, sizeof(md.m));
            std::memcpy(md.cm, m.cm, sizeof(md.cm));
            std::memcpy(md.l, m.l, sizeof(md.l));
            std::memcpy(md.amb, m.amb, sizeof(md.amb));
            std::memcpy(md.lc, m.lc, sizeof(md.lc));
            // Transient meshes (key bit 63) go with every draw.
            const uint32_t gen = gsgpu::MeshGeneration();
            const bool transient = (m.key >> 63) != 0;
            auto it = transient ? meshSent.end() : meshSent.find(m.key);
            const bool send = transient || it == meshSent.end() || it->second != gen;
            md.dataBytes = send ? m.dataBytes : 0u;
            if (send && !transient)
                meshSent[m.key] = gen;
            recorder.model(s, md, send ? data : nullptr);
            return true;
        }

        // Native strips: one state conversion and one recorder call per packet.
        // RT_GSLISTCHECK=1 also sends each list the old way, one primitive at a time, and compares
        // the vertices the recorder receives.
        std::vector<gsgpu::Vertex> listVerts, *listCapture = nullptr;
        bool listCheck = envSet("RT_GSLISTCHECK");
        uint64_t listChecked = 0, listBad = 0;
        void SubmitList(const GSDrawState &st, const GSVertex *v, uint32_t n, uint32_t per) override
        {
            const uint32_t type = st.prim.type;
            const bool tri = type == GS_PRIM_TRIANGLE || type == GS_PRIM_TRISTRIP || type == GS_PRIM_TRIFAN;
            if (!tri || per != 3)
            {
                GSRasterBackend::SubmitList(st, v, n, per);
                return;
            }
            prims.fetch_add(n, std::memory_order_relaxed);
            const gsgpu::State s = convert(st);
            const GSContext &c = st.context;
            const uint32_t zpsm = c.zbuf.psm;
            const bool flat = !st.prim.iip;
            listVerts.resize(3ull * n);
            for (uint32_t i = 0; i < n; ++i)
                for (int k = 0; k < 3; ++k)
                    listVerts[3 * i + k] = vert(v[3 * i + k], c, zpsm, flat ? v[3 * i + 2] : v[3 * i + k]);
            if (!listCheck)
            {
                recorder.triangles(s, listVerts.data(), 3 * n);
                return;
            }
            prims.fetch_sub(n, std::memory_order_relaxed);
            std::vector<gsgpu::Vertex> old;
            listCapture = &old;
            GSRasterBackend::SubmitList(st, v, n, per);
            listCapture = nullptr;
            ++listChecked;
            if (old.size() != listVerts.size() || std::memcmp(old.data(), listVerts.data(), old.size() * sizeof(gsgpu::Vertex)) != 0)
                ++listBad;
            if ((listChecked & 0xFFFF) == 0)
                std::fprintf(stderr, "[gslist] %llu lists checked, %llu differ\n", (unsigned long long)listChecked, (unsigned long long)listBad);
        }

        void LoadClut(const GSTex0Reg &t, const GSTexClutReg &tc) override
        {
            gsgpu::Tex0Event e{};
            e.cbp = static_cast<uint16_t>(t.cbp); e.psm = t.psm; e.cpsm = t.cpsm; e.csm = t.csm; e.csa = t.csa;
            e.cld = t.cld; e.cbw = tc.cbw; e.cou = tc.cou; e.cov = tc.cov;
            recorder.tex0(e);
        }

        void BeginTransfer(const GSTransferCommand &cmd) override
        {
            if (cmd.direction == 1)
            {
                // GS -> host (the game reads the Z buffer around the sun to dim its glare when
                // something is in front). The GPU answers a frame or two late: the result of the
                // last replayed read of the same buffer and size; zeros until there is one.
                const auto &bb = cmd.bitbltbuf;
                const uint32_t w = cmd.trxreg.rrw, h = cmd.trxreg.rrh;
                recorder.readback(bb.sbp, bb.sbw, bb.spsm, cmd.trxpos.ssax, cmd.trxpos.ssay, w, h);
                readbackPos = 0;
                if (!gsgpu::ReadbackResult(bb.sbp, bb.sbw, bb.spsm, w, h, readbackBytes))
                {
                    const uint32_t zf = bb.spsm & 0xFu;
                    const uint32_t bytesPer = (bb.spsm & 0x30u) != 0x30u ? 4u : zf == 0 ? 4u : zf == 1 ? 3u : 2u;
                    readbackBytes.assign(static_cast<size_t>(w) * h * bytesPer, 0);
                }
                // RT_XFERLOG=1: every 16th readback with how much of it is non-zero.
                static const bool xferLog = std::getenv("RT_XFERLOG") != nullptr;
                static uint32_t seen = 0;
                if (xferLog && (seen++ & 15) == 0)
                {
                    uint32_t nz = 0;
                    for (uint8_t v : readbackBytes) nz += v != 0;
                    std::fprintf(stderr, "[gsgpu] readback sbp %u sbw %u spsm %#x at %u,%u %ux%u: %zu bytes, %u nonzero\n", bb.sbp, bb.sbw, bb.spsm,
                                 cmd.trxpos.ssax, cmd.trxpos.ssay, w, h, readbackBytes.size(), nz);
                }
                xfer.active = false;
                return;
            }
            if (cmd.direction != 0)
            {
                xfer.active = false;
                unsupported.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            const auto &bb = cmd.bitbltbuf;
            xfer.begin(bb.dbp, bb.dbw, bb.dpsm, cmd.trxpos.dsax, cmd.trxpos.dsay, cmd.trxreg.rrw, cmd.trxreg.rrh);
            recorder.transferBegin(bb.dbp, bb.dbw, bb.dpsm, cmd.trxpos.dsax, cmd.trxpos.dsay, cmd.trxreg.rrw, cmd.trxreg.rrh);
        }

        void UploadImage(const uint8_t *data, uint32_t size) override
        {
            if (!xfer.active || !vram)
                return;
            // The renderer keeps its own copy of GS memory; the runtime's copy is only read by
            // captures and debug views, so skip that (costly, swizzled) write on the game thread.
            if (shadowVram)
                xfer.data(vram, data, size);
            recorder.transferData(data, size);
        }

        void Flush() override {}
        void TextureFlush() override {}
        void Sync(GSSyncReason) override {}
        PresentationFrame Present(const GSPresentationRequest &) override { return {}; }

        bool ClearFramebuffer(const GSContext &c, uint32_t rgba) override
        {
            GSDrawState d{};
            d.context = c;
            recorder.clear(convert(d), rgba);
            return true;
        }
        uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override
        {
            if (!dst || readbackPos >= readbackBytes.size())
                return 0;
            const size_t n = std::min<size_t>(maxBytes, readbackBytes.size() - readbackPos);
            std::memcpy(dst, readbackBytes.data() + readbackPos, n);
            readbackPos += n;
            return static_cast<uint32_t>(n);
        }
        std::vector<uint8_t> readbackBytes;   // the pending GS -> host transfer
        size_t readbackPos = 0;
        uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override
        {
            return vram ? gsmem::readPixel(vram, psm, base, bw, x, y) : 0;
        }
        void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override
        {
            if (vram)
                gsmem::writePixel(vram, psm, base, bw, x, y, value);
        }
        void SnapshotVram(std::vector<uint8_t> &out) const override
        {
            if (vram)
                out.assign(vram, vram + gsmem::kVramSize);
        }
        GSTransferSnapshot GetTransferSnapshot() const override
        {
            GSTransferSnapshot t{};
            t.x = xfer.x;
            t.y = xfer.y;
            t.totalPixels = xfer.rrw * xfer.rrh;
            t.copiedPixels = xfer.y * xfer.rrw + xfer.x;
            t.direction = xfer.active ? 0u : 3u;
            return t;
        }
    };

    void writePpm(const char *path, const std::vector<uint8_t> &rgba, uint32_t w, uint32_t h)
    {
        std::FILE *f = std::fopen(path, "wb");
        if (!f)
            return;
        std::fprintf(f, "P6\n%u %u\n255\n", w, h);
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
                std::fwrite(&rgba[(static_cast<size_t>(y) * w + x) * 4], 1, 3, f);
        std::fclose(f);
    }

    // While a movie plays its picture replaces the game's: padded with black so that, drawn at
    // the window's display aspect (RT_ASPECT), the movie keeps its own aspect.
    Texture2D g_movieTex{};
    uint64_t g_movieSeq = 0;
    std::vector<uint8_t> g_moviePix, g_moviePad;
    int g_movieW = 0, g_movieH = 0;
    float g_movieAspect = 4.0f / 3.0f;

    float DisplayAspect(int screenW, int screenH)
    {
        // Set from the graphics menu: > 0 width/height, -1 stretch, -2 native.
        const float menu = ps2x_capture::g_displayAspect.load(std::memory_order_relaxed);
        if (menu > 0.0f) return menu;
        if (menu == -1.0f) return static_cast<float>(std::max(1, screenW)) / std::max(1, screenH);
        if (menu == -2.0f) return 0.0f;
        const char *a = std::getenv("RT_ASPECT");
        const char *ws = std::getenv("RT_WIDESCREEN");
        const bool wide = !ws || !*ws || std::atoi(ws) != 0;
        if (!a || !*a) return wide ? 16.0f / 9.0f : 4.0f / 3.0f;
        if (!std::strcmp(a, "stretch")) return static_cast<float>(std::max(1, screenW)) / std::max(1, screenH);
        if (!std::strcmp(a, "native")) return 0.0f;
        int x = 0, y = 0;
        return std::sscanf(a, "%d:%d", &x, &y) == 2 && x > 0 && y > 0 ? static_cast<float>(x) / y : 4.0f / 3.0f;
    }

    bool MovieFrame(int screenW, int screenH, uint32_t &tex, uint32_t &w, uint32_t &h)
    {
        const uint64_t before = g_movieSeq;
        if (!rufftrigger::MovieOverlayGet(g_movieSeq, g_moviePix, g_movieW, g_movieH, g_movieAspect) || g_movieW <= 0 || g_movieH <= 0)
            return false;
        float A = DisplayAspect(screenW, screenH);
        if (A <= 0.0f)
            A = g_movieAspect;
        // texture (pw x ph) shown at aspect A; the movie part (mw x mh of it) must show at g_movieAspect
        int pw = g_movieW, ph = g_movieH;
        if (A > g_movieAspect)
            pw = static_cast<int>(std::lround(g_movieW * A / g_movieAspect));
        else
            ph = static_cast<int>(std::lround(g_movieH * g_movieAspect / A));
        if (before != g_movieSeq || g_movieTex.id == 0 || g_movieTex.width != pw || g_movieTex.height != ph)
        {
            g_moviePad.assign(static_cast<size_t>(pw) * ph * 4, 0);
            for (size_t i = 3; i < g_moviePad.size(); i += 4) g_moviePad[i] = 255;
            const int ox = (pw - g_movieW) / 2, oy = (ph - g_movieH) / 2;
            for (int y = 0; y < g_movieH; ++y)
                std::memcpy(&g_moviePad[(static_cast<size_t>(y + oy) * pw + ox) * 4], &g_moviePix[static_cast<size_t>(y) * g_movieW * 4], static_cast<size_t>(g_movieW) * 4);
            if (g_movieTex.id == 0 || g_movieTex.width != pw || g_movieTex.height != ph)
            {
                if (g_movieTex.id)
                    UnloadTexture(g_movieTex);
                Image img{g_moviePad.data(), pw, ph, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
                g_movieTex = LoadTextureFromImage(img);
                SetTextureFilter(g_movieTex, TEXTURE_FILTER_BILINEAR);
            }
            else
                UpdateTexture(g_movieTex, g_moviePad.data());
        }
        tex = g_movieTex.id;
        w = static_cast<uint32_t>(pw);
        h = static_cast<uint32_t>(ph);
        return true;
    }

    // RT_FPSLOG=1: every 2 s, host and game frame pacing.
    //   host: presents per second, longest gap between presents, time spent replaying the GS
    //   game: display-buffer flips per second, guest vsyncs per second, longest flip interval
    struct FpsLog
    {
        bool on = false;
        std::chrono::steady_clock::time_point t0, lastPresent, lastFlip;
        uint64_t presents = 0, flips = 0, vsync0 = 0, lastDispfb = ~0ull;
        double maxGap = 0, maxFlipGap = 0, replayMs = 0, maxReplay = 0;
        uint64_t slowFlips = 0;   // flips more than 25 ms apart
        uint64_t frames0 = 0, waits0 = 0, draws0 = 0, manual0 = 0, uploads0 = 0, readbacks0 = 0, tupl0 = 0;
        uint64_t lastVs = 0, perPresent[3] = {0, 0, 0};   // presents that showed 0 / 1 / 2+ new guest vsyncs
    } g_fps;

    // RT_BENCH=from:to  prints the wall time the guest took from vsync `from` to vsync `to`.
    void BenchTick()
    {
        static uint64_t from = 0, to = 0;
        static int state = -1;
        static std::chrono::steady_clock::time_point t0;
        if (state == -1)
        {
            const char *b = std::getenv("RT_BENCH");
            unsigned long long f = 0, t = 0;
            state = b && std::sscanf(b, "%llu:%llu", &f, &t) == 2 && t > f ? 0 : 3;
            from = f;
            to = t;
        }
        if (state >= 2)
            return;
        const uint64_t vs = g_runtime->memory().gs().vsyncTick.load(std::memory_order_relaxed);
        if (state == 0 && vs >= from)
        {
            t0 = std::chrono::steady_clock::now();
            from = vs;
            state = 1;
        }
        else if (state == 1 && vs >= to)
        {
            const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            std::fprintf(stderr, "[bench] vsync %llu..%llu in %.2f s = %.2f vsync/s\n", (unsigned long long)from,
                         (unsigned long long)vs, s, (vs - from) / s);
            state = 2;
        }
    }

    void FpsTick(double replayMs)
    {
        BenchTick();
        if (!g_fps.on)
            return;
        const auto now = std::chrono::steady_clock::now();
        const GSRegisters &gs = g_runtime->memory().gs();
        const uint64_t vs = gs.vsyncTick.load(std::memory_order_relaxed);
        if (g_fps.presents == 0 && g_fps.t0 == std::chrono::steady_clock::time_point{})
        {
            g_fps.t0 = g_fps.lastPresent = g_fps.lastFlip = now;
            g_fps.vsync0 = vs;
        }
        if (g_fps.lastVs != 0)
            ++g_fps.perPresent[std::min<uint64_t>(vs - g_fps.lastVs, 2)];
        g_fps.lastVs = vs;
        const double gap = std::chrono::duration<double, std::milli>(now - g_fps.lastPresent).count();
        g_fps.maxGap = std::max(g_fps.maxGap, gap);
        g_fps.lastPresent = now;
        ++g_fps.presents;
        g_fps.replayMs += replayMs;
        g_fps.maxReplay = std::max(g_fps.maxReplay, replayMs);
        const uint64_t fb = (gs.pmode & 2) ? gs.dispfb2 : gs.dispfb1;
        if (fb != g_fps.lastDispfb)
        {
            const double fg = std::chrono::duration<double, std::milli>(now - g_fps.lastFlip).count();
            if (g_fps.lastDispfb != ~0ull)
            {
                g_fps.maxFlipGap = std::max(g_fps.maxFlipGap, fg);
                if (fg > 25.0) ++g_fps.slowFlips;
                ++g_fps.flips;
            }
            g_fps.lastDispfb = fb;
            g_fps.lastFlip = now;
        }
        const double el = std::chrono::duration<double>(now - g_fps.t0).count();
        if (el >= 2.0)
        {
            const uint64_t gf = rufftrigger::g_gameFrames.load(std::memory_order_relaxed);
            const uint64_t wt = g_backend->recorder.waits();
            const gsgpu::Stats &st = g_renderer->stats();
            const double nP = static_cast<double>(std::max<uint64_t>(1, g_fps.presents));
            std::fprintf(stderr, "[fps] presents with 0/1/2+ new vsyncs: %llu/%llu/%llu\n", (unsigned long long)g_fps.perPresent[0],
                         (unsigned long long)g_fps.perPresent[1], (unsigned long long)g_fps.perPresent[2]);
            std::fprintf(stderr, "[fps] recorder waits %llu | per present: draws %.0f manual %.0f uploads %.0f readbacks %.2f target uploads %.2f\n",
                         (unsigned long long)(wt - g_fps.waits0), (st.draws - g_fps.draws0) / nP,
                         (st.manualBlend - g_fps.manual0) / nP, (st.uploads - g_fps.uploads0) / nP,
                         (st.readbacks - g_fps.readbacks0) / nP, (st.targetUploads - g_fps.tupl0) / nP);
            std::fprintf(stderr, "[fps] speed %.0f%% (guest vsync %.1f/s) | window %.1f fps, longest gap %.1f ms, GS replay avg %.2f ms max %.1f ms\n",
                         (vs - g_fps.vsync0) / el / 60.0 * 100.0, (vs - g_fps.vsync0) / el, g_fps.presents / el, g_fps.maxGap,
                         g_fps.replayMs / std::max<uint64_t>(1, g_fps.presents), g_fps.maxReplay);
            {
                // How busy the game thread was and where it ran: below ~95% busy while the speed is
                // under 100% means it waited for something, not that the CPU is too slow.
                static double lastCpu = -1.0;
                const long long cid = g_rtGameThreadClock.load();
                double cpu = -1.0;
                if (cid != 0x7fffffffffffffffLL)
                {
                    timespec ts{};
                    if (clock_gettime(static_cast<clockid_t>(cid), &ts) == 0)
                        cpu = ts.tv_sec + ts.tv_nsec * 1e-9;
                }
                if (cpu >= 0.0 && lastCpu >= 0.0)
                    std::fprintf(stderr, "[fps] game thread busy %.0f%% (on CPU %d) | game frames %.1f/s\n", (cpu - lastCpu) / el * 100.0,
                                 g_rtGameThreadCpu.load(std::memory_order_relaxed), (gf - g_fps.frames0) / el);
                lastCpu = cpu;
            }
            const uint64_t keepFb = g_fps.lastDispfb;
            const auto keepFlip = g_fps.lastFlip;
            g_fps = FpsLog{};
            g_fps.on = true;
            g_fps.t0 = g_fps.lastPresent = now;
            g_fps.lastFlip = keepFlip;
            g_fps.lastDispfb = keepFb;
            g_fps.vsync0 = vs;
            g_fps.presents = 1;
            g_fps.lastVs = vs;
            g_fps.frames0 = gf;
            g_fps.waits0 = wt;
            g_fps.draws0 = st.draws;
            g_fps.manual0 = st.manualBlend;
            g_fps.uploads0 = st.uploads;
            g_fps.readbacks0 = st.readbacks;
            g_fps.tupl0 = st.targetUploads;
        }
    }

    bool PresentHook(int screenW, int screenH, uint32_t &tex, uint32_t &w, uint32_t &h)
    {
        const auto hookStart = std::chrono::steady_clock::now();
        if (const int req = g_scaleRequest.exchange(-1); req >= 0)
        {
            g_autoScale = req == 0;
            if (!g_autoScale)
                g_scale = static_cast<uint32_t>(req);
        }
        if (const float hs = g_hudSquash.exchange(-1.0f); hs > 0.0f)
        {
            g_hudSquashNow = hs;
            g_hudSquashSet = true;
            if (g_renderer)
                g_renderer->setHudSquash(hs);
        }
        // F9 cycles the internal resolution: follow the window, then 1x .. 8x.
        if (IsKeyPressed(KEY_F9))
        {
            static const uint32_t kSteps[] = {0, 1, 2, 3, 4, 6, 8};
            const uint32_t cur = g_autoScale ? 0u : g_scale;
            size_t k = 0;
            while (k < std::size(kSteps) && kSteps[k] != cur)
                ++k;
            const uint32_t next = kSteps[(k + 1) % std::size(kSteps)];
            g_autoScale = next == 0;
            if (!g_autoScale)
                g_scale = next;
            std::fprintf(stderr, "[gsgpu] F9: internal resolution %s\n", g_autoScale ? "follows the window" : (std::to_string(next) + "x").c_str());
        }
        if (g_autoScale)
            g_scale = AutoScale(screenH);
        if (g_renderer && g_renderer->scale() != g_scale)
        {
            std::fprintf(stderr, "[gsgpu] internal resolution %ux\n", g_scale);
            g_renderer->setScale(g_scale);
        }
        if (!g_renderer)
        {
            g_renderer = new gsgpu::Renderer();
            const bool ok = g_renderer->init(&glfwGetProcAddress, g_backend->vram, g_scale);
            // Widescreen (RT_WIDESCREEN, default on): keep 2D overlays 4:3-shaped unless RT_WIDE_HUD=0.
            const char *ws = std::getenv("RT_WIDESCREEN"), *wh = std::getenv("RT_WIDE_HUD");
            if (!g_hudSquashSet && (!ws || !*ws || std::atoi(ws) != 0) && (!wh || !*wh || std::atoi(wh) != 0))
                g_hudSquashNow = 0.75f;
            if (ok)
                g_renderer->setHudSquash(g_hudSquashNow);
            // RT_GPUMODELS=0: the VU1 programs' transform stays on the CPU (native programs).
            if (ok && g_renderer->modelsOk())
            {
                g_gsModelCapable.store(true);
                const char *gm = std::getenv("RT_GPUMODELS");
                g_gsModelSupport.store(!(gm && *gm && std::atoi(gm) == 0));
            }
            if (!ok)
            {
                std::fprintf(stderr, "[gsgpu] init failed, falling back to the CPU renderer\n");
                ps2x_capture::g_presentHook.store(nullptr);
                return false;
            }
        }
        g_backend->recorder.take(g_batch);
        static const bool noReplay = std::getenv("RT_GPU_NOREPLAY") && *std::getenv("RT_GPU_NOREPLAY");   // profiling only
        if (!noReplay)
            g_renderer->replay(g_batch);
        // Show the frame as it stood at the last guest vblank. The recorder publishes in chunks
        // while the game draws, so presenting the live frame buffer here showed half-drawn
        // frames (flicker and jitter, worst when the game runs below full speed).
        bool ok = !noReplay && g_renderer->lastFrame(tex, w, h);
        if (!ok)
        {
            const GSRegisters &gs = g_runtime->memory().gs();
            gsgpu::DisplayRegs regs{gs.pmode, gs.dispfb1, gs.display1, gs.dispfb2, gs.display2, gs.bgcolor};
            ok = g_renderer->present(regs, tex, w, h);
        }
        ++g_frames;
        if (g_dumpAt >= 0 && std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count() >= g_dumpAt)
        {
            g_dumpAt = -1;
            std::vector<uint8_t> px;
            uint32_t dw = 0, dh = 0;
            if (g_renderer->dumpTarget(0, 10, px, dw, dh))
                writePpm("gpu_display.ppm", px, dw, dh);
            if (g_renderer->dumpTarget(150, 8, px, dw, dh))
                writePpm("gpu_fb150.ppm", px, dw, dh);
            g_renderer->describe(stderr);
            const gsgpu::Stats &st = g_renderer->stats();
            std::fprintf(stderr, "[gsgpu] dump: draws=%llu runs=%llu verts=%llu uploads=%llu rtSamples=%llu unsupported=%llu\n",
                         (unsigned long long)st.draws, (unsigned long long)st.runs, (unsigned long long)st.vertices,
                         (unsigned long long)st.uploads, (unsigned long long)st.rtSamples, (unsigned long long)st.unsupported);
        }
        g_renderer->restoreHostState(screenW, screenH);
        FpsTick(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - hookStart).count());
        if (MovieFrame(screenW, screenH, tex, w, h))
            return true;
        return ok;
    }
}

namespace rufftrigger
{
    // Called from the game override at startup.
    void InstallGpuRenderer(PS2Runtime &runtime)
    {
        const char *env = std::getenv("RT_GPU");
        if (env && *env && std::atoi(env) == 0)
            return;
        if (const char *s = std::getenv("RT_GPU_SCALE"); s && *s && std::strcmp(s, "auto") != 0)
        {
            g_scale = static_cast<uint32_t>(std::max(1, std::atoi(s)));
            g_autoScale = false;
        }
        if (const char *d = std::getenv("RT_GPU_DUMP"); d && *d)
            g_dumpAt = std::atof(d);
        g_start = std::chrono::steady_clock::now();
        g_fps.on = std::getenv("RT_FPSLOG") != nullptr;
        g_runtime = &runtime;
        auto backend = std::make_unique<GpuBackend>();
        g_backend = backend.get();
        // About two frames of this game's geometry.
        backend->recorder.setLimit(std::getenv("RT_GPU_QUEUE") ? std::strtoul(std::getenv("RT_GPU_QUEUE"), nullptr, 10) : 400000);
        runtime.gs().setRasterBackend(std::move(backend));
        ps2x_capture::g_presentHook.store(&PresentHook, std::memory_order_release);
        ps2x_capture::g_vblankHook.store([] {
            if (!g_backend)
                return;
            // Frames are marked by GpuGameFrame while the game runs unlocked; fall back to the
            // vblank (loading screens, movies) once it has stopped calling.
            const int64_t mark = g_gameFrameMarkNs.load(std::memory_order_relaxed);
            if (mark && std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() - mark < 100000000)
                return;
            const GSRegisters &gs = g_runtime->memory().gs();
            g_backend->recorder.frame(gsgpu::DisplayRegs{gs.pmode, gs.dispfb1, gs.display1, gs.dispfb2, gs.display2, gs.bgcolor, gs.smode2, static_cast<uint32_t>(gs.vsyncTick.load(std::memory_order_relaxed) & 1u)});
        }, std::memory_order_release);
        if (g_autoScale)
            std::fprintf(stderr, "[gsgpu] GPU renderer installed (internal resolution follows the window)\n");
        else
            std::fprintf(stderr, "[gsgpu] GPU renderer installed (scale %ux)\n", g_scale);
    }

    uint64_t GpuRendererWaits() { return g_backend ? g_backend->recorder.waits() : 0; }

    // ---- graphics menu (rufftrigger_settings.cpp) ----------------------------------------------
    bool GpuRendererActive() { return g_backend != nullptr; }
    // 0 = follow the window, else a fixed multiplier (1..8).
    void GpuSetScale(int scale) { g_scaleRequest.store(std::clamp(scale, 0, 8)); }
    int GpuScaleSetting() { return g_autoScale ? 0 : static_cast<int>(g_scale); }
    uint32_t GpuCurrentScale() { return g_scale; }
    void GpuSetHudSquash(float k) { g_hudSquash.store(k > 0.0f ? k : 1.0f); }
    // Game thread, when a game frame is finished (the game's frame limiter): show it.
    void GpuGameFrame()
    {
        if (!g_backend)
            return;
        g_gameFrameMarkNs.store(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(),
                                std::memory_order_relaxed);
        const GSRegisters &gs = g_runtime->memory().gs();
        g_backend->recorder.frame(gsgpu::DisplayRegs{gs.pmode, gs.dispfb1, gs.display1, gs.dispfb2, gs.display2, gs.bgcolor, gs.smode2, static_cast<uint32_t>(gs.vsyncTick.load(std::memory_order_relaxed) & 1u)});
    }
    void GpuStopGameFrames() { g_gameFrameMarkNs.store(0, std::memory_order_relaxed); }

    void GpuRendererStats(uint64_t &prims, uint64_t &unsupported, uint64_t &frames)
    {
        prims = g_backend ? g_backend->prims.load() : 0;
        unsupported = g_backend ? g_backend->unsupported.load() : 0;
        frames = g_frames;
    }
}
