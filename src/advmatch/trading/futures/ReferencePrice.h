#ifndef ADVMATCH_REFERENCE_PRICE_H
#define ADVMATCH_REFERENCE_PRICE_H

#include "Contract.h"
#include <vector>

namespace advmatch
{
    enum class PriceSourceType : std::uint8_t
    {
        Spot,
        External
    };

    struct PriceSource
    {
        PriceSourceType type = PriceSourceType::Spot;
        // Spot: SymbolId. External: application-assigned feed ID.
        std::uint64_t sourceId = 0;
        std::uint32_t weight = 1;
        // Explicitly normalize the source into the contract's price units.
        std::uint32_t numerator = 1;
        std::uint32_t denominator = 1;
    };

    struct ReferencePriceConfig
    {
        // Empty selects the Spot pair with the same base/quote assets.
        std::vector<PriceSource> sources;
        std::uint32_t minimumSources = 1;
        Timestamp maxAge = 30'000;
        MMRRate maxDeviationRate = 50'000;
        MMRRate markClampRate = 20'000;
        std::int32_t interestRate = 100;
        MMRRate fundingClampRate = 500;
        bool automaticFunding = true;
        bool automaticDelivery = true;
        std::uint32_t maxCatchUpPeriods = 8;
    };

    struct ExternalPrice
    {
        std::uint64_t sourceId = 0;
        Price price = 0;
        Timestamp timestamp = 0;
    };

    struct WeightedPrice
    {
        Price price = 0;
        Timestamp timestamp = 0;
        std::uint32_t weight = 1;
    };

    struct ReferencePrices
    {
        Price indexPrice = 0;
        Price markPrice = 0;
        std::int32_t premiumRate = 0;
        std::int32_t fundingRate = 0;
        Price deliveryPrice = 0;
        std::uint32_t sourceCount = 0;
    };

    struct ContractSchedule
    {
        Timestamp fundingInterval = 8 * 60 * 60 * 1000;
        // Zero retains the current next deadline; a new deadline must be future.
        Timestamp nextFundingTimestamp = 0;
        MMRRate maxFundingRate = 10'000;
        // Zero retains the current expiry; only futures have an expiry.
        Timestamp expiryTimestamp = 0;
    };
}

#endif // ADVMATCH_REFERENCE_PRICE_H
