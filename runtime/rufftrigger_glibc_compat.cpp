// Lets a binary built on a new distribution (glibc 2.39) run on glibc 2.34 and later
// (Ubuntu 22.04, Debian 12, Fedora 35, ...).
//
// Building against glibc 2.38+ headers binds a few functions to new symbol versions: strtol &
// co. and sscanf become __isoc23_* (C23 parsing of "0b" prefixes), fmod/fmodf get a 2.38 version
// and hypot/hypotf a 2.35 one. Defining those names in the executable makes the link resolve
// them here instead of importing them, and the definitions call the old versions of the same
// functions, which every glibc since 2.2.5 has.
#if defined(__has_include)
#if __has_include(<features.h>)
#include <features.h>
#endif
#endif
#if defined(__GLIBC__) && defined(__x86_64__) && !defined(RT_NO_GLIBC_COMPAT)
#include <cstdarg>

extern "C"
{
    long rt_old_strtol(const char *, char **, int);
    unsigned long rt_old_strtoul(const char *, char **, int);
    long long rt_old_strtoll(const char *, char **, int);
    unsigned long long rt_old_strtoull(const char *, char **, int);
    int rt_old_vsscanf(const char *, const char *, va_list);
    int rt_old_vfscanf(void *, const char *, va_list);
    double rt_old_fmod(double, double);
    float rt_old_fmodf(float, float);
    double rt_old_hypot(double, double);
    float rt_old_hypotf(float, float);
}

__asm__(".symver rt_old_strtol,strtol@GLIBC_2.2.5");
__asm__(".symver rt_old_strtoul,strtoul@GLIBC_2.2.5");
__asm__(".symver rt_old_strtoll,strtoll@GLIBC_2.2.5");
__asm__(".symver rt_old_strtoull,strtoull@GLIBC_2.2.5");
__asm__(".symver rt_old_vsscanf,vsscanf@GLIBC_2.2.5");
__asm__(".symver rt_old_vfscanf,vfscanf@GLIBC_2.2.5");
__asm__(".symver rt_old_fmod,fmod@GLIBC_2.2.5");
__asm__(".symver rt_old_fmodf,fmodf@GLIBC_2.2.5");
__asm__(".symver rt_old_hypot,hypot@GLIBC_2.2.5");
__asm__(".symver rt_old_hypotf,hypotf@GLIBC_2.2.5");

extern "C"
{
    long __isoc23_strtol(const char *s, char **e, int b) { return rt_old_strtol(s, e, b); }
    unsigned long __isoc23_strtoul(const char *s, char **e, int b) { return rt_old_strtoul(s, e, b); }
    long long __isoc23_strtoll(const char *s, char **e, int b) { return rt_old_strtoll(s, e, b); }
    unsigned long long __isoc23_strtoull(const char *s, char **e, int b) { return rt_old_strtoull(s, e, b); }
    int __isoc23_vsscanf(const char *s, const char *f, va_list a) { return rt_old_vsscanf(s, f, a); }
    int __isoc23_sscanf(const char *s, const char *f, ...)
    {
        va_list a;
        va_start(a, f);
        const int r = rt_old_vsscanf(s, f, a);
        va_end(a);
        return r;
    }
    int __isoc23_fscanf(void *fp, const char *f, ...)
    {
        va_list a;
        va_start(a, f);
        const int r = rt_old_vfscanf(fp, f, a);
        va_end(a);
        return r;
    }
    double fmod(double x, double y) { return rt_old_fmod(x, y); }
    float fmodf(float x, float y) { return rt_old_fmodf(x, y); }
    double hypot(double x, double y) { return rt_old_hypot(x, y); }
    float hypotf(float x, float y) { return rt_old_hypotf(x, y); }

    // From the statically linked libstdc++/libgcc: std::random_device uses arc4random (2.36),
    // the unwinder _dl_find_object (2.35; in rufftrigger_dlfind_compat.cpp).
    long syscall(long, ...) noexcept;
    unsigned int arc4random(void)
    {
        unsigned int v = 0;
        if (syscall(318 /* SYS_getrandom */, &v, sizeof(v), 0) != static_cast<long>(sizeof(v)))
            v = static_cast<unsigned int>(reinterpret_cast<unsigned long>(&v)) * 2654435761u;
        return v;
    }
}
#endif
