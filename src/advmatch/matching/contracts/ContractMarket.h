#ifndef ADVMATCH_CONTRACT_MARKET_H
#define ADVMATCH_CONTRACT_MARKET_H

#include "advmatch/ledging/account/impl/ContractAccount.h"
#include "advmatch/matching/Matching.h"
#include "advmatch/trading/futures/Contract.h"
#include "LiquidationQueue.h"
#include "advmatch/trading/futures/ReferencePrice.h"
#include "advmatch/trading/Symbol.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace advmatch
{
    struct LiquidationRetryState;

    struct ContractParticipant
    {
        ContractMarket* market = nullptr;
        ContractAccount* account = nullptr;
        Position* position = nullptr;
        CrossMarginState* crossMargin = nullptr;
        TradingRiskState* riskState = nullptr;
        OrderNode* firstOrder = nullptr;
        std::size_t openOrderCount = 0;
        std::size_t orderReferences = 0;
        std::size_t indexSlot = 0;
        LiquidationLink liquidationLink;
        ContractParticipant* crossPrevious = nullptr;
        ContractParticipant* crossNext = nullptr;
        bool crossLinked = false;
        // Owned by ContractSettleEngine's pool; reclaimed before this stable
        // participant is archived. Never copied into FOK shadow participants.
        LiquidationRetryState* liquidationRetry = nullptr;
    };

    struct ContractMarket
    {
        const Symbol* symbol = nullptr;
        ContractEventSink* eventSink = nullptr;
        ContractSpec spec;
        ContractStatus status = ContractStatus::Trading;
        Price markPrice = 0;
        Timestamp markTimestamp = 0;
        Timestamp commandTimestamp = 0;
        Timestamp nextFundingTimestamp = 0;
        Timestamp lastFundingTimestamp = 0;
        ReferencePriceConfig referenceConfig;
        std::vector<ExternalPrice> externalPrices;
        SymbolId defaultSpotSymbolId = 0;
        ReferencePrices referencePrices;
        Timestamp referenceTimestamp = 0;
        std::int32_t lastFundingRate = 0;
        Price deliveryPrice = 0;
        Balance insuranceBalance = 0;
        // Signed clearing cash: offsets all realized PnL. Funding rounding
        // residues and fees have separate accounts, so cash is auditable.
        BalanceChange clearingBalance = 0;
        Balance feeBalance = 0;
        Balance fundingRoundingBalance = 0;
        std::uint64_t liquidityRevision = 0;
        std::uint64_t lastLiquidationRevision = std::numeric_limits<std::uint64_t>::max();
        mutable Price bandMarkPrice = 0;
        mutable MMRRate bandRate = 0;
        mutable Price bandTick = 0;
        mutable std::pair<Price, Price> cachedPriceBand{};
        mutable Price cachedLiquidationFloor = 0;
        BigAllocs<ContractParticipant>* participantPool = nullptr;
        DenseMap<PositionId, ContractParticipant*> participants;
        DenseMap<PositionId, ContractParticipant*> activePositions;
        DenseMap<PositionId, ContractParticipant*> orderParticipants;
        LiquidationQueue pendingLiquidations;

        bool hasPendingLiquidation(PositionId id) const noexcept
        {
            const auto it = this->participants.find(id);
            return it != this->participants.end() && it->second->liquidationLink.linked;
        }

        std::pair<Price, Price> priceBand() const noexcept
        {
            if (this->spec.priceBandRate == 0)
            {
                return {1, static_cast<Price>(std::numeric_limits<Balance>::max())};
            }
            return this->reservationPriceBand();
        }

        // Keep a bounded mark estimate when execution protection is disabled.
        // Limit prices and settlement still require their actual notional.
        std::pair<Price, Price> reservationPriceBand() const noexcept
        {
            const MMRRate rate = this->spec.priceBandRate == 0 ? 100000 : this->spec.priceBandRate;
            const Price tick = this->symbol && this->symbol->tradingRules.tickAwarePriceBand ? this->symbol->tradingRules.priceTick : 0;
            if (this->bandMarkPrice != this->markPrice || this->bandRate != rate || this->bandTick != tick)
            {
                const WideBalance mark = this->markPrice;
                const WideBalance lower = mark * (RATE_SCALE - rate);
                this->cachedLiquidationFloor = static_cast<Price>(lower / RATE_SCALE);
                this->cachedPriceBand = {
                    static_cast<Price>((lower + RATE_SCALE - 1) / RATE_SCALE),
                    static_cast<Price>(mark * (RATE_SCALE + rate) / RATE_SCALE)};
                // A nonzero percentage band must permit at least one price tick
                // on either side, even when integer rounding collapses it.
                if (tick != 0 && rate != 0)
                {
                    const WideBalance floor = std::max<WideBalance>(1, mark - tick);
                    const WideBalance ceiling = std::min<WideBalance>(std::numeric_limits<Balance>::max(), mark + tick);
                    this->cachedPriceBand.first = std::min(this->cachedPriceBand.first, static_cast<Price>(floor));
                    this->cachedPriceBand.second = std::max(this->cachedPriceBand.second, static_cast<Price>(ceiling));
                    this->cachedLiquidationFloor = std::max<Price>(1, std::min(this->cachedLiquidationFloor, static_cast<Price>(floor)));
                }
                this->bandMarkPrice = this->markPrice;
                this->bandRate = rate;
                this->bandTick = tick;
            }

            return this->cachedPriceBand;
        }

        Price liquidationPriceLimit(bool buy) const noexcept
        {
            const auto band = this->reservationPriceBand();
            // Sell IOC limits historically round down, while the execution
            // band's lower endpoint rounds up. Preserve both conventions.
            return buy ? band.second : this->cachedLiquidationFloor;
        }

        ContractParticipant& createParticipant(PositionId id)
        {
            const auto [iterator, inserted] = this->participants.try_emplace(id, nullptr);
            if (!inserted)
            {
                return *iterator->second;
            }

            try
            {
                ContractParticipant* participant = this->participantPool->emplace();
                iterator->second = participant;
                return *participant;
            }
            catch (...)
            {
                this->participants.erase(iterator);
                throw;
            }
        }

        void removeParticipant(PositionId id)
        {
            auto it = this->participants.find(id);
            if (it != this->participants.end())
            {
                ContractParticipant* participant = it->second;
                this->pendingLiquidations.remove(participant->liquidationLink);
                this->participants.erase(it);
                this->participantPool->release(participant);
            }
        }
    };

    struct PositionPlan
    {
        ContractEventType eventType = ContractEventType::Fill;
        Price executionPrice = 0;
        Quantity executionQuantity = 0;
        OrderId orderId = 0;
        OrderId counterOrderId = 0;
        Quantity size = 0;
        Balance entryNotional = 0;
        Balance margin = 0;
        Balance maintenanceMargin = 0;
        Balance estimatedCloseFee = 0;
        BalanceChange unrealizedPnL = 0;
        BalanceChange realizedPnL = 0;
        Balance fee = 0;
        BalanceChange fundingChange = 0;
        Balance insurance = 0;
        Balance orderReservation = 0;
        BalanceChange activeChange = 0;
        BalanceChange reservedChange = 0;
    };
}

#endif // ADVMATCH_CONTRACT_MARKET_H
