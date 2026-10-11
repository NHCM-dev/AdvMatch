#ifndef ADVMATCH_TRADING_OBSERVER_H
#define ADVMATCH_TRADING_OBSERVER_H

#include "advmatch/matching/MatchingEngine.h"

#include "../model/ObservationTypes.h"
#include "../abstract/OrderBookListener.h"
#include "advmatch/management/AssetManager.h"
#include "advmatch/api/command/model/TradingCommand.h"

#include <functional>
#include <memory>

namespace advmatch
{
    class PersistentTradingEngine;
}

namespace advmatch::observe
{
    class TradingObserver final : private OrderBookListener
    {
        const AssetManager& assets;
        const UserManager& users;
        const SymbolManager& symbols;
        const MatchingEngine& matching;
        ObservationConfig config;
        std::function<ReadStamp()> versionProvider;
        std::uint64_t session;
        std::uint64_t revision = 0;
        std::uint64_t globalRevision = 0;
        DenseMap<UserId, std::uint64_t> userRevisions;
        DenseMap<SymbolId, std::uint64_t> symbolRevisions;
        bool inCommand = false;
        DenseMap<UserId, DenseSet<OrderId>> userOrders;
        std::size_t restingOrderCount = 0;
        bool indexHealthy = false;
        std::vector<TradeView> trades;
        std::vector<ContractEventView> events;
        std::uint64_t nextTradeId = 1;
        std::uint64_t nextEventId = 1;
        CommandSequence recordedThrough = 0;

        void onRested(const OrderNode& node) noexcept override;
        void onRemoved(const OrderNode& node) noexcept override;
        void onUserChanged(UserId userId) noexcept override;
        void subscribe();
        const Account* retainedAccount(AccountId accountId) const noexcept;
        OrderView copyOrder(const OrderNode& node) const noexcept;
        PositionView copyPosition(const Position& position, AccountId accountId) const noexcept;
        AccountView copyAccount(const Account& account) const noexcept;
        BalanceView copyBalance(const Account& account, AssetId assetId, const AssetBalance& balance) const noexcept;
        CrossMarginView copyCrossMargin(const ContractAccount& account, AssetId assetId, const CrossMarginState& margin) const noexcept;
        ReadPage prepare(const ObservationQuery& query) const;
        void readData(ObservationResult& result) const;

    public:
        // Core objects must outlive the observer. All methods run at a command
        // boundary on the owner thread. Returned views contain no engine pointers.
        TradingObserver(const AssetManager& assets, const UserManager& users, const SymbolManager& symbols,
            const MatchingEngine& matching, ObservationConfig config = {}, std::function<ReadStamp()> versionProvider = {});
        explicit TradingObserver(const PersistentTradingEngine& engine, ObservationConfig config = {});
        ~TradingObserver();
        TradingObserver(const TradingObserver&) = delete;
        TradingObserver& operator=(const TradingObserver&) = delete;

        void notifyChanged() noexcept;
        void notifyCommand(const TradingCommand& command) noexcept;
        void endCommand() noexcept { this->inCommand = false; }
        bool changed(const ObservationQuery& query, const ReadStamp& known) const;
        ObservationBatch readChanged(std::span<const ObservationQuery> queries, std::span<const ReadStamp> known) const;
        void rebuildOpenOrderIndex();
        ReadStamp stamp() const;
        OverviewView overview() const noexcept;
        std::optional<AssetView> asset(AssetId assetId) const;
        std::optional<UserView> user(UserId userId) const noexcept;
        std::optional<AccountView> account(AccountId accountId) const noexcept;
        std::optional<BalanceView> balance(AccountId accountId, AssetId assetId) const noexcept;
        std::optional<CrossMarginView> crossMargin(AccountId accountId, AssetId assetId) const noexcept;
        std::optional<SymbolView> symbol(SymbolId symbolId) const;
        std::optional<OrderView> order(OrderId orderId) const noexcept;
        std::optional<PositionView> position(PositionId positionId, AccountId accountId = 0) const noexcept;
        std::optional<BookView> book(SymbolId symbolId) const noexcept;
        std::optional<DepthView> level(SymbolId symbolId, OrderSide side, Price price) const noexcept;
        std::optional<MarketView> market(SymbolId symbolId) const;

        // Reuse caller-owned buffers for allocation-free numeric point/depth reads.
        std::size_t readOrders(std::span<const OrderId> ids, std::span<std::optional<OrderView>> output) const noexcept;
        ReadPage readDepth(const ObservationQuery& query, std::span<DepthView> output) const;
        ObservationResult read(const ObservationQuery& query) const;
        ObservationBatch readBatch(std::span<const ObservationQuery> queries) const;

        // Supply one complete batch per command, after execution. Bounded tapes
        // are preallocated; repeated/older batches are ignored. No core hook
        // records history automatically or captures uncommitted recovery events.
        void recordBatch(CommandSequence sequence, std::span<const Trade> trades, std::span<const ContractEvent> events) noexcept;
    };
}

#endif // ADVMATCH_TRADING_OBSERVER_H
