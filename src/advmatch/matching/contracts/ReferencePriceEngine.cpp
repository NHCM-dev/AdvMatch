#include "ReferencePriceEngine.h"
#include "ContractEngine.h"
#include "advmatch/matching/MatchingEngine.h"

#include <array>
#include <algorithm>
#include <cstdlib>

namespace advmatch
{
    namespace
    {
        Price midpoint(const MatchingEngine& matching, SymbolId symbolId)
        {
            const auto bid = matching.bestBid(symbolId);
            const auto ask = matching.bestAsk(symbolId);
            if (!bid || !ask || bid->price > ask->price)
            {
                return 0;
            }

            return bid->price + (ask->price - bid->price) / 2;
        }

        bool validPrice(Price price)
        {
            return price != 0 && price <= static_cast<Price>(std::numeric_limits<Balance>::max());
        }
    }

    ContractResult ReferencePriceEngine::configure(ContractMarket& market, const ReferencePriceConfig& config, const SymbolManager& symbols) const
    {
        if (!ReferencePriceAlgorithm::valid(config))
        {
            return {ContractState::InvalidConfiguration};
        }

        for (const auto& source : config.sources)
        {
            if (source.type == PriceSourceType::Spot)
            {
                const auto* symbol = symbols.getSymbol(static_cast<SymbolId>(source.sourceId));
                if (!symbol || symbol->symbolType != SymbolType::Spot || symbol->baseAssetId != market.symbol->baseAssetId)
                {
                    return {ContractState::InvalidSymbol};
                }
            }
        }

        // Allocate before changing the active configuration. Preserve feeds
        // still bound by the replacement, and drop obsolete external inputs.
        std::vector<ExternalPrice> external;
        external.reserve(config.sources.size());
        for (const auto& source : config.sources)
        {
            if (source.type == PriceSourceType::External)
            {
                auto previous = std::find_if(market.externalPrices.begin(), market.externalPrices.end(), [&](const auto& value) { return value.sourceId == source.sourceId; });
                external.push_back(previous == market.externalPrices.end() ? ExternalPrice{source.sourceId, 0, 0} : *previous);
            }
        }

        market.referenceConfig = config;
        market.externalPrices.swap(external);
        market.defaultSpotSymbolId = 0;
        return {};
    }

    ContractResult ReferencePriceEngine::publish(ContractMarket& market, const ExternalPrice& price) const
    {
        auto source = std::find_if(market.externalPrices.begin(), market.externalPrices.end(), [&](const auto& value) { return value.sourceId == price.sourceId; });
        if (source == market.externalPrices.end())
        {
            return {ContractState::InvalidConfiguration};
        }

        if (!validPrice(price.price))
        {
            return {ContractState::InvalidPrice};
        }

        if (price.timestamp < source->timestamp || (price.timestamp == source->timestamp && source->price != 0 && price.price != source->price))
        {
            return {ContractState::InvalidTimestamp};
        }

        *source = price;
        return {};
    }

    std::optional<ReferencePrices> ReferencePriceEngine::calculate(ContractMarket& market, const MatchingEngine& matching, const SymbolManager& symbols, Timestamp timestamp) const
    {
        std::array<WeightedPrice, 32> samples{};
        std::size_t count = 0;
        const auto append = [&](const PriceSource& source)
        {
            WeightedPrice sample;
            sample.weight = source.weight;
            if (source.type == PriceSourceType::Spot)
            {
                const auto* symbol = symbols.getSymbol(static_cast<SymbolId>(source.sourceId));
                if (!symbol || symbol->symbolType != SymbolType::Spot || symbol->baseAssetId != market.symbol->baseAssetId)
                {
                    return;
                }

                sample.price = midpoint(matching, symbol->symbolId);
                sample.timestamp = matching.bookTimestamp(symbol->symbolId);
            }
            else
            {
                const auto value = std::find_if(market.externalPrices.begin(), market.externalPrices.end(), [&](const auto& price) { return price.sourceId == source.sourceId; });
                if (value == market.externalPrices.end())
                {
                    return;
                }

                sample.price = value->price;
                sample.timestamp = value->timestamp;
            }

            const WideBalance normalized = static_cast<WideBalance>(sample.price) * source.numerator / source.denominator;
            if (normalized <= 0 || normalized > std::numeric_limits<Balance>::max())
            {
                return;
            }

            sample.price = static_cast<Price>(normalized);
            samples[count++] = sample;
        };

        if (market.referenceConfig.sources.empty())
        {
            const Symbol* spot = symbols.getSymbol(SymbolType::Spot, market.symbol->baseAssetId, market.symbol->quoteAssetId);
            market.defaultSpotSymbolId = spot ? spot->symbolId : 0;
            append(PriceSource{PriceSourceType::Spot, market.defaultSpotSymbolId});
        }
        else
        {
            for (const auto& source : market.referenceConfig.sources)
            {
                append(source);
            }
        }

        const Timestamp contractTimestamp = matching.bookTimestamp(market.symbol->symbolId);
        const Price contractMidpoint = contractTimestamp <= timestamp && timestamp - contractTimestamp <= market.referenceConfig.maxAge ? midpoint(matching, market.symbol->symbolId) : 0;
        return this->algorithm.calculate(std::span(samples.data(), count), contractMidpoint, market.referenceConfig, market.spec, timestamp);
    }

