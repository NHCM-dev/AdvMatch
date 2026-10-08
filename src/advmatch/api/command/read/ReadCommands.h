#ifndef ADVMATCH_READ_COMMANDS_H
#define ADVMATCH_READ_COMMANDS_H

#include "../Command.h"
#include "../../model/ReadResponse.h"

namespace advmatch::api::command
{
    class Read final : public Command<observe::ObservationResult>
    {
        observe::ObservationQuery query;

    public:
        explicit Read(observe::ObservationQuery query)
            : query(std::move(query))
        {
        }

        observe::ObservationResult execute(CommandContext& context) const override
        {
            return context.observation().read(this->query);
        }
    };

    template <observe::ReadKind Kind, typename Row>
    class ReadOperation final : public Command<std::conditional_t<Kind == observe::ReadKind::Overview, OverviewResponse, ReadResponse<Row>>>
    {
        observe::ObservationQuery query;

    public:
        using ResponseType = std::conditional_t<Kind == observe::ReadKind::Overview, OverviewResponse, ReadResponse<Row>>;

        explicit ReadOperation(observe::ObservationQuery query = {})
            : query(std::move(query))
        {
            this->query.kind = Kind;
        }

        ResponseType execute(CommandContext& context) const override
        {
            auto result = context.observation().read(this->query);
            ResponseType response{result.query, result.page, {}};
            if constexpr (Kind == observe::ReadKind::Overview)
            {
                if (const auto* value = std::get_if<observe::OverviewView>(&result.data))
                {
                    response.value = *value;
                }
            }
            else
            {
                if (auto* values = std::get_if<std::vector<Row>>(&result.data))
                {
                    response.values = std::move(*values);
                }
            }

            return response;
        }
    };

    using ReadOverview = ReadOperation<observe::ReadKind::Overview, observe::OverviewView>;
    using ReadAsset = ReadOperation<observe::ReadKind::Asset, observe::AssetView>;
    using ReadAssets = ReadOperation<observe::ReadKind::Assets, observe::AssetView>;
    using ReadUser = ReadOperation<observe::ReadKind::User, observe::UserView>;
    using ReadUsers = ReadOperation<observe::ReadKind::Users, observe::UserView>;
    using ReadAccount = ReadOperation<observe::ReadKind::Account, observe::AccountView>;
    using ReadAccounts = ReadOperation<observe::ReadKind::Accounts, observe::AccountView>;
    using ReadBalance = ReadOperation<observe::ReadKind::Balance, observe::BalanceView>;
    using ReadBalances = ReadOperation<observe::ReadKind::Balances, observe::BalanceView>;
    using ReadCrossMargin = ReadOperation<observe::ReadKind::CrossMargin, observe::CrossMarginView>;
    using ReadCrossMargins = ReadOperation<observe::ReadKind::CrossMargins, observe::CrossMarginView>;
    using ReadSymbol = ReadOperation<observe::ReadKind::Symbol, observe::SymbolView>;
    using ReadSymbols = ReadOperation<observe::ReadKind::Symbols, observe::SymbolView>;
    using ReadBook = ReadOperation<observe::ReadKind::Book, observe::BookView>;
    using ReadBooks = ReadOperation<observe::ReadKind::Books, observe::BookView>;
    using ReadLevel = ReadOperation<observe::ReadKind::Level, observe::DepthView>;
    using ReadDepth = ReadOperation<observe::ReadKind::Depth, observe::DepthView>;
    using ReadBookOrders = ReadOperation<observe::ReadKind::BookOrders, observe::OrderView>;
    using ReadOrder = ReadOperation<observe::ReadKind::Order, observe::OrderView>;
    using ReadOrders = ReadOperation<observe::ReadKind::Orders, observe::OrderView>;
    using ReadUserOpenOrders = ReadOperation<observe::ReadKind::UserOpenOrders, observe::OrderView>;
    using ReadPosition = ReadOperation<observe::ReadKind::Position, observe::PositionView>;
    using ReadPositions = ReadOperation<observe::ReadKind::Positions, observe::PositionView>;
    using ReadAccountPositions = ReadOperation<observe::ReadKind::AccountPositions, observe::PositionView>;
    using ReadManagedPositions = ReadOperation<observe::ReadKind::ManagedPositions, observe::PositionView>;
    using ReadMarketPositions = ReadOperation<observe::ReadKind::MarketPositions, observe::PositionView>;
    using ReadMarket = ReadOperation<observe::ReadKind::Market, observe::MarketView>;
    using ReadMarkets = ReadOperation<observe::ReadKind::Markets, observe::MarketView>;
    using ReadPendingLiquidations = ReadOperation<observe::ReadKind::PendingLiquidations, observe::PositionView>;
    using ReadTrades = ReadOperation<observe::ReadKind::Trades, observe::TradeView>;
    using ReadContractEvents = ReadOperation<observe::ReadKind::ContractEvents, observe::ContractEventView>;

    class ReadBatch final : public Command<observe::ObservationBatch>
    {
        std::vector<observe::ObservationQuery> queries;

    public:
        explicit ReadBatch(std::vector<observe::ObservationQuery> queries)
            : queries(std::move(queries))
        {
        }

        observe::ObservationBatch execute(CommandContext& context) const override
        {
            return context.observation().readBatch(this->queries);
        }
    };

    class RefreshObservation final : public Command<std::shared_ptr<const observe::ObservationResult>>
    {
        std::shared_ptr<observe::ObservationSlot> slot;

    public:
        explicit RefreshObservation(std::shared_ptr<observe::ObservationSlot> slot)
            : slot(std::move(slot))
        {
        }

        std::shared_ptr<const observe::ObservationResult> execute(CommandContext& context) const override
        {
            return context.observation().refresh(this->slot);
        }
    };

    class RebuildOrderIndex final : public Command<void>
    {
    public:
        void execute(CommandContext& context) const override
        {
            context.observation().rebuildOpenOrderIndex();
        }
    };

    class CreateObservationSlot final : public Command<std::shared_ptr<observe::ObservationSlot>>
    {
        observe::ObservationQuery query;

    public:
        explicit CreateObservationSlot(observe::ObservationQuery query)
            : query(std::move(query))
        {
        }

        std::shared_ptr<observe::ObservationSlot> execute(CommandContext& context) const override
        {
            return context.observation().createSlot(this->query);
        }
    };
}

#endif // ADVMATCH_READ_COMMANDS_H
