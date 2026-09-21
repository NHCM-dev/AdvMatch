#include "SpotRiskEngine.h"
#include "advmatch/account/User.h"
#include "advmatch/trading/Symbol.h"

#include <limits>
#include <stdexcept>

namespace advmatch
{
    std::optional<PlaceOrderRejectReason> SpotRiskEngine::checkPlaceOrder(OrderNode& node)
    {
        BasicOrder& order = node.order;
        User& user = *node.user;
        const Symbol& symbol = *node.symbol;
        const Price price = order.getPrice();
        const Quantity quantity = order.getQuantity();

        if (symbol.symbolType != SymbolType::Spot || symbol.symbolId != order.symbolId)
        {
            return PlaceOrderRejectReason::InvalidSymbol;
        }

        if (user.getUserId() != order.userId)
        {
            return PlaceOrderRejectReason::UserNotFound;
        }

        if (order.orderSide == OrderSide::Bid)
        {
            if (util::willMultiplyOverflow(price, quantity))
            {
                return PlaceOrderRejectReason::Overflow;
            }

            const std::uint64_t requiredUnsigned = price * quantity;
            if (requiredUnsigned > static_cast<std::uint64_t>(std::numeric_limits<Balance>::max()))
            {
                return PlaceOrderRejectReason::Overflow;
            }

            const Balance available = user.queryActiveBalance(node.quoteAssetId).value_or(-1);
            return available < static_cast<Balance>(requiredUnsigned)
                       ? std::optional<PlaceOrderRejectReason>(PlaceOrderRejectReason::InsufficientBalance)
                       : std::nullopt;
        }

        const Balance available = user.queryActiveBalance(node.baseAssetId).value_or(-1);
        if (available < 0 || static_cast<std::uint64_t>(available) < quantity)
        {
            return PlaceOrderRejectReason::InsufficientBalance;
        }

        return std::nullopt;
    }

    std::optional<ModifyOrderState> SpotRiskEngine::checkModifyPrice(OrderNode& node, Price price)
    {
        BasicOrder& order = node.order;
        if (order.orderSide == OrderSide::Ask)
        {
            return std::nullopt;
        }

        const Quantity quantity = order.getUnfilledQuantity();
        const Price oldPrice = order.getPrice();
        if (util::willMultiplyOverflow(oldPrice, quantity) || util::willMultiplyOverflow(price, quantity))
        {
            return ModifyOrderState::Overflow;
        }

        const std::uint64_t oldRequiredUnsigned = oldPrice * quantity;
        const std::uint64_t newRequiredUnsigned = price * quantity;
        if (oldRequiredUnsigned > static_cast<std::uint64_t>(std::numeric_limits<Balance>::max()) ||
            newRequiredUnsigned > static_cast<std::uint64_t>(std::numeric_limits<Balance>::max()))
        {
            return ModifyOrderState::Overflow;
        }

        if (newRequiredUnsigned <= oldRequiredUnsigned)
        {
            return std::nullopt;
        }

        const Balance additional = static_cast<Balance>(newRequiredUnsigned - oldRequiredUnsigned);
        const Balance available = node.user->queryActiveBalance(node.quoteAssetId).value_or(-1);
        return available < additional
                   ? std::optional<ModifyOrderState>(ModifyOrderState::InsufficientBalance)
                   : std::nullopt;
    }

    std::optional<ModifyOrderState> SpotRiskEngine::checkModifyQuantity(OrderNode& node, QuantityChange quantityChange)
    {
        BasicOrder& order = node.order;
        const Quantity oldQuantity = order.getUnfilledQuantity();
        Quantity newQuantity;

        if (quantityChange >= 0)
        {
            const auto increase = static_cast<std::uint64_t>(quantityChange);
            if (util::willAddOverflow(oldQuantity, increase))
            {
                return ModifyOrderState::Overflow;
            }

            newQuantity = oldQuantity + increase;
        }
        else
        {
            const std::uint64_t decrease = static_cast<std::uint64_t>(-(quantityChange + 1)) + 1;
            if (!util::enoughSubtract(oldQuantity, decrease))
            {
                return ModifyOrderState::InvalidQuantity;
            }

            newQuantity = oldQuantity - decrease;
        }

        if (newQuantity <= oldQuantity)
        {
            return std::nullopt;
        }

        const Quantity additionalQuantity = newQuantity - oldQuantity;
        if (order.orderSide == OrderSide::Bid)
        {
            const Price price = order.getPrice();
            if (util::willMultiplyOverflow(price, additionalQuantity))
            {
                return ModifyOrderState::Overflow;
            }

            const std::uint64_t additionalUnsigned = price * additionalQuantity;
            if (additionalUnsigned > static_cast<std::uint64_t>(std::numeric_limits<Balance>::max()))
            {
                return ModifyOrderState::Overflow;
            }

            const Balance available = node.user->queryActiveBalance(node.quoteAssetId).value_or(-1);
            return available < static_cast<Balance>(additionalUnsigned)
                       ? std::optional<ModifyOrderState>(ModifyOrderState::InsufficientBalance)
                       : std::nullopt;
        }

        const Balance available = node.user->queryActiveBalance(node.baseAssetId).value_or(-1);
        if (available < 0 || static_cast<std::uint64_t>(available) < additionalQuantity)
        {
            return ModifyOrderState::InsufficientBalance;
        }

        return std::nullopt;
    }

