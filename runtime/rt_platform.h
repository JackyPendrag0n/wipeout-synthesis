// Small POSIX pieces the runtime uses that Windows spells differently.
#pragma once
#include <cstdlib>

#if defined(_WIN32)
inline int setenv(const char *name, const char *value, int overwrite)
{
    if (!overwrite && std::getenv(name))
        return 0;
    return _putenv_s(name, value);
}
#endif
