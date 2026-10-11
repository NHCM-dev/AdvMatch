#ifndef ADVMATCH_MATCHING_ENGINE_H
#define ADVMATCH_MATCHING_ENGINE_H

#include "../management/UserManager.h"
#include "Matching.h"
#include "advmatch/trading/futures/Contract.h"
#include "TradeBuffer.h"
#include "HistorySink.h"
#include "orderbooks/OrderBook.h"
#include "advmatch/ledging/ledgers/impl/SpotAccountLedger.h"
#include "riskengines/impl/SpotRiskEngine.h"
#include "settleengines/impl/SpotSettleEngine.h"
#include "../util/BigAllocs.h"

#include <memory>
#include <optional>
#include <vector>

#include "advmatch/management/SymbolManager.h"
#include "advmatch/trading/checker/BasicOrderChecker.h"
#include "conditional/ConditionalOrderEngine.h"
#include "rules/TradingRuleEngine.h"
#include "control/TradingControlEngine.h"
#include "advmatch/audit/AuditEngine.h"

namespace advmatch
{
    class ContractEngine;
    // Single-writer matching state. Queue consumers call this object; producer
    // threads must not call its mutating methods directly. The referenced
    // managers must outlive this engine because OrderNode caches their stable
    // User and Symbol addresses.
    class MatchingEngine
    {
        friend class observe::TradingObserver;
        friend class ::advmatch::StateSnapshotCodec;
        mutable observe::OrderBookListener* observationListener = nullptr;
        std::size_t orderBookCount = 0;
        struct BookServices
        {
            RiskEngine* risk = nullptr;
            AccountLedger* ledger = nullptr;
            SettleEngine* settlement = nullptr;
        };

        std::unique_ptr<TradingRuleEngine> tradingRules;
        std::unique_ptr<TradingControlEngine> tradingControl;
        std::unique_ptr<audit::AuditEngine> auditEngine;
        std::unique_ptr<ContractEngine> contracts;
        std::unique_ptr<ConditionalOrderEngine> conditionalOrders;
        std::vector<BookServices> bookServices;
        friend class ContractEngine;
        friend class LiquidateEngine;
        friend class ContractSettleEngine;
        friend class ConditionalOrderEngine;
        friend class TradingRuleEngine;
        friend class TradingControlEngine;
        friend class SpotSettleEngine;
        friend class audit::AuditEngine;

        // Borrowed only by a synchronous submission on the owner thread. The
        // internal liquidation path resolves these once, but shares factory,
        // risk, reservation, matching and settlement with public submissions.
        struct OrderSubmissionContext
        {
            User* user;
            const Symbol* symbol;
            OrderBook* book;
            ContractParticipant* participant;
        };

        PlaceOrderResult submitOrder(UserId userId, SymbolId symbolId, OrderSide side, OrderType type,
            TimeInForce timeInForce, Price price, Quantity quantity, Timestamp timestamp, TradeBuffer& trades,
            const ContractOrderOptions* options, bool liquidation, const OrderSubmissionContext* context = nullptr);
        UserManager& userManager;
        SymbolManager& symbolManager;
        SpotAccountLedger accountLedger;
        SpotRiskEngine riskEngine;
        SpotSettleEngine settleEngine;
        BasicOrderChecker orderChecker;

        // IDs are monotonically allocated from one, so id - 1 is a direct O(1)
        // index. The pool owns stable-address nodes; the vector only indexes
        // them and never owns or moves an OrderNode.
        BigAllocs<OrderNode> orderPool;
        std::vector<OrderNode*> orders;
        std::vector<std::unique_ptr<OrderBook>> orderBooks;
        // Direct slots; pricing refreshes read these without traversing orders.
        std::vector<Timestamp> bookUpdateTimestamps;
        OrderId nextOrderId = 1;
        std::size_t historyCursor = 0;
        bool historyPositionTurn = false;

        OrderNode* findNode(OrderId orderId) noexcept;
        const OrderNode* findNode(OrderId orderId) const noexcept;
        const OrderBook* findBook(SymbolId symbolId) const noexcept;
        OrderBook* findBook(SymbolId symbolId) noexcept;

    public:
        TradingControlEngine& getTradingControlEngine() noexcept
        {
            return *this->tradingControl;
        }

        const TradingControlEngine& getTradingControlEngine() const noexcept
        {
            return *this->tradingControl;
        }

        audit::AuditEngine& getAuditEngine() noexcept
        {
            return *this->auditEngine;
        }

        const audit::AuditEngine& getAuditEngine() const noexcept
        {
            return *this->auditEngine;
        }

        TradingRuleResult withdrawSpotFees(Account& account, AssetId assetId, PositiveBalanceChange amount)
        {
            return this->settleEngine.withdrawFees(account, assetId, amount);
        }

