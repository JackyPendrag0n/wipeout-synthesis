// GPU renderer for the PS2 GS command stream (OpenGL 3.3).
//
// The game thread records draws, VRAM transfers and CLUT loads (Recorder); the thread that
// owns the GL context replays them in order (Renderer) into upscaled render targets and
// composites the displayed frame buffer. A shadow copy of GS local memory is kept on the GL
// side so textures are decoded from VRAM exactly as it was at draw time.
//
// Scope: what Ruff Trigger uses (triangles/strips/fans/sprites, Gouraud, STQ/UV texturing with
// CLUT formats and mipmaps, fog, Z24/Z32/Z16 tests, alpha test incl. FB_ONLY, the common
// alpha blend equations, FBA, frame buffer masks, render-to-texture with CT32 targets).
#pragma once

#include <cstdint>
#include <cstdio>
#include <memory>
#include <condition_variable>
#include <mutex>
#include <vector>

namespace gsgpu
{
    struct Vertex
    {
        float x, y;        // window coordinates (XYOFFSET removed), GS pixels
        float z;           // depth normalized to [0,1] for the Z buffer format
        float r, g, b, a;  // 0..255
        float s, t, q;     // STQ
        float u, v;        // UV in texels (FST)
        float fog;         // 0..255
    };

    // Draw state; compared bytewise to merge consecutive primitives, so always value-initialize.
    struct State
    {
        uint8_t type, iip, tme, fge, abe, aa1, fst, pad0;
        uint16_t fbp; uint8_t fbw, fpsm; uint32_t fbmsk;
        uint16_t zbp; uint8_t zpsm, zmsk;
        uint16_t tbp0; uint8_t tbw, tpsm, tw, th, tcc, tfx;
        uint16_t cbp; uint8_t cpsm, csm, csa, pad1; uint16_t pad2;
        uint64_t tex1, miptbp1, miptbp2, clamp, alpha, test, fba;
        uint16_t sx0, sx1, sy0, sy1;
        uint8_t ta0, aem, ta1, fogr, fogg, fogb, colclamp, pabe;
        uint8_t dthe, notHud;   // notHud: drawn between GS register 0x7E writes of bit 0 = 1 and 0 (a port's marker),
        uint16_t notHudX;       // whose bits 16-31 give its centre x (12.4 fixed point, frame pixels)
        uint8_t pad3[4];
    };

    // A batch of a VU1 drawing program transformed on the GPU (model shader): a mesh of the
    // program's raw input vertices (stride qwords each: position, integer colour, STQ, normal)
    // kept in a GPU buffer by key, and the matrices and lights the program would have used.
    struct ModelDraw
    {
        uint64_t key;
        uint32_t n, stride;
        uint32_t mode;           // 0 triangles, 1 strip, 2 fan
        uint32_t flags;          // 1 lit, 2 XYZF2, 4 VU clip planes, 8 flat shading, 16 env-mapped STQ
        uint32_t dataOff, dataBytes;   // mesh bytes in Batch::data (dataBytes 0: already sent)
        uint32_t zpsm;
        float ofx, ofy, fog;     // XYOFFSET in pixels, FOG for XYZ2
        float m[16], cm[16], l[16], amb[4], lc[16];
    };
    // Bumped when the GL thread drops its mesh store: meshes sent before must be sent again.
    uint32_t MeshGeneration();

    struct Tex0Event
    {
        uint16_t cbp; uint8_t psm, cpsm, csm, csa, cld, cbw, cou; uint16_t cov;
    };

    struct DisplayRegs
    {
        uint64_t pmode, dispfb1, display1, dispfb2, display2, bgcolor;
        uint64_t smode2 = 0; // INT|FFMD=3: field mode, each field reads every other line
        uint32_t field = 0;  // vblank parity of this picture (0 = top/even field)
    };

    // Deinterlacing of the displayed picture, as PCSX2 offers it: 0 off, 1 blend (average of the
    // last two fields), 2/3 bob (top/bottom field first: shift every other field by half a line),
    // 4/5 weave (top/bottom field first: interleave the last two fields line by line).
    void SetDeinterlaceMode(int mode);
    // Called by the game each frame its race HUD is updated: while it keeps coming, a widescreen
    // picture lays the HUD out to the screen edges instead of squashing it about the centre.
    void NoteRaceHud();
    // Turns that layout on (the picture is shown at 16:9 and the player chose it).
    void SetEdgeHud(bool on);
    // Which race HUD the edge layout follows: 0 one player, 1 two players one above the other,
    // 2 two players side by side.
    void SetHudLayout(int layout);

    // The bytes of the most recent replayed readback of this buffer, format and size (any
    // position: the game reads the same small window every frame as it follows the sun), in
    // GS -> host transfer order. False if none was replayed yet. Any thread.
    bool ReadbackResult(uint32_t sbp, uint32_t sbw, uint32_t spsm, uint32_t rrw, uint32_t rrh, std::vector<uint8_t> &out);

    struct Stats
    {
        uint64_t draws = 0, runs = 0, vertices = 0, uploads = 0, textures = 0, rtSamples = 0, unsupported = 0, depthFills = 0, manualBlend = 0, depthInverts = 0, hudDraws = 0, readbacks = 0, targetUploads = 0;
    };

