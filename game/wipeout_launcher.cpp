// Finds the game files before the runtime starts, so the binary can be started by double-click or
// by dropping the disc image on it:
//   wipeout-fusion                      uses ./SLUS_204.62, else <exe dir>/game/SLUS_204.62
//   wipeout-fusion "WipEout Fusion.iso" extracts the disc into <exe dir>/game once, then boots it
//   wipeout-fusion path/to/SLUS_204.62  boots that file (the old behaviour)
// The game opens its files relative to the working directory, so this also changes into the
// folder that holds SLUS_204.62.
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace fs = std::filesystem;

namespace
{
    constexpr const char *kElf = "SLUS_204.62";
    constexpr uint32_t kSector = 2048;

    void Tell(const std::string &msg)
    {
        std::cerr << msg << std::endl;
#if defined(_WIN32)
        MessageBoxA(nullptr, msg.c_str(), "WipEout Fusion", MB_OK | MB_ICONINFORMATION);
#endif
    }

    fs::path ExeDir(const char *argv0)
    {
        std::error_code ec;
#if defined(_WIN32)
        wchar_t buf[32768];
        const DWORD n = GetModuleFileNameW(nullptr, buf, 32768);
        if (n > 0 && n < 32768)
            return fs::path(std::wstring(buf, n)).parent_path();
#elif defined(__APPLE__)
        char buf[4096];
        uint32_t size = sizeof(buf);
        if (_NSGetExecutablePath(buf, &size) == 0)
            return fs::weakly_canonical(fs::path(buf), ec).parent_path();
#else
        const fs::path self = fs::read_symlink("/proc/self/exe", ec);
        if (!ec)
            return self.parent_path();
#endif
        return fs::absolute(fs::path(argv0 ? argv0 : "."), ec).parent_path();
    }

    // argv is in the ANSI code page on Windows; take the wide command line so any path works.
    fs::path ArgPath(const char *arg)
    {
#if defined(_WIN32)
        int n = 0;
        if (LPWSTR *w = CommandLineToArgvW(GetCommandLineW(), &n))
        {
            fs::path p = n >= 2 ? fs::path(w[1]) : fs::path(arg);
            LocalFree(w);
            return p;
        }
#endif
        return fs::path(arg);
    }

    bool HasExt(const fs::path &p, const char *ext)
    {
        std::string e = p.extension().string();
        std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        return e == ext;
    }

    bool IsGameDir(const fs::path &dir)
    {
        std::error_code ec;
        return fs::is_regular_file(dir / kElf, ec) && fs::is_regular_file(dir / "WOF" / "FILESYS.WAD", ec);
    }

    // ISO9660 extraction (same rules as tools/iso_extract.py: names without ";1", disc spelling kept).
    struct Iso
    {
        std::ifstream f;
        uint64_t done = 0, total = 0;
        int lastPct = -1;
        std::set<uint32_t> seen;

        bool Read(uint64_t off, void *dst, size_t len)
        {
            f.clear();
            f.seekg(std::streamoff(off));
            f.read(static_cast<char *>(dst), std::streamsize(len));
            return size_t(f.gcount()) == len;
        }

        bool CopyFile(uint32_t lba, uint32_t size, const fs::path &out)
        {
            std::ofstream o(out, std::ios::binary | std::ios::trunc);
            if (!o)
                return false;
            std::vector<char> buf(4u << 20);
            uint64_t off = uint64_t(lba) * kSector;
            for (uint32_t left = size; left > 0;)
            {
                const uint32_t n = std::min<uint32_t>(left, uint32_t(buf.size()));
                if (!Read(off, buf.data(), n))
                    return false;
                o.write(buf.data(), n);
                off += n;
                left -= n;
                done += n;
                const int pct = total ? int(done * 100 / total) : 0;
                if (pct != lastPct)
                {
                    lastPct = pct;
                    std::fprintf(stderr, "\r  extracting... %3d%%", pct);
                }
            }
            return bool(o);
        }

