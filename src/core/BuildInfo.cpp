#include "core/BuildInfo.h"

namespace overdub::core
{
BuildInfo currentBuildInfo()
{
#if defined(__x86_64__)
    const char* architecture = "x86_64";
#elif defined(__arm64__)
    const char* architecture = "arm64";
#else
    #error "overdub: unsupported architecture"
#endif

    return BuildInfo { OVERDUB_VERSION_STRING, architecture };
}
} // namespace overdub::core