    ContractResult ContractEngine::configureReference(SymbolId symbolId, const ReferencePriceConfig& config)
    {
        ContractMarket* market = this->findMarket(symbolId);
        if (!market || market->status == ContractStatus::Settled)
        {
            return {ContractState::InvalidSymbol};
        }

        return this->reference.configure(*market, config, this->matching.symbolManager);
    }

    ContractResult ContractEngine::configureProtection(SymbolId symbolId, MMRRate priceBandRate)
    {
        ContractMarket* market = this->findMarket(symbolId);
        if (!market || market->status != ContractStatus::Trading)
        {
            return {ContractState::InvalidSymbol};
        }
        if (priceBandRate >= RATE_SCALE)
        {
            return {ContractState::InvalidConfiguration};
        }
        market->spec.priceBandRate = priceBandRate;
        return {};
    }

    ContractResult ContractEngine::configureSchedule(SymbolId symbolId, const ContractSchedule& schedule, Timestamp timestamp)
    {
        ContractMarket* market = this->findMarket(symbolId);
        if (!market || market->status != ContractStatus::Trading)
        {
            return {ContractState::InvalidSymbol};
        }

        if (timestamp < market->commandTimestamp)
        {
            return {ContractState::InvalidTimestamp};
        }

        const bool perpetual = market->symbol->symbolType == SymbolType::Perpetual;
        const Timestamp nextFunding = schedule.nextFundingTimestamp == 0 ? market->nextFundingTimestamp : schedule.nextFundingTimestamp;
        const Timestamp expiry = schedule.expiryTimestamp == 0 ? market->spec.expiryTimestamp : schedule.expiryTimestamp;
        if (schedule.maxFundingRate > RATE_SCALE || (perpetual && (schedule.fundingInterval == 0 || nextFunding <= timestamp || nextFunding <= market->lastFundingTimestamp || schedule.expiryTimestamp != 0 || schedule.fundingInterval > std::numeric_limits<Timestamp>::max() - nextFunding)) || (!perpetual && (expiry <= timestamp || schedule.nextFundingTimestamp != 0)))
        {
            return {ContractState::InvalidConfiguration};
        }

        market->spec.maxFundingRate = schedule.maxFundingRate;
        market->referencePrices.fundingRate = static_cast<std::int32_t>(std::clamp(static_cast<std::int64_t>(market->referencePrices.fundingRate), -static_cast<std::int64_t>(schedule.maxFundingRate), static_cast<std::int64_t>(schedule.maxFundingRate)));
        if (perpetual)
        {
            market->spec.fundingInterval = schedule.fundingInterval;
            market->nextFundingTimestamp = nextFunding;
        }
        else
        {
            market->spec.expiryTimestamp = expiry;
        }

        market->commandTimestamp = timestamp;
        return {};
    }

    ContractResult ContractEngine::publishPrice(SymbolId symbolId, const ExternalPrice& price)
    {
        ContractMarket* market = this->findMarket(symbolId);
        return market && market->status != ContractStatus::Settled ? this->reference.publish(*market, price) : ContractResult{ContractState::InvalidSymbol};
    }

