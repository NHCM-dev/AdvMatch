#ifndef ADVMATCH_PRICE_TIME_ORDER_BOOK_IMPL
#define ADVMATCH_PRICE_TIME_ORDER_BOOK_IMPL

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace advmatch
{
    template <typename BidLevels, typename AskLevels>
    std::optional<Balance> PriceTimeOrderBook<BidLevels, AskLevels>::estimateMarketBuyCost(Quantity quantity) const noexcept
    {
        Balance totalBalance = 0;
        bool overflow = false;
        this->asks.forEachWhile([&](const PriceLevel& level)
        {
            const Quantity fill = std::min(quantity, level.totalQuantity);
            const auto capacity = static_cast<PositiveBalanceChange>(std::numeric_limits<Balance>::max() - totalBalance);
            if (level.price != 0 && fill > capacity / level.price)
            {
                overflow = true;
                return false;
            }

            totalBalance += static_cast<Balance>(level.price * fill);
            quantity -= fill;
            return quantity != 0;
        });

        return overflow ? std::nullopt : std::optional<Balance>(totalBalance);
    }

    template <typename BidLevels, typename AskLevels>
    std::optional<Quote> PriceTimeOrderBook<BidLevels, AskLevels>::quote(const PriceLevel* level) noexcept
    {
        if (!level)
        {
            return std::nullopt;
        }

        return Quote{level->price, level->totalQuantity, level->orderCount};
    }

    template <typename BidLevels, typename AskLevels>
    bool PriceTimeOrderBook<BidLevels, AskLevels>::crosses(const BasicOrder& taker, Price makerPrice) noexcept
    {
        if (taker.orderType == OrderType::Market)
        {
            return true;
        }

        return taker.orderSide == OrderSide::Bid ? makerPrice <= taker.getPrice() : makerPrice >= taker.getPrice();
    }

    template <typename BidLevels, typename AskLevels>
    bool PriceTimeOrderBook<BidLevels, AskLevels>::canFillAll(const BasicOrder& order) const noexcept
    {
        Quantity needed = order.getUnfilledQuantity();
        const auto visit = [&](const auto& levels)
        {
            levels.forEachWhile([&](const PriceLevel& level)
            {
                if (!crosses(order, level.price))
                {
                    return false;
                }

                if (level.totalQuantity >= needed)
                {
                    needed = 0;
                    return false;
                }

                needed -= level.totalQuantity;
                return true;
            });
        };

        if (order.orderSide == OrderSide::Bid)
        {
            visit(this->asks);
        }
        else
        {
            visit(this->bids);
        }

        return needed == 0;
    }

    template <typename BidLevels, typename AskLevels>
    void PriceTimeOrderBook<BidLevels, AskLevels>::unlink(OrderNode& node, Quantity remaining) noexcept
    {
        PriceLevel* level = node.priceLevel;
        if (!level)
        {
            return;
        }

        level->totalQuantity -= remaining;
        --level->orderCount;
        if (node.prev)
        {
            node.prev->next = node.next;
        }
        else
        {
            level->head = node.next;
        }

        if (node.next)
        {
            node.next->prev = node.prev;
        }
        else
        {
            level->tail = node.prev;
        }

        const Price price = level->price;
        node.priceLevel = nullptr;
        node.prev = nullptr;
        node.next = nullptr;

        this->notifyRemoved(node);

        if (level->orderCount == 0)
        {
            if (node.basicOrder.orderSide == OrderSide::Bid)
            {
                this->bids.erase(price);
            }
            else
            {
                this->asks.erase(price);
            }
        }
    }

    template <typename BidLevels, typename AskLevels>
    void PriceTimeOrderBook<BidLevels, AskLevels>::append(OrderNode& node)
    {
        const Price price = node.basicOrder.getPrice();
        PriceLevel& level = node.basicOrder.orderSide == OrderSide::Bid
                                ? this->bids.getOrCreate(price)
                                : this->asks.getOrCreate(price);
        const Quantity quantity = node.basicOrder.getUnfilledQuantity();

        if (quantity > std::numeric_limits<Quantity>::max() - level.totalQuantity)
        {
            throw std::overflow_error("price level quantity overflow");
        }

        node.priceLevel = &level;
        node.prev = level.tail;
        node.next = nullptr;

        if (level.tail)
        {
            level.tail->next = &node;
        }
        else
        {
            level.head = &node;
        }

        level.tail = &node;
        level.totalQuantity += quantity;
        ++level.orderCount;
        this->notifyRested(node);
    }

    template <typename BidLevels, typename AskLevels>
    void PriceTimeOrderBook<BidLevels, AskLevels>::match(OrderNode& taker, Timestamp timestamp, TradeBuffer& trades)
    {
        while (taker.basicOrder.getUnfilledQuantity() != 0)
        {
            PriceLevel* level = taker.basicOrder.orderSide == OrderSide::Bid ? this->asks.best() : this->bids.best();
            if (!level || !crosses(taker.basicOrder, level->price))
            {
                break;
            }

            OrderNode& maker = *level->head;
            Quantity quantity = std::min(taker.basicOrder.getUnfilledQuantity(), maker.basicOrder.getUnfilledQuantity());
            if (this->matchPolicy)
            {
                const MatchAction action = this->matchPolicy->prepare(maker, taker, level->price, quantity);
                if (action == MatchAction::CancelMaker || action == MatchAction::CancelBoth)
                {
                    static_cast<void>(this->cancel(maker, timestamp));
                    this->matchPolicy->cancelled(maker);
                    if (action == MatchAction::CancelMaker)
                    {
                        continue;
                    }
                }

                if (action == MatchAction::CancelTaker || action == MatchAction::CancelBoth)
                {
                    static_cast<void>(taker.basicOrder.cancel(timestamp));
                    this->matchPolicy->cancelled(taker);
                    break;
                }
            }

            appendTrade(trades, {maker.basicOrder.orderId, taker.basicOrder.orderId, level->price, quantity}, maker,
                        taker);
            [[maybe_unused]] auto makerResult = maker.basicOrder.fill(quantity, timestamp);
            [[maybe_unused]] auto takerResult = taker.basicOrder.fill(quantity, timestamp);
            level->totalQuantity -= quantity;
            if (maker.basicOrder.getUnfilledQuantity() == 0)
            {
                unlink(maker, 0);
            }

            if (this->matchPolicy)
            {
                this->matchPolicy->commit(trades[trades.size() - 1], maker, taker, timestamp);
                if (this->synchronousSettlement)
                {
                    markSettled(trades);
                }
            }
        }
    }

    template <typename BidLevels, typename AskLevels>
    PlaceOrderResult PriceTimeOrderBook<BidLevels, AskLevels>::place(OrderNode& node, Timestamp timestamp, TradeBuffer& trades)
    {
        BasicOrder& order = node.basicOrder;
        const auto reject = [&](PlaceOrderState reason)
        {
            return PlaceOrderResult(order.orderId, {OrderStatus::Rejected, 0, order.getQuantity()}, reason);
        };

        if (order.symbolId != this->bookSymbolId)
        {
            return reject(PlaceOrderState::InvalidSymbol);
        }

        if (order.getQuantity() == 0)
        {
            return reject(PlaceOrderState::InvalidQuantity);
        }

        if (order.orderType == OrderType::Limit && order.getPrice() == 0)
        {
            return reject(PlaceOrderState::InvalidPrice);
        }

        if (order.timeInForce != TimeInForce::GTC && order.timeInForce != TimeInForce::IOC && order.timeInForce !=
            TimeInForce::FOK && order.timeInForce != TimeInForce::PostOnly)
        {
            return reject(PlaceOrderState::InvalidTimeInForce);
        }

        if (order.orderType == OrderType::Market && (order.timeInForce == TimeInForce::GTC || order.timeInForce == TimeInForce::PostOnly))
        {
            return reject(PlaceOrderState::InvalidTimeInForce);
        }

        if (order.timeInForce == TimeInForce::FOK && (!canFillAll(order) || (this->matchPolicy && !this->matchPolicy->canFillAll(node, *this))))
        {
            return reject(PlaceOrderState::InsufficientLiquidity);
        }

        const auto opposing = order.orderSide == OrderSide::Bid ? this->bestAsk() : this->bestBid();
        if (order.timeInForce == TimeInForce::PostOnly && opposing && crosses(order, opposing->price))
        {
            return reject(PlaceOrderState::PostOnlyWouldTrade);
        }

        if (order.orderType == OrderType::Limit && (order.timeInForce == TimeInForce::GTC || order.timeInForce == TimeInForce::PostOnly))
        {
            const PriceLevel* existing = order.orderSide == OrderSide::Bid
                                             ? this->bids.find(order.getPrice())
                                             : this->asks.find(order.getPrice());
            if (existing && order.getQuantity() > std::numeric_limits<Quantity>::max() - existing->totalQuantity)
            {
                return reject(PlaceOrderState::Overflow);
            }
        }

        match(node, timestamp, trades);

        if (order.getUnfilledQuantity() != 0 && !isOrderStatusTerminal(order.getStatus()))
        {
            if (order.orderType == OrderType::Limit && (order.timeInForce == TimeInForce::GTC || order.timeInForce == TimeInForce::PostOnly))
            {
                append(node);
            }
            else
            {
                [[maybe_unused]] auto result = order.cancel(timestamp);
            }
        }

        return {order.orderId, order.getFillingStatus()};
    }

    template <typename BidLevels, typename AskLevels>
    ModifyOrderResult PriceTimeOrderBook<BidLevels, AskLevels>::modifyPrice(OrderNode& node, Price price, Timestamp timestamp,
                                                        TradeBuffer& trades)
    {
        BasicOrder& order = node.basicOrder;
        if (!node.priceLevel)
        {
            return order.changePrice(price, timestamp);
        }

        if (order.orderType != OrderType::Limit)
        {
            return {ModifyOrderState::InvalidOrderType};
        }

        if (price == 0)
        {
            return {ModifyOrderState::InvalidPrice};
        }

        if (price == order.getPrice())
        {
            return {ModifyOrderState::Success};
        }

        const auto opposing = order.orderSide == OrderSide::Bid ? this->bestAsk() : this->bestBid();
        if (order.timeInForce == TimeInForce::PostOnly && opposing && (order.orderSide == OrderSide::Bid ? price >= opposing->price : price <= opposing->price))
        {
            return {ModifyOrderState::PostOnlyWouldTrade};
        }

        const PriceLevel* target = order.orderSide == OrderSide::Bid ? this->bids.find(price) : this->asks.find(price);
        if (target && order.getUnfilledQuantity() > std::numeric_limits<Quantity>::max() - target->totalQuantity)
        {
            return {ModifyOrderState::Overflow};
        }

        unlink(node, order.getUnfilledQuantity());
        auto result = order.changePrice(price, timestamp);

        if (!result)
        {
            return result;
        }

        match(node, timestamp, trades);

        if (order.getUnfilledQuantity() != 0 && !isOrderStatusTerminal(order.getStatus()))
        {
            append(node);
        }

        return result;
    }

    template <typename BidLevels, typename AskLevels>
    ModifyOrderResult PriceTimeOrderBook<BidLevels, AskLevels>::modifyQuantity(OrderNode& node, QuantityChange change, Timestamp timestamp)
    {
        BasicOrder& order = node.basicOrder;
        const Quantity before = order.getUnfilledQuantity();

        if (change > 0 && node.priceLevel && static_cast<Quantity>(change) > std::numeric_limits<Quantity>::max() - node.
            priceLevel->totalQuantity)
        {
            return {ModifyOrderState::Overflow};
        }

        auto result = order.changeQuantity(change, timestamp);

        if (!result || !node.priceLevel)
        {
            return result;
        }

        const Quantity after = order.getUnfilledQuantity();
        if (after == 0)
        {
            unlink(node, before);
            return result;
        }

        PriceLevel& level = *node.priceLevel;
        level.totalQuantity = level.totalQuantity - before + after;

        if (after > before && level.tail != &node)
        {
            if (node.prev)
            {
                node.prev->next = node.next;
            }
            else
            {
                level.head = node.next;
            }

            node.next->prev = node.prev;
            node.prev = level.tail;
            node.next = nullptr;
            level.tail->next = &node;
            level.tail = &node;
        }

        return result;
    }

    template <typename BidLevels, typename AskLevels>
    CancelOrderResult PriceTimeOrderBook<BidLevels, AskLevels>::cancel(OrderNode& node, Timestamp timestamp)
    {
        const Quantity remaining = node.basicOrder.getUnfilledQuantity();
        auto result = node.basicOrder.cancel(timestamp);

        if (result && node.priceLevel)
        {
            unlink(node, remaining);
        }

        return result;
    }
} // advmatch

#endif // ADVMATCH_PRICE_TIME_ORDER_BOOK_IMPL
