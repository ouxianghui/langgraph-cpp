#include "foundation/utils/executable_path.hpp"

#include <cstdlib>
#include <cstring>
#include <vector>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#if defined(__linux__)
#include <climits>
#include <unistd.h>
#endif

namespace lgc {

std::filesystem::path executableDirectory(char* argv0)
{
    namespace fs = std::filesystem;
    std::error_code ec;

#if defined(__APPLE__)
    std::uint32_t exeSize = 0;
    if (_NSGetExecutablePath(nullptr, &exeSize) == -1 && exeSize > 0) {
        std::vector<char> buf(exeSize);
        if (_NSGetExecutablePath(buf.data(), &exeSize) == 0) {
            return fs::weakly_canonical(fs::path(buf.data()), ec).parent_path();
        }
    }
#elif defined(__linux__)
    std::vector<char> buf(static_cast<std::size_t>(PATH_MAX) + 1U);
    const ssize_t n = ::readlink("/proc/self/exe", buf.data(), PATH_MAX);
    if (n > 0) {
        buf[static_cast<std::size_t>(n)] = '\0';
        return fs::weakly_canonical(fs::path(buf.data()), ec).parent_path();
    }
#endif
    if (argv0 != nullptr && argv0[0] != '\0') {
        fs::path p(argv0);
        if (p.is_absolute()) {
            return fs::weakly_canonical(p, ec).parent_path();
        }
        return (fs::current_path(ec) / p).lexically_normal().parent_path();
    }
    return fs::current_path(ec);
}

std::filesystem::path resolveConfigDirectory(char* argv0)
{
    namespace fs = std::filesystem;
    if (const char* override = std::getenv("BOT_CONFIG_DIR");
        override != nullptr && *override != '\0') {
        return fs::path(override);
    }
    return executableDirectory(argv0);
}

} // namespace lgc
