#ifndef ADVMATCH_SPOT_SETTLE_ENGINE_H
#define ADVMATCH_SPOT_SETTLE_ENGINE_H

#include "../SettleEngine.h"
#include "advmatch/ledging/ledgers/impl/SpotAccountLedger.h"
#include "advmatch/trading/Asset.h"
#include "advmatch/matching/orderbooks/OrderBook.h"

namespace advmatch
{
    class MatchingEngine;

    class SpotSettleEngine final : public SettleEngine, public MatchPolicy
    {
        SpotAccountLedger& accountLedger;
        MatchingEngine* matching;
        DenseMap<AssetId, Balance> fees;
        SettlementContext context;
        std::optional<SpotAccountLedger::SettlementPlan> preparedPlan;
        Balance preparedBuyerFee = 0;
        Balance preparedSellerFee = 0;

        [[nodiscard]]
        static Balance requiredReservationBalance(const OrderNode& node);

        void settleTrade(const Trade& trade, OrderNode& maker, OrderNode& taker) override;
        void settleTrade(const Trade& trade, OrderNode& maker, OrderNode& taker, SettlementContext* context);

    public:
        bool settlesTrades() const noexcept override
        {
            return true;
        }

        bool canFillAll(OrderNode& taker, const OrderBook& book) override;
        MatchAction prepare(OrderNode& maker, OrderNode& taker, Price price, Quantity& quantity) override;
        void commit(const Trade& trade, OrderNode& maker, OrderNode& taker, Timestamp timestamp) override;
        void cancelled(OrderNode& node) override;
        Balance feeBalance(AssetId assetId) const noexcept;
        TradingRuleResult withdrawFees(Account& account, AssetId assetId, PositiveBalanceChange amount);

        explicit SpotSettleEngine(SpotAccountLedger& accountLedger, MatchingEngine* matching = nullptr)
            : accountLedger(accountLedger), matching(matching)
        {
        }

        void settleTrades(TradeBuffer& trades) override;
        void reconcileReservation(OrderNode& order) override;
        void releaseReservation(OrderNode& order) override;
    };
} // advmatch

#endif // ADVMATCH_SPOT_SETTLE_ENGINE_H
