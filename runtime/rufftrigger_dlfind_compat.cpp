// See rufftrigger_glibc_compat.cpp: keeps the statically linked unwinder working on glibc 2.34.
#if defined(__has_include)
#if __has_include(<features.h>)
#include <features.h>
#endif
#endif
#if defined(__GLIBC__) && defined(__x86_64__) && !defined(RT_NO_GLIBC_COMPAT) && (__GLIBC__ > 2 || __GLIBC_MINOR__ >= 35)
#include <cstdint>

// _dl_find_object: the unwinder (used for every C++ exception, which the runtime throws on guest
// thread switches) cannot work without it, so forward to glibc's when it has one (2.35+) and
// otherwise answer from dl_iterate_phdr.
#include <dlfcn.h>
#include <link.h>
#include <atomic>

namespace
{
    using RtDlFindObject = struct dl_find_object;
    using FindFn = int (*)(void *, RtDlFindObject *);
    std::atomic<FindFn> g_realFind{nullptr};
    std::atomic<int> g_findState{0};   // 0 unknown, 1 real, 2 fallback

    struct FindCtx
    {
        uintptr_t pc;
        RtDlFindObject *out;
        bool found;
    };

    int FindCallback(struct dl_phdr_info *info, size_t, void *data)
    {
        FindCtx *c = static_cast<FindCtx *>(data);
        uintptr_t lo = ~uintptr_t(0), hi = 0, eh = 0;
        bool inside = false;
        for (int i = 0; i < info->dlpi_phnum; ++i)
        {
            const ElfW(Phdr) &ph = info->dlpi_phdr[i];
            const uintptr_t start = info->dlpi_addr + ph.p_vaddr;
            if (ph.p_type == PT_LOAD)
            {
                lo = start < lo ? start : lo;
                hi = start + ph.p_memsz > hi ? start + ph.p_memsz : hi;
                if (c->pc >= start && c->pc < start + ph.p_memsz)
                    inside = true;
            }
            else if (ph.p_type == PT_GNU_EH_FRAME)
                eh = start;
        }
        if (!inside)
            return 0;
        c->out->dlfo_flags = 0;
        c->out->dlfo_map_start = reinterpret_cast<void *>(lo);
        c->out->dlfo_map_end = reinterpret_cast<void *>(hi);
        c->out->dlfo_link_map = nullptr;
        c->out->dlfo_eh_frame = reinterpret_cast<void *>(eh);
        c->found = true;
        return 1;
    }
}

extern "C" int _dl_find_object(void *pc, struct dl_find_object *out) noexcept
{
    int state = g_findState.load(std::memory_order_acquire);
    if (state == 0)
    {
        FindFn f = reinterpret_cast<FindFn>(dlvsym(RTLD_NEXT, "_dl_find_object", "GLIBC_2.35"));
        g_realFind.store(f, std::memory_order_relaxed);
        state = f ? 1 : 2;
        g_findState.store(state, std::memory_order_release);
    }
    if (state == 1)
        return g_realFind.load(std::memory_order_relaxed)(pc, out);
    FindCtx c{reinterpret_cast<uintptr_t>(pc), out, false};
    dl_iterate_phdr(FindCallback, &c);
    return c.found ? 0 : -1;
}
#endif