        bool Dir(uint32_t lba, uint32_t size, const fs::path &out)
        {
            if (!seen.insert(lba).second)
                return true;
            std::error_code ec;
            fs::create_directories(out, ec);
            std::vector<uint8_t> d((size + kSector - 1) / kSector * kSector);
            if (!Read(uint64_t(lba) * kSector, d.data(), d.size()))
                return false;
            for (size_t pos = 0; pos < d.size();)
            {
                const uint8_t len = d[pos];
                if (len == 0)   // records never cross a sector: skip to the next one
                {
                    pos = (pos / kSector + 1) * kSector;
                    continue;
                }
                if (pos + len > d.size() || len < 34)
                    break;
                const uint8_t *r = &d[pos];
                pos += len;
                uint32_t clba, csize;
                std::memcpy(&clba, r + 2, 4);
                std::memcpy(&csize, r + 10, 4);
                const bool isDir = r[25] & 2;
                std::string name(reinterpret_cast<const char *>(r + 33), r[32]);
                if (name == std::string(1, '\0') || name == std::string(1, '\1'))
                    continue;
                name = name.substr(0, name.find(';'));
                if (!isDir && !name.empty() && name.back() == '.')
                    name.pop_back();
                if (name.empty() || name.find("..") != std::string::npos || name.find('/') != std::string::npos ||
                    name.find('\\') != std::string::npos)
                    continue;
                if (isDir ? !Dir(clba, csize, out / name) : !CopyFile(clba, csize, out / name))
                    return false;
            }
            return true;
        }
    };

    bool ExtractIso(const fs::path &iso, const fs::path &dest)
    {
        Iso x;
        x.f.open(iso, std::ios::binary);
        uint8_t pvd[kSector];
        if (!x.f || !x.Read(16 * kSector, pvd, kSector) || pvd[0] != 1 || std::memcmp(pvd + 1, "CD001", 5) != 0)
        {
            Tell("That file is not an ISO9660 disc image:\n" + iso.string());
            return false;
        }
        std::error_code ec;
        x.total = fs::file_size(iso, ec);
        uint32_t rootLba, rootSize;
        std::memcpy(&rootLba, pvd + 156 + 2, 4);
        std::memcpy(&rootSize, pvd + 156 + 10, 4);
        const fs::path tmp = dest.string() + ".tmp";
        fs::remove_all(tmp, ec);
        std::cerr << "Extracting " << iso.string() << " into " << dest.string() << " (one time, about 2.4 GB)" << std::endl;
        const bool ok = x.Dir(rootLba, rootSize, tmp);
        std::fprintf(stderr, "\n");
        if (!ok)
        {
            Tell("Extracting the disc failed (disk full, or a damaged image?):\n" + iso.string());
            return false;
        }
        if (!IsGameDir(tmp))
        {
            fs::remove_all(tmp, ec);
            Tell("That image has no SLUS_204.62 / WOF/FILESYS.WAD.\nThis port needs the NTSC-U (SLUS-20462) WipEout Fusion disc.");
            return false;
        }
        fs::remove_all(dest, ec);
        fs::rename(tmp, dest, ec);
        if (ec)
        {
            Tell("Could not rename " + tmp.string() + " to " + dest.string() + ": " + ec.message());
            return false;
        }
        std::cerr << "Done." << std::endl;
        return true;
    }

    fs::path Boot(const fs::path &dir)
    {
        std::error_code ec;
        fs::current_path(dir, ec);
        return fs::absolute(dir / kElf, ec);
    }
}

// Returns the ELF to boot (and changes into its folder), or an empty path when there is nothing to
// boot (the reason has been shown). Removes an .iso argument from argv.
fs::path WipeoutResolveBoot(int &argc, char **argv)
{
    std::error_code ec;
    const fs::path exeDir = ExeDir(argc > 0 ? argv[0] : nullptr);
    const fs::path gameDir = exeDir / "game";
    if (argc >= 2 && argv[1] && argv[1][0])
    {
        const fs::path arg = ArgPath(argv[1]);
        if (HasExt(arg, ".iso") || HasExt(arg, ".bin") || HasExt(arg, ".img"))
        {
            for (int i = 1; i + 1 < argc; ++i)
                argv[i] = argv[i + 1];
            --argc;
            if (!IsGameDir(gameDir) && !ExtractIso(arg, gameDir))
                return {};
            return Boot(gameDir);
        }
        if (fs::is_directory(arg, ec) && IsGameDir(arg))
            return Boot(arg);
        const fs::path elf = fs::absolute(arg, ec);
        fs::current_path(elf.parent_path(), ec);
        return elf;
    }
    if (IsGameDir(fs::current_path(ec)))
        return Boot(fs::current_path(ec));
    if (IsGameDir(gameDir))
        return Boot(gameDir);
    for (const auto &e : fs::directory_iterator(exeDir, ec))
        if (e.is_regular_file(ec) && HasExt(e.path(), ".iso"))
            return ExtractIso(e.path(), gameDir) ? Boot(gameDir) : fs::path();
    Tell("No game files yet.\n\nDrag your own WipEout Fusion (NTSC-U, SLUS-20462) .iso onto this program,\n"
         "or put the .iso in this folder:\n" + exeDir.string() + "\n\nThe disc is extracted once into a \"game\" folder next to the program.");
    return {};
}
