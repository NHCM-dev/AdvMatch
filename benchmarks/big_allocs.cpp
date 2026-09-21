#include "advmatch/util/BigAllocs.h"

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
    struct BenchmarkObject
    {
        explicit BenchmarkObject(std::uint64_t id)
            : id(id)
        {
            this->values[0] = id * 17;
        }

        std::uint64_t id;
        std::array<std::uint64_t, 7> values{};
    };

    struct Result
    {
        double seconds;
        std::uint64_t checksum;
    };

    Result runPool(std::size_t objectCount)
    {
        advmatch::BigAllocs<BenchmarkObject> pool;
        pool.reserve(objectCount);

        std::vector<BenchmarkObject*> objects;
        objects.reserve(objectCount);

        const auto started = std::chrono::steady_clock::now();
        for (std::size_t index = 0; index < objectCount; ++index)
        {
            objects.push_back(pool.emplace(index));
        }

        std::uint64_t checksum = 0;
        for (std::size_t index = objectCount; index > 0; --index)
        {
            BenchmarkObject* object = objects[index - 1];
            checksum += object->id + object->values[0];
            pool.release(object);
        }

        const auto stopped = std::chrono::steady_clock::now();
        return {std::chrono::duration<double>(stopped - started).count(), checksum};
    }

    Result runNewDelete(std::size_t objectCount)
    {
        std::vector<BenchmarkObject*> objects;
        objects.reserve(objectCount);

        const auto started = std::chrono::steady_clock::now();
        for (std::size_t index = 0; index < objectCount; ++index)
        {
            objects.push_back(new BenchmarkObject(index));
        }

        std::uint64_t checksum = 0;
        for (std::size_t index = objectCount; index > 0; --index)
        {
            BenchmarkObject* object = objects[index - 1];
            checksum += object->id + object->values[0];
            delete object;
        }

        const auto stopped = std::chrono::steady_clock::now();
        return {std::chrono::duration<double>(stopped - started).count(), checksum};
    }

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
