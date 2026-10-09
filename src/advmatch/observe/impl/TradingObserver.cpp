#include "TradingObserver.h"
#include "advmatch/matching/contracts/ContractEngine.h"
#include "advmatch/persistence/PersistentTradingEngine.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <stdexcept>

namespace advmatch::observe
{
    namespace
    {
        std::atomic<std::uint64_t> nextSession{1};

        ReadIdentity identity(const ObservationQuery& query)
        {
            return {query.kind, query.userId, query.accountId, query.assetId, query.symbolId, query.orderId,
                query.positionId, query.price, query.side, query.positionFilter, query.includeInactive, query.includeTerminal};
        }

        std::optional<Balance> narrowBalance(WideBalance value) noexcept
        {
            if (value < std::numeric_limits<Balance>::min() || value > std::numeric_limits<Balance>::max())
            {
                return std::nullopt;
            }

            return static_cast<Balance>(value);
        }
    }

    TradingObserver::TradingObserver(const AssetManager& assets, const UserManager& users, const SymbolManager& symbols,
        const MatchingEngine& matching, ObservationConfig config, std::function<ReadStamp()> versionProvider)
        : assets(assets), users(users), symbols(symbols), matching(matching), config(config),
          versionProvider(std::move(versionProvider)), session(nextSession.fetch_add(1, std::memory_order_relaxed)),
          trades(config.tradeHistoryCapacity), events(config.contractEventCapacity)
    {
        if (config.maxRows == 0 || config.maxScan == 0 || config.maxBatchQueries == 0 || config.maxBatchScan == 0)
        {
            throw std::invalid_argument("observation row/scan limits must be positive");
        }

        if (config.indexUserOpenOrders)
        {
            this->subscribe();
        }
    }

    TradingObserver::TradingObserver(const PersistentTradingEngine& engine, ObservationConfig config)
        : TradingObserver(engine.getAssetManager(), engine.getUserManager(), engine.getSymbolManager(), engine.getMatchingEngine(), config, [&engine]
        {
            ReadStamp stamp;
            stamp.sequence = engine.lastSequence();
            stamp.durableThrough = engine.durableThrough();
            stamp.sourceFaulted = engine.isFaulted();
            return stamp;
        })
    {
    }

    TradingObserver::~TradingObserver()
    {
        if (this->matching.observationListener == this)
        {
            this->matching.observationListener = nullptr;
            for (const auto& book : this->matching.orderBooks)
            {
                if (book)
                {
                    book->observationListener = nullptr;
                }
            }
        }
    }

    void TradingObserver::subscribe()
    {
        if (this->matching.observationListener != nullptr)
        {
            throw std::logic_error("an open-order observer is already attached");
        }

        this->rebuildOpenOrderIndex();
        this->matching.observationListener = this;
        for (const auto& book : this->matching.orderBooks)
        {
            if (book)
            {
                book->observationListener = this;
            }
        }
    }

    void TradingObserver::rebuildOpenOrderIndex()
    {
        this->indexHealthy = false;
        this->userOrders.clear();
        this->restingOrderCount = 0;
        if (!this->config.indexUserOpenOrders)
        {
            return;
        }

        this->userOrders.reserve(this->config.expectedUsersWithOrders);
        this->matching.forEachOrderBook([&](const OrderBook& book)
        {
            for (const auto side : {OrderSide::Bid, OrderSide::Ask})
            {
                book.forEachOrder(side, [&](const BasicOrder& order)
                {
                    this->restingOrderCount += this->userOrders[order.userId].insert(order.orderId).second;
                });
            }
        });
        this->indexHealthy = true;
        this->notifyChanged();
    }

    void TradingObserver::onRested(const OrderNode& node) noexcept
    {
        if (!this->indexHealthy)
        {
            return;
        }

        try
        {
            this->restingOrderCount += this->userOrders[node.basicOrder.userId].insert(node.basicOrder.orderId).second;
        }
        catch (...)
        {
            // The trading command continues. Never return a partial index as
            // a complete account order list after resource exhaustion.
            this->indexHealthy = false;
        }
    }

