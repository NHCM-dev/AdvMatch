#ifndef ADVMATCH_CONTRACT_H
#define ADVMATCH_CONTRACT_H

#include "Position.h"
#include "advmatch/trading/Symbol.h"

namespace advmatch
{
    enum class ContractState : std::uint8_t
    {
        Success,
        InvalidSymbol,
        InvalidConfiguration,
        InvalidPrice,
        InvalidLeverage,
        InvalidMarginMode,
        InvalidTimestamp,
        PositionNotFound,
        ReduceOnlyViolation,
        FundingRequired,
        InsufficientBalance,
        InsufficientInsurance,
        OpenOrdersExist,
        Expired,
        AlreadySettled,
        NotDue,
        LiquidationNotRequired,
        PendingLiquidation,
        Overflow,
        PriceUnavailable,
        RiskLimitExceeded
    };

    struct ContractResult
    {
        ContractState state = ContractState::Success;
        std::size_t affectedPositions = 0;

        explicit operator bool() const noexcept
        {
            return this->state == ContractState::Success;
        }
    };

    enum class ContractEventType : std::uint8_t
    {
        Fill,
        Funding,
        Liquidation,
        Delivery
    };

    struct ContractEvent
    {
        ContractEventType type;
        SymbolId symbolId;
        PositionId positionId;
        UserId userId;
        Price price;
        Quantity quantity;
        BalanceChange realizedPnL;
        BalanceChange fundingChange;
        Balance fee;
        Balance insurance;
        Timestamp timestamp;
        OrderId orderId = 0;
        OrderId counterOrderId = 0;
    };

    // Events run after committing a complete trade/batch. The sink must not
    // throw or call mutating engine methods; queue values to another consumer.
    class ContractEventSink
    {
    public:
        virtual ~ContractEventSink() = default;
        virtual void onEvent(const ContractEvent& event) noexcept = 0;
    };

    enum class ContractStatus : std::uint8_t
    {
        Trading,
        Expired,
        Settled
    };

    // Linear, quote-margined, cash-settled contracts. A quantity unit represents
    // contractMultiplier base units. Prices and balances use quote atomic units.
    struct ContractSpec
    {
        Quantity contractMultiplier = 1;
        LeverageMultiplier defaultLeverage = 10;
        LeverageMultiplier maxLeverage = 100;
        MMRRate maintenanceMarginRate = 5'000;
        MMRRate makerFeeRate = 0;
        MMRRate takerFeeRate = 500;
        MMRRate liquidationFeeRate = 1'000;
        MMRRate priceBandRate = 100'000;
        MMRRate maxFundingRate = 10'000;
        Balance maxPositionNotional = 1'000'000'000'000'000;
        Timestamp expiryTimestamp = 0;
        Timestamp fundingInterval = 8 * 60 * 60 * 1000;
        Timestamp firstFundingTimestamp = 0;
    };

    struct ContractOrderOptions
    {
        // Zero selects the position's current leverage, or the market default.
        LeverageMultiplier leverage = 0;
        // Zero creates an independent position. Otherwise increase that same
        // side, or close it with the opposite side (automatically reduce-only).
        PositionId positionId = 0;
        MarginMode marginMode = MarginMode::Isolated;
        bool reduceOnly = false;
        // Required for market orders. Buy is a ceiling; sell is a floor.
        Price priceLimit = 0;
    };
}

#endif // ADVMATCH_CONTRACT_H
