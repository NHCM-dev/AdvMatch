//
// Created by NHCM on 2026/9/20.
//

#ifndef ADVMATCH_UTILS_H
#define ADVMATCH_UTILS_H
#include "Types.h"
#include "Result.h"
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <chrono>

namespace advmatch::util
{
    [[nodiscard]]
    constexpr bool willAddOverflow(int64_t a, int64_t b) noexcept
    {
        return (b > 0 && a > std::numeric_limits<int64_t>::max() - b) ||
            (b < 0 && a < std::numeric_limits<int64_t>::min() - b);
    }

    [[nodiscard]]
    constexpr bool willAddOverflow(uint64_t a, uint64_t b) noexcept
    {
        return a > std::numeric_limits<uint64_t>::max() - b;
    }

    [[nodiscard]]
    constexpr bool willAddOverflow(int64_t a, uint64_t b) noexcept
    {
        if (a >= 0)
        {
            return b > static_cast<uint64_t>(
                std::numeric_limits<int64_t>::max() - a
            );
        }

        const uint64_t negativeMagnitude =
            static_cast<uint64_t>(-(a + 1)) + 1;

        if (b <= negativeMagnitude)
        {
            return false;
        }

        return b - negativeMagnitude >
            static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    }

    [[nodiscard]]
    constexpr bool willAddOverflow(uint64_t a, int64_t b) noexcept
    {
        if (b >= 0)
        {
            return a > std::numeric_limits<uint64_t>::max()
                - static_cast<uint64_t>(b);
        }

        const uint64_t negativeMagnitude =
            static_cast<uint64_t>(-(b + 1)) + 1;

        return a < negativeMagnitude;
    }

    [[nodiscard]]
    constexpr bool willMultiplyOverflow(std::int64_t a, std::int64_t b) noexcept
    {
        if (a == 0 || b == 0)
        {
            return false;
        }

        if ((a == std::numeric_limits<std::int64_t>::min() && b == -1) ||
            (b == std::numeric_limits<std::int64_t>::min() && a == -1))
        {
            return true;
        }

        if (a > 0)
        {
            if (b > 0)
            {
                return a > std::numeric_limits<std::int64_t>::max() / b;
            }

            return b < std::numeric_limits<std::int64_t>::min() / a;
        }

        if (b > 0)
        {
            return a < std::numeric_limits<std::int64_t>::min() / b;
        }

        // a < 0 && b < 0
        return a < std::numeric_limits<std::int64_t>::max() / b;
    }


    [[nodiscard]]
    constexpr bool willMultiplyOverflow(std::uint64_t a, std::uint64_t b) noexcept
    {
#if defined(__GNUC__) || defined(__clang__)
        std::uint64_t product;
        return __builtin_mul_overflow(a, b, &product);
#else
        if (a == 0 || b == 0)
        {
            return false;
        }

        return a > std::numeric_limits<std::uint64_t>::max() / b;
#endif
    }


    [[nodiscard]]
    constexpr bool willMultiplyOverflow(std::int64_t a, std::uint64_t b) noexcept
    {
        if (a == 0 || b == 0)
        {
            return false;
        }

        if (a < 0)
        {
            const std::uint64_t magnitude =
                static_cast<std::uint64_t>(-(a + 1)) + 1;

            const std::uint64_t minMagnitude =
                std::uint64_t{1} << 63;

            return magnitude > minMagnitude / b;
        }

        return static_cast<std::uint64_t>(a) >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max()
            ) / b;
    }


    [[nodiscard]]
    constexpr bool willMultiplyOverflow(std::uint64_t a, std::int64_t b) noexcept
    {
        return willMultiplyOverflow(b, a);
    }

    [[nodiscard]]
    constexpr bool enoughSubtract(std::int64_t a, std::uint64_t b) noexcept
    {
        if (a < 0)
        {
            return false;
        }

        if (b > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        {
            return false;
        }

        return static_cast<std::uint64_t>(a) >= b;
    }

    [[nodiscard]]
    constexpr bool enoughSubtract(std::uint64_t a, std::int64_t b) noexcept
    {
        if (b <= 0)
        {
            return true;
        }

        return a >= static_cast<std::uint64_t>(b);
    }

    [[nodiscard]]
    constexpr bool enoughSubtract(std::uint64_t a, std::uint64_t b) noexcept
    {
        return a >= b;
    }

    [[nodiscard]]
    inline Timestamp getCurrentTime() noexcept
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }
} // advmatch

#endif //ADVMATCH_UTILS_H