    void TradingObserver::onRemoved(const OrderNode& node) noexcept
    {
        if (!this->indexHealthy)
        {
            return;
        }

        const auto found = this->userOrders.find(node.basicOrder.userId);
        if (found != this->userOrders.end())
        {
            this->restingOrderCount -= found->second.erase(node.basicOrder.orderId);
            if (found->second.empty())
            {
                this->userOrders.erase(found);
            }
        }
    }

    void TradingObserver::notifyChanged() noexcept
    {
        if (++this->revision == 0)
        {
            this->session = nextSession.fetch_add(1, std::memory_order_relaxed);
        }
    }

    ReadStamp TradingObserver::stamp() const
    {
        auto stamp = this->versionProvider ? this->versionProvider() : ReadStamp{};
        stamp.session = this->session;
        stamp.revision = this->revision;
        return stamp;
    }

    const Account* TradingObserver::retainedAccount(AccountId accountId) const noexcept
    {
        const auto& accounts = this->users.accountManager.accounts;
        return accountId != 0 && accountId <= accounts.size() ? accounts[accountId - 1] : nullptr;
    }

    OverviewView TradingObserver::overview() const noexcept
    {
        const auto& accounts = this->users.accountManager;
        const auto& contracts = this->matching.getContractEngine();
        const auto oldestTrade = this->nextTradeId == 1 || this->trades.empty() ? 0 : this->nextTradeId - std::min<std::uint64_t>(this->nextTradeId - 1, this->trades.size());
        const auto oldestEvent = this->nextEventId == 1 || this->events.empty() ? 0 : this->nextEventId - std::min<std::uint64_t>(this->nextEventId - 1, this->events.size());
        return {this->assets.assets.size(), this->users.activeUserCount, this->users.users.size(), accounts.activeAccountCount,
            accounts.spotAccountPool.size() + accounts.contractAccountPool.size(), this->symbols.activeSymbolCount,
            this->symbols.symbols.size(), this->matching.orderBookCount, this->matching.orderPool.size(), this->matching.orders.size(),
            accounts.positionPool.size(), contracts.positionIndex.size(), contracts.markets.size(), contracts.automaticLiquidationBudget,
            this->indexHealthy ? std::optional<std::size_t>(this->restingOrderCount) : std::nullopt, this->matching.nextOrderId,
            contracts.nextPositionId, oldestTrade, oldestTrade != 0 ? this->nextTradeId - 1 : 0, oldestEvent, oldestEvent != 0 ? this->nextEventId - 1 : 0};
    }

    std::optional<AssetView> TradingObserver::asset(AssetId assetId) const
    {
        const auto* value = this->assets.getAsset(assetId);
        return value != nullptr ? std::optional<AssetView>({value->assetId, value->scale, value->displayName, value->assetName}) : std::nullopt;
    }

    std::optional<UserView> TradingObserver::user(UserId userId) const noexcept
    {
        const auto found = this->users.users.find(userId);
        if (found == this->users.users.end())
        {
            return std::nullopt;
        }

        const auto& value = found->second;
        return UserView{userId, value.getSpotAccount().getAccountId(), value.getContractAccount().getAccountId(), value.isActive()};
    }

    AccountView TradingObserver::copyAccount(const Account& value) const noexcept
    {
        AccountView result{value.getAccountId(), value.getUserId(), value.getAccountType(), value.isActive(), value.getAssetBalanceCount()};
        if (value.getAccountType() == AccountType::Contract)
        {
            const auto& contract = static_cast<const ContractAccount&>(value);
            result.positionCount = contract.getPositionCount();
            result.openPositionCount = contract.activePositions.size();
            result.tradingPositionCount = contract.tradingPositions.size();
            result.managedPositionCount = contract.managedPositionCount;
            result.managedOrderCount = contract.managedOrderCount;
        }

        return result;
    }

    std::optional<AccountView> TradingObserver::account(AccountId accountId) const noexcept
    {
        const auto* value = this->retainedAccount(accountId);
        return value != nullptr ? std::optional<AccountView>(this->copyAccount(*value)) : std::nullopt;
    }

