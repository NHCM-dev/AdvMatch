#include "advmatch/core.hpp"

#include <fmt/format.h>

namespace advmatch
{
    std::string buildDescription()
    {
        return fmt::format("AdvMatch {}", ADVMATCH_VERSION);
    }
}
