// Host audio output for the emulated SPU2: a raylib audio stream pulls 48 kHz stereo frames
// from the IOP's output ring on raylib's audio thread.
//   RT_AUDIO=0 disables output (the SPU2 still runs, so the game's sound driver behaves the same).
//   RT_AUDIO_WAV=<file> records the output to a WAV file instead (for headless testing).
#include "raylib.h"
#include "ps2x/iop/iop_audio.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>
#include <vector>
#include <mutex>
#include <atomic>
#include <algorithm>

namespace rufftrigger
{
    namespace
    {
        AudioStream g_stream{};
        bool g_started = false;
        bool g_primed = false;
        bool g_recording = false;

        // Movie soundtrack (48 kHz stereo), mixed over the SPU2 output.
        std::mutex g_movMutex;
        std::vector<int16_t> g_mov;
        size_t g_movRead = 0;
        std::atomic<uint64_t> g_movPlayed{0};

        void mixMovie(int16_t *out, size_t frames)
        {
            std::lock_guard<std::mutex> lock(g_movMutex);
            const size_t avail = (g_mov.size() - g_movRead) / 2;
            const size_t n = std::min(avail, frames);
            const int16_t *src = g_mov.data() + g_movRead;
            for (size_t i = 0; i < n * 2; ++i)
                out[i] = static_cast<int16_t>(std::clamp(static_cast<int>(out[i]) + src[i], -32768, 32767));
            g_movRead += n * 2;
            g_movPlayed.fetch_add(n, std::memory_order_relaxed);
            if (g_movRead == g_mov.size())
            {
                g_mov.clear();
                g_movRead = 0;
            }
        }

        void fill(void *buffer, unsigned int frames)
        {
            int16_t *out = static_cast<int16_t *>(buffer);
            // Wait for a little backlog first so early underruns don't crackle.
            if (!g_primed && ps2x::iop::queuedAudioFrames() < 2048)
                std::memset(out, 0, static_cast<size_t>(frames) * 4);
            else
            {
                g_primed = true;
                const size_t got = ps2x::iop::readAudio(out, frames);
                if (got < frames)
                {
                    std::memset(out + got * 2, 0, (frames - got) * 4);
                    if (got == 0)
                        g_primed = false;
                }
            }
            mixMovie(out, frames);
        }
    }

    namespace
    {
        void writeWavHeader(std::FILE *f, uint32_t frames)
        {
            const uint32_t data = frames * 4, riff = 36 + data, fmtLen = 16, rate = ps2x::iop::kAudioRate, bps = rate * 4;
            const uint16_t pcm = 1, ch = 2, align = 4, bits = 16;
            std::fseek(f, 0, SEEK_SET);
            std::fwrite("RIFF", 1, 4, f); std::fwrite(&riff, 4, 1, f); std::fwrite("WAVEfmt ", 1, 8, f);
            std::fwrite(&fmtLen, 4, 1, f); std::fwrite(&pcm, 2, 1, f); std::fwrite(&ch, 2, 1, f);
            std::fwrite(&rate, 4, 1, f); std::fwrite(&bps, 4, 1, f); std::fwrite(&align, 2, 1, f); std::fwrite(&bits, 2, 1, f);
            std::fwrite("data", 1, 4, f); std::fwrite(&data, 4, 1, f);
            std::fseek(f, 0, SEEK_END);
        }

        void recordWav(const char *path)
        {
            std::FILE *f = std::fopen(path, "wb");
            if (!f)
                return;
            writeWavHeader(f, 0);
            ps2x::iop::enableAudioOutput(true);
            std::thread([f] {
                std::vector<int16_t> buf(8192 * 2);
                uint32_t total = 0, lastHeader = 0;
                for (;;)
                {
                    const size_t n = ps2x::iop::readAudio(buf.data(), 8192);
                    mixMovie(buf.data(), n);
                    if (n)
                    {
                        std::fwrite(buf.data(), 4, n, f);
                        total += static_cast<uint32_t>(n);
                    }
                    if (total - lastHeader >= ps2x::iop::kAudioRate)
                    {
                        writeWavHeader(f, total);
                        std::fflush(f);
                        lastHeader = total;
                    }
                    if (n < 8192)
                        std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
            }).detach();
            g_recording = true;
            std::fprintf(stderr, "[audio] recording SPU2 output to %s\n", path);
        }
    }

    void InstallAudio()
    {
        if (const char *wav = std::getenv("RT_AUDIO_WAV"); wav && *wav)
        {
            recordWav(wav);
            return;
        }
        const char *env = std::getenv("RT_AUDIO");
        if (env && *env && std::atoi(env) == 0)
            return;
        if (g_started || !IsAudioDeviceReady())
        {
            if (!IsAudioDeviceReady())
                std::fprintf(stderr, "[audio] no audio device; SPU2 output discarded\n");
            return;
        }
        SetAudioStreamBufferSizeDefault(1024);
        g_stream = LoadAudioStream(ps2x::iop::kAudioRate, 16, 2);
        SetAudioStreamCallback(g_stream, fill);
        PlayAudioStream(g_stream);
        ps2x::iop::enableAudioOutput(true);
        g_started = true;
        std::fprintf(stderr, "[audio] SPU2 output at %u Hz\n", ps2x::iop::kAudioRate);
    }

    // Movie soundtrack: interleaved stereo at the SPU2 output rate.
    bool MovieAudioEnabled() { return g_started || g_recording; }

    void MovieAudioPush(const int16_t *pcm, size_t frames)
    {
        std::lock_guard<std::mutex> lock(g_movMutex);
        if (g_movRead > (1u << 20))
        {
            g_mov.erase(g_mov.begin(), g_mov.begin() + static_cast<std::ptrdiff_t>(g_movRead));
            g_movRead = 0;
        }
        g_mov.insert(g_mov.end(), pcm, pcm + frames * 2);
    }

    size_t MovieAudioQueued()
    {
        std::lock_guard<std::mutex> lock(g_movMutex);
        return (g_mov.size() - g_movRead) / 2;
    }

    uint64_t MovieAudioPlayed() { return g_movPlayed.load(std::memory_order_relaxed); }

    void MovieAudioClear()
    {
        std::lock_guard<std::mutex> lock(g_movMutex);
        g_mov.clear();
        g_movRead = 0;
    }
}
