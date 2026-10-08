#ifndef ADVMATCH_CONTRACT_SETTLE_ENGINE_H
#define ADVMATCH_CONTRACT_SETTLE_ENGINE_H

#include "../SettleEngine.h"
#include "advmatch/matching/contracts/ContractMarket.h"
#include "advmatch/ledging/ledgers/impl/ContractAccountLedger.h"

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
        struct SimulationWorkspace;
        std::unique_ptr<SimulationWorkspace> simulation;

        void settleTrade(const Trade&, OrderNode&, OrderNode&) override;

    public:
        explicit ContractSettleEngine(ContractAccountLedger& accountLedger, MatchingEngine& matching);
        ~ContractSettleEngine() override;

        // Pure preparation is shared by execution and cash delivery. No state
        // changes until every account and clearing account has passed checks.
        // executionNotional, when supplied, must be the checked notional for
        // this price, quantity and contract multiplier from the current fill.
        static ContractState plan(const ContractMarket& market, const ContractParticipant& participant,
            OrderSide side, Price price, Quantity quantity, Balance fee, Balance reservation,
            Balance oldReservation, bool liquidation, PositionPlan& result, bool batch = false,
            std::optional<Balance> executionNotional = std::nullopt, Account::BalanceUpdate* balanceUpdate = nullptr) noexcept;
        static void apply(ContractMarket& market, ContractParticipant& participant,
            const PositionPlan& plan, Timestamp timestamp, bool applyAccount = true, bool applyCross = true, bool applyMarket = true,
            const Account::BalanceUpdate* balanceUpdate = nullptr);
        static void publish(const ContractMarket& market, const ContractParticipant& participant,
            const PositionPlan& plan, Timestamp timestamp) noexcept;
        static void attach(OrderNode& node) noexcept;
        static void detach(OrderNode& node) noexcept;

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
