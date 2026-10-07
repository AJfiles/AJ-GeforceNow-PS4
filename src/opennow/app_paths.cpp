#include "app_paths.hpp"

#include <cstdio>
#include <sys/stat.h>

namespace opennow
{
const std::string& AppHomePath()
{
    static const std::string path = "/data/gfnps4";
    return path;
}

const std::string& LegacyAppHomePath()
{
    static const std::string path = "/data/gfnps4";
    return path;
}

void PrepareAppStorage()
{
    mkdir(AppHomePath().c_str(), 0700);
}

} // namespace opennow