    std::optional<BalanceView> TradingObserver::balance(AccountId accountId, AssetId assetId) const noexcept
    {
        const auto* owner = this->retainedAccount(accountId);
        const auto* value = owner != nullptr ? owner->findBalance(assetId) : nullptr;
        return value != nullptr ? std::optional<BalanceView>(this->copyBalance(*owner, assetId, *value)) : std::nullopt;
    }

    BalanceView TradingObserver::copyBalance(const Account& account, AssetId assetId, const AssetBalance& value) const noexcept
    {
        return {account.getAccountId(), account.getUserId(), assetId, value.active, value.reserved, narrowBalance(static_cast<WideBalance>(value.active) + value.reserved)};
    }

    CrossMarginView TradingObserver::copyCrossMargin(const ContractAccount& account, AssetId assetId, const CrossMarginState& value) const noexcept
    {
        const auto* balance = account.findBalance(assetId);
        const auto active = balance != nullptr ? balance->active : 0;
        const auto reserved = balance != nullptr ? balance->reserved : 0;
        return {account.getAccountId(), assetId, active, reserved, value.initialMargin, value.maintenanceMargin,
            value.liquidationFeeReserve, value.unrealizedPnL, value.orderReservation, value.liquidating,
            narrowBalance(static_cast<WideBalance>(active) + value.unrealizedPnL + value.orderReservation)};
    }

    std::optional<CrossMarginView> TradingObserver::crossMargin(AccountId accountId, AssetId assetId) const noexcept
    {
        const auto* owner = this->retainedAccount(accountId);
        if (owner == nullptr || owner->getAccountType() != AccountType::Contract)
        {
            return std::nullopt;
        }

        const auto& account = static_cast<const ContractAccount&>(*owner);
        const auto* value = account.findCrossMargin(assetId);
        return value != nullptr ? std::optional<CrossMarginView>(this->copyCrossMargin(account, assetId, *value)) : std::nullopt;
    }

    std::optional<SymbolView> TradingObserver::symbol(SymbolId symbolId) const
    {
        const auto found = this->symbols.symbols.find(symbolId);
        if (found == this->symbols.symbols.end())
        {
            return std::nullopt;
        }

        const auto& value = found->second;
        return SymbolView{symbolId, value.symbolType, value.baseAssetId, value.quoteAssetId, value.isActive(), value.name};
    }

    OrderView TradingObserver::copyOrder(const OrderNode& node) const noexcept
    {
        const auto& order = node.basicOrder;
        const auto* position = node.contractParticipant != nullptr ? node.contractParticipant->position : nullptr;
        return {order.orderId, order.userId, node.account->getAccountId(), order.symbolId, node.baseAssetId, node.quoteAssetId,
            order.orderSide, order.orderType, order.timeInForce, order.getPrice(), order.getQuantity(), order.getFillingStatus(),
            order.getCreatedTimestamp(), order.getUpdatedTimestamp(), order.getFirstFilledTimestamp(), order.getLastFilledTimestamp(),
            order.getCompletedTimestamp(), node.reservationAssetId, node.reservedBalance, position != nullptr ? position->positionId : 0,
            node.leverage, node.reduceOnly, node.liquidationOrder, node.priceLevel != nullptr, node.executionPriceLimit,
            position != nullptr ? std::optional<MarginMode>(position->marginMode) : std::nullopt};
    }

    std::optional<OrderView> TradingObserver::order(OrderId orderId) const noexcept
    {
        const auto* node = this->matching.findNode(orderId);
        return node != nullptr ? std::optional<OrderView>(this->copyOrder(*node)) : std::nullopt;
    }

    PositionView TradingObserver::copyPosition(const Position& value, AccountId accountId) const noexcept
    {
        const auto* found = this->matching.getContractEngine().participantIndex.get(value.positionId);
        const auto* participant = found && found->position == &value ? found : nullptr;
        return {value.positionId, value.userId, accountId, value.symbolId, value.positionSide, value.marginMode,
            value.averageEntryPrice, value.predictLiquidatePrice, value.predictBankruptPrice, value.positionSize, value.entryNotional,
            value.realizedPnL, value.updatedUnrealizedPnl, value.initialMargin, value.mmr, value.maintenanceMargin,
            value.estimatedCloseFee, value.contractMultiplier, value.leverage, value.fundingPnL, value.paidFees, value.closed,
            value.liquidating, value.managed, value.createdTimestamp, value.updatedTimestamp,
            participant != nullptr ? participant->openOrderCount : 0, participant != nullptr && participant->liquidationLink.linked};
    }

