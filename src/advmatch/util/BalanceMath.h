#ifndef ADVMATCH_BALANCE_MATH_H
#define ADVMATCH_BALANCE_MATH_H

#include "Types.h"
#include <limits>
#include <optional>

namespace advmatch
{
    class BalanceMath
    {
    public:
        static std::optional<Balance> multiply(std::uint64_t left, std::uint64_t right) noexcept
        {
            const auto maximum = static_cast<std::uint64_t>(std::numeric_limits<Balance>::max());
#if defined(__GNUC__) || defined(__clang__)
            std::uint64_t amount;
            if (__builtin_mul_overflow(left, right, &amount) || amount > maximum)
            {
                return std::nullopt;
            }
#else
            if (right != 0 && left > maximum / right)
            {
                return std::nullopt;
            }

            const auto amount = left * right;
#endif

            return static_cast<Balance>(amount);
        }
    };
}

#endif // ADVMATCH_BALANCE_MATH_H
