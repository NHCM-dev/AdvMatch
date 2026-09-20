#include "advmatch/core.hpp"

#include <fmt/format.h>

namespace advmatch {
std::string build_description() {
    return fmt::format("AdvMatch {}", ADVMATCH_VERSION);
}
}
