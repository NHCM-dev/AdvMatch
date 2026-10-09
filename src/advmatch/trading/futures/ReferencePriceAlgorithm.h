#ifndef ADVMATCH_REFERENCE_PRICE_ALGORITHM_H
#define ADVMATCH_REFERENCE_PRICE_ALGORITHM_H

#include "ReferencePrice.h"
#include "advmatch/sdk/Export.h"
#include <optional>
#include <span>

namespace advmatch
{
    // Custom strategies run in the caller's calculation layer. Submit their
    // resolved ReferencePrices, so recovery never invokes application code.
    class ADVMATCH_SDK_API ReferencePriceAlgorithm
    {
    public:
        virtual ~ReferencePriceAlgorithm() = default;
        virtual Price index(std::span<const WeightedPrice> prices) const = 0;
        virtual Price mark(Price indexPrice, Price contractMidpoint, const ReferencePriceConfig& config) const = 0;
        virtual std::int32_t funding(std::int32_t premiumRate, const ReferencePriceConfig& config, const ContractSpec& spec) const = 0;
        virtual Price delivery(Price indexPrice, Price markPrice) const = 0;

        // Rejects future/stale sources, discards median outliers, then invokes
        // the strategy. At most 32 observations; no wall-clock reads or I/O.
        std::optional<ReferencePrices> calculate(std::span<const WeightedPrice> prices, Price contractMidpoint, const ReferencePriceConfig& config, const ContractSpec& spec, Timestamp timestamp) const;
        static bool valid(const ReferencePriceConfig& config) noexcept;
    };

    class ADVMATCH_SDK_API DefaultReferencePriceAlgorithm : public ReferencePriceAlgorithm
    {
    public:
        Price index(std::span<const WeightedPrice> prices) const override;
        Price mark(Price indexPrice, Price contractMidpoint, const ReferencePriceConfig& config) const override;
        std::int32_t funding(std::int32_t premiumRate, const ReferencePriceConfig& config, const ContractSpec& spec) const override;
        Price delivery(Price indexPrice, Price markPrice) const override;
    };
}

#endif // ADVMATCH_REFERENCE_PRICE_ALGORITHM_H
