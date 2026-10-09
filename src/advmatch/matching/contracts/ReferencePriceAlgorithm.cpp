#include "advmatch/trading/futures/ReferencePriceAlgorithm.h"
#include "advmatch/util/Types.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>

namespace advmatch
{
    bool ReferencePriceAlgorithm::valid(const ReferencePriceConfig& config) noexcept
    {
        const std::size_t sourceCount = config.sources.empty() ? 1 : config.sources.size();
        if (sourceCount > 32 || config.minimumSources == 0 || config.minimumSources > sourceCount || config.maxAge == 0 || config.maxDeviationRate > RATE_SCALE || config.markClampRate > RATE_SCALE || config.fundingClampRate > RATE_SCALE || std::abs(static_cast<std::int64_t>(config.interestRate)) > RATE_SCALE || config.maxCatchUpPeriods == 0 || config.maxCatchUpPeriods > 32)
        {
            return false;
        }

        for (std::size_t index = 0; index < config.sources.size(); ++index)
        {
            const auto& source = config.sources[index];
            if (source.type > PriceSourceType::External || source.sourceId == 0 || source.weight == 0 || source.numerator == 0 || source.denominator == 0 || (source.type == PriceSourceType::Spot && source.sourceId > std::numeric_limits<SymbolId>::max()))
            {
                return false;
            }

            for (std::size_t previous = 0; previous < index; ++previous)
            {
                if (source.type == config.sources[previous].type && source.sourceId == config.sources[previous].sourceId)
                {
                    return false;
                }
            }
        }

        return true;
    }

    std::optional<ReferencePrices> ReferencePriceAlgorithm::calculate(std::span<const WeightedPrice> prices, Price contractMidpoint, const ReferencePriceConfig& config, const ContractSpec& spec, Timestamp timestamp) const
    {
        if (!valid(config) || prices.size() > 32 || spec.maxFundingRate > RATE_SCALE)
        {
            return std::nullopt;
        }

        std::array<WeightedPrice, 32> selected{};
        std::size_t count = 0;
        for (const auto& price : prices)
        {
            if (price.price != 0 && price.price <= static_cast<Price>(std::numeric_limits<Balance>::max()) && price.weight != 0 && price.timestamp <= timestamp && timestamp - price.timestamp <= config.maxAge)
            {
                selected[count++] = price;
            }
        }

        if (count < config.minimumSources)
        {
            return std::nullopt;
        }

        std::sort(selected.begin(), selected.begin() + count, [](const auto& left, const auto& right) { return left.price < right.price; });
        const Price lower = selected[(count - 1) / 2].price;
        const Price median = lower + (selected[count / 2].price - lower) / 2;
        std::size_t accepted = 0;
        for (std::size_t index = 0; index < count; ++index)
        {
            const WideBalance difference = selected[index].price > median ? selected[index].price - median : median - selected[index].price;
            if (difference * RATE_SCALE <= static_cast<WideBalance>(median) * config.maxDeviationRate)
            {
                selected[accepted++] = selected[index];
            }
        }

        if (accepted < config.minimumSources)
        {
            return std::nullopt;
        }

        const Price indexPrice = this->index(std::span(selected.data(), accepted));
        if (indexPrice == 0 || indexPrice > static_cast<Price>(std::numeric_limits<Balance>::max()))
        {
            return std::nullopt;
        }

        const WideBalance premium = contractMidpoint == 0 ? 0 : (static_cast<WideBalance>(contractMidpoint) - indexPrice) * RATE_SCALE / indexPrice;
        const auto premiumRate = static_cast<std::int32_t>(std::clamp(premium, WideBalance(-static_cast<std::int64_t>(RATE_SCALE)), WideBalance(RATE_SCALE)));
        const Price markPrice = this->mark(indexPrice, contractMidpoint, config);
        const Price deliveryPrice = this->delivery(indexPrice, markPrice);
        const std::int32_t fundingRate = this->funding(premiumRate, config, spec);
        if (markPrice == 0 || deliveryPrice == 0 || markPrice > static_cast<Price>(std::numeric_limits<Balance>::max()) || deliveryPrice > static_cast<Price>(std::numeric_limits<Balance>::max()) || std::abs(static_cast<std::int64_t>(fundingRate)) > spec.maxFundingRate)
        {
            return std::nullopt;
        }

        return ReferencePrices{indexPrice, markPrice, premiumRate, fundingRate, deliveryPrice, static_cast<std::uint32_t>(accepted)};
    }

    Price DefaultReferencePriceAlgorithm::index(std::span<const WeightedPrice> prices) const
    {
        WideBalance numerator = 0;
        std::uint64_t denominator = 0;
        for (const auto& price : prices)
        {
            numerator += static_cast<WideBalance>(price.price) * price.weight;
            denominator += price.weight;
        }

        return denominator == 0 ? 0 : static_cast<Price>(numerator / denominator);
    }

    Price DefaultReferencePriceAlgorithm::mark(Price indexPrice, Price contractMidpoint, const ReferencePriceConfig& config) const
    {
        const WideBalance limit = static_cast<WideBalance>(indexPrice) * config.markClampRate / RATE_SCALE;
        const WideBalance basis = contractMidpoint == 0 ? 0 : static_cast<WideBalance>(contractMidpoint) - indexPrice;
        return static_cast<Price>(static_cast<WideBalance>(indexPrice) + std::clamp(basis, WideBalance(-limit), limit));
    }

    std::int32_t DefaultReferencePriceAlgorithm::funding(std::int32_t premiumRate, const ReferencePriceConfig& config, const ContractSpec& spec) const
    {
        const std::int64_t adjustment = std::clamp(static_cast<std::int64_t>(config.interestRate) - premiumRate, -static_cast<std::int64_t>(config.fundingClampRate), static_cast<std::int64_t>(config.fundingClampRate));
        return static_cast<std::int32_t>(std::clamp(premiumRate + adjustment, -static_cast<std::int64_t>(spec.maxFundingRate), static_cast<std::int64_t>(spec.maxFundingRate)));
    }

    Price DefaultReferencePriceAlgorithm::delivery(Price indexPrice, Price) const
    {
        return indexPrice;
    }
}
