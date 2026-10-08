#ifndef ADVMATCH_CONTRACT_ENGINE_H
#define ADVMATCH_CONTRACT_ENGINE_H

#include "ContractMarket.h"
#include "advmatch/matching/HistorySink.h"
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
        std::size_t automaticLiquidationBudget = std::numeric_limits<std::size_t>::max();
        struct MarkUpdate
        {
            ContractParticipant* participant;
            BalanceChange pnl;
            Balance maintenance;
            Balance closeFee;
        };
        struct CrossUpdate
        {
            WideBalance pnl = 0;
            WideBalance maintenance = 0;
            WideBalance closeFee = 0;
        };
        SmallVector<MarkUpdate, 16> markUpdates;
        DenseMap<CrossMarginState*, CrossUpdate> crossUpdates;
        BigAllocs<ContractParticipant> participantPool;
        NodeHashMap<SymbolId, ContractMarket> markets;
        PositionId nextPositionId = 1;
        std::size_t historyCursor = 0;
        DenseMap<PositionId, ContractParticipant*> positionIndex;
        ContractAccountLedger ledger;
        ContractRiskEngine risk;
        ContractSettleEngine settlement;
        FundingRateEngine funding;
        DeliveryEngine delivery;
        LiquidateEngine liquidation;

    public:
        HistoryArchiveResult archiveHistory(HistorySink& sink, std::size_t scanBudget);
        explicit ContractEngine(MatchingEngine& matching)
            : matching(matching), settlement(this->ledger, matching)
        {
        }

        ~ContractEngine();

        // Applies to automatic passes after marks/funding/orders. Explicit
        // force=true passes retain their own maxCandidates budget. Zero pauses
        // automatic execution; risk checks and enqueue still run normally.
        void setAutomaticLiquidationBudget(std::size_t maxCandidates) noexcept
        {
            this->automaticLiquidationBudget = maxCandidates;
        }

        void reserve(std::size_t marketCount, std::size_t positionCount)
        {
            this->markets.reserve(marketCount);
            this->positionIndex.reserve(positionCount);
            this->participantPool.reserve(positionCount);
        }

        void setEventSink(ContractEventSink* sink) noexcept;
        const Position* queryPosition(PositionId positionId) const noexcept;
        ContractParticipant* findParticipant(PositionId positionId) noexcept;
        ContractMarket* findMarket(SymbolId symbolId) noexcept;
        const ContractMarket* findMarket(SymbolId symbolId) const noexcept;
        ContractResult addMarket(const Symbol& symbol, const ContractSpec& spec, Price markPrice, Timestamp timestamp);
        void discardPreparedOrder(OrderNode& node);
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
        // maxCandidates caps queue scheduling, not the number of fills or
        // orders canceled by one account liquidation. Zero does no work.
        ContractResult processLiquidations(SymbolId symbolId, Timestamp timestamp, bool force = true,
            std::size_t maxCandidates = std::numeric_limits<std::size_t>::max());
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
