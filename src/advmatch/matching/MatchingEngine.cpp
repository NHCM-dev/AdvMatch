#include "MatchingEngine.h"
#include "orderbooks/impl/IndexedTreeOrderBook.h"

#include <limits>

namespace advmatch
{
    bool MatchingEngine::addSpotBook(SymbolId symbolId)
    {
        const Symbol* symbol = this->symbolManager.getSymbol(symbolId);
        if (!symbol || symbol->symbolType != SymbolType::Spot || symbol->baseAssetId == symbol->quoteAssetId)
        {
            return false;
        }

        if (this->orderBooks.size() < symbolId)
        {
            this->orderBooks.resize(symbolId);
        }

        std::unique_ptr<OrderBook>& book = this->orderBooks[symbolId - 1];
        if (book)
        {
            return false;
        }

        book = std::make_unique<IndexedTreeOrderBook>(symbolId);
        return true;
    }

    OrderNode* MatchingEngine::findNode(OrderId orderId) noexcept
    {
        if (orderId == 0 || orderId > this->orders.size())
        {
            return nullptr;
        }

        return this->orders[orderId - 1];
    }

    const OrderNode* MatchingEngine::findNode(OrderId orderId) const noexcept
    {
        if (orderId == 0 || orderId > this->orders.size())
        {
            return nullptr;
        }

        return this->orders[orderId - 1];
    }

    OrderBook* MatchingEngine::findBook(SymbolId symbolId) noexcept
    {
        if (symbolId == 0 || symbolId > this->orderBooks.size())
        {
            return nullptr;
        }

        return this->orderBooks[symbolId - 1].get();
    }

    const OrderBook* MatchingEngine::findBook(SymbolId symbolId) const noexcept
    {
        if (symbolId == 0 || symbolId > this->orderBooks.size())
        {
            return nullptr;
        }

        return this->orderBooks[symbolId - 1].get();
    }

    PlaceOrderResult MatchingEngine::placeOrder(UserId userId, SymbolId symbolId, OrderSide side, OrderType type,
                                                TimeInForce timeInForce, Price price,
                                                Quantity quantity, Timestamp timestamp, TradeBuffer& trades)
    {
        trades.clear();

        const auto reject = [&](PlaceOrderState reason)
        {
            return PlaceOrderResult(0, {OrderStatus::Rejected, 0, quantity}, reason);
        };

        User* user = this->userManager.getUser(userId);
        if (!user)
        {
            return reject(PlaceOrderState::UserNotFound);
        }

        const Symbol* symbol = this->symbolManager.getSymbol(symbolId);
        if (!symbol)
        {
            return reject(PlaceOrderState::InvalidSymbol);
        }

        OrderBook* book = findBook(symbolId);
        if (!book)
        {
            return reject(PlaceOrderState::InvalidSymbol);
        }

        if (this->nextOrderId == 0)
        {
            return reject(PlaceOrderState::OrderIdOverflow);
        }

        const OrderId id = this->nextOrderId++;

        if (this->orders.size() < id)
        {
            this->orders.resize(id, nullptr);
        }

        CreateOrderResult orderResult = this->orderFactory.createOrder(id, userId, symbolId, side, type, timeInForce, price, quantity, timestamp);
        if (!orderResult)
        {
            return reject(orderResult.state);
        }

        MKTOverflowBidResult mktBidResult = this->orderFactory.checkMKTOverflowBid(*orderResult.content, book);
        if (mktBidResult.first != PlaceOrderState::Success)
        {
            return reject(mktBidResult.first);
        }

        OrderNode* node = this->orderPool.emplace(
            *orderResult.content,
            *user, user->getSpotAccount(), *symbol, symbol->baseAssetId, symbol->quoteAssetId);

        const PlaceOrderRiskDecision riskDecision = this->riskEngine.checkPlaceOrder(*node, mktBidResult.second);
        if (riskDecision.rejectReason)
        {
            this->orderPool.release(node);
            return reject(*riskDecision.rejectReason);
        }

        this->orders[id - 1] = node;
        if (!this->accountLedger.reserve(*node, riskDecision.requiredReservationBalance))
        {
            this->orders[id - 1] = nullptr;
            this->orderPool.release(node);
            return reject(PlaceOrderState::InsufficientBalance);
        }

        auto result = book->place(*node, timestamp, trades);

        if (!result)
        {
            this->settleEngine.releaseReservation(*node);
            this->orders[id - 1] = nullptr;
            this->orderPool.release(node);
            return result;
        }

        this->settleEngine.settleTrades(trades);
        this->settleEngine.reconcileReservation(*node);
        return result;
    }

