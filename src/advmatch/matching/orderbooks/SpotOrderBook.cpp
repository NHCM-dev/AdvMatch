#include "SpotOrderBook.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace advmatch
{
    std::optional<Balance> SpotOrderBook::marketBuyCost(Quantity quantity) const noexcept
    {
        std::uint64_t total = 0;

        for (const auto& [price, level] : this->asks.ordered)
        {
            const Quantity fill = std::min(quantity, level.totalQuantity);
            if (price != 0 && fill > (static_cast<std::uint64_t>(std::numeric_limits<Balance>::max()) - total) / price)
            {
                return std::nullopt;
            }

            total += price * fill;
            quantity -= fill;
            if (quantity == 0)
            {
                break;
            }
        }

        return static_cast<Balance>(total);
    }

    std::optional<Quote> SpotOrderBook::quote(const PriceLevel* level) noexcept
    {
        if (!level)
        {
            return std::nullopt;
        }

        return Quote{level->price, level->totalQuantity, level->orderCount};
    }

    bool SpotOrderBook::crosses(const BasicOrder& taker, Price makerPrice) const noexcept
    {
        if (taker.orderType == OrderType::Market)
        {
            return true;
        }

        return taker.orderSide == OrderSide::Bid ? makerPrice <= taker.getPrice() : makerPrice >= taker.getPrice();
    }

    bool SpotOrderBook::canFillAll(const BasicOrder& order) const noexcept
    {
        Quantity needed = order.getUnfilledQuantity();
        if (order.orderSide == OrderSide::Bid)
        {
            for (const auto& [price, level] : this->asks.ordered)
            {
                if (!crosses(order, price))
                {
                    break;
                }

                if (level.totalQuantity >= needed)
                {
                    return true;
                }

                needed -= level.totalQuantity;
            }
        }
        else
        {
            for (const auto& [price, level] : this->bids.ordered)
            {
                if (!crosses(order, price))
                {
                    break;
                }

                if (level.totalQuantity >= needed)
                {
                    return true;
                }

                needed -= level.totalQuantity;
            }
        }

        return false;
    }

    void SpotOrderBook::unlink(OrderNode& node, Quantity remaining) noexcept
    {
        PriceLevel* level = node.level;
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
        node.level = nullptr;
        node.prev = nullptr;
        node.next = nullptr;

        if (level->orderCount == 0)
        {
            if (node.order.orderSide == OrderSide::Bid)
            {
                this->bids.erase(price);
            }
            else
            {
                this->asks.erase(price);
            }
        }
    }

    void SpotOrderBook::append(OrderNode& node)
    {
        const Price price = node.order.getPrice();
        PriceLevel& level = node.order.orderSide == OrderSide::Bid ? this->bids.getOrCreate(price) : this->asks.getOrCreate(price);
        const Quantity quantity = node.order.getUnfilledQuantity();

        if (quantity > std::numeric_limits<Quantity>::max() - level.totalQuantity)
        {
            throw std::overflow_error("price level quantity overflow");
        }

        node.level = &level;
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
    }

    void SpotOrderBook::match(OrderNode& taker, Timestamp timestamp, Sequence sequence, std::vector<Trade>& trades)
    {
        while (taker.order.getUnfilledQuantity() != 0)
        {
            PriceLevel* level = taker.order.orderSide == OrderSide::Bid ? this->asks.best() : this->bids.best();
            if (!level || !crosses(taker.order, level->price))
            {
                break;
            }

            OrderNode& maker = *level->head;
            const Quantity quantity = std::min(taker.order.getUnfilledQuantity(), maker.order.getUnfilledQuantity());

            trades.push_back({maker.order.orderId, taker.order.orderId, level->price, quantity, sequence});
            [[maybe_unused]] auto makerResult = maker.order.fill(quantity, timestamp, sequence);
            [[maybe_unused]] auto takerResult = taker.order.fill(quantity, timestamp, sequence);
            level->totalQuantity -= quantity;

            if (maker.order.getUnfilledQuantity() == 0)
            {
                unlink(maker, 0);
            }
        }
    }

    PlaceOrderResult SpotOrderBook::place(OrderNode& node, Timestamp timestamp, Sequence sequence, std::vector<Trade>& trades)
    {
        BasicOrder& order = node.order;
        const auto reject = [&](PlaceOrderRejectReason reason)
        {
            return PlaceOrderResult(order.orderId, {OrderStatus::Rejected, 0, order.getQuantity()}, reason);
        };

        if (order.symbolId != this->bookSymbolId)
        {
            return reject(PlaceOrderRejectReason::InvalidSymbol);
        }

        if (order.getQuantity() == 0)
        {
            return reject(PlaceOrderRejectReason::InvalidQuantity);
        }

        if (order.orderType != OrderType::Market && order.orderType != OrderType::Limit)
        {
            return reject(PlaceOrderRejectReason::InvalidOrderType);
        }

        if (order.orderType == OrderType::Limit && order.getPrice() == 0)
        {
            return reject(PlaceOrderRejectReason::InvalidPrice);
        }

        if (order.timeInForce != TimeInForce::GTC && order.timeInForce != TimeInForce::IOC && order.timeInForce != TimeInForce::FOK)
        {
            return reject(PlaceOrderRejectReason::InvalidTimeInForce);
        }

        if (order.orderType == OrderType::Market && order.timeInForce == TimeInForce::GTC)
        {
            return reject(PlaceOrderRejectReason::InvalidTimeInForce);
        }

        if (order.timeInForce == TimeInForce::FOK && !canFillAll(order))
        {
            return reject(PlaceOrderRejectReason::InsufficientLiquidity);
        }

        if (order.orderType == OrderType::Limit && order.timeInForce == TimeInForce::GTC)
        {
            const PriceLevel* existing = order.orderSide == OrderSide::Bid ? this->bids.find(order.getPrice()) : this->asks.find(order.getPrice());
            if (existing && order.getQuantity() > std::numeric_limits<Quantity>::max() - existing->totalQuantity)
            {
                return reject(PlaceOrderRejectReason::Overflow);
            }
        }

        match(node, timestamp, sequence, trades);

        if (order.getUnfilledQuantity() != 0)
        {
            if (order.orderType == OrderType::Limit && order.timeInForce == TimeInForce::GTC)
            {
                append(node);
            }
            else
            {
                [[maybe_unused]] auto result = order.cancel(timestamp, sequence);
            }
        }

        return {order.orderId, order.getFillingStatus()};
    }

    ModifyOrderResult SpotOrderBook::modifyPrice(OrderNode& node, Price price, Timestamp timestamp, Sequence sequence, std::vector<Trade>& trades)
    {
        BasicOrder& order = node.order;
        if (!node.level)
        {
            return order.changePrice(price, timestamp, sequence);
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

        const PriceLevel* target = order.orderSide == OrderSide::Bid ? this->bids.find(price) : this->asks.find(price);
        if (target && order.getUnfilledQuantity() > std::numeric_limits<Quantity>::max() - target->totalQuantity)
        {
            return {ModifyOrderState::Overflow};
        }

        unlink(node, order.getUnfilledQuantity());
        auto result = order.changePrice(price, timestamp, sequence);

        if (!result)
        {
            return result;
        }

        match(node, timestamp, sequence, trades);

        if (order.getUnfilledQuantity() != 0)
        {
            append(node);
        }

        return result;
    }

    ModifyOrderResult SpotOrderBook::modifyQuantity(OrderNode& node, QuantityChange change, Timestamp timestamp, Sequence sequence)
    {
        BasicOrder& order = node.order;
        const Quantity before = order.getUnfilledQuantity();

        if (change > 0 && node.level && static_cast<Quantity>(change) > std::numeric_limits<Quantity>::max() - node.level->totalQuantity)
        {
            return {ModifyOrderState::Overflow};
        }

        auto result = order.changeQuantity(change, timestamp, sequence);

        if (!result || !node.level)
        {
            return result;
        }

        const Quantity after = order.getUnfilledQuantity();
        if (after == 0)
        {
            unlink(node, before);
            return result;
        }

        PriceLevel& level = *node.level;
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

    CancelOrderResult SpotOrderBook::cancel(OrderNode& node, Timestamp timestamp, Sequence sequence)
    {
        const Quantity remaining = node.order.getUnfilledQuantity();
        auto result = node.order.cancel(timestamp, sequence);

        if (result && node.level)
        {
            unlink(node, remaining);
        }

        return result;
    }
} // advmatch
