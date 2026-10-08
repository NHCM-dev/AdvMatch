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
#include "advmatch/trading/factory/impl/BasicOrderFactory.h"

namespace advmatch
{
    class ContractEngine;
    // Single-writer matching state. Queue consumers call this object; producer
    // threads must not call its mutating methods directly. The referenced
    // managers must outlive this engine because OrderNode caches their stable
    // User and Symbol addresses.
    class MatchingEngine
    {
        struct BookServices
        {
            RiskEngine* risk = nullptr;
            AccountLedger* ledger = nullptr;
            SettleEngine* settlement = nullptr;
        };

        std::unique_ptr<ContractEngine> contracts;
        std::vector<BookServices> bookServices;
        friend class ContractEngine;
        friend class LiquidateEngine;
        friend class ContractSettleEngine;

        PlaceOrderResult submitOrder(UserId userId, SymbolId symbolId, OrderSide side, OrderType type,
            TimeInForce timeInForce, Price price, Quantity quantity, Timestamp timestamp, TradeBuffer& trades,
            const ContractOrderOptions* options, bool liquidation);
        UserManager& userManager;
        SymbolManager& symbolManager;
        SpotAccountLedger accountLedger;
        SpotRiskEngine riskEngine;
        SpotSettleEngine settleEngine;
        BasicOrderFactory orderFactory = BasicOrderFactory();

        // IDs are monotonically allocated from one, so id - 1 is a direct O(1)
        // index. The pool owns stable-address nodes; the vector only indexes
        // them and never owns or moves an OrderNode.
        BigAllocs<OrderNode> orderPool;
        std::vector<OrderNode*> orders;
        std::vector<std::unique_ptr<OrderBook>> orderBooks;
        OrderId nextOrderId = 1;
        std::size_t historyCursor = 0;
        bool historyPositionTurn = false;

        OrderNode* findNode(OrderId orderId) noexcept;
        const OrderNode* findNode(OrderId orderId) const noexcept;
        const OrderBook* findBook(SymbolId symbolId) const noexcept;
        OrderBook* findBook(SymbolId symbolId) noexcept;

    public:
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
