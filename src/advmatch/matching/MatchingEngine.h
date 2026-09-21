#ifndef ADVMATCH_MATCHING_ENGINE_H
#define ADVMATCH_MATCHING_ENGINE_H

#include "../management/UserManager.h"
#include "Matching.h"
#include "abstract/OrderBook.h"
#include "riskengines/SpotRiskEngine.h"

#include <memory>
#include <optional>
#include <vector>

#include "advmatch/management/SymbolManager.h"

namespace advmatch
{
    // Single-writer matching state. Queue consumers call this object; producer
    // threads must not call its mutating methods directly.
    class MatchingEngine
    {
        UserManager& userManager;
        SymbolManager& symbolManager;
        SpotRiskEngine riskEngine;

        // IDs are monotonically allocated from one, so id - 1 is a direct O(1)
        // index. unique_ptr keeps nodes and books stable when vectors grow.
        std::vector<std::unique_ptr<OrderNode>> orders;
        std::vector<std::unique_ptr<OrderBook>> orderBooks;
        OrderId nextOrderId = 1;
        Sequence nextSequence = 1;

        OrderNode* findNode(OrderId orderId) noexcept;
        const OrderNode* findNode(OrderId orderId) const noexcept;
        const OrderBook* findBook(SymbolId symbolId) const noexcept;
        OrderBook* findBook(SymbolId symbolId) noexcept;
        void settleTrades(const std::vector<Trade>& trades);

    public:
        explicit MatchingEngine(UserManager& userManager, SymbolManager& symbolManager)
        : userManager(userManager), symbolManager(symbolManager), riskEngine(userManager, symbolManager)
        {}

        // activeOrderCount sizes the risk reservation table independently from
        // retained order history, which can be substantially larger.
        void reserve(std::size_t orderCount, std::size_t bookCount, std::size_t activeOrderCount)
        {
            this->orders.reserve(orderCount);
            this->orderBooks.reserve(bookCount);
            this->riskEngine.reserve(activeOrderCount);
        }

        bool addSpotBook(SymbolId symbolId);

        PlaceOrderResult placeOrder(UserId userId, SymbolId symbolId, OrderSide side, OrderType type,
            TimeInForce timeInForce, Price price, Quantity quantity, Timestamp timestamp, std::vector<Trade>& trades);
        ModifyOrderResult modifyPrice(UserId userId, OrderId orderId, Price price,
            Timestamp timestamp, std::vector<Trade>& trades);
        ModifyOrderResult modifyQuantity(UserId userId, OrderId orderId, QuantityChange change, Timestamp timestamp);
        CancelOrderResult cancelOrder(UserId userId, OrderId orderId, Timestamp timestamp);

        std::optional<BasicOrder> queryOrder(OrderId orderId) const;
        std::optional<Quote> bestBid(SymbolId symbolId) const noexcept;
        std::optional<Quote> bestAsk(SymbolId symbolId) const noexcept;
        std::optional<Quote> bidAt(SymbolId symbolId, Price price) const noexcept;
        std::optional<Quote> askAt(SymbolId symbolId, Price price) const noexcept;
    };
} // advmatch

#endif // ADVMATCH_MATCHING_ENGINE_H
