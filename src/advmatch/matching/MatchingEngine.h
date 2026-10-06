#ifndef ADVMATCH_MATCHING_ENGINE_H
#define ADVMATCH_MATCHING_ENGINE_H

#include "../management/UserManager.h"
#include "Matching.h"
#include "TradeBuffer.h"
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
    // Single-writer matching state. Queue consumers call this object; producer
    // threads must not call its mutating methods directly. The referenced
    // managers must outlive this engine because OrderNode caches their stable
    // User and Symbol addresses.
    class MatchingEngine
    {
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

        OrderNode* findNode(OrderId orderId) noexcept;
        const OrderNode* findNode(OrderId orderId) const noexcept;
        const OrderBook* findBook(SymbolId symbolId) const noexcept;
        OrderBook* findBook(SymbolId symbolId) noexcept;

    public:
        explicit MatchingEngine(UserManager& userManager, SymbolManager& symbolManager)
            : userManager(userManager), symbolManager(symbolManager), settleEngine(this->accountLedger)
        {
        }

        // activeOrderCount is retained for source compatibility. Reservation
        // state is stored directly in OrderNode, so it needs no separate table.
        void reserve(std::size_t orderCount, std::size_t bookCount, std::size_t activeOrderCount)
        {
            this->orders.reserve(orderCount);
            this->orderPool.reserve(orderCount);
            this->orderBooks.reserve(bookCount);
            static_cast<void>(activeOrderCount);
        }

        bool addSpotBook(SymbolId symbolId);

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
    };
} // advmatch

#endif // ADVMATCH_MATCHING_ENGINE_H
