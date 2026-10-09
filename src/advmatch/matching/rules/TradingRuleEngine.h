#ifndef ADVMATCH_TRADING_RULE_ENGINE_H
#define ADVMATCH_TRADING_RULE_ENGINE_H

#include "advmatch/trading/rules/TradingRules.h"
#include "advmatch/matching/Matching.h"

#include <memory>
#include <vector>

namespace advmatch
{
    class MatchingEngine;

    struct TradingRiskState
    {
        std::uint64_t activeOrders = 0;
        Balance pendingNotional = 0;
        Balance positionNotional = 0;
    };

    class TradingRuleEngine
    {
        friend class audit::AuditEngine;
        MatchingEngine& matching;
        using UserStates = NodeHashMap<UserId, TradingRiskState>;
        std::vector<std::unique_ptr<UserStates>> userStates;

    public:
        explicit TradingRuleEngine(MatchingEngine& matching)
            : matching(matching)
        {
        }

        void addSymbol(SymbolId symbolId);
        TradingRuleResult configure(SymbolId symbolId, const TradingRules& rules);
        static bool valid(const TradingRules& rules) noexcept;
        PlaceOrderState check(const Symbol& symbol, OrderSide side, OrderType type, Price price, Quantity quantity, Price priceLimit = 0) const noexcept;
        PlaceOrderState prepare(OrderNode& node);
        PlaceOrderState check(const OrderNode& node, Price price, Quantity quantity) const noexcept;
        static std::optional<Balance> notional(const OrderNode& node, Price price, Quantity quantity) noexcept;
        static void acquire(OrderNode& node);
        static void reconcile(OrderNode& node);
        static void release(OrderNode& node) noexcept;
        static ModifyOrderState modifyState(PlaceOrderState state) noexcept;
        std::optional<UserRiskView> query(SymbolId symbolId, UserId userId) const;
    };
}

#endif // ADVMATCH_TRADING_RULE_ENGINE_H
