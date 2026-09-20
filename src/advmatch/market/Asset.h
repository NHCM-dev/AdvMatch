//
// Created by NHCM on 2026/9/20.
//

#ifndef ADVMATCH_ASSET_H
#define ADVMATCH_ASSET_H
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "../util/Utils.h"

namespace advmatch
{
    using AssetId = std::uint32_t;

    using Balance = std::int64_t;

    using BalanceChange = std::int64_t;
    using PositiveBalanceChange = std::uint64_t;

    using Scale = std::uint8_t;

    struct Asset
    {
        AssetId id;
        Scale scale;
    };

    struct AssetBalance
    {
        Balance active;
        Balance locked;
    };

    enum class BalanceOpResult : std::uint8_t
    {
        Success,
        Overflow,
        Insufficient,
        NoSuchAsset,
        Unknown
    };

    struct BalanceResult : Result<BalanceOpResult>
    {
        BalanceResult(BalanceOpResult result)
        : Result(result)
        {}

        BalanceResult(BalanceOpResult result, std::string errorMessage)
        : Result(result, std::move(errorMessage))
        {}

        explicit operator bool() const noexcept
        {
            return this->state == BalanceOpResult::Success;
        }
    };
} // advmatch

#endif //ADVMATCH_ASSET_H