#include "MatchingEngine.h"
#include "orderbooks/impl/IndexedTreeOrderBook.h"
#include "../util/AccountLedger.h"

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

        const auto reject = [&](PlaceOrderRejectReason reason)
        {
            return PlaceOrderResult(0, {OrderStatus::Rejected, 0, quantity}, reason);
        };

        User* user = this->userManager.getUser(userId);
        if (!user)
        {
            return reject(PlaceOrderRejectReason::UserNotFound);
        }

        const Symbol* symbol = this->symbolManager.getSymbol(symbolId);
        if (!symbol)
        {
            return reject(PlaceOrderRejectReason::InvalidSymbol);
        }

        OrderBook* book = findBook(symbolId);
        if (!book)
        {
            return reject(PlaceOrderRejectReason::InvalidSymbol);
        }

        if (quantity == 0)
        {
            return reject(PlaceOrderRejectReason::InvalidQuantity);
        }

        if (util::willMultiplyOverflow(price, quantity))
        {
            return reject(PlaceOrderRejectReason::Overflow);
        }

        if (type != OrderType::Market && type != OrderType::Limit)
        {
            return reject(PlaceOrderRejectReason::InvalidOrderType);
        }

        if (type == OrderType::Limit && price == 0)
        {
            return reject(PlaceOrderRejectReason::InvalidPrice);
        }

        if (timeInForce != TimeInForce::GTC && timeInForce != TimeInForce::IOC && timeInForce != TimeInForce::FOK)
        {
            return reject(PlaceOrderRejectReason::InvalidTimeInForce);
        }

        if (type == OrderType::Market && timeInForce == TimeInForce::GTC)
        {
            return reject(PlaceOrderRejectReason::InvalidTimeInForce);
        }

        Balance estimatedMarketBuyBalance = 0;
        if (type == OrderType::Market && side == OrderSide::Bid)
        {
            auto cost = book->estimateMarketBuyCost(quantity);
            if (!cost)
            {
                return reject(PlaceOrderRejectReason::Overflow);
            }

            estimatedMarketBuyBalance = *cost;
        }

        if (this->nextOrderId == 0 || this->nextSequence == 0)
        {
            return reject(PlaceOrderRejectReason::Overflow);
        }

        const OrderId id = this->nextOrderId++;
        const Sequence sequence = this->nextSequence++;

        if (this->orders.size() < id)
        {
            this->orders.resize(id, nullptr);
        }

        OrderNode* node = this->orderPool.emplace(
            BasicOrder(id, userId, symbolId, side, type, timeInForce, price, quantity, timestamp, sequence),
            *user, *symbol, symbol->baseAssetId, symbol->quoteAssetId);

        const PlaceOrderRiskDecision riskDecision = this->riskEngine.checkPlaceOrder(*node, estimatedMarketBuyBalance);
        if (riskDecision.rejectReason)
        {
            this->orderPool.release(node);
            return reject(*riskDecision.rejectReason);
        }

        this->orders[id - 1] = node;
        if (!util::AccountLedger::reserve(*node, riskDecision.requiredReservationBalance))
        {
            this->orders[id - 1] = nullptr;
            this->orderPool.release(node);
            return reject(PlaceOrderRejectReason::InsufficientBalance);
        }

        auto result = book->place(*node, timestamp, sequence, trades);

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

        if (node->order.userId != userId)
        {
            return {ModifyOrderState::WrongUser};
        }

        OrderBook* book = findBook(node->order.symbolId);
        if (!book)
        {
            return {ModifyOrderState::OrderNotFound};
        }

        BasicOrder& order = node->order;
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

        if (!util::AccountLedger::reserve(*node, riskDecision.requiredReservationBalance))
        {
            return {ModifyOrderState::InsufficientBalance};
        }

        auto result = book->modifyPrice(*node, price, timestamp, this->nextSequence++, trades);
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

        if (node->order.userId != userId)
        {
            return {ModifyOrderState::WrongUser};
        }

        OrderBook* book = findBook(node->order.symbolId);
        if (!book)
        {
            return {ModifyOrderState::OrderNotFound};
        }

        BasicOrder& order = node->order;
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

        if (!util::AccountLedger::reserve(*node, riskDecision.requiredReservationBalance))
        {
            return {ModifyOrderState::InsufficientBalance};
        }

        auto result = book->modifyQuantity(*node, change, timestamp, this->nextSequence++);
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

        if (node->order.userId != userId)
        {
            return {CancelOrderState::WrongUser};
        }

        OrderBook* book = findBook(node->order.symbolId);
        if (!book)
        {
            return {CancelOrderState::OrderNotFound};
        }

        auto result = book->cancel(*node, timestamp, this->nextSequence++);
        if (result)
        {
            this->settleEngine.reconcileReservation(*node);
        }

        return result;
    }

    std::optional<BasicOrder> MatchingEngine::queryOrder(OrderId orderId) const
    {
        const OrderNode* node = this->findNode(orderId);
        if (!node)
        {
            return std::nullopt;
        }

        return node->order;
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
