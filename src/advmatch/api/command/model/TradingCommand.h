#ifndef ADVMATCH_TRADING_COMMAND_H
#define ADVMATCH_TRADING_COMMAND_H

#include "advmatch/matching/orderbooks/OrderBookTypes.h"
#include "advmatch/trading/Asset.h"
#include "advmatch/trading/futures/Contract.h"
#include "advmatch/trading/futures/ReferencePrice.h"
#include "advmatch/trading/conditional/ConditionalOrder.h"

#include "../../model/CommandResult.h"

#include <variant>
#include <vector>
#include <limits>

namespace advmatch
{
    namespace commands
    {
        struct CreateAsset
        {
            std::string displayName;
            std::string assetName;
            Scale scale;
        };

        struct RemoveAsset
        {
            AssetId assetId;
        };

        struct CreateUser
        {
        };

        struct RemoveUser
        {
            UserId userId;
        };

        struct CreateSymbol
        {
            std::string name;
            SymbolType type;
            AssetId base;
            AssetId quote;
        };

        struct RemoveSymbol
        {
            SymbolId symbolId;
        };

        struct UpdateBalance
        {
            AccountId accountId;
            AssetId assetId;
            BalanceChange change;
        };

        struct Transfer
        {
            AccountId sourceAccountId;
            AccountId targetAccountId;
            AssetId assetId;
            PositiveBalanceChange amount;
        };

        struct AddSpotBook
        {
            SymbolId symbolId;
            OrderBookImplementation implementation = OrderBookImplementation::Radix;
        };

        struct AddContractBook
        {
            SymbolId symbolId;
            ContractSpec spec;
            Price markPrice;
            Timestamp timestamp;
            OrderBookImplementation implementation = OrderBookImplementation::Radix;
        };

        struct PlaceOrder
        {
            UserId userId;
            SymbolId symbolId;
            OrderSide side;
            OrderType type;
            TimeInForce timeInForce;
            Price price;
            Quantity quantity;
            Timestamp timestamp;
            std::optional<ContractOrderOptions> contractOptions = std::nullopt;
        };

        struct ModifyPrice
        {
            UserId userId;
            OrderId orderId;
            Price price;
            Timestamp timestamp;
        };

        struct ModifyQuantity
        {
            UserId userId;
            OrderId orderId;
            QuantityChange change;
            Timestamp timestamp;
        };

        struct CancelOrder
        {
            UserId userId;
            OrderId orderId;
            Timestamp timestamp;
        };

        struct SetLeverage
        {
            PositionId positionId;
            UserId userId;
            SymbolId symbolId;
            LeverageMultiplier leverage;
            Timestamp timestamp;
        };

        struct AddMargin
        {
            PositionId positionId;
            UserId userId;
            SymbolId symbolId;
            BalanceChange amount;
        };

        struct UpdateMarkPrice
        {
            SymbolId symbolId;
            Price price;
            Timestamp timestamp;
        };

        struct FundInsurance
        {
            SymbolId symbolId;
            AccountId sourceAccountId;
            PositiveBalanceChange amount;
        };

        struct SettleFunding
        {
            SymbolId symbolId;
            std::int32_t rate;
            Timestamp timestamp;
        };

        struct Expire
        {
            SymbolId symbolId;
            Timestamp timestamp;
        };

        struct Deliver
        {
            SymbolId symbolId;
            Price price;
            Timestamp timestamp;
        };

        struct Liquidate
        {
            SymbolId symbolId;
            PositionId positionId;
            Timestamp timestamp;
        };

        struct ProcessLiquidations
        {
            SymbolId symbolId;
            Timestamp timestamp;
            bool force = true;
            std::uint64_t maxCandidates = std::numeric_limits<std::uint64_t>::max();
        };

        struct SetAutomaticLiquidationBudget
        {
            std::uint64_t maxCandidates;
        };

        // Persist historical values to an external store before approving IDs.
        // Replay uses these same IDs; no external archival callback is rerun.
        struct ArchiveHistory
        {
            std::uint64_t scanBudget;
            std::vector<OrderId> orders;
            std::vector<PositionId> positions;
        };

        struct ConfigureReferencePrice
        {
            SymbolId symbolId;
            ReferencePriceConfig config;
        };

        struct ConfigureContractSchedule
        {
            SymbolId symbolId;
            ContractSchedule schedule;
            Timestamp timestamp;
        };

        struct PublishExternalPrice
        {
            SymbolId symbolId;
            ExternalPrice price;
        };

        struct RefreshReferencePrice
        {
            SymbolId symbolId;
            Timestamp timestamp;
        };

        struct UpdateReferencePrice
        {
            SymbolId symbolId;
            ReferencePrices prices;
            Timestamp timestamp;
        };

        struct PlaceConditionalOrder
        {
            ConditionalOrderRequest request;
        };

        struct ModifyConditionalOrder
        {
            UserId userId;
            ConditionalOrderId conditionalOrderId;
            Price triggerPrice;
            Price price;
            Quantity quantity;
            Timestamp timestamp;
        };

        struct CancelConditionalOrder
        {
            UserId userId;
            ConditionalOrderId conditionalOrderId;
            Timestamp timestamp;
        };

        struct ConfigureTradingRules
        {
            SymbolId symbolId;
            TradingRules rules;
        };

        struct WithdrawSpotFees
        {
            AccountId accountId;
            AssetId assetId;
            PositiveBalanceChange amount;
        };

        struct SetTradingHalt
        {
            SymbolId symbolId = 0;
            bool halted = true;
            Timestamp timestamp = 0;
            std::string reason;
        };

        struct BalanceTransaction
        {
            std::vector<BalanceDelta> changes;
            // Transfers/exchanges must conserve each asset. External custody
            // and issuance explicitly opt out and are recorded as external flow.
            bool conserve = true;
        };
    }

    // Indexes are on-disk operation codes. Only append new alternatives; changing
    // existing layouts/semantics requires a format migration/version change.
    using TradingCommand = std::variant<commands::CreateAsset, commands::RemoveAsset, commands::CreateUser,
        commands::RemoveUser, commands::CreateSymbol, commands::RemoveSymbol, commands::UpdateBalance,
        commands::Transfer, commands::AddSpotBook, commands::AddContractBook, commands::PlaceOrder,
        commands::ModifyPrice, commands::ModifyQuantity, commands::CancelOrder, commands::SetLeverage,
        commands::AddMargin, commands::UpdateMarkPrice, commands::FundInsurance, commands::SettleFunding,
        commands::Expire, commands::Deliver, commands::Liquidate, commands::ProcessLiquidations,
        commands::SetAutomaticLiquidationBudget, commands::ArchiveHistory, commands::ConfigureReferencePrice,
        commands::ConfigureContractSchedule, commands::PublishExternalPrice, commands::RefreshReferencePrice, commands::UpdateReferencePrice,
        commands::PlaceConditionalOrder, commands::ModifyConditionalOrder, commands::CancelConditionalOrder,
        commands::ConfigureTradingRules, commands::WithdrawSpotFees, commands::SetTradingHalt,
        commands::BalanceTransaction>;

}

#endif // ADVMATCH_TRADING_COMMAND_H
