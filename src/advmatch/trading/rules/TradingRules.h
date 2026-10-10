#ifndef ADVMATCH_TRADING_RULES_H
#define ADVMATCH_TRADING_RULES_H

#include "advmatch/util/ValueTypes.h"

namespace advmatch
{
    enum class SelfTradePrevention : std::uint8_t
    {
        Allow,
        CancelTaker,
        CancelMaker,
        CancelBoth
    };

    enum class TradingRuleState : std::uint8_t
    {
        Success,
        InvalidSymbol,
        InvalidConfiguration,
        OpenExposure,
        InsufficientFees,
        InvalidAccount,
        Overflow
    };

    struct FeeSchedule
    {
        MMRRate makerRate = 0;
        MMRRate takerRate = 0;
    };

    struct RiskLimits
    {
        // Zero disables the additional limit. ContractSpec limits still apply.
        Quantity maxOrderQuantity = 0;
        Balance maxOrderNotional = 0;
        Balance maxPositionNotional = 0;
        // Per user, per symbol: gross entry notional + pending opening orders.
        Balance maxAccountNotional = 0;
        std::uint32_t maxActiveOrders = 0;
        LeverageMultiplier maxLeverage = 0;
    };

    struct TradingRules
    {
        Price priceTick = 1;
        Quantity quantityStep = 1;
        Quantity minQuantity = 1;
        Balance minOrderNotional = 0;
        FeeSchedule fees;
        RiskLimits risk;
        // Preserve legacy journal behavior; explicitly enable protection for
        // new markets. Identity is UserId, including independent positions.
        SelfTradePrevention selfTradePrevention = SelfTradePrevention::Allow;
        // Opt in through a durable rule update; legacy journal replay stays exact.
        bool tickAwarePriceBand = false;
    };

    struct UserRiskView
    {
        SymbolId symbolId = 0;
        UserId userId = 0;
        std::uint64_t activeOrders = 0;
        Balance pendingNotional = 0;
        Balance positionNotional = 0;
    };

    struct FeeBalanceView
    {
        AssetId assetId = 0;
        Balance balance = 0;
    };

    struct TradingRuleResult
    {
        TradingRuleState state = TradingRuleState::Success;

        explicit operator bool() const noexcept
        {
            return this->state == TradingRuleState::Success;
        }
    };
}

#endif // ADVMATCH_TRADING_RULES_H
