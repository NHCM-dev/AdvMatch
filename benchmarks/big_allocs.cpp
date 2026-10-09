#include "advmatch/api/binding/AllocatorBenchmark.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string_view>
#include <vector>

namespace
{
    using advmatch::binding::benchmark::runPool;
    using advmatch::binding::benchmark::runNewDelete;
    using Result = advmatch::binding::benchmark::AllocationResult;

    double operationsPerSecond(std::size_t objectCount, double seconds)
    {
        return static_cast<double>(objectCount) * 2.0 / seconds;
    }
}

int main(int argumentCount, char** arguments)
{
    std::size_t objectCount = 1'000'000;
    if (argumentCount == 3 && std::string_view(arguments[1]) == "--objects")
    {
        objectCount = static_cast<std::size_t>(std::strtoull(arguments[2], nullptr, 10));
    }

    const Result pool = runPool(objectCount);
    const Result normal = runNewDelete(objectCount);
    if (pool.checksum != normal.checksum)
    {
        std::cerr << "checksum mismatch\n";
        return 1;
    }

    std::cout << "objects=" << objectCount << '\n';
    std::cout << "BigAllocs seconds=" << pool.seconds
              << " operations_per_second=" << operationsPerSecond(objectCount, pool.seconds) << '\n';
    std::cout << "new_delete seconds=" << normal.seconds
              << " operations_per_second=" << operationsPerSecond(objectCount, normal.seconds) << '\n';
    std::cout << "speedup=" << normal.seconds / pool.seconds << "x\n";
}
