// Memory card saves: the game's libmc2 calls (sceMc2*, synchronous wrappers) are replaced with
// host file operations on a directory that stands for the card in port 1.
//
//   RT_MEMCARD=<dir>  card directory (default: the runtime's mc0 folder next to the game ELF)
//   RT_MCLOG=1        log every call
//
// The game only uses these entry points (all called with the socket returned by
// sceMc2CreateSocket): GetInfo, Format(?), Read, Write, CreateFile, Delete, Mkdir, Chdir and
// SearchFile. Results follow libmc2: 0 = ok, 0x8101xxxx = error with the code in the low 16 bits
// (the game tests (r & 0xFFFF) == 2 for "not found" and 0x11 for "already exists"); Read and
// Write return the byte count.
#include "ps2_runtime.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace rufftrigger
{
    namespace
    {
        constexpr int32_t kOk = 0;
        constexpr int32_t kErrNotFound = static_cast<int32_t>(0x81010002u);
        constexpr int32_t kErrExists = static_cast<int32_t>(0x81010011u);
        constexpr int32_t kErrNoCard = static_cast<int32_t>(0x81010001u);
        constexpr int32_t kErrIo = static_cast<int32_t>(0x81010005u);
        constexpr uint32_t kCardKB = 8000;   // 8 MB card minus the file system

        std::mutex g_mutex;
        fs::path g_root;
        std::string g_cwd = "/";             // card-side current directory
        int32_t g_nextSocket = 1;
        bool g_log = false;

        uint32_t R(const R5900Context *ctx, int r) { return static_cast<uint32_t>(_mm_extract_epi32(ctx->r[r], 0)); }

        void Return(R5900Context *ctx, int32_t v)
        {
            ctx->r[2] = _mm_set_epi64x(0, v);
            ctx->pc = R(ctx, 31);
        }

        std::string GuestStr(const uint8_t *rdram, uint32_t addr)
        {
            std::string s;
            addr &= 0x01FFFFFFu;
            while (addr < 0x02000000u && rdram[addr] && s.size() < 256)
                s.push_back(static_cast<char>(rdram[addr++]));
            return s;
        }

        // Card path (absolute or relative to the current directory) -> normalized "/a/b".
        bool Normalize(const std::string &in, std::string &out)
        {
            std::string p = (!in.empty() && in[0] == '/') ? in : g_cwd + "/" + in;
            std::vector<std::string> parts;
            size_t i = 0;
            while (i <= p.size())
            {
                size_t j = p.find('/', i);
                if (j == std::string::npos) j = p.size();
                std::string part = p.substr(i, j - i);
                i = j + 1;
                if (part.empty() || part == ".") continue;
                if (part == "..")
                {
                    if (!parts.empty()) parts.pop_back();
                    continue;
                }
                if (part.find('\\') != std::string::npos || part.find(':') != std::string::npos)
                    return false;
                parts.push_back(part);
            }
            out = "/";
            for (size_t k = 0; k < parts.size(); ++k)
                out += (k ? "/" : "") + parts[k];
            return true;
        }

        bool Host(const std::string &cardPath, fs::path &host)
        {
            std::string n;
            if (!Normalize(cardPath, n))
                return false;
            host = g_root / n.substr(1);
            return true;
        }

        uint32_t UsedKB()
        {
            std::error_code ec;
            uint64_t used = 0;
            for (auto it = fs::recursive_directory_iterator(g_root, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
            {
                if (it->is_regular_file(ec))
                    used += (it->file_size(ec) + 1023) / 1024;
                else
                    used += 1;
            }
            return static_cast<uint32_t>(used);
        }

        bool CardSocket(uint32_t socket) { return socket == 1; }

        void W32(uint8_t *rdram, uint32_t addr, uint32_t v) { std::memcpy(rdram + (addr & 0x01FFFFFFu), &v, 4); }

        template <typename F>
        void Op(uint8_t *rdram, R5900Context *ctx, const char *name, F &&body)
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            // std::filesystem throws on host errors (permissions, a file where a folder should
            // be, a full disk); an exception escaping into the recompiled game ends the program,
            // so report it to the game as a card I/O error instead.
            int32_t r = kErrNoCard;
            if (CardSocket(R(ctx, 4)))
            {
                try
                {
                    r = body();
                }
                catch (const std::exception &e)
                {
                    std::fprintf(stderr, "[mc2] %s failed: %s\n", name, e.what());
                    r = kErrIo;
                }
            }
            if (g_log)
                std::fprintf(stderr, "[mc2] %s(%u, \"%s\") -> 0x%X\n", name, R(ctx, 4),
                             R(ctx, 5) > 0x1000 ? GuestStr(rdram, R(ctx, 5)).c_str() : "", static_cast<uint32_t>(r));
            Return(ctx, r);
        }

        // ---- libmc2 entry points --------------------------------------------------------------
        void Mc2Ready(uint8_t *, R5900Context *ctx, PS2Runtime *) { Return(ctx, 1); }       // 0x002DA110
        void Mc2Init(uint8_t *, R5900Context *ctx, PS2Runtime *) { Return(ctx, 0); }        // 0x002D2118
        void Mc2Zero(uint8_t *, R5900Context *ctx, PS2Runtime *) { Return(ctx, 0); }        // socket delete / end

        void Mc2CreateSocket(uint8_t *, R5900Context *ctx, PS2Runtime *)                    // 0x002D2398
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            const int32_t s = g_nextSocket++;
            if (g_log)
                std::fprintf(stderr, "[mc2] CreateSocket -> %d\n", s);
            Return(ctx, s);
        }

        // GetInfo(socket, info): info {type (2 = PS2 card), formatted, free KB, ...}
        void Mc2GetInfo(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)                    // 0x002D36F8
        {
            Op(rdram, ctx, "GetInfo", [&] {
                const uint32_t info = R(ctx, 5);
                std::error_code ec;
                fs::create_directories(g_root, ec);
                const uint32_t used = UsedKB();
                if (info)
                {
                    std::memset(rdram + (info & 0x01FFFFFFu), 0, 0x10);
                    W32(rdram, info + 0, 2);
                    W32(rdram, info + 4, 1);
                    W32(rdram, info + 8, used < kCardKB ? kCardKB - used : 0);
                }
                return kOk;
            });
        }

        void Mc2Format(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)                     // 0x002D3738
        {
            Op(rdram, ctx, "Format", [&] {
                std::error_code ec;
                fs::create_directories(g_root, ec);
                return kOk;
            });
        }

        // Read/Write(socket, name, buf, offset, size) -> bytes
        void Mc2Read(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)                       // 0x002D37B8
        {
            Op(rdram, ctx, "Read", [&]() -> int32_t {
                fs::path h;
                if (!Host(GuestStr(rdram, R(ctx, 5)), h) || !fs::is_regular_file(h))
                    return kErrNotFound;
                std::ifstream f(h, std::ios::binary);
                const uint32_t buf = R(ctx, 6) & 0x01FFFFFFu, off = R(ctx, 7), size = R(ctx, 8);
                if (!f || buf + static_cast<uint64_t>(size) > 0x02000000u)
                    return kErrIo;
                f.seekg(off);
                f.read(reinterpret_cast<char *>(rdram + buf), size);
                return static_cast<int32_t>(f.gcount());
            });
        }

        void Mc2Write(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)                      // 0x002D37F8
        {
            Op(rdram, ctx, "Write", [&]() -> int32_t {
                fs::path h;
                if (!Host(GuestStr(rdram, R(ctx, 5)), h) || !fs::is_directory(h.parent_path()))
                    return kErrNotFound;
                if (!fs::exists(h))
                    std::ofstream(h, std::ios::binary).flush();
                std::fstream f(h, std::ios::binary | std::ios::in | std::ios::out);
                const uint32_t buf = R(ctx, 6) & 0x01FFFFFFu, off = R(ctx, 7), size = R(ctx, 8);
                if (!f || buf + static_cast<uint64_t>(size) > 0x02000000u)
                    return kErrIo;
                f.seekp(off);
                f.write(reinterpret_cast<const char *>(rdram + buf), size);
                f.flush();
                return f ? static_cast<int32_t>(size) : kErrIo;
            });
        }

        void Mc2CreateFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)                 // 0x002D3838
        {
            Op(rdram, ctx, "CreateFile", [&]() -> int32_t {
                fs::path h;
                if (!Host(GuestStr(rdram, R(ctx, 5)), h) || !fs::is_directory(h.parent_path()))
                    return kErrNotFound;
                if (fs::exists(h))
                    return kErrExists;
                std::ofstream f(h, std::ios::binary);
                return f ? kOk : kErrIo;
            });
        }

        void Mc2Delete(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)                     // 0x002D3878
        {
            Op(rdram, ctx, "Delete", [&]() -> int32_t {
                fs::path h;
                std::error_code ec;
                if (!Host(GuestStr(rdram, R(ctx, 5)), h) || h == g_root || !fs::exists(h, ec))
                    return kErrNotFound;
                fs::remove(h, ec);   // directories only when empty, like the card
                return ec ? kErrIo : kOk;
            });
        }

        void Mc2Mkdir(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)                      // 0x002D38F8
        {
            Op(rdram, ctx, "Mkdir", [&]() -> int32_t {
                fs::path h;
                std::error_code ec;
                if (!Host(GuestStr(rdram, R(ctx, 5)), h) || !fs::is_directory(h.parent_path()))
                    return kErrNotFound;
                if (fs::exists(h))
                    return kErrExists;
                return fs::create_directory(h, ec) ? kOk : kErrIo;
            });
        }

        void Mc2Chdir(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)                      // 0x002D3938
        {
            Op(rdram, ctx, "Chdir", [&]() -> int32_t {
                std::string n;
                if (const uint32_t old = R(ctx, 6))
                {
                    const size_t len = std::min<size_t>(g_cwd.size(), 1023);
                    std::memcpy(rdram + (old & 0x01FFFFFFu), g_cwd.data(), len);
                    rdram[(old + len) & 0x01FFFFFFu] = 0;
                }
                if (!Normalize(GuestStr(rdram, R(ctx, 5)), n) || !fs::is_directory(g_root / n.substr(1)))
                    return kErrNotFound;
                g_cwd = n;
                return kOk;
            });
        }

        // SearchFile(socket, path, dirent): the game only checks whether the file exists.
        void Mc2SearchFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)                 // 0x002D39F8
        {
            Op(rdram, ctx, "SearchFile", [&]() -> int32_t {
                fs::path h;
                return Host(GuestStr(rdram, R(ctx, 5)), h) && fs::exists(h) ? kOk : kErrNotFound;
            });
        }
    }

    void InstallMemcard(PS2Runtime &runtime)
    {
        const char *env = std::getenv("RT_MEMCARD");
        g_root = env && *env ? fs::path(env) : PS2Runtime::getIoPaths().mcRoot;
        if (g_root.empty())
            g_root = "mc0";
        std::error_code ec;
        fs::create_directories(g_root, ec);
        g_root = fs::absolute(g_root, ec);
        g_log = std::getenv("RT_MCLOG") != nullptr;
        runtime.replaceFunction(0x002DA110u, &Mc2Ready);
        runtime.replaceFunction(0x002D2118u, &Mc2Init);
        runtime.replaceFunction(0x002D2398u, &Mc2CreateSocket);
        runtime.replaceFunction(0x002D2428u, &Mc2Zero);
        runtime.replaceFunction(0x002D22C0u, &Mc2Zero);
        runtime.replaceFunction(0x002D36F8u, &Mc2GetInfo);
        runtime.replaceFunction(0x002D3738u, &Mc2Format);
        runtime.replaceFunction(0x002D37B8u, &Mc2Read);
        runtime.replaceFunction(0x002D37F8u, &Mc2Write);
        runtime.replaceFunction(0x002D3838u, &Mc2CreateFile);
        runtime.replaceFunction(0x002D3878u, &Mc2Delete);
        runtime.replaceFunction(0x002D38F8u, &Mc2Mkdir);
        runtime.replaceFunction(0x002D3938u, &Mc2Chdir);
        runtime.replaceFunction(0x002D39F8u, &Mc2SearchFile);
        std::fprintf(stderr, "[mc2] memory card 1: %s\n", g_root.string().c_str());
    }
}
