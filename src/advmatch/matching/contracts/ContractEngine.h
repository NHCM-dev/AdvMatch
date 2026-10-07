#ifndef ADVMATCH_CONTRACT_ENGINE_H
#define ADVMATCH_CONTRACT_ENGINE_H

#include "ContractMarket.h"
#include "FundingRateEngine.h"
#include "DeliveryEngine.h"
#include "LiquidateEngine.h"
#include "advmatch/matching/riskengines/impl/ContractRiskEngine.h"
#include "advmatch/matching/settleengines/impl/ContractSettleEngine.h"

namespace advmatch
{
    class MatchingEngine;

    // Coordinates product policies; owns stable market/participant addresses.
    // Accounts own positions. All commands execute on the matching thread.
    class ContractEngine
    {
        friend class MatchingEngine;
        ContractState beginCommand(SymbolId symbolId, Timestamp timestamp);
        static ContractState commandState(const ContractMarket& market, Timestamp timestamp) noexcept;
        MatchingEngine& matching;
        ContractEventSink* eventSink = nullptr;
        bool processingLiquidations = false;
        NodeHashMap<SymbolId, ContractMarket> markets;
        PositionId nextPositionId = 1;
        NodeHashMap<PositionId, ContractParticipant*> positionIndex;
        ContractAccountLedger ledger;
        ContractRiskEngine risk;
        ContractSettleEngine settlement;
        FundingRateEngine funding;
        DeliveryEngine delivery;
        LiquidateEngine liquidation;

    public:
        explicit ContractEngine(MatchingEngine& matching)
            : matching(matching), settlement(this->ledger, matching)
        {
        }

        void reserve(std::size_t marketCount, std::size_t positionCount)
        {
            this->markets.reserve(marketCount);
            this->positionIndex.reserve(positionCount);
        }

        void setEventSink(ContractEventSink* sink) noexcept;
        const Position* queryPosition(PositionId positionId) const noexcept;
        ContractParticipant* findParticipant(PositionId positionId) noexcept;
        ContractMarket* findMarket(SymbolId symbolId) noexcept;
        const ContractMarket* findMarket(SymbolId symbolId) const noexcept;
        ContractResult addMarket(const Symbol& symbol, const ContractSpec& spec, Price markPrice, Timestamp timestamp);
        void discardPreparedOrder(const OrderNode& node);
        ContractState prepareOrder(OrderNode& node, const ContractOrderOptions& options, Timestamp timestamp);
        ContractResult setLeverage(PositionId positionId, UserId userId, SymbolId symbolId,
            LeverageMultiplier leverage, Timestamp timestamp);
        ContractResult addMargin(PositionId positionId, UserId userId, SymbolId symbolId, BalanceChange amount);
        ContractResult updateMarkPrice(SymbolId symbolId, Price price, Timestamp timestamp);
        ContractResult fundInsurance(SymbolId symbolId, Account& source, PositiveBalanceChange amount);
        ContractResult settleFunding(SymbolId symbolId, std::int32_t rate, Timestamp timestamp);
        ContractResult expire(SymbolId symbolId, Timestamp timestamp);
        ContractResult deliver(SymbolId symbolId, Price price, Timestamp timestamp);
        ContractResult liquidate(SymbolId symbolId, PositionId positionId, Timestamp timestamp, TradeBuffer& trades);
        ContractResult processLiquidations(SymbolId symbolId, Timestamp timestamp);
        void cancelOrders(ContractMarket& market, ContractParticipant& participant, Timestamp timestamp);

        RiskEngine& getRiskEngine() noexcept
        {
            return this->risk;
        }

        AccountLedger& getLedger() noexcept
        {
            return this->ledger;
        }

        ContractSettleEngine& getSettleEngine() noexcept
        {
            return this->settlement;
        }
    };
}

#endif // ADVMATCH_CONTRACT_ENGINE_H
