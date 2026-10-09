#ifndef ADVMATCH_OBSERVATION_TYPES_H
#define ADVMATCH_OBSERVATION_TYPES_H

#include "advmatch/matching/orderbooks/OrderBookTypes.h"
#include "advmatch/ledging/account/AccountTypes.h"
#include "advmatch/matching/Trade.h"
#include "advmatch/trading/futures/Contract.h"
#include "advmatch/trading/futures/Position.h"

#include <span>
#include <variant>
#include <vector>

namespace advmatch::observe
{
    enum class ReadState : std::uint8_t
    {
        Success,
        NotFound,
        InvalidQuery,
        CursorExpired,
        IndexUnavailable,
        HistoryUnavailable,
        SourceFaulted
    };

    enum class ReadKind : std::uint8_t
    {
        Overview,
        Asset,
        Assets,
        User,
        Users,
        Account,
        Accounts,
        Balance,
        Balances,
        CrossMargin,
        CrossMargins,
        Symbol,
        Symbols,
        Book,
        Books,
        Depth,
        BookOrders,
        Order,
        Orders,
        UserOpenOrders,
        Position,
        Positions,
        AccountPositions,
        Market,
        Markets,
        PendingLiquidations,
        Trades,
        ContractEvents,
        Level,
        ManagedPositions,
        MarketPositions
    };

    enum class PositionFilter : std::uint8_t
    {
        All,
        Open,
        Trading
    };

    struct ReadStamp
    {
        std::uint64_t session = 0;
        std::uint64_t revision = 0;
        CommandSequence sequence = 0;
        CommandSequence durableThrough = 0;
        bool sourceFaulted = false;
    };

    struct ReadIdentity
    {
        ReadKind kind;
        UserId userId;
        AccountId accountId;
        AssetId assetId;
        SymbolId symbolId;
        OrderId orderId;
        PositionId positionId;
        Price price;
        OrderSide side;
        PositionFilter positionFilter;
        bool includeInactive;
        bool includeTerminal;

        bool operator==(const ReadIdentity&) const = default;
    };

    // Plain IDs/indexes only. No borrowed iterator/node survives a command.
    struct ReadCursor
    {
        std::uint64_t session = 0;
        std::uint64_t revision = 0;
        CommandSequence sequence = 0;
        std::uint64_t anchor = 0;
        std::uint64_t secondary = 0;
        ReadIdentity identity{};
        bool started = false;

        bool operator==(const ReadCursor&) const = default;
    };

    struct ObservationQuery
    {
        ReadKind kind = ReadKind::Overview;
        UserId userId = 0;
        AccountId accountId = 0;
        AssetId assetId = 0;
        SymbolId symbolId = 0;
        OrderId orderId = 0;
        PositionId positionId = 0;
        Price price = 0;
        OrderSide side = OrderSide::Bid;
        PositionFilter positionFilter = PositionFilter::All;
        bool includeInactive = false;
        bool includeTerminal = true;
        std::size_t limit = 64;
        std::size_t scanBudget = 1024;
        ReadCursor cursor;

        bool operator==(const ObservationQuery&) const = default;
    };

    struct ObservationConfig
    {
        std::size_t maxRows = 1024;
        std::size_t maxScan = 16'384;
        std::size_t maxBatchQueries = 16;
        std::size_t maxBatchScan = 65'536;
        bool indexUserOpenOrders = false;
        std::size_t expectedUsersWithOrders = 0;
        std::size_t tradeHistoryCapacity = 4096;
        std::size_t contractEventCapacity = 4096;
    };

    struct AssetView
    {
        AssetId assetId;
        Scale scale;
        std::string displayName;
        std::string assetName;
    };

    struct UserView
    {
        UserId userId;
        AccountId spotAccountId;
        AccountId contractAccountId;
        bool active;
    };

    struct AccountView
    {
        AccountId accountId;
        UserId userId;
        AccountType type;
        bool active;
        std::size_t balanceCount;
        std::size_t positionCount = 0;
        std::size_t openPositionCount = 0;
        std::size_t tradingPositionCount = 0;
        std::size_t managedPositionCount = 0;
        std::size_t managedOrderCount = 0;
    };

    struct BalanceView
    {
        AccountId accountId;
        UserId userId;
        AssetId assetId;
        Balance active;
        Balance reserved;
        std::optional<Balance> total;
    };

    struct CrossMarginView
    {
        AccountId accountId;
        AssetId assetId;
        Balance active;
        Balance reserved;
        Balance initialMargin;
        Balance maintenanceMargin;
        Balance liquidationFeeReserve;
        BalanceChange unrealizedPnL;
        Balance orderReservation;
        bool liquidating;
        std::optional<Balance> liquidationEquity;
    };

    struct SymbolView
    {
        SymbolId symbolId;
        SymbolType type;
        AssetId baseAssetId;
        AssetId quoteAssetId;
        bool active;
        std::string name;
    };