    std::optional<PositionView> TradingObserver::position(PositionId positionId, AccountId accountId) const noexcept
    {
        if (accountId != 0)
        {
            const auto* owner = this->retainedAccount(accountId);
            if (owner == nullptr || owner->getAccountType() != AccountType::Contract)
            {
                return std::nullopt;
            }

            const auto* value = static_cast<const ContractAccount*>(owner)->findPosition(positionId);
            return value != nullptr ? std::optional<PositionView>(this->copyPosition(*value, accountId)) : std::nullopt;
        }

        const auto* found = this->matching.getContractEngine().participantIndex.get(positionId);
        return found ? std::optional<PositionView>(this->copyPosition(*found->position, found->account->getAccountId())) : std::nullopt;
    }

    std::optional<BookView> TradingObserver::book(SymbolId symbolId) const noexcept
    {
        const auto* book = this->matching.findBook(symbolId);
        return book != nullptr ? std::optional<BookView>({symbolId, book->bestBid(), book->bestAsk(),
            book->observedLevelCount(OrderSide::Bid), book->observedLevelCount(OrderSide::Ask)}) : std::nullopt;
    }

    std::optional<MarketView> TradingObserver::market(SymbolId symbolId) const
    {
        const auto* value = this->matching.getContractEngine().findMarket(symbolId);
        return value != nullptr ? std::optional<MarketView>({symbolId, value->spec, value->status, value->markPrice, value->markTimestamp,
            value->commandTimestamp, value->nextFundingTimestamp, value->lastFundingTimestamp, value->deliveryPrice, value->insuranceBalance,
            value->clearingBalance, value->feeBalance, value->fundingRoundingBalance, value->liquidityRevision, value->lastLiquidationRevision,
            value->participants.size(), value->activePositions.size(), value->orderParticipants.size(), value->pendingLiquidations.size(),
            value->referenceConfig, value->referencePrices, value->referenceTimestamp, value->lastFundingRate, value->externalPrices}) : std::nullopt;
    }

    std::optional<DepthView> TradingObserver::level(SymbolId symbolId, OrderSide side, Price price) const noexcept
    {
        const auto quote = side == OrderSide::Bid ? this->matching.bidAt(symbolId, price) : this->matching.askAt(symbolId, price);
        return quote ? std::optional<DepthView>({symbolId, side, quote->price, quote->quantity, quote->orderCount}) : std::nullopt;
    }

    std::size_t TradingObserver::readOrders(std::span<const OrderId> ids, std::span<std::optional<OrderView>> output) const noexcept
    {
        const auto count = std::min(ids.size(), output.size());
        for (std::size_t index = 0; index < count; ++index)
        {
            output[index] = this->order(ids[index]);
        }

        return count;
    }

    ReadPage TradingObserver::prepare(const ObservationQuery& query) const
    {
        ReadPage page;
        page.stamp = this->stamp();
        page.next = query.cursor;
        if (page.stamp.sourceFaulted)
        {
            page.state = ReadState::SourceFaulted;
        }
        else if (query.kind > ReadKind::MarketPositions || query.positionFilter > PositionFilter::Trading || query.limit == 0 || query.limit > this->config.maxRows || query.scanBudget == 0 || query.scanBudget > this->config.maxScan)
        {
            page.state = ReadState::InvalidQuery;
        }
        else if (!query.cursor.started && (query.cursor.anchor != 0 || query.cursor.secondary != 0))
        {
            page.state = ReadState::InvalidQuery;
        }
        else if (query.cursor.started)
        {
            const bool tape = query.kind == ReadKind::Trades || query.kind == ReadKind::ContractEvents;
            if (query.cursor.session != page.stamp.session || query.cursor.identity != identity(query) ||
                (!tape && (query.cursor.sequence != page.stamp.sequence || query.cursor.revision != page.stamp.revision)))
            {
                page.state = ReadState::CursorExpired;
            }
        }

        page.next.session = page.stamp.session;
        page.next.revision = page.stamp.revision;
        page.next.sequence = page.stamp.sequence;
        page.next.identity = identity(query);
        page.next.started = true;
        return page;
    }