    // Commands recorded on the game thread.
    enum class Op : uint8_t { Draw, TransferBegin, TransferData, Tex0, Clear, Frame, Readback, Model };
    struct Cmd
    {
        Op op;
        uint32_t a, b;          // Draw: first vertex, count. TransferData: data offset, size. Tex0/Clear/Frame/Model: index
        uint32_t state;         // Draw/Clear: state index
        uint32_t x[7];          // TransferBegin: dbp, dbw, dpsm, dsax, dsay, rrw, rrh; Clear: rgba; Readback: sbp, sbw, spsm, ssax, ssay, rrw, rrh
    };

    struct Batch
    {
        std::vector<Cmd> cmds;
        std::vector<State> states;
        std::vector<Vertex> vertices;
        std::vector<uint8_t> data;
        std::vector<Tex0Event> tex0;
        std::vector<DisplayRegs> frames;   // Frame: display registers at the guest vblank
        std::vector<ModelDraw> models;
        size_t modelVertices = 0;          // for the backpressure limit
        void clear() { cmds.clear(); states.clear(); vertices.clear(); data.clear(); tex0.clear(); frames.clear(); models.clear(); modelVertices = 0; }
    };

    class Recorder
    {
    public:
        void triangle(const State &s, const Vertex v[3]);
        // n / 3 triangles in one go (n a multiple of 3).
        void triangles(const State &s, const Vertex *v, uint32_t n);
        void sprite(const State &s, const Vertex &v0, const Vertex &v1);
        // A model batch; mesh = its vertex bytes when they must be sent (m.dataBytes of them).
        void model(const State &s, const ModelDraw &m, const uint8_t *mesh);
        void transferBegin(uint32_t dbp, uint32_t dbw, uint32_t dpsm, uint32_t dsax, uint32_t dsay, uint32_t rrw, uint32_t rrh);
        void transferData(const uint8_t *p, uint32_t n);
        // GS -> host transfer (BITBLTBUF/TRXPOS/TRXREG with TRXDIR 1): the replay reads the
        // region back from the GPU and keeps it for ReadbackResult.
        void readback(uint32_t sbp, uint32_t sbw, uint32_t spsm, uint32_t ssax, uint32_t ssay, uint32_t rrw, uint32_t rrh);
        void tex0(const Tex0Event &e);
        void clear(const State &s, uint32_t rgba);
        // Recording thread: make what was recorded since the last publish visible to take().
        // Commands are recorded without locking into a thread-local batch and published in
        // chunks (and at every vblank), not one lock per triangle.
        void publish();
        // Recording thread, at each guest vblank: mark the end of a displayed frame (with the
        // display registers as they are now) and publish. The GL thread only shows the picture
        // as of the last mark, never a frame that is still being drawn.
        void frame(const DisplayRegs &regs);
        // GL thread: take everything published so far.
        void take(Batch &out);
        // Backpressure: the recording thread waits while more than this many vertices are queued
        // (the GPU is behind), like a game waiting on the GS. 0 disables.
        void setLimit(size_t vertices) { m_limit = vertices; }
        uint64_t waits() const { return m_waits; }

    private:
        uint32_t stateIndex(const State &s);
        void throttle(std::unique_lock<std::mutex> &lock);
        std::mutex m_mutex;
        std::condition_variable m_cv;
        size_t m_limit = 0;
        uint64_t m_waits = 0;
        Batch m_batch;     // published, shared with the GL thread
        Batch m_local;     // recording thread only
        bool m_lastDraw = false;
        void maybePublish();
    };

    class Renderer
    {
    public:
        Renderer();
        ~Renderer();
        bool init(void *(*getProc)(const char *), const uint8_t *initialVram, uint32_t scale);
        // Replays the batch; at each Frame mark the displayed frame buffer is composited into
        // the display texture (see lastFrame).
        void replay(const Batch &b);
        // The picture composited at the most recent Frame mark. False until there is one (and
        // after setScale), in which case present() the live registers instead.
        bool lastFrame(uint32_t &tex, uint32_t &w, uint32_t &h) const;
        // Composites the displayed frame buffer into a texture (alpha forced to 1).
        bool present(const DisplayRegs &regs, uint32_t &tex, uint32_t &w, uint32_t &h);
        // Restore GL state expected by the host UI (raylib).
        void restoreHostState(int screenW, int screenH);
        // Read back a render target as RGBA8 (top row first), for debugging.
        bool dumpTarget(uint32_t fbp, uint32_t fbw, std::vector<uint8_t> &rgba, uint32_t &w, uint32_t &h);
        const Stats &stats() const;
        // The model programs (ModelDraw) compiled and linked.
        bool modelsOk() const;
        void describe(std::FILE *out) const;
        uint32_t scale() const;
        // Change the internal resolution multiplier: GPU-drawn data is written back to the VRAM
        // shadow and every render target is recreated at the new size on next use.
        void setScale(uint32_t scale);
        // Horizontal squash applied to 2D overlay draws (1 = off; 0.75 for a 16:9 widescreen hack).
        void setHudSquash(float k);

    private:
        struct Impl;
        std::unique_ptr<Impl> m;
        mutable Stats m_stats;
    };
}
