// FMV playback. The game's own player (IPU MPEG decoding + IOP audio streaming) is replaced at
// its API: open / start / update (once per game frame) / stop / is-done / close, all called
// from one place (f_0019c3d0, the cut-scene sequencer).
//
// The .PSS file (an MPEG-2 program stream: MPEG-2 video in stream 0xE0, audio in private
// stream 1 as an "SShd"/"SSbd" PCM or PS-ADPCM block stream) is read from
//   1. RT_MOVIE_DIR (laid out like the disc's VIDEO folder, or all files in one folder), if set,
//   2. <disc folder>/VIDEO/<name>.PSS (the extracted disc),
//   3. the VIDEO directory of the disc image (RT_ISO),
// matching names without regard to case. A missing movie ends at once (as if skipped).
//
// A decoder thread demuxes the file, decodes the video with FFmpeg (to RGBA) and the audio to
// 48 kHz stereo; the frames are shown by the GPU renderer's present hook in place of the game
// picture (letterboxed to the movie's aspect), the audio is mixed over the SPU2 output.
//   RT_MOVIES=0   skip all movies
//   RT_MOVIELOG=1 log
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include "ps2_runtime.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace rufftrigger
{
    bool MovieAudioEnabled();
    void MovieAudioPush(const int16_t *pcm, size_t frames);
    size_t MovieAudioQueued();
    uint64_t MovieAudioPlayed();
    void MovieAudioClear();

    namespace
    {
        // The picture shown in place of the game's while a movie plays (read by the present hook).
        struct MovieOverlay
        {
            std::mutex m;
            bool active = false;
            std::vector<uint8_t> rgba;   // w*h*4, top row first
            int w = 0, h = 0;
            float aspect = 4.0f / 3.0f;  // display aspect of the picture
            uint64_t seq = 0;
        } g_movieOverlay;

        bool g_log = false;
        constexpr int kRate = 48000;

        // ---- file location -------------------------------------------------------------------
        struct Source
        {
            fs::path file;
            uint64_t offset = 0, size = 0;
        };

        bool SameName(std::string a, std::string b)
        {
            if (size_t semi = a.find(';'); semi != std::string::npos) a.resize(semi);
            if (size_t semi = b.find(';'); semi != std::string::npos) b.resize(semi);
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); ++i)
                if (std::toupper(static_cast<unsigned char>(a[i])) != std::toupper(static_cast<unsigned char>(b[i])))
                    return false;
            return true;
        }

        bool FindInDir(const fs::path &dir, const std::string &name, Source &out)
        {
            std::error_code ec;
            for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
                if (SameName(it->path().filename().string(), name) && it->is_regular_file(ec))
                {
                    out.file = it->path();
                    out.offset = 0;
                    out.size = it->file_size(ec);
                    return true;
                }
            return false;
        }

        bool FindDirIn(const fs::path &dir, const std::string &name, fs::path &out)
        {
            std::error_code ec;
            for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
                if (SameName(it->path().filename().string(), name) && it->is_directory(ec))
                {
                    out = it->path();
                    return true;
                }
            return false;
        }

        // ISO9660: look up DIR/NAME starting at the root directory.
        bool FindInIso(const fs::path &iso, const std::vector<std::string> &parts, Source &out)
        {
            std::ifstream f(iso, std::ios::binary);
            if (!f)
                return false;
            uint8_t pvd[2048];
            f.seekg(16 * 2048);
            if (!f.read(reinterpret_cast<char *>(pvd), 2048) || pvd[0] != 1 || std::memcmp(pvd + 1, "CD001", 5) != 0)
                return false;
            auto u32 = [](const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; };
            uint32_t lba = u32(pvd + 156 + 2), size = u32(pvd + 156 + 10);
            for (size_t k = 0; k < parts.size(); ++k)
            {
                std::vector<uint8_t> dir((size + 2047) / 2048 * 2048);
                f.seekg(static_cast<std::streamoff>(lba) * 2048);
                if (dir.empty() || !f.read(reinterpret_cast<char *>(dir.data()), static_cast<std::streamsize>(dir.size())))
                    return false;
                bool found = false;
                for (size_t pos = 0; pos < dir.size();)
                {
                    const uint8_t len = dir[pos];
                    if (len == 0)
                    {
                        pos = (pos / 2048 + 1) * 2048;
                        continue;
                    }
                    if (pos + len > dir.size() || len < 34)
                        break;
                    const uint8_t *r = &dir[pos];
                    const std::string name(reinterpret_cast<const char *>(r + 33), r[32]);
                    const bool isDir = (r[25] & 2) != 0;
                    if (SameName(name, parts[k]) && isDir == (k + 1 < parts.size()))
                    {
                        lba = u32(r + 2);
                        size = u32(r + 10);
                        found = true;
                        break;
                    }
                    pos += len;
                }
                if (!found)
                    return false;
            }
            out.file = iso;
            out.offset = static_cast<uint64_t>(lba) * 2048;
            out.size = size;
            return true;
        }

        bool Locate(const std::string &guestPath, Source &out)
        {
            // "\VIDEO\movie_00.PSS" -> ["VIDEO", "movie_00.PSS"]
            std::vector<std::string> parts;
            std::string cur;
            for (char c : guestPath)
            {
                if (c == '\\' || c == '/')
                {
                    if (!cur.empty()) parts.push_back(cur);
                    cur.clear();
                }
                else
                    cur.push_back(c);
            }
            if (!cur.empty()) parts.push_back(cur);
            if (parts.empty())
                return false;
            // root/<parts[first..]>, directories matched without regard to case
            auto findPath = [&](fs::path dir, size_t first) {
                bool ok = !dir.empty();
                for (size_t k = first; ok && k + 1 < parts.size(); ++k)
                    ok = FindDirIn(dir, parts[k], dir);
                return ok && FindInDir(dir, parts.back(), out);
            };
            if (const char *d = std::getenv("RT_MOVIE_DIR"); d && *d)
                if (findPath(d, 1) || FindInDir(d, parts.back(), out))   // VIDEO's layout, or flat
                    return true;
            const auto &paths = PS2Runtime::getIoPaths();
            if (findPath(paths.cdRoot, 0))
                return true;
            const char *iso = std::getenv("RT_ISO");
            const fs::path image = iso && *iso ? fs::path(iso) : paths.cdImage;
            return !image.empty() && FindInIso(image, parts, out);
        }

        // ---- audio: SShd/SSbd blocks ------------------------------------------------------------
        struct AudioDecoder
        {
            bool haveHeader = false, failed = false;
            size_t prefix = 0;          // bytes before the audio data in each private-stream-1 packet
            uint32_t type = 0, rate = kRate, channels = 2, interleave = 0x200;
            std::vector<uint8_t> pending;
            double hist[2][2] = {};
            double resamplePos = 0;
            int16_t last[2] = {};
            std::vector<int16_t> out;   // stereo at kRate

            void emit(const std::vector<int16_t> &l, const std::vector<int16_t> &r)
            {
                if (rate == static_cast<uint32_t>(kRate))
                {
                    for (size_t i = 0; i < l.size(); ++i)
                    {
                        out.push_back(l[i]);
                        out.push_back(r[i]);
                    }
                    return;
                }
                const double step = static_cast<double>(rate) / kRate;
                // linear interpolation across block boundaries using the previous last sample
                const size_t n = l.size();
                while (resamplePos < static_cast<double>(n))
                {
                    const double p = resamplePos;
                    const size_t i = static_cast<size_t>(p);
                    const double t = p - static_cast<double>(i);
                    const double l0 = i == 0 ? last[0] : l[i - 1], r0 = i == 0 ? last[1] : r[i - 1];
                    out.push_back(static_cast<int16_t>(l0 + (l[i] - l0) * t));
                    out.push_back(static_cast<int16_t>(r0 + (r[i] - r0) * t));
                    resamplePos += step;
                }
                resamplePos -= static_cast<double>(n);
                if (n)
                {
                    last[0] = l[n - 1];
                    last[1] = r[n - 1];
                }
            }

            void adpcm(const uint8_t *src, size_t bytes, int ch, std::vector<int16_t> &dst)
            {
                static const double f[5][2] = {{0, 0}, {60 / 64.0, 0}, {115 / 64.0, -52 / 64.0}, {98 / 64.0, -55 / 64.0}, {122 / 64.0, -60 / 64.0}};
                for (size_t b = 0; b + 16 <= bytes; b += 16)
                {
                    const uint8_t *fr = src + b;
                    const int shift = fr[0] & 0xF, filt = std::min(fr[0] >> 4, 4);
                    for (int i = 0; i < 28; ++i)
                    {
                        int nib = (fr[2 + i / 2] >> ((i & 1) * 4)) & 0xF;
                        if (nib & 8) nib -= 16;
                        double s = static_cast<double>((nib * 4096) >> std::min(shift, 12));
                        s += hist[ch][0] * f[filt][0] + hist[ch][1] * f[filt][1];
                        hist[ch][1] = hist[ch][0];
                        hist[ch][0] = s;
                        dst.push_back(static_cast<int16_t>(std::clamp(s, -32768.0, 32767.0)));
                    }
                }
            }

            void decodeBlocks()
            {
                const size_t set = static_cast<size_t>(interleave) * channels;
                size_t pos = 0;
                std::vector<int16_t> ch[2];
                while (pending.size() - pos >= set)
                {
                    for (uint32_t c = 0; c < channels && c < 2; ++c)
                    {
                        const uint8_t *blk = pending.data() + pos + static_cast<size_t>(c) * interleave;
                        ch[c].clear();
                        if (type == 0x10)
                            adpcm(blk, interleave, static_cast<int>(c), ch[c]);
                        else
                            for (uint32_t i = 0; i + 1 < interleave; i += 2)
                                ch[c].push_back(type == 0x02 ? static_cast<int16_t>(blk[i] << 8 | blk[i + 1])
                                                             : static_cast<int16_t>(blk[i] | blk[i + 1] << 8));
                    }
                    if (channels == 1)
                        ch[1] = ch[0];
                    emit(ch[0], ch[1]);
                    pos += set;
                }
                pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(pos));
            }

            void packet(const uint8_t *p, size_t n)
            {
                if (failed)
                    return;
                if (!haveHeader)
                {
                    size_t k = 0;
                    while (k + 4 <= n && k < 32 && std::memcmp(p + k, "SShd", 4) != 0)
                        ++k;
                    if (k + 0x28 > n || std::memcmp(p + k, "SShd", 4) != 0)
                        return;   // not seen the header yet
                    auto u32 = [&](size_t o) { return uint32_t(p[o]) | uint32_t(p[o + 1]) << 8 | uint32_t(p[o + 2]) << 16 | uint32_t(p[o + 3]) << 24; };
                    prefix = k;
                    type = u32(k + 8);
                    rate = u32(k + 12);
                    channels = u32(k + 16);
                    interleave = u32(k + 20);
                    // "SShd" u32 0x18 {type, rate, channels, interleave, loop start, loop end} "SSbd" u32 size, data
                    const size_t body = k + 0x28;
                    if (std::memcmp(p + k + 0x20, "SSbd", 4) != 0 || channels < 1 || channels > 2 || interleave == 0 ||
                        interleave > 0x10000 || rate < 8000 || rate > 96000 || (type != 1 && type != 2 && type != 0x10))
                    {
                        std::fprintf(stderr, "[movie] unsupported audio header (type 0x%X, %u Hz, %u ch, interleave 0x%X)\n", type, rate, channels, interleave);
                        failed = true;
                        return;
                    }
                    haveHeader = true;
                    if (g_log)
                        std::fprintf(stderr, "[movie] audio: type 0x%X %u Hz %u ch interleave 0x%X (prefix %zu)\n", type, rate, channels, interleave, prefix);
                    pending.insert(pending.end(), p + body, p + n);
                }
                else if (n > prefix)
                    pending.insert(pending.end(), p + prefix, p + n);
                decodeBlocks();
            }
        };

        // ---- player ----------------------------------------------------------------------------
        struct Frame
        {
            std::vector<uint8_t> rgba;
            int w, h;
            double pts;
        };

        struct Player
        {
            Source src;
            std::thread thread;
            std::mutex m;
            std::condition_variable cv;
            std::deque<Frame> frames;
            bool eof = false, stop = false, started = false, hasAudio = false;
            float aspect = 4.0f / 3.0f;
            double fps = 30000.0 / 1001.0, lastPts = -1;
            // clock
            std::chrono::steady_clock::time_point wallStart;
            uint64_t audioBase = 0;
            bool useAudioClock = false;
            double frozenClock = -1;
            std::chrono::steady_clock::time_point frozenAt;
            bool finished = false;

            void run();
            double clock();
        };

        std::unique_ptr<Player> g_player;
        std::mutex g_playerMutex;

        void Player::run()
        {
            std::ifstream f(src.file, std::ios::binary);
            f.seekg(static_cast<std::streamoff>(src.offset));
            const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_MPEG2VIDEO);
            AVCodecContext *cc = codec ? avcodec_alloc_context3(codec) : nullptr;
            AVCodecParserContext *parser = av_parser_init(AV_CODEC_ID_MPEG2VIDEO);
            AVPacket *pkt = av_packet_alloc();
            AVFrame *frame = av_frame_alloc();
            SwsContext *sws = nullptr;
            int64_t frameIndex = 0;
            AudioDecoder audio;
            const bool audioOut = MovieAudioEnabled();
            if (!cc || avcodec_open2(cc, codec, nullptr) < 0 || !parser)
            {
                std::fprintf(stderr, "[movie] no MPEG-2 decoder\n");
                std::lock_guard<std::mutex> lock(m);
                eof = true;
                cv.notify_all();
                return;
            }

            auto receive = [&]() {
                while (avcodec_receive_frame(cc, frame) == 0)
                {
                    if (frameIndex == 0)
                    {
                        if (cc->framerate.num > 0 && cc->framerate.den > 0)
                            fps = av_q2d(cc->framerate);
                        const AVRational sar = frame->sample_aspect_ratio;
                        const double dar = sar.num > 0 && sar.den > 0 ? static_cast<double>(frame->width) * sar.num / (static_cast<double>(frame->height) * sar.den) : 0.0;
                        aspect = dar > 1.6 ? 16.0f / 9.0f : 4.0f / 3.0f;
                        if (g_log)
                            std::fprintf(stderr, "[movie] video %dx%d %.3f fps, aspect %.3f\n", frame->width, frame->height, fps, aspect);
                    }
                    Frame out;
                    out.w = frame->width;
                    out.h = frame->height;
                    out.pts = static_cast<double>(frameIndex++) / fps;
                    out.rgba.resize(static_cast<size_t>(out.w) * out.h * 4);
                    sws = sws_getCachedContext(sws, frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
                                               out.w, out.h, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
                    uint8_t *dst[1] = {out.rgba.data()};
                    const int stride[1] = {out.w * 4};
                    if (sws)
                        sws_scale(sws, frame->data, frame->linesize, 0, frame->height, dst, stride);
                    std::unique_lock<std::mutex> lock(m);
                    cv.wait(lock, [&] { return stop || frames.size() < 6; });
                    if (stop)
                        return false;
                    lastPts = out.pts;
                    frames.push_back(std::move(out));
                }
                return true;
            };

            auto video = [&](const uint8_t *p, size_t n) {
                while (n > 0)
                {
                    uint8_t *data = nullptr;
                    int size = 0;
                    const int used = av_parser_parse2(parser, cc, &data, &size, p, static_cast<int>(n), AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
                    if (used < 0)
                        return true;
                    p += used;
                    n -= static_cast<size_t>(used);
                    if (size > 0)
                    {
                        pkt->data = data;
                        pkt->size = size;
                        avcodec_send_packet(cc, pkt);
                        if (!receive())
                            return false;
                    }
                }
                return true;
            };

            std::vector<uint8_t> buf;
            size_t pos = 0;
            uint64_t left = src.size;
            bool running = true, inputDone = false;
            while (running)
            {
                if (buf.size() - pos < 65536 + 6 && !inputDone)
                {
                    buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(pos));
                    pos = 0;
                    const size_t want = static_cast<size_t>(std::min<uint64_t>(left, 1u << 18));
                    const size_t old = buf.size();
                    buf.resize(old + want);
                    f.read(reinterpret_cast<char *>(buf.data() + old), static_cast<std::streamsize>(want));
                    const size_t got = static_cast<size_t>(f.gcount());
                    buf.resize(old + got);
                    left -= got;
                    if (got == 0 || left == 0)
                        inputDone = true;
                }
                const size_t avail = buf.size() - pos;
                if (avail < 6)
                    break;
                const uint8_t *b = buf.data() + pos;
                if (b[0] != 0 || b[1] != 0 || b[2] != 1)
                {
                    ++pos;
                    continue;
                }
                const uint8_t code = b[3];
                size_t len;
                if (code == 0xBA)
                    len = (b[4] & 0xC0) == 0x40 ? 14 + (avail > 13 ? (b[13] & 7) : 0) : 12;
                else if (code == 0xB9)
                {
                    pos += 4;
                    continue;
                }
                else if (code >= 0xBB)
                    len = 6 + (static_cast<size_t>(b[4]) << 8 | b[5]);
                else
                {
                    ++pos;
                    continue;
                }
                if (len > avail)
                {
                    if (inputDone)
                        break;
                    continue;   // refill
                }
                if (code == 0xBD || (code >= 0xE0 && code <= 0xEF))
                {
                    const uint8_t *pl = b + 6;
                    size_t n = len - 6, h = 0;
                    if (n >= 3 && (pl[0] & 0xC0) == 0x80)
                        h = 3 + pl[2];
                    else
                    {
                        while (h < n && pl[h] == 0xFF) ++h;
                        if (h < n && (pl[h] & 0xC0) == 0x40) h += 2;
                        if (h < n && (pl[h] & 0xF0) == 0x20) h += 5;
                        else if (h < n && (pl[h] & 0xF0) == 0x30) h += 10;
                        else ++h;
                    }
                    if (h < n)
                    {
                        if (code == 0xE0)
                            running = video(pl + h, n - h);
                        else if (code == 0xBD)
                        {
                            audio.packet(pl + h, n - h);
                            if (!audio.out.empty())
                            {
                                hasAudio = audio.haveHeader;
                                if (audioOut)
                                    MovieAudioPush(audio.out.data(), audio.out.size() / 2);
                                audio.out.clear();
                            }
                        }
                    }
                }
                pos += len;
                {
                    std::lock_guard<std::mutex> lock(m);
                    if (stop)
                        running = false;
                }
            }
            if (running)
            {
                avcodec_send_packet(cc, nullptr);   // flush the decoder
                receive();
            }
            sws_freeContext(sws);
            av_frame_free(&frame);
            av_packet_free(&pkt);
            av_parser_close(parser);
            avcodec_free_context(&cc);
            std::lock_guard<std::mutex> lock(m);
            eof = true;
            if (g_log)
                std::fprintf(stderr, "[movie] decoder done: %lld frames%s\n", static_cast<long long>(frameIndex), hasAudio ? " with audio" : "");
            cv.notify_all();
        }

        double Player::clock()
        {
            const auto now = std::chrono::steady_clock::now();
            const double wall = std::chrono::duration<double>(now - wallStart).count();
            if (!useAudioClock)
                return wall;
            if (frozenClock >= 0)
                return frozenClock + std::chrono::duration<double>(now - frozenAt).count();
            const double a = static_cast<double>(MovieAudioPlayed() - audioBase) / kRate;
            bool audioDone;
            {
                std::lock_guard<std::mutex> lock(m);
                audioDone = eof && MovieAudioQueued() == 0;
            }
            if (audioDone || (eof && !hasAudio))
            {
                frozenClock = a;
                frozenAt = now;
            }
            return a;
        }

        void PublishBlack()
        {
            std::lock_guard<std::mutex> lock(g_movieOverlay.m);
            g_movieOverlay.w = 16;
            g_movieOverlay.h = 12;
            g_movieOverlay.rgba.assign(16 * 12 * 4, 0);
            for (size_t i = 3; i < g_movieOverlay.rgba.size(); i += 4) g_movieOverlay.rgba[i] = 255;
            g_movieOverlay.aspect = 4.0f / 3.0f;
            g_movieOverlay.active = true;
            ++g_movieOverlay.seq;
        }

        void StopPlayer()
        {
            std::unique_ptr<Player> p;
            {
                std::lock_guard<std::mutex> lock(g_playerMutex);
                p = std::move(g_player);
            }
            if (p)
            {
                {
                    std::lock_guard<std::mutex> lock(p->m);
                    p->stop = true;
                }
                p->cv.notify_all();
                if (p->thread.joinable())
                    p->thread.join();
            }
            MovieAudioClear();
            std::lock_guard<std::mutex> lock(g_movieOverlay.m);
            g_movieOverlay.active = false;
        }

        uint32_t Reg(const R5900Context *ctx, int r) { return static_cast<uint32_t>(_mm_extract_epi32(ctx->r[r], 0)); }

        void Return(R5900Context *ctx, int32_t v)
        {
            ctx->r[2] = _mm_set_epi64x(0, v);
            ctx->pc = Reg(ctx, 31);
        }

        // ---- game entry points -----------------------------------------------------------------
        // open(obj, path, a2, a3, t0) -> nonzero when the movie can play
        void MovieOpen(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            std::string path;
            for (uint32_t a = Reg(ctx, 5) & 0x01FFFFFFu; rdram[a] && path.size() < 200; ++a)
                path.push_back(static_cast<char>(rdram[a]));
            StopPlayer();
            const char *env = std::getenv("RT_MOVIES");
            Source src;
            const bool off = env && *env && std::atoi(env) == 0;
            const bool found = !off && Locate(path, src);
            std::fprintf(stderr, "[movie] %s: %s\n", path.c_str(),
                         off ? "skipped (RT_MOVIES=0)" : found ? src.file.string().c_str() : "not on the disc, skipped");
            // A missing movie still "plays" (for zero frames): when open fails the game also skips
            // the code after the movie that shows the menu text again, leaving every text hidden.
            auto p = std::make_unique<Player>();
            p->src = src;
            Player *raw = p.get();
            if (found)
                p->thread = std::thread([raw] { raw->run(); });
            else
            {
                p->eof = true;
                p->finished = true;
            }
            {
                std::lock_guard<std::mutex> lock(g_playerMutex);
                g_player = std::move(p);
            }
            Return(ctx, 1);
        }

        void MovieStart(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            std::lock_guard<std::mutex> lock(g_playerMutex);
            if (Player *p = g_player.get(); p && !p->finished)
            {
                // Give the decoder a moment to queue the first frame and some sound.
                for (int i = 0; i < 200; ++i)
                {
                    {
                        std::lock_guard<std::mutex> l(p->m);
                        if (p->eof || p->frames.size() >= 3)
                            break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                p->started = true;
                p->wallStart = std::chrono::steady_clock::now();
                p->audioBase = MovieAudioPlayed();
                p->useAudioClock = MovieAudioEnabled() && MovieAudioQueued() > 0;
                PublishBlack();
            }
            Return(ctx, 0);
        }

        // update: show the newest frame that is due; finish when everything has played.
        void MovieUpdate(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            std::lock_guard<std::mutex> lock(g_playerMutex);
            Player *p = g_player.get();
            if (p && p->started && !p->finished)
            {
                const double t = p->clock();
                Frame show;
                bool have = false, done = false;
                {
                    std::lock_guard<std::mutex> l(p->m);
                    while (!p->frames.empty() && p->frames.front().pts <= t)
                    {
                        show = std::move(p->frames.front());
                        p->frames.pop_front();
                        have = true;
                    }
                    done = p->eof && p->frames.empty() && t >= p->lastPts + 1.0 / p->fps &&
                           (!p->useAudioClock || p->frozenClock >= 0);
                }
                if (have)
                {
                    p->cv.notify_all();
                    std::lock_guard<std::mutex> o(g_movieOverlay.m);
                    g_movieOverlay.rgba = std::move(show.rgba);
                    g_movieOverlay.w = show.w;
                    g_movieOverlay.h = show.h;
                    g_movieOverlay.aspect = p->aspect;
                    g_movieOverlay.active = true;
                    ++g_movieOverlay.seq;
                }
                if (done)
                {
                    p->finished = true;
                    if (g_log)
                        std::fprintf(stderr, "[movie] finished at %.2fs\n", t);
                }
            }
            Return(ctx, 0);
        }

        void MovieStop(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            std::lock_guard<std::mutex> lock(g_playerMutex);
            if (g_player)
            {
                g_player->finished = true;
                if (g_log)
                    std::fprintf(stderr, "[movie] skipped by the player\n");
            }
            Return(ctx, 0);
        }

        // The sequencer loops until this returns nonzero.
        void MovieIsDone(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            std::lock_guard<std::mutex> lock(g_playerMutex);
            Return(ctx, !g_player || g_player->finished ? 1 : 0);
        }

        void MovieClose(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            StopPlayer();
            Return(ctx, 0);
        }
    }

    // Present hook side: false when no movie is showing. When the picture changed since `seq`,
    // copies it out and updates `seq`.
    bool MovieOverlayGet(uint64_t &seq, std::vector<uint8_t> &rgba, int &w, int &h, float &aspect)
    {
        std::lock_guard<std::mutex> lock(g_movieOverlay.m);
        if (!g_movieOverlay.active)
            return false;
        if (seq != g_movieOverlay.seq)
        {
            seq = g_movieOverlay.seq;
            rgba = g_movieOverlay.rgba;
            w = g_movieOverlay.w;
            h = g_movieOverlay.h;
            aspect = g_movieOverlay.aspect;
        }
        return true;
    }

    void InstallMovies(PS2Runtime &runtime)
    {
        g_log = std::getenv("RT_MOVIELOG") != nullptr;
        runtime.replaceFunction(0x0021A798u, &MovieOpen);
        runtime.replaceFunction(0x0021A960u, &MovieStart);
        runtime.replaceFunction(0x0021AA58u, &MovieUpdate);
        runtime.replaceFunction(0x0021AB30u, &MovieStop);
        runtime.replaceFunction(0x0021ABE8u, &MovieIsDone);
        runtime.replaceFunction(0x0021AB40u, &MovieClose);
    }
}