    ReadPage TradingObserver::readDepth(const ObservationQuery& query, std::span<DepthView> output) const
    {
        auto page = this->prepare(query);
        if (page.state != ReadState::Success)
        {
            return page;
        }

        if (query.kind != ReadKind::Depth || output.empty())
        {
            page.state = ReadState::InvalidQuery;
            return page;
        }

        const auto* book = this->matching.findBook(query.symbolId);
        if (book == nullptr)
        {
            page.state = ReadState::NotFound;
            return page;
        }

        const auto price = static_cast<Price>(query.cursor.anchor);
        if (query.cursor.started && price != 0 && !(query.side == OrderSide::Bid ? book->bidAt(price) : book->askAt(price)))
        {
            page.state = ReadState::CursorExpired;
            return page;
        }

        const auto* level = price != 0 ? book->observedNextLevel(query.side, price) : book->observedBestLevel(query.side);
        const auto limit = std::min(query.limit, output.size());
        while (level != nullptr && page.count < limit && page.examined < query.scanBudget)
        {
            output[page.count++] = {query.symbolId, query.side, level->price, level->totalQuantity, level->orderCount};
            ++page.examined;
            page.next.anchor = level->price;
            level = book->observedNextLevel(query.side, level->price);
        }

        page.hasMore = level != nullptr;
        return page;
    }

    ObservationResult TradingObserver::read(const ObservationQuery& query) const
    {
        ObservationResult result{query, this->prepare(query), {}};
        if (result.page.state == ReadState::Success)
        {
            this->readData(result);
        }

        return result;
    }

    ObservationBatch TradingObserver::readBatch(std::span<const ObservationQuery> queries) const
    {
        ObservationBatch result;
        result.stamp = this->stamp();
        std::size_t remaining = this->config.maxBatchScan;
        if (queries.empty() || queries.size() > this->config.maxBatchQueries)
        {
            result.state = ReadState::InvalidQuery;
            return result;
        }

        for (const auto& query : queries)
        {
            if (query.scanBudget > remaining)
            {
                result.state = ReadState::InvalidQuery;
                return result;
            }

            remaining -= query.scanBudget;
        }

        if (result.stamp.sourceFaulted)
        {
            result.state = ReadState::SourceFaulted;
            return result;
        }

        result.results.reserve(queries.size());
        for (const auto& query : queries)
        {
            result.results.push_back(this->read(query));
        }

        return result;
    }

    void TradingObserver::recordBatch(CommandSequence sequence, std::span<const Trade> trades, std::span<const ContractEvent> events) noexcept
    {
        if (sequence == 0 || sequence <= this->recordedThrough)
        {
            return;
        }

        for (const auto& trade : trades)
        {
            if (!this->trades.empty() && this->nextTradeId != std::numeric_limits<std::uint64_t>::max())
            {
                const auto* taker = this->matching.findNode(trade.takerOrderId);
                const auto* maker = this->matching.findNode(trade.makerOrderId);
                this->trades[(this->nextTradeId - 1) % this->trades.size()] = {this->nextTradeId, sequence,
                    taker != nullptr ? taker->basicOrder.symbolId : 0, taker != nullptr ? taker->basicOrder.getLastFilledTimestamp() : 0,
                    maker != nullptr ? maker->basicOrder.userId : 0, taker != nullptr ? taker->basicOrder.userId : 0,
                    maker != nullptr ? maker->account->getAccountId() : 0, taker != nullptr ? taker->account->getAccountId() : 0, trade};
                ++this->nextTradeId;
            }
        }

        for (const auto& event : events)
        {
            if (!this->events.empty() && this->nextEventId != std::numeric_limits<std::uint64_t>::max())
            {
                const auto owner = this->user(event.userId);
                this->events[(this->nextEventId - 1) % this->events.size()] = {this->nextEventId, sequence, event, owner ? owner->contractAccountId : 0};
                ++this->nextEventId;
            }
        }

        this->recordedThrough = sequence;
    }
}
