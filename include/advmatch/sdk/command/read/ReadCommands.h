#ifndef ADVMATCH_SDK_READ_COMMANDS_H
#define ADVMATCH_SDK_READ_COMMANDS_H

#include "../Command.h"

namespace advmatch::sdk::command
{
    template <ReadKind Kind, typename Row>
    class ReadOperation final : public Command<std::conditional_t<Kind == ReadKind::Overview, OverviewResponse, ReadResponse<Row>>>
    {
        ObservationQuery query;

    public:
        using ResponseType = std::conditional_t<Kind == ReadKind::Overview, OverviewResponse, ReadResponse<Row>>;

        explicit ReadOperation(ObservationQuery query = {})
            : query(std::move(query))
        {
            this->query.kind = Kind;
        }

        CompletableFuture<ResponseType> execute(ExchangeApi& api) const override
        {
            if constexpr (Kind == ReadKind::Overview)
            {
                return api.read(this->query).thenApply([](const ObservationResult& result)
                {
                    OverviewResponse response{result.query, result.page, std::nullopt};
                    if (const auto* value = std::get_if<OverviewView>(&result.data))
                    {
                        response.value = *value;
                    }

                    return response;
                });
            }
            else
            {
                return api.readRows<Row>(this->query);
            }
        }
    };

    using ReadOverview = ReadOperation<ReadKind::Overview, OverviewView>;
    using ReadAsset = ReadOperation<ReadKind::Asset, AssetView>;
    using ReadAssets = ReadOperation<ReadKind::Assets, AssetView>;
    using ReadUser = ReadOperation<ReadKind::User, UserView>;
    using ReadUsers = ReadOperation<ReadKind::Users, UserView>;
    using ReadAccount = ReadOperation<ReadKind::Account, AccountView>;
    using ReadAccounts = ReadOperation<ReadKind::Accounts, AccountView>;
    using ReadBalance = ReadOperation<ReadKind::Balance, BalanceView>;
    using ReadBalances = ReadOperation<ReadKind::Balances, BalanceView>;
    using ReadCrossMargin = ReadOperation<ReadKind::CrossMargin, CrossMarginView>;
    using ReadCrossMargins = ReadOperation<ReadKind::CrossMargins, CrossMarginView>;
    using ReadSymbol = ReadOperation<ReadKind::Symbol, SymbolView>;
    using ReadSymbols = ReadOperation<ReadKind::Symbols, SymbolView>;
    using ReadBook = ReadOperation<ReadKind::Book, BookView>;
    using ReadBooks = ReadOperation<ReadKind::Books, BookView>;
    using ReadLevel = ReadOperation<ReadKind::Level, DepthView>;
    using ReadDepth = ReadOperation<ReadKind::Depth, DepthView>;
    using ReadBookOrders = ReadOperation<ReadKind::BookOrders, OrderView>;
    using ReadOrder = ReadOperation<ReadKind::Order, OrderView>;
    using ReadOrders = ReadOperation<ReadKind::Orders, OrderView>;
    using ReadUserOpenOrders = ReadOperation<ReadKind::UserOpenOrders, OrderView>;
    using ReadPosition = ReadOperation<ReadKind::Position, PositionView>;
    using ReadPositions = ReadOperation<ReadKind::Positions, PositionView>;
    using ReadAccountPositions = ReadOperation<ReadKind::AccountPositions, PositionView>;
    using ReadManagedPositions = ReadOperation<ReadKind::ManagedPositions, PositionView>;
    using ReadMarketPositions = ReadOperation<ReadKind::MarketPositions, PositionView>;
    using ReadConditionalOrder = ReadOperation<ReadKind::ConditionalOrder, ConditionalOrderView>;
    using ReadConditionalOrders = ReadOperation<ReadKind::ConditionalOrders, ConditionalOrderView>;
    using ReadMarket = ReadOperation<ReadKind::Market, MarketView>;
    using ReadMarkets = ReadOperation<ReadKind::Markets, MarketView>;
    using ReadPendingLiquidations = ReadOperation<ReadKind::PendingLiquidations, PositionView>;
    using ReadTrades = ReadOperation<ReadKind::Trades, TradeView>;
    using ReadContractEvents = ReadOperation<ReadKind::ContractEvents, ContractEventView>;

    class Read final : public Command<ObservationResult>
    {
        ObservationQuery query;

    public:
        explicit Read(ObservationQuery query = {})
            : query(std::move(query))
        {
        }

        CompletableFuture<ObservationResult> execute(ExchangeApi& api) const override
        {
            return api.read(this->query);
        }
    };

    class ReadBatch final : public Command<ObservationBatch>
    {
        std::vector<ObservationQuery> queries;

    public:
        explicit ReadBatch(std::vector<ObservationQuery> queries)
            : queries(std::move(queries))
        {
        }

        CompletableFuture<ObservationBatch> execute(ExchangeApi& api) const override
        {
            return api.readBatch(this->queries);
        }
    };

    class CreateObservationSlot final : public Command<std::shared_ptr<ObservationSlot>>
    {
        ObservationQuery query;

    public:
        explicit CreateObservationSlot(ObservationQuery query)
            : query(std::move(query))
        {
        }

        CompletableFuture<std::shared_ptr<ObservationSlot>> execute(ExchangeApi& api) const override
        {
            return api.createObservationSlot(this->query);
        }
    };

    class RefreshObservation final : public Command<std::shared_ptr<const ObservationResult>>
    {
        std::shared_ptr<ObservationSlot> slot;

    public:
        explicit RefreshObservation(std::shared_ptr<ObservationSlot> slot)
            : slot(std::move(slot))
        {
        }

        CompletableFuture<std::shared_ptr<const ObservationResult>> execute(ExchangeApi& api) const override
        {
            if (this->slot == nullptr || !this->slot->belongsTo(api))
            {
                throw std::invalid_argument("observation slot belongs to another exchange");
            }

            return this->slot->refresh();
        }
    };

    class RebuildOrderIndex final : public Command<void>
    {
    public:
        CompletableFuture<void> execute(ExchangeApi& api) const override
        {
            return api.rebuildOrderIndex();
        }
    };
}

#endif // ADVMATCH_SDK_READ_COMMANDS_H
