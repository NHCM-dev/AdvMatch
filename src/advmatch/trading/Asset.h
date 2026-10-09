//
// Created by NHCM on 2026/9/20.
//

#ifndef ADVMATCH_ASSET_H
#define ADVMATCH_ASSET_H
#include "../util/ValueTypes.h"
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "../util/Result.h"

namespace advmatch
{
    struct Asset
    {
        Asset(std::string displayName, std::string assetName, AssetId assetId, Scale scale)
            : displayName(std::move(displayName)), assetName(std::move(assetName)), assetId(assetId), scale(scale)
        {
            if (scale >= 8)
            {
                throw std::range_error("Scale too big");
            }
        }

        std::string displayName;
        std::string assetName;

        AssetId assetId;
        Scale scale;
    };

    struct AssetBalance
    {
        Balance active;
        Balance reserved;
    };

    struct AssetBalanceEntry
    {
        AssetId assetId;
        AssetBalance balance;
    };

    enum class BalanceOpResult : std::uint8_t
    {
        Success,
        Overflow,
        Insufficient,
        NoSuchAsset,
        InvalidAccount,
        Unknown,
        StaleUpdate
    };

    struct BalanceResult : Result<BalanceOpResult>
    {
        BalanceResult(BalanceOpResult result)
            : Result(result)
        {
        }

        BalanceResult(BalanceOpResult result, std::string errorMessage)
            : Result(result, std::move(errorMessage))
        {
        }

        explicit operator bool() const noexcept
        {
            return this->state == BalanceOpResult::Success;
        }
    };
} // advmatch

#endif //ADVMATCH_ASSET_H
