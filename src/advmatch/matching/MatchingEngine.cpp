#include "MatchingEngine.h"
#include "orderbooks/SpotOrderBook.h"

#include <stdexcept>
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

        return this->orderBooks.try_emplace(symbolId, std::make_unique<SpotOrderBook>(symbolId)).second;
    }

    void MatchingEngine::settleTrades(const std::vector<Trade>& trades)
    {
        for (const Trade& trade : trades)
        {
            const BasicOrder& maker = this->orders.at(trade.makerOrderId)->order;
            const BasicOrder& taker = this->orders.at(trade.takerOrderId)->order;
            this->riskEngine.settleTrade(trade, maker, taker);
            this->riskEngine.reconcileOrder(maker);
        }
    }

    OrderNode* MatchingEngine::findNode(OrderId orderId) noexcept
    {
        auto it = this->orders.find(orderId);
        return it == this->orders.end() ? nullptr : it->second.get();
    }

    OrderBook* MatchingEngine::findBook(SymbolId symbolId) noexcept
    {
        auto it = this->orderBooks.find(symbolId);
        return it == this->orderBooks.end() ? nullptr : it->second.get();
    }

    const OrderBook* MatchingEngine::findBook(SymbolId symbolId) const noexcept
    {
        auto it = this->orderBooks.find(symbolId);
        return it == this->orderBooks.end() ? nullptr : it->second.get();
    }

    PlaceOrderResult MatchingEngine::placeOrder(UserId userId, SymbolId symbolId, OrderSide side, OrderType type, TimeInForce timeInForce, Price price,
                                                Quantity quantity, Timestamp timestamp, std::vector<Trade>& trades)
    {
        trades.clear();

        const auto reject = [&](PlaceOrderRejectReason reason)
        {
            return PlaceOrderResult(0, {OrderStatus::Rejected, 0, quantity}, reason);
        };

        if (!this->userManager.containsUser(userId))
        {
            return reject(PlaceOrderRejectReason::UserNotFound);
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

        Balance marketBuyCost = 0;
        if (type == OrderType::Market && side == OrderSide::Bid)
        {
            auto cost = static_cast<SpotOrderBook*>(book)->marketBuyCost(quantity);
            if (!cost)
            {
                return reject(PlaceOrderRejectReason::Overflow);
            }

            marketBuyCost = *cost;
        }

        if (this->nextOrderId == 0 || this->nextSequence == 0)
        {
            return reject(PlaceOrderRejectReason::Overflow);
        }

        const OrderId id = this->nextOrderId++;
        const Sequence sequence = this->nextSequence++;

        auto node = std::make_unique<OrderNode>(BasicOrder(id, userId, symbolId, side, type, timeInForce, price, quantity, timestamp, sequence));
        OrderNode& order = *node;

        if (auto reason = this->riskEngine.checkPlaceOrder(order.order))
        {
            return reject(*reason);
        }

        this->orders.emplace(id, std::move(node));
        if (auto reason = this->riskEngine.reservePlaceOrder(order.order, marketBuyCost))
        {
            this->orders.erase(id);
            return reject(*reason);
        }

        auto result = book->place(order, timestamp, sequence, trades);

        if (!result)
        {
            this->riskEngine.releaseOrder(order.order);
            this->orders.erase(id);
            return result;
        }

        settleTrades(trades);
        this->riskEngine.reconcileOrder(order.order);
        return result;
    }

    ModifyOrderResult MatchingEngine::modifyPrice(UserId userId, OrderId orderId, Price price, Timestamp timestamp, std::vector<Trade>& trades)
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

        if (auto reason = this->riskEngine.checkModifyPrice(order, price))
        {
            return {*reason};
        }

        if (auto reason = this->riskEngine.reserveModifyOrder(order, price, order.getUnfilledQuantity()))
        {
            return {*reason};
        }

        auto result = book->modifyPrice(*node, price, timestamp, this->nextSequence++, trades);
        if (result)
        {
            settleTrades(trades);
        }

        this->riskEngine.reconcileOrder(order);
        return result;
    }

    ModifyOrderResult MatchingEngine::modifyQuantity(UserId userId, OrderId orderId, QuantityChange change, Timestamp timestamp)
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

        if (change > 0 && static_cast<Quantity>(change) > std::numeric_limits<Quantity>::max() - order.getUnfilledQuantity())
        {
            return {ModifyOrderState::Overflow};
        }

        const Quantity decrease = change < 0 ? static_cast<Quantity>(-(change + 1)) + 1 : 0;
        if (decrease > order.getUnfilledQuantity())
        {
            return {ModifyOrderState::InsufficientQuantity};
        }

        const Quantity remaining = change < 0 ? order.getUnfilledQuantity() - decrease : order.getUnfilledQuantity() + static_cast<Quantity>(change);
        if (auto reason = this->riskEngine.checkModifyQuantity(order, change))
        {
            return {*reason};
        }

        if (auto reason = this->riskEngine.reserveModifyOrder(order, order.getPrice(), remaining))
        {
            return {*reason};
        }

        auto result = book->modifyQuantity(*node, change, timestamp, this->nextSequence++);
        this->riskEngine.reconcileOrder(order);
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
            this->riskEngine.reconcileOrder(node->order);
        }

        return result;
    }

    std::optional<BasicOrder> MatchingEngine::queryOrder(OrderId orderId) const
    {
        auto it = this->orders.find(orderId);
        if (it == this->orders.end())
        {
            return std::nullopt;
        }

        return it->second->order;
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
