#ifndef ADVMATCH_VALUE_TYPES_H
#define ADVMATCH_VALUE_TYPES_H

#include <cstddef>
#include <cstdint>

namespace advmatch
{
    using UserId = std::uint64_t;
    using AccountId = std::uint64_t;
    using AssetId = std::uint32_t;
    using SymbolId = std::uint32_t;
    using OrderId = std::uint64_t;
    using ConditionalOrderId = std::uint64_t;
    using PositionId = std::uint64_t;

    using Price = std::uint64_t;
    using Quantity = std::uint64_t;
    using QuantityChange = std::int64_t;

    // Stored balance, signed delta and non-negative transfer amount are
    // different semantic types, even where their underlying integer is equal.
    using Balance = std::int64_t;
    using BalanceChange = std::int64_t;
    using PositiveBalanceChange = std::uint64_t;

    using Scale = std::uint8_t;
    using LeverageMultiplier = std::uint16_t;
    // Fixed-point decimal rate; Position.h defines the scale.
    using MMRRate = std::uint32_t;

    // Milliseconds since the Unix epoch.
    using Timestamp = std::uint64_t;
    using CommandSequence = std::uint64_t;

}

#endif // ADVMATCH_VALUE_TYPES_H
