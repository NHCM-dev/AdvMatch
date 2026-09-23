#include "SpotSettleEngine.h"

#include "advmatch/account/User.h"
#include "advmatch/matching/Matching.h"
#include "advmatch/util/AccountLedger.h"

#include <limits>
#include <stdexcept>

namespace advmatch
{
    Balance SpotSettleEngine::requiredReservationBalance(const OrderNode& node)
    {
        if (isOrderStatusTerminal(node.order.getStatus()))
        {
            return 0;
        }

        const Quantity remaining = node.order.getUnfilledQuantity();
        if (node.order.orderSide == OrderSide::Ask)
        {
            if (remaining > static_cast<Quantity>(std::numeric_limits<Balance>::max()))
            {
                throw std::overflow_error("spot sell reservation would overflow");
            }

            return static_cast<Balance>(remaining);
        }

        if (node.order.orderType == OrderType::Market)
        {
            throw std::logic_error("active market order cannot retain a reservation");
        }

        const Price price = node.order.getPrice();
        if (price != 0 && remaining > static_cast<Quantity>(std::numeric_limits<Balance>::max()) / price)
        {
            throw std::overflow_error("spot buy reservation would overflow");
        }

        return static_cast<Balance>(price * remaining);
    }

    void SpotSettleEngine::settleTrades(TradeBuffer& trades)
    {
        try
        {
            for (std::size_t index = 0; index < trades.size(); ++index)
            {
                const Trade& trade = trades[index];
                OrderNode& makerOrder = maker(trades, index);
                OrderNode& takerOrder = taker(trades, index);
                if (trade.makerOrderId != makerOrder.order.orderId ||
                    trade.takerOrderId != takerOrder.order.orderId)
                {
                    throw std::logic_error("trade node reference does not match its order id");
                }

                this->settleTrade(trade, makerOrder, takerOrder);
                this->reconcileReservation(makerOrder);
            }
        }
        catch (...)
        {
            clearNodeReferences(trades);
            throw;
        }

        clearNodeReferences(trades);
    }

    void SpotSettleEngine::settleTrade(const Trade& trade, OrderNode& makerOrder, OrderNode& takerOrder)
    {
        if (makerOrder.symbol != takerOrder.symbol || makerOrder.order.symbolId != takerOrder.order.symbolId ||
            makerOrder.order.orderSide == takerOrder.order.orderSide ||
            makerOrder.baseAssetId != takerOrder.baseAssetId ||
            makerOrder.quoteAssetId != takerOrder.quoteAssetId ||
            trade.quantity > static_cast<Quantity>(std::numeric_limits<Balance>::max()) ||
            (trade.price != 0 &&
             trade.quantity > static_cast<Quantity>(std::numeric_limits<Balance>::max()) / trade.price))
        {
            throw std::logic_error("invalid spot trade amount");
        }

        OrderNode& buyer = makerOrder.order.orderSide == OrderSide::Bid ? makerOrder : takerOrder;
        OrderNode& seller = makerOrder.order.orderSide == OrderSide::Ask ? makerOrder : takerOrder;
        const PositiveBalanceChange baseAmount = static_cast<PositiveBalanceChange>(trade.quantity);
        const PositiveBalanceChange quoteAmount = static_cast<PositiveBalanceChange>(trade.price * trade.quantity);
        const Balance buyerBaseBalance = buyer.user->queryActiveBalance(buyer.baseAssetId).value_or(0);
        const Balance sellerQuoteBalance = seller.user->queryActiveBalance(seller.quoteAssetId).value_or(0);

        if (buyer.reservedBalance < 0 || seller.reservedBalance < 0 ||
            quoteAmount > static_cast<PositiveBalanceChange>(buyer.reservedBalance) ||
            baseAmount > static_cast<PositiveBalanceChange>(seller.reservedBalance))
        {
            throw std::logic_error("spot trade reservation is insufficient");
        }

        if (buyerBaseBalance < 0 || sellerQuoteBalance < 0 ||
            baseAmount > static_cast<PositiveBalanceChange>(std::numeric_limits<Balance>::max() - buyerBaseBalance) ||
            quoteAmount > static_cast<PositiveBalanceChange>(std::numeric_limits<Balance>::max() - sellerQuoteBalance))
        {
            throw std::overflow_error("spot settlement balance would overflow");
        }

        util::AccountLedger::consume(buyer, quoteAmount);
        util::AccountLedger::consume(seller, baseAmount);
        util::AccountLedger::credit(*buyer.user, buyer.baseAssetId, baseAmount);
        util::AccountLedger::credit(*seller.user, seller.quoteAssetId, quoteAmount);
    }

    void SpotSettleEngine::reconcileReservation(OrderNode& order)
    {
        util::AccountLedger::releaseTo(order, requiredReservationBalance(order));
    }

    void SpotSettleEngine::releaseReservation(OrderNode& order)
    {
        util::AccountLedger::releaseAll(order);
    }
} // advmatch