    struct OrderView
    {
        OrderId orderId;
        UserId userId;
        AccountId accountId;
        SymbolId symbolId;
        AssetId baseAssetId;
        AssetId quoteAssetId;
        OrderSide side;
        OrderType type;
        TimeInForce timeInForce;
        Price price;
        Quantity quantity;
        OrderFillingStatus filling;
        Timestamp createdTimestamp;
        Timestamp updatedTimestamp;
        Timestamp firstFilledTimestamp;
        Timestamp lastFilledTimestamp;
        Timestamp completedTimestamp;
        AssetId reservationAssetId;
        Balance reservedBalance;
        PositionId positionId;
        LeverageMultiplier leverage;
        bool reduceOnly;
        bool liquidationOrder;
        bool resting;
        Price executionPriceLimit;
        std::optional<MarginMode> marginMode;
    };

    struct PositionView
    {
        PositionId positionId;
        UserId userId;
        AccountId accountId;
        SymbolId symbolId;
        PositionSide side;
        MarginMode marginMode;
        Price averageEntryPrice;
        Price predictedLiquidationPrice;
        Price predictedBankruptcyPrice;
        Quantity size;
        Balance entryNotional;
        BalanceChange realizedPnL;
        BalanceChange unrealizedPnL;
        Balance initialMargin;
        MMRRate maintenanceMarginRate;
        Balance maintenanceMargin;
        Balance estimatedCloseFee;
        Quantity contractMultiplier;
        LeverageMultiplier leverage;
        BalanceChange fundingPnL;
        Balance paidFees;
        bool closed;
        bool liquidating;
        bool managed;
        Timestamp createdTimestamp;
        Timestamp updatedTimestamp;
        std::size_t openOrderCount;
        bool liquidationQueued;
    };

    struct BookView
    {
        SymbolId symbolId;
        std::optional<Quote> bestBid;
        std::optional<Quote> bestAsk;
        std::size_t bidLevels;
        std::size_t askLevels;
    };

    struct DepthView
    {
        SymbolId symbolId;
        OrderSide side;
        Price price;
        Quantity quantity;
        std::uint32_t orderCount;
    };

    struct MarketView
    {
        SymbolId symbolId;
        ContractSpec spec;
        ContractStatus status;
        Price markPrice;
        Timestamp markTimestamp;
        Timestamp commandTimestamp;
        Timestamp nextFundingTimestamp;
        Timestamp lastFundingTimestamp;
        Price deliveryPrice;
        Balance insuranceBalance;
        BalanceChange clearingBalance;
        Balance feeBalance;
        Balance fundingRoundingBalance;
        std::uint64_t liquidityRevision;
        std::uint64_t lastLiquidationRevision;
        std::size_t retainedPositions;
        std::size_t openPositions;
        std::size_t orderParticipants;
        std::size_t pendingLiquidations;
    };

    struct OverviewView
    {
        std::size_t assets;
        std::size_t activeUsers;
        std::size_t retainedUsers;
        std::size_t activeAccounts;
        std::size_t retainedAccounts;
        std::size_t activeSymbols;
        std::size_t retainedSymbols;
        std::size_t books;
        std::size_t retainedOrders;
        std::size_t orderIdSlots;
        std::size_t retainedPositions;
        std::size_t managedPositions;
        std::size_t markets;
        std::size_t automaticLiquidationBudget;
        std::optional<std::size_t> restingOrders;
        OrderId nextOrderId;
        PositionId nextPositionId;
        std::uint64_t oldestTrade;
        std::uint64_t newestTrade;
        std::uint64_t oldestContractEvent;
        std::uint64_t newestContractEvent;
    };

    struct TradeView
    {
        std::uint64_t recordId = 0;
        CommandSequence sequence = 0;
        SymbolId symbolId = 0;
        Timestamp timestamp = 0;
        UserId makerUserId = 0;
        UserId takerUserId = 0;
        AccountId makerAccountId = 0;
        AccountId takerAccountId = 0;
        Trade trade{};
    };

    struct ContractEventView
    {
        std::uint64_t recordId = 0;
        CommandSequence sequence = 0;
        ContractEvent event{};
        AccountId accountId = 0;
    };

    struct ReadPage
    {
        ReadState state = ReadState::Success;
        ReadStamp stamp;
        ReadCursor next;
        std::size_t count = 0;
        std::size_t examined = 0;
        bool hasMore = false;
    };

    using ObservationData = std::variant<std::monostate, OverviewView, std::vector<AssetView>,
        std::vector<UserView>, std::vector<AccountView>, std::vector<BalanceView>, std::vector<CrossMarginView>,
        std::vector<SymbolView>, std::vector<OrderView>, std::vector<PositionView>, std::vector<BookView>,
        std::vector<DepthView>, std::vector<MarketView>, std::vector<TradeView>, std::vector<ContractEventView>>;

    struct ObservationResult
    {
        ObservationQuery query;
        ReadPage page;
        ObservationData data;

        template <typename Row>
        std::span<const Row> rows() const noexcept
        {
            const auto* values = std::get_if<std::vector<Row>>(&this->data);
            return values != nullptr ? std::span<const Row>(*values) : std::span<const Row>();
        }
    };

    struct ObservationBatch
    {
        ReadState state = ReadState::Success;
        ReadStamp stamp;
        std::vector<ObservationResult> results;
    };
}

#endif // ADVMATCH_OBSERVATION_TYPES_H
