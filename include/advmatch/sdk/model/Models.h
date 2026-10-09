#ifndef ADVMATCH_SDK_MODELS_H
#define ADVMATCH_SDK_MODELS_H

#include "advmatch/util/ValueTypes.h"
#include "advmatch/trading/Asset.h"
#include "advmatch/api/model/CommandResult.h"
#include "advmatch/api/model/ReadResponse.h"
#include "advmatch/api/command/model/TradingCommand.h"
#include "advmatch/api/async/CompletableFuture.h"
#include "advmatch/api/async/impl/ThreadPoolExecutor.h"
#include "advmatch/config/ExchangeConfig.h"
#include "advmatch/persistence/model/PersistenceStatus.h"

namespace advmatch::sdk
{
    using advmatch::UserId;
    using advmatch::AccountId;
    using advmatch::AssetId;
    using advmatch::SymbolId;
    using advmatch::OrderId;
    using advmatch::PositionId;
    using advmatch::Price;
    using advmatch::Quantity;
    using advmatch::QuantityChange;
    using advmatch::Balance;
    using advmatch::BalanceChange;
    using advmatch::PositiveBalanceChange;
    using advmatch::Scale;
    using advmatch::LeverageMultiplier;
    using advmatch::MMRRate;
    using advmatch::Timestamp;
    using advmatch::CommandSequence;
    using advmatch::OrderSide;
    using advmatch::OrderType;
    using advmatch::TimeInForce;
    using advmatch::OrderStatus;
    using advmatch::OrderFillingStatus;
    using advmatch::PlaceOrderState;
    using advmatch::PlaceOrderResult;
    using advmatch::ModifyOrderOP;
    using advmatch::ModifyOrderState;
    using advmatch::ModifyOrderResult;
    using advmatch::CancelOrderState;
    using advmatch::CancelOrderResult;
    using advmatch::OrderBookImplementation;
    using advmatch::MatchAction;
    using advmatch::BalanceOpResult;
    using advmatch::BalanceResult;
    using advmatch::AssetBalance;
    using advmatch::AssetBalanceEntry;
    using advmatch::AccountType;
    using advmatch::SymbolType;
    using advmatch::PositionSide;
    using advmatch::MarginMode;
    using advmatch::ContractState;
    using advmatch::ContractResult;
    using advmatch::ContractStatus;
    using advmatch::ContractSpec;
    using advmatch::ContractOrderOptions;
    using advmatch::ContractEvent;
    using advmatch::ContractEventType;
    using advmatch::Quote;
    using advmatch::Trade;
    using advmatch::CommandResult;
    using advmatch::CommandReceipt;
    using advmatch::CommandResultType;
    using advmatch::ManagementState;
    using advmatch::TradingCommand;
    using advmatch::ExchangeConfig;
    using advmatch::PersistenceConfig;
    using advmatch::FlushPolicy;
    using advmatch::TailRecovery;
    using advmatch::CommittedCommand;
    using advmatch::persistence::PersistenceStatus;
    using advmatch::persistence::CommittedQuery;
    using advmatch::persistence::CommittedPage;
    using advmatch::observe::ReadState;
    using advmatch::observe::ReadKind;
    using advmatch::observe::PositionFilter;
    using advmatch::observe::ReadStamp;
    using advmatch::observe::ReadIdentity;
    using advmatch::observe::ReadCursor;
    using advmatch::observe::ReadPage;
    using advmatch::observe::ObservationQuery;
    using advmatch::observe::ObservationConfig;
    using advmatch::observe::ObservationData;
    using advmatch::observe::ObservationResult;
    using advmatch::observe::ObservationBatch;
    using advmatch::observe::AssetView;
    using advmatch::observe::UserView;
    using advmatch::observe::AccountView;
    using advmatch::observe::BalanceView;
    using advmatch::observe::CrossMarginView;
    using advmatch::observe::SymbolView;
    using advmatch::observe::OrderView;
    using advmatch::observe::PositionView;
    using advmatch::observe::BookView;
    using advmatch::observe::DepthView;
    using advmatch::observe::MarketView;
    using advmatch::observe::OverviewView;
    using advmatch::observe::TradeView;
    using advmatch::observe::ContractEventView;
    using advmatch::api::OverviewResponse;
    using advmatch::api::async::Executor;
    using advmatch::api::async::ThreadPoolExecutor;
    using advmatch::api::async::FutureCancelled;
    using advmatch::api::async::FutureTimeout;

    inline constexpr auto RateScale = advmatch::RATE_SCALE;

    template <typename T>
    using CompletableFuture = advmatch::api::async::CompletableFuture<T>;

    template <typename T>
    using CompletionSource = advmatch::api::async::CompletionSource<T>;

    template <typename T>
    using FutureOutcome = advmatch::api::async::FutureOutcome<T>;

    template <typename T>
    using ReadResponse = advmatch::api::ReadResponse<T>;

    struct CreationResult
    {
        CommandReceipt receipt;

        std::uint64_t id() const noexcept
        {
            return this->receipt.result.accepted ? this->receipt.result.entityId : 0;
        }

        std::uint64_t handle() const noexcept
        {
            return this->id();
        }
    };
}

#endif // ADVMATCH_SDK_MODELS_H