    ContractResult ContractEngine::refreshReference(SymbolId symbolId, Timestamp timestamp)
    {
        ContractMarket* market = this->findMarket(symbolId);
        if (!market || market->status == ContractStatus::Settled)
        {
            return {market ? ContractState::AlreadySettled : ContractState::InvalidSymbol};
        }

        if (timestamp < market->commandTimestamp || timestamp < market->referenceTimestamp)
        {
            return {ContractState::InvalidTimestamp};
        }

        auto prices = this->reference.calculate(*market, this->matching, this->matching.symbolManager, timestamp);
        if (!prices)
        {
            return {ContractState::PriceUnavailable};
        }

        if (market->symbol->symbolType == SymbolType::Perpetual && timestamp > market->nextFundingTimestamp)
        {
            const std::uint64_t periods = (timestamp - market->nextFundingTimestamp - 1) / market->spec.fundingInterval + 1;
            if (!market->referenceConfig.automaticFunding || periods > market->referenceConfig.maxCatchUpPeriods)
            {
                return {ContractState::FundingRequired};
            }

            const Timestamp lastDue = market->nextFundingTimestamp + (periods - 1) * market->spec.fundingInterval;
            // Late scheduling uses the previously published rate/mark, never
            // today's quote for an earlier period. Refuse stale history.
            if (market->referencePrices.indexPrice == 0 || market->referenceTimestamp > market->nextFundingTimestamp || market->markTimestamp > market->nextFundingTimestamp || lastDue - market->referenceTimestamp > market->referenceConfig.maxAge)
            {
                return {ContractState::PriceUnavailable};
            }

            for (std::uint64_t period = 0; period < periods; ++period)
            {
                const auto result = this->settleFunding(symbolId, market->referencePrices.fundingRate, market->nextFundingTimestamp);
                if (!result)
                {
                    return result;
                }
            }

            prices = this->reference.calculate(*market, this->matching, this->matching.symbolManager, timestamp);
        }

        return prices ? this->updateReference(symbolId, *prices, timestamp) : ContractResult{ContractState::PriceUnavailable};
    }

    ContractResult ContractEngine::updateReference(SymbolId symbolId, const ReferencePrices& prices, Timestamp timestamp)
    {
        ContractMarket* market = this->findMarket(symbolId);
        if (!market || market->status == ContractStatus::Settled)
        {
            return {market ? ContractState::AlreadySettled : ContractState::InvalidSymbol};
        }

        if (timestamp < market->commandTimestamp || timestamp < market->referenceTimestamp)
        {
            return {ContractState::InvalidTimestamp};
        }

        if (!validPrice(prices.indexPrice) || !validPrice(prices.markPrice) || !validPrice(prices.deliveryPrice) || std::abs(static_cast<std::int64_t>(prices.premiumRate)) > RATE_SCALE || std::abs(static_cast<std::int64_t>(prices.fundingRate)) > market->spec.maxFundingRate || prices.sourceCount > 32)
        {
            return {ContractState::InvalidPrice};
        }

        const bool perpetual = market->symbol->symbolType == SymbolType::Perpetual;
        const bool fundingDue = perpetual && timestamp >= market->nextFundingTimestamp;
        // Do not invent prices for skipped funding periods. Submit each exact
        // deadline, or settle authoritative historical rates before refreshing.
        if (fundingDue && (!market->referenceConfig.automaticFunding || timestamp != market->nextFundingTimestamp))
        {
            return {ContractState::FundingRequired};
        }

        const auto mark = this->applyMarkPrice(*market, prices.markPrice, timestamp, false);
        if (!mark)
        {
            return mark;
        }

        market->referencePrices = prices;
        market->referenceTimestamp = timestamp;
        if (fundingDue)
        {
            const auto funding = this->settleFunding(symbolId, prices.fundingRate, timestamp);
            if (!funding)
            {
                return funding;
            }
        }

        if (!perpetual && timestamp >= market->spec.expiryTimestamp)
        {
            return market->referenceConfig.automaticDelivery ? this->deliver(symbolId, prices.deliveryPrice, timestamp) : this->expire(symbolId, timestamp);
        }

        const auto result = this->processLiquidations(symbolId, timestamp, false);
        this->matching.getConditionalOrderEngine().price(symbolId, TriggerPriceSource::Mark, prices.markPrice, timestamp);
        this->matching.getConditionalOrderEngine().price(symbolId, TriggerPriceSource::Index, prices.indexPrice, timestamp);
        this->matching.getConditionalOrderEngine().process();
        return result;
    }
}
