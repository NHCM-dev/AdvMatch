//
// Created by NHCM on 2026/9/20.
//

#ifndef ADVMATCH_UTILS_H
#define ADVMATCH_UTILS_H
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>

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
} // advmatch

namespace advmatch
{
    template <typename State>
    struct Result
    {
        explicit Result(State state)
        : state(state), errorMessage(std::nullopt)
        {}

        explicit Result(State state, std::string errorMessage)
        : state(state), errorMessage(std::move(errorMessage))
        {}

        State state;
        std::optional<std::string> errorMessage;
    };
}

#endif //ADVMATCH_UTILS_H