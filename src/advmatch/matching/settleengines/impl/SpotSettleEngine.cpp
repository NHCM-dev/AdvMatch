#include "SpotSettleEngine.h"

#include "advmatch/ledging/User.h"
#include "advmatch/matching/Matching.h"
#include "advmatch/util/BalanceMath.h"

#include <limits>
#include <stdexcept>

namespace advmatch
{
    Balance SpotSettleEngine::requiredReservationBalance(const OrderNode& node)
    {
        if (isOrderStatusTerminal(node.basicOrder.getStatus()))
        {
            return 0;
        }

        const Quantity remaining = node.basicOrder.getUnfilledQuantity();
        if (node.basicOrder.orderSide == OrderSide::Ask)
        {
            if (remaining > static_cast<Quantity>(std::numeric_limits<Balance>::max()))
            {
                throw std::overflow_error("spot sell reservation would overflow");
            }

            return static_cast<Balance>(remaining);
        }

        if (node.basicOrder.orderType == OrderType::Market)
        {
            throw std::logic_error("active market order cannot retain a reservation");
        }

        const Price price = node.basicOrder.getPrice();
        const auto required = BalanceMath::multiply(price, remaining);
        if (!required)
        {
            throw std::overflow_error("spot buy reservation would overflow");
        }

        return *required;
    }

    void SpotSettleEngine::settleTrades(TradeBuffer& trades)
    {
        SettlementContext context;
        // A single fill cannot reuse a resolution. Keep its original direct
        // path; multi-fill sweeps reuse the taker's balances between makers.
        SettlementContext* sharedContext = trades.size() > 1 ? &context : nullptr;
        try
        {
            for (std::size_t index = 0; index < trades.size(); ++index)
            {
                const Trade& trade = trades[index];
                OrderNode& makerOrder = maker(trades, index);
                OrderNode& takerOrder = taker(trades, index);
                if (trade.makerOrderId != makerOrder.basicOrder.orderId ||
                    trade.takerOrderId != takerOrder.basicOrder.orderId)
                {
                    throw std::logic_error("trade node reference does not match its order id");
                }

                this->settleTrade(trade, makerOrder, takerOrder, sharedContext);
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
        this->settleTrade(trade, makerOrder, takerOrder, nullptr);
    }

    void SpotSettleEngine::settleTrade(const Trade& trade, OrderNode& makerOrder, OrderNode& takerOrder, SettlementContext* context)
    {
        const auto notional = BalanceMath::multiply(trade.price, trade.quantity);
        if (makerOrder.symbol != takerOrder.symbol || makerOrder.basicOrder.symbolId != takerOrder.basicOrder.symbolId ||
            makerOrder.basicOrder.orderSide == takerOrder.basicOrder.orderSide ||
            makerOrder.baseAssetId != takerOrder.baseAssetId ||
            makerOrder.quoteAssetId != takerOrder.quoteAssetId ||
            trade.quantity > static_cast<Quantity>(std::numeric_limits<Balance>::max()) ||
            !notional)
        {
            throw std::logic_error("invalid spot trade amount");
        }

        OrderNode& buyer = makerOrder.basicOrder.orderSide == OrderSide::Bid ? makerOrder : takerOrder;
        OrderNode& seller = makerOrder.basicOrder.orderSide == OrderSide::Ask ? makerOrder : takerOrder;
        const PositiveBalanceChange baseAmount = static_cast<PositiveBalanceChange>(trade.quantity);
        const PositiveBalanceChange quoteAmount = static_cast<PositiveBalanceChange>(*notional);
        if (!this->accountLedger.settle(buyer, seller, baseAmount, quoteAmount, context))
        {
            throw std::logic_error("spot settlement balances or reservations are invalid");
        }
    }

    void SpotSettleEngine::reconcileReservation(OrderNode& order)
    {
        this->accountLedger.releaseTo(order, requiredReservationBalance(order));
    }

    void SpotSettleEngine::releaseReservation(OrderNode& order)
    {
        this->accountLedger.releaseAll(order);
    }
} // advmatch