    std::optional<Balance> SpotRiskEngine::requiredHold(const BasicOrder& order, Price price,
                                                        Quantity remaining) noexcept
    {
        if (order.orderSide == OrderSide::Ask)
        {
            if (remaining > static_cast<Quantity>(std::numeric_limits<Balance>::max()))
            {
                return std::nullopt;
            }

            return static_cast<Balance>(remaining);
        }

        if (price != 0 && remaining > static_cast<Quantity>(std::numeric_limits<Balance>::max()) / price)
        {
            return std::nullopt;
        }

        return static_cast<Balance>(price * remaining);
    }

    bool SpotRiskEngine::increaseHold(OrderNode& node, Balance amount)
    {
        if (amount == 0)
        {
            return true;
        }

        if (amount < 0 || amount > std::numeric_limits<Balance>::max() - node.heldAmount)
        {
            return false;
        }

        if (!node.user->lockBalance(node.holdAssetId, static_cast<PositiveBalanceChange>(amount)))
        {
            return false;
        }

        node.heldAmount += amount;
        return true;
    }

    void SpotRiskEngine::reduceHold(OrderNode& node, Balance amount)
    {
        if (amount == 0)
        {
            return;
        }

        if (amount < 0 || amount > node.heldAmount ||
            !node.user->unlockBalance(node.holdAssetId, static_cast<PositiveBalanceChange>(amount)))
        {
            throw std::logic_error("spot reservation cannot be released");
        }

        node.heldAmount -= amount;
    }

    std::optional<PlaceOrderRejectReason> SpotRiskEngine::reservePlaceOrder(OrderNode& node, Balance marketBuyCost)
    {
        const BasicOrder& order = node.order;
        const auto amount = order.orderType == OrderType::Market && order.orderSide == OrderSide::Bid
                                ? std::optional<Balance>(marketBuyCost)
                                : requiredHold(order, order.getPrice(), order.getUnfilledQuantity());

        if (!amount || *amount < 0)
        {
            return PlaceOrderRejectReason::Overflow;
        }

        if (!increaseHold(node, *amount))
        {
            return PlaceOrderRejectReason::InsufficientBalance;
        }

        return std::nullopt;
    }

    std::optional<ModifyOrderState> SpotRiskEngine::reserveModifyOrder(OrderNode& node, Price price, Quantity remaining)
    {
        const auto amount = requiredHold(node.order, price, remaining);
        if (!amount)
        {
            return ModifyOrderState::Overflow;
        }

        if (*amount > node.heldAmount && !increaseHold(node, *amount - node.heldAmount))
        {
            return ModifyOrderState::InsufficientBalance;
        }

        return std::nullopt;
    }

    void SpotRiskEngine::settleOrder(OrderNode& node, Price tradePrice, Quantity quantity)
    {
        const Balance debit = node.order.orderSide == OrderSide::Ask
                                  ? static_cast<Balance>(quantity)
                                  : static_cast<Balance>(tradePrice * quantity);

        if (debit > node.heldAmount ||
            !node.user->consumeLockedBalance(node.holdAssetId, static_cast<PositiveBalanceChange>(debit)))
        {
            throw std::logic_error("spot reservation insufficient during settlement");
        }

        node.heldAmount -= debit;
    }

    void SpotRiskEngine::settleTrade(const Trade& trade, OrderNode& maker, OrderNode& taker)
    {
        if (maker.symbol != taker.symbol || maker.order.symbolId != taker.order.symbolId ||
            maker.order.orderSide == taker.order.orderSide || maker.baseAssetId != taker.baseAssetId ||
            maker.quoteAssetId != taker.quoteAssetId ||
            trade.quantity > static_cast<Quantity>(std::numeric_limits<Balance>::max()) ||
            (trade.price != 0 && trade.quantity > static_cast<Quantity>(std::numeric_limits<Balance>::max()) / trade.
                price))
        {
            throw std::logic_error("invalid spot trade amount");
        }

        OrderNode& buyer = maker.order.orderSide == OrderSide::Bid ? maker : taker;
        OrderNode& seller = maker.order.orderSide == OrderSide::Ask ? maker : taker;
        const Balance baseAmount = static_cast<Balance>(trade.quantity);
        const Balance quoteAmount = static_cast<Balance>(trade.price * trade.quantity);
        const Balance buyerBase = buyer.user->queryActiveBalance(buyer.baseAssetId).value_or(0);
        const Balance sellerQuote = seller.user->queryActiveBalance(seller.quoteAssetId).value_or(0);

        if (baseAmount > std::numeric_limits<Balance>::max() - buyerBase ||
            quoteAmount > std::numeric_limits<Balance>::max() - sellerQuote)
        {
            throw std::overflow_error("spot settlement balance overflow");
        }

        settleOrder(buyer, trade.price, trade.quantity);
        settleOrder(seller, trade.price, trade.quantity);
        if (!buyer.user->updateBalance(buyer.baseAssetId, baseAmount) || !seller.user->updateBalance(
            seller.quoteAssetId, quoteAmount))
        {
            throw std::logic_error("spot settlement credit failed");
        }
    }

    void SpotRiskEngine::reconcileOrder(OrderNode& node)
    {
        Balance required = 0;
        if (!isOrderStatusTerminal(node.order.getStatus()))
        {
            const auto amount = requiredHold(node.order, node.order.getPrice(), node.order.getUnfilledQuantity());
            if (!amount)
            {
                throw std::logic_error("spot order hold overflow");
            }

            required = *amount;
        }

        if (required > node.heldAmount)
        {
            throw std::logic_error("spot order hold below required amount");
        }

        reduceHold(node, node.heldAmount - required);
    }

    void SpotRiskEngine::releaseOrder(OrderNode& node)
    {
        reduceHold(node, node.heldAmount);
    }
} // advmatch
