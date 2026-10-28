// Built-in sampling profiler (no perf in the build container).
//   RT_PROF=<file> RT_PROF_AT=<seconds after start> RT_PROF_SECS=<duration>
// SIGPROF fires every 1 ms of process CPU time; the handler records the interrupted PC and the
// thread id. After the window the samples are written as text lines "tid pc-offset" (offset from
// the executable's load base), to be symbolized with tools/profsym.py.
#if defined(__linux__)
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <link.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <thread>
#include <ucontext.h>
#include <unistd.h>

namespace rufftrigger
{
    namespace
    {
        constexpr size_t kMaxSamples = 1u << 22;
        struct Sample { uint64_t pc; uint32_t tid; };
        Sample *g_samples = nullptr;
        std::atomic<size_t> g_count{0};
        std::atomic<bool> g_on{false};
        uintptr_t g_base = 0;

        void onProf(int, siginfo_t *, void *uc)
        {
            if (!g_on.load(std::memory_order_relaxed))
                return;
            const size_t k = g_count.fetch_add(1, std::memory_order_relaxed);
            if (k >= kMaxSamples)
                return;
            const auto *ctx = static_cast<const ucontext_t *>(uc);
#if defined(__x86_64__)
            g_samples[k].pc = static_cast<uint64_t>(ctx->uc_mcontext.gregs[REG_RIP]);
#elif defined(__aarch64__)
            g_samples[k].pc = static_cast<uint64_t>(ctx->uc_mcontext.pc);
#else
            g_samples[k].pc = 0;
#endif
            g_samples[k].tid = static_cast<uint32_t>(syscall(SYS_gettid));
        }

        int findBase(struct dl_phdr_info *info, size_t, void *)
        {
            if (!info->dlpi_name || !*info->dlpi_name)   // the main executable
            {
                g_base = info->dlpi_addr;
                return 1;
            }
            return 0;
        }
    }

    std::atomic<bool> g_profTrigger{false};

    void StartProfiler()
    {
        const char *path = std::getenv("RT_PROF");
        if (!path || !*path)
            return;
        const double at = std::getenv("RT_PROF_AT") ? std::atof(std::getenv("RT_PROF_AT")) : 0.0;
        const double secs = std::getenv("RT_PROF_SECS") ? std::atof(std::getenv("RT_PROF_SECS")) : 20.0;
        g_samples = new Sample[kMaxSamples];
        dl_iterate_phdr(findBase, nullptr);
        struct sigaction sa{};
        sa.sa_sigaction = onProf;
        sa.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGPROF, &sa, nullptr);
        itimerval tv{};
        tv.it_interval.tv_usec = 1000;
        tv.it_value.tv_usec = 1000;
        setitimer(ITIMER_PROF, &tv, nullptr);
        std::string out = path;
        std::thread([out, at, secs] {
            if (at < 0)   // RT_PROF_AT=-1: start when the debug test reaches its fire loop
                while (!g_profTrigger.load()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            else
                std::this_thread::sleep_for(std::chrono::duration<double>(at));
            g_on = true;
            std::fprintf(stderr, "[prof] sampling for %.0f s\n", secs);
            std::this_thread::sleep_for(std::chrono::duration<double>(secs));
            g_on = false;
            const size_t n = std::min(g_count.load(), kMaxSamples);
            if (std::FILE *f = std::fopen(out.c_str(), "w"))
            {
                for (size_t k = 0; k < n; ++k)
                    std::fprintf(f, "%u %llx\n", g_samples[k].tid, static_cast<unsigned long long>(g_samples[k].pc - g_base));
                std::fclose(f);
            }
            {
                // Shared library layout, for symbolizing samples outside the executable.
                std::FILE *in = std::fopen("/proc/self/maps", "r");
                std::FILE *mo = std::fopen((out + ".maps").c_str(), "w");
                char line[1024];
                while (in && mo && std::fgets(line, sizeof(line), in))
                    std::fputs(line, mo);
                if (mo) std::fprintf(mo, "# base %llx\n", static_cast<unsigned long long>(g_base));
                if (in) std::fclose(in);
                if (mo) std::fclose(mo);
            }
            std::fprintf(stderr, "[prof] wrote %zu samples to %s\n", n, out.c_str());
        }).detach();
    }
}
#else
// The sampling profiler uses SIGPROF and ucontext, which only Linux has here.
#include <atomic>
namespace rufftrigger
{
    std::atomic<bool> g_profTrigger{false};
    void StartProfiler() {}
}
#endif