    ModifyOrderResult MatchingEngine::modifyPrice(UserId userId, OrderId orderId, Price price, Timestamp timestamp,
                                                  TradeBuffer& trades)
    {
        trades.clear();

        OrderNode* node = findNode(orderId);
        if (!node)
        {
            return {ModifyOrderState::OrderNotFound};
        }

        if (node->basicOrder.userId != userId)
        {
            return {ModifyOrderState::WrongUser};
        }

        OrderBook* book = findBook(node->basicOrder.symbolId);
        if (!book)
        {
            return {ModifyOrderState::OrderNotFound};
        }

        BasicOrder& order = node->basicOrder;
        if (order.getStatus() == OrderStatus::Filled)
        {
            return {ModifyOrderState::OrderAlreadyFilled};
        }

        if (order.getStatus() == OrderStatus::Cancelled)
        {
            return {ModifyOrderState::OrderCancelled};
        }

        if (order.orderType != OrderType::Limit)
        {
            return {ModifyOrderState::InvalidOrderType};
        }

        if (price == 0)
        {
            return {ModifyOrderState::InvalidPrice};
        }

        const ModifyOrderRiskDecision riskDecision = this->riskEngine.checkModifyPrice(*node, price);
        if (riskDecision.rejectReason)
        {
            return {*riskDecision.rejectReason};
        }

        if (!this->accountLedger.reserve(*node, riskDecision.requiredReservationBalance))
        {
            return {ModifyOrderState::InsufficientBalance};
        }

        auto result = book->modifyPrice(*node, price, timestamp, trades);
        if (result)
        {
            this->settleEngine.settleTrades(trades);
        }

        this->settleEngine.reconcileReservation(*node);
        return result;
    }

    ModifyOrderResult MatchingEngine::modifyQuantity(UserId userId, OrderId orderId, QuantityChange change,
                                                     Timestamp timestamp)
    {
        OrderNode* node = findNode(orderId);
        if (!node)
        {
            return {ModifyOrderState::OrderNotFound};
        }

        if (node->basicOrder.userId != userId)
        {
            return {ModifyOrderState::WrongUser};
        }

        OrderBook* book = findBook(node->basicOrder.symbolId);
        if (!book)
        {
            return {ModifyOrderState::OrderNotFound};
        }

        BasicOrder& order = node->basicOrder;
        if (order.getStatus() == OrderStatus::Filled)
        {
            return {ModifyOrderState::OrderAlreadyFilled};
        }

        if (order.getStatus() == OrderStatus::Cancelled)
        {
            return {ModifyOrderState::OrderCancelled};
        }

        const Quantity unfilledQuantity = order.getUnfilledQuantity();
        if (change > 0 && static_cast<Quantity>(change) > std::numeric_limits<Quantity>::max() - unfilledQuantity)
        {
            return {ModifyOrderState::Overflow};
        }

        const Quantity decrease = change < 0 ? static_cast<Quantity>(-(change + 1)) + 1 : 0;
        if (decrease > unfilledQuantity)
        {
            return {ModifyOrderState::InsufficientQuantity};
        }

        const ModifyOrderRiskDecision riskDecision = this->riskEngine.checkModifyQuantity(*node, change);
        if (riskDecision.rejectReason)
        {
            return {*riskDecision.rejectReason};
        }

        if (!this->accountLedger.reserve(*node, riskDecision.requiredReservationBalance))
        {
            return {ModifyOrderState::InsufficientBalance};
        }

        auto result = book->modifyQuantity(*node, change, timestamp);
        this->settleEngine.reconcileReservation(*node);
        return result;
    }

    CancelOrderResult MatchingEngine::cancelOrder(UserId userId, OrderId orderId, Timestamp timestamp)
    {
        OrderNode* node = findNode(orderId);
        if (!node)
        {
            return {CancelOrderState::OrderNotFound};
        }

        if (node->basicOrder.userId != userId)
        {
            return {CancelOrderState::WrongUser};
        }

        OrderBook* book = findBook(node->basicOrder.symbolId);
        if (!book)
        {
            return {CancelOrderState::OrderNotFound};
        }

        auto result = book->cancel(*node, timestamp);
        if (result)
        {
            this->settleEngine.reconcileReservation(*node);
        }

        return result;
    }

    BalanceResult MatchingEngine::transferToContract(UserId userId, AssetId assetId, PositiveBalanceChange amount)
    {
        User* user = this->userManager.getUser(userId);
        if (!user)
        {
            return {BalanceOpResult::InvalidAccount, "User ledging was not found"};
        }

        return user->transferToContract(assetId, amount);
    }

    BalanceResult MatchingEngine::transferToSpot(UserId userId, AssetId assetId, PositiveBalanceChange amount)
    {
        User* user = this->userManager.getUser(userId);
        if (!user)
        {
            return {BalanceOpResult::InvalidAccount, "User ledging was not found"};
        }

        return user->transferToSpot(assetId, amount);
    }

    std::optional<BasicOrder> MatchingEngine::queryOrder(OrderId orderId) const
    {
        const OrderNode* node = this->findNode(orderId);
        if (!node)
        {
            return std::nullopt;
        }

        return node->basicOrder;
    }

    std::optional<Quote> MatchingEngine::bestBid(SymbolId symbolId) const noexcept
    {
        const OrderBook* book = findBook(symbolId);
        return book ? book->bestBid() : std::nullopt;
    }

    std::optional<Quote> MatchingEngine::bestAsk(SymbolId symbolId) const noexcept
    {
        const OrderBook* book = findBook(symbolId);
        return book ? book->bestAsk() : std::nullopt;
    }

    std::optional<Quote> MatchingEngine::bidAt(SymbolId symbolId, Price price) const noexcept
    {
        const OrderBook* book = findBook(symbolId);
        return book ? book->bidAt(price) : std::nullopt;
    }

    std::optional<Quote> MatchingEngine::askAt(SymbolId symbolId, Price price) const noexcept
    {
        const OrderBook* book = findBook(symbolId);
        return book ? book->askAt(price) : std::nullopt;
    }
} // advmatch
