#ifndef ADVMATCH_CONTRACT_SETTLE_ENGINE_H
#define ADVMATCH_CONTRACT_SETTLE_ENGINE_H

#include "../SettleEngine.h"
#include "advmatch/matching/contracts/ContractMarket.h"
#include "advmatch/ledging/ledgers/impl/ContractAccountLedger.h"
#include "advmatch/matching/contracts/LiquidationRetryState.h"

namespace advmatch
{
    class MatchingEngine;

    class ContractSettleEngine final : public SettleEngine, public MatchPolicy
    {
        ContractAccountLedger& accountLedger;
        MatchingEngine& matching;
        PositionPlan makerPlan;
        PositionPlan takerPlan;
        Account::BalanceUpdate makerBalance;
        Account::BalanceUpdate takerBalance;
        bool insurancePrefilter = true;
        std::uint64_t insuranceRejections = 0;
        bool liquidationRetryCache = true;
        std::uint64_t retryCacheHits = 0;
        BigAllocs<LiquidationRetryState, 1, 256> retryPool;
        struct SimulationWorkspace;
        std::unique_ptr<SimulationWorkspace> simulation;

        void settleTrade(const Trade&, OrderNode&, OrderNode&) override;
        MatchAction prepareFill(OrderNode& maker, OrderNode& taker, Price price, Quantity quantity, ContractState* takerFailure = nullptr);
        MatchAction prepareLiquidation(OrderNode& maker, OrderNode& taker, Price price, Quantity quantity);

    public:
        explicit ContractSettleEngine(ContractAccountLedger& accountLedger, MatchingEngine& matching);
        ~ContractSettleEngine() override;

        // Optional pure rejection filter for differential validation. No
        // balances, orders or positions are changed by the filter itself.
        void setInsurancePrefilter(bool enabled) noexcept
        {
            this->insurancePrefilter = enabled;
        }

        std::uint64_t insurancePrefilterRejections() const noexcept
        {
            return this->insuranceRejections;
        }

        // Memoize only an unsuccessful prepare decision. The IOC is still
        // created/cancelled normally, retaining IDs/history/WAL semantics.
        void setLiquidationRetryCache(bool enabled) noexcept
        {
            this->liquidationRetryCache = enabled;
        }

        std::uint64_t liquidationRetryCacheHits() const noexcept
        {
            return this->retryCacheHits;
        }

        void discardLiquidationRetry(ContractParticipant& participant) noexcept;
        bool matchesLiquidationRetry(const OrderNode& maker, const OrderNode& taker, Price price);

        // Pure preparation is shared by execution and cash delivery. No state
        // changes until every account and clearing account has passed checks.
        // executionNotional, when supplied, must be the checked notional for
        // this price, quantity and contract multiplier from the current fill.
        static ContractState plan(const ContractMarket& market, const ContractParticipant& participant,
            OrderSide side, Price price, Quantity quantity, Balance fee, Balance reservation,
            Balance oldReservation, bool liquidation, PositionPlan& result, bool batch = false,
            std::optional<Balance> executionNotional = std::nullopt, Account::BalanceUpdate* balanceUpdate = nullptr, bool insuranceFirst = false) noexcept;
        static void apply(ContractMarket& market, ContractParticipant& participant,
            const PositionPlan& plan, Timestamp timestamp, bool applyAccount = true, bool applyCross = true, bool applyMarket = true,
            const Account::BalanceUpdate* balanceUpdate = nullptr);
        static void publish(const ContractMarket& market, const ContractParticipant& participant,
            const PositionPlan& plan, Timestamp timestamp) noexcept;
        static void attach(OrderNode& node) noexcept;
        static void detach(OrderNode& node) noexcept;
        static void linkTradingParticipant(ContractParticipant& participant) noexcept;
        static void unlinkTradingParticipant(ContractParticipant& participant) noexcept;

        bool canFillAll(OrderNode& taker, const OrderBook& book) override;
        MatchAction prepare(OrderNode& maker, OrderNode& taker, Price price, Quantity& quantity) override;
        void commit(const Trade& trade, OrderNode& maker, OrderNode& taker, Timestamp timestamp) override;
        void cancelled(OrderNode& node) override;
        void settleTrades(TradeBuffer& trades) override;
        void reconcileReservation(OrderNode& order) override;
        void releaseReservation(OrderNode& order) override;
    };
}

#endif // ADVMATCH_CONTRACT_SETTLE_ENGINE_H