        TradingRuleEngine& getTradingRuleEngine() noexcept
        {
            return *this->tradingRules;
        }

        const TradingRuleEngine& getTradingRuleEngine() const noexcept
        {
            return *this->tradingRules;
        }

        const SpotSettleEngine& getSpotSettleEngine() const noexcept
        {
            return this->settleEngine;
        }

        ConditionalOrderEngine& getConditionalOrderEngine() noexcept
        {
            return *this->conditionalOrders;
        }

        const ConditionalOrderEngine& getConditionalOrderEngine() const noexcept
        {
            return *this->conditionalOrders;
        }

        Timestamp bookTimestamp(SymbolId symbolId) const noexcept
        {
            return symbolId != 0 && symbolId <= this->bookUpdateTimestamps.size() ? this->bookUpdateTimestamps[symbolId - 1] : 0;
        }

        explicit MatchingEngine(UserManager& userManager, SymbolManager& symbolManager);
        ~MatchingEngine();

        ContractEngine& getContractEngine() noexcept;
        const ContractEngine& getContractEngine() const noexcept;
        ContractResult addContractBook(SymbolId symbolId, const ContractSpec& spec, Price markPrice, Timestamp timestamp,
            OrderBookImplementation implementation = OrderBookImplementation::Radix);
        PlaceOrderResult placeContractOrder(UserId userId, SymbolId symbolId, OrderSide side, OrderType type,
            TimeInForce timeInForce, Price price, Quantity quantity, Timestamp timestamp,
            const ContractOrderOptions& options, TradeBuffer& trades);

        // activeOrderCount is retained for source compatibility. Reservation
        // state is stored directly in OrderNode, so it needs no separate table.
        void reserve(std::size_t orderCount, std::size_t bookCount, std::size_t activeOrderCount)
        {
            this->orders.reserve(orderCount);
            this->orderPool.reserve(orderCount);
            this->orderBooks.reserve(bookCount);
            static_cast<void>(activeOrderCount);
        }

        bool addSpotBook(SymbolId symbolId, OrderBookImplementation implementation = OrderBookImplementation::Radix);
        bool reservePriceLevels(SymbolId symbolId, std::size_t bidLevels, std::size_t askLevels);

        // Explicit retention boundary: queryOrder/queryPosition no longer
        // return records acknowledged by sink. IDs are never reused. Budget
        // bounds scanning work; active orders and referenced positions survive.
        HistoryArchiveResult archiveHistory(HistorySink& sink, std::size_t scanBudget = 1024);

        std::size_t retainedOrderCount() const noexcept
        {
            return this->orderPool.size();
        }

        PlaceOrderResult placeOrder(UserId userId, SymbolId symbolId, OrderSide side, OrderType type,
                                    TimeInForce timeInForce, Price price, Quantity quantity, Timestamp timestamp,
                                    TradeBuffer& trades);
        ModifyOrderResult modifyPrice(UserId userId, OrderId orderId, Price price,
                                      Timestamp timestamp, TradeBuffer& trades);
        ModifyOrderResult modifyQuantity(UserId userId, OrderId orderId, QuantityChange change, Timestamp timestamp);
        CancelOrderResult cancelOrder(UserId userId, OrderId orderId, Timestamp timestamp);

        [[nodiscard]]
        BalanceResult transferToContract(UserId userId, AssetId assetId, PositiveBalanceChange amount);

        [[nodiscard]]
        BalanceResult transferToSpot(UserId userId, AssetId assetId, PositiveBalanceChange amount);

        [[nodiscard]]
        std::optional<BasicOrder> queryOrder(OrderId orderId) const;
        [[nodiscard]]
        std::optional<Quote> bestBid(SymbolId symbolId) const noexcept;
        [[nodiscard]]
        std::optional<Quote> bestAsk(SymbolId symbolId) const noexcept;
        [[nodiscard]]
        std::optional<Quote> bidAt(SymbolId symbolId, Price price) const noexcept;
        [[nodiscard]]
        std::optional<Quote> askAt(SymbolId symbolId, Price price) const noexcept;
        // Run on the matching thread. Do not post mutations from the callback.
        // Includes retained completed/cancelled orders, in order ID order.
        template <typename Visitor>
        void forEachOrder(Visitor&& visitor) const
        {
            for (const OrderNode* node : this->orders)
            {
                if (node != nullptr)
                {
                    visitor(static_cast<const BasicOrder&>(node->basicOrder));
                }
            }
        }

        template <typename Visitor>
        void forEachOrderBook(Visitor&& visitor) const
        {
            for (const auto& book : this->orderBooks)
            {
                if (book)
                {
                    visitor(static_cast<const OrderBook&>(*book));
                }
            }
        }

    };
} // advmatch

#endif // ADVMATCH_MATCHING_ENGINE_H
