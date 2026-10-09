#ifndef ADVMATCH_ALLOCATOR_BENCHMARK_H
#define ADVMATCH_ALLOCATOR_BENCHMARK_H

#include "advmatch/util/BigAllocs.h"

#include <array>
#include <chrono>
#include <memory>
#include <vector>

namespace advmatch::binding::benchmark
{
    struct Object
    {
        explicit Object(std::uint64_t id) : id(id)
        {
            this->values[0] = id * 17;
        }

        std::uint64_t id;
        std::array<std::uint64_t, 7> values{};
    };

    struct AllocationResult
    {
        double seconds;
        std::uint64_t checksum;
    };

    // C++ executable and Java JNI benchmark measure this exact workload.
    inline AllocationResult runPool(std::size_t count)
    {
        BigAllocs<Object> pool;
        pool.reserve(count);
        std::vector<Object*> objects;
        objects.reserve(count);
        const auto started = std::chrono::steady_clock::now();
        for (std::size_t index = 0; index < count; ++index)
        {
            objects.push_back(pool.emplace(index));
        }

        std::uint64_t checksum = 0;
        for (std::size_t index = count; index > 0; --index)
        {
            auto* object = objects[index - 1];
            checksum += object->id + object->values[0];
            pool.release(object);
        }

        return {std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count(), checksum};
    }

    inline AllocationResult runNewDelete(std::size_t count)
    {
        // unique_ptr also cleans up a partially allocated workload on failure.
        std::vector<std::unique_ptr<Object>> objects;
        objects.reserve(count);
        const auto started = std::chrono::steady_clock::now();
        for (std::size_t index = 0; index < count; ++index)
        {
            objects.push_back(std::make_unique<Object>(index));
        }

        std::uint64_t checksum = 0;
        for (std::size_t index = count; index > 0; --index)
        {
            const auto* object = objects[index - 1].get();
            checksum += object->id + object->values[0];
            objects[index - 1].reset();
        }

        return {std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count(), checksum};
    }
}

#endif // ADVMATCH_ALLOCATOR_BENCHMARK_H
