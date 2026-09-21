//
// Created by NHCM on 2026/9/20.
//

#include "SpotRiskEngine.h"

#include <limits>
#include <stdexcept>

namespace advmatch
{
    std::optional<PlaceOrderRejectReason> SpotRiskEngine::checkPlaceOrder(BasicOrder& order)
    {
        Price price = order.getPrice();
        Quantity quantity = order.getQuantity();
        UserId userId = order.userId;

        Symbol* symbol = symbolManager.getSymbol(order.symbolId);
        if (!symbol)
        {
            return PlaceOrderRejectReason::InvalidSymbol;
        }
        User* user = userManager.getUser(userId);
        if (!user)
        {
            return PlaceOrderRejectReason::UserNotFound;
        }

        if (symbol->symbolType != SymbolType::Spot)
        {
            return PlaceOrderRejectReason::InvalidSymbol;
        }

        if (order.orderSide == OrderSide::Bid)
        {
            if (util::willMultiplyOverflow(price, quantity))
            {
                return PlaceOrderRejectReason::Overflow;
            }

            std::uint64_t balanceNeededUnsigned = price * quantity;

            if (balanceNeededUnsigned > static_cast<std::uint64_t>(std::numeric_limits<Balance>::max()))
            {
                return PlaceOrderRejectReason::Overflow;
            }

            auto balanceNeeded = static_cast<Balance>(balanceNeededUnsigned);
            AssetId quoteAssetId = symbol->quoteAssetId;

            std::optional<Balance> balance = user->queryActiveBalance(quoteAssetId);

            if (!balance.has_value())
            {
                return PlaceOrderRejectReason::InsufficientBalance;
            }

            if (balance.value() < balanceNeeded)
            {
                return PlaceOrderRejectReason::InsufficientBalance;
            }

            return std::nullopt;
        }
        else
        {
            AssetId baseAssetId = symbol->baseAssetId;

            std::optional<Balance> balance = user->queryActiveBalance(baseAssetId);

            if (!balance.has_value())
            {
                return PlaceOrderRejectReason::InsufficientBalance;
            }

            if (balance.value() < 0)
            {
                return PlaceOrderRejectReason::InsufficientBalance;
            }

            if (static_cast<std::uint64_t>(balance.value()) < quantity)
            {
                return PlaceOrderRejectReason::InsufficientBalance;
            }

            return std::nullopt;
        }
    }

    std::optional<ModifyOrderState> SpotRiskEngine::checkModifyPrice(BasicOrder& order, Price price)
    {
        if (order.orderSide == OrderSide::Ask)
        {
            return std::nullopt;
        }

        Quantity quantity = order.getUnfilledQuantity();
        UserId userId = order.userId;

        Symbol* symbol = symbolManager.getSymbol(order.symbolId);
        if (!symbol)
        {
            return ModifyOrderState::InvalidSymbol;
        }
        User* user = userManager.getUser(userId);
        if (!user)
        {
            return ModifyOrderState::UserNotFound;
        }

        Price oldPrice = order.getPrice();

        if (util::willMultiplyOverflow(oldPrice, quantity))
        {
            return ModifyOrderState::Overflow;
        }
        if (util::willMultiplyOverflow(price, quantity))
        {
            return ModifyOrderState::Overflow;
        }

        std::uint64_t oldBalanceNeededUnsigned = oldPrice * quantity;
        std::uint64_t newBalanceNeededUnsigned = price * quantity;

        if (oldBalanceNeededUnsigned > static_cast<std::uint64_t>(std::numeric_limits<Balance>::max()))
        {
            return ModifyOrderState::Overflow;
        }
        if (newBalanceNeededUnsigned > static_cast<std::uint64_t>(std::numeric_limits<Balance>::max()))
        {
            return ModifyOrderState::Overflow;
        }

        auto oldBalanceNeeded = static_cast<Balance>(oldBalanceNeededUnsigned);
        auto newBalanceNeeded = static_cast<Balance>(newBalanceNeededUnsigned);

        if (newBalanceNeeded <= oldBalanceNeeded)
        {
            return std::nullopt;
        }

        Balance additionalBalanceNeeded = newBalanceNeeded - oldBalanceNeeded;
        AssetId quoteAssetId = symbol->quoteAssetId;

        std::optional<Balance> balance = user->queryActiveBalance(quoteAssetId);

        if (!balance.has_value())
        {
            return ModifyOrderState::InsufficientBalance;
        }

        if (balance.value() < additionalBalanceNeeded)
        {
            return ModifyOrderState::InsufficientBalance;
        }

        return std::nullopt;
    }

    std::optional<ModifyOrderState> SpotRiskEngine::checkModifyQuantity(
        BasicOrder& order,
        QuantityChange quantityChange)
    {
        UserId userId = order.userId;

        Symbol* symbol = symbolManager.getSymbol(order.symbolId);
        if (!symbol)
        {
            return ModifyOrderState::InvalidSymbol;
        }
        User* user = userManager.getUser(userId);
        if (!user)
        {
            return ModifyOrderState::UserNotFound;
        }

        Quantity oldQuantity = order.getUnfilledQuantity();
        Quantity newQuantity;

        if (quantityChange >= 0)
        {
            auto increase = static_cast<std::uint64_t>(quantityChange);

            if (util::willAddOverflow(oldQuantity, increase))
            {
                return ModifyOrderState::Overflow;
            }

            newQuantity = oldQuantity + increase;
        }
        else
        {
            std::uint64_t decrease =
                static_cast<std::uint64_t>(-(quantityChange + 1)) + 1;

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

        Quantity additionalQuantity = newQuantity - oldQuantity;

        if (order.orderSide == OrderSide::Bid)
        {
            Price price = order.getPrice();

            if (util::willMultiplyOverflow(price, additionalQuantity))
            {
                return ModifyOrderState::Overflow;
            }

            std::uint64_t additionalBalanceNeededUnsigned =
                price * additionalQuantity;

            if (additionalBalanceNeededUnsigned >
                static_cast<std::uint64_t>(std::numeric_limits<Balance>::max()))
            {
                return ModifyOrderState::Overflow;
            }

            auto additionalBalanceNeeded =
                static_cast<Balance>(additionalBalanceNeededUnsigned);

            AssetId quoteAssetId = symbol->quoteAssetId;

            std::optional<Balance> balance =
                user->queryActiveBalance(quoteAssetId);

            if (!balance.has_value())
            {
                return ModifyOrderState::InsufficientBalance;
            }

            if (balance.value() < additionalBalanceNeeded)
            {
                return ModifyOrderState::InsufficientBalance;
            }

            return std::nullopt;
        }
        else
        {
            AssetId baseAssetId = symbol->baseAssetId;

            std::optional<Balance> balance =
                user->queryActiveBalance(baseAssetId);

            if (!balance.has_value())
            {
                return ModifyOrderState::InsufficientBalance;
            }

            if (balance.value() < 0)
            {
                return ModifyOrderState::InsufficientBalance;
            }

            if (static_cast<std::uint64_t>(balance.value()) < additionalQuantity)
            {
                return ModifyOrderState::InsufficientBalance;
            }

            return std::nullopt;
        }
    }

    std::optional<Balance> SpotRiskEngine::requiredHold(const BasicOrder& order, Price price, Quantity remaining) noexcept
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

    bool SpotRiskEngine::increaseHold(const BasicOrder& order, Balance amount)
    {
        if (amount == 0)
        {
            return true;
        }

        const Symbol* symbol = this->symbolManager.getSymbol(order.symbolId);
        User* user = this->userManager.getUser(order.userId);
        if (!symbol || !user)
        {
            return false;
        }

        const AssetId assetId = order.orderSide == OrderSide::Bid ? symbol->quoteAssetId : symbol->baseAssetId;
        auto iterator = this->holds.find(order.orderId);
        const Balance previous = iterator == this->holds.end() ? 0 : iterator->second.amount;
        if (amount > std::numeric_limits<Balance>::max() - previous)
        {
            return false;
        }

        if (iterator == this->holds.end())
        {
            iterator = this->holds.emplace(order.orderId, Hold{assetId, 0}).first;
        }

        if (!user->lockBalance(assetId, static_cast<PositiveBalanceChange>(amount)))
        {
            if (iterator->second.amount == 0)
            {
                this->holds.erase(iterator);
            }

            return false;
        }

        iterator->second.amount += amount;

        return true;
    }

    void SpotRiskEngine::reduceHold(const BasicOrder& order, Balance amount)
    {
        if (amount == 0)
        {
            return;
        }

        auto iterator = this->holds.find(order.orderId);
        User* user = this->userManager.getUser(order.userId);
        if (iterator == this->holds.end() || !user || amount > iterator->second.amount)
        {
            throw std::logic_error("spot reservation cannot be released");
        }

        if (!user->unlockBalance(iterator->second.assetId, static_cast<PositiveBalanceChange>(amount)))
        {
            throw std::logic_error("spot reservation cannot be released");
        }

        iterator->second.amount -= amount;
        if (iterator->second.amount == 0)
        {
            this->holds.erase(iterator);
        }
    }

    std::optional<PlaceOrderRejectReason> SpotRiskEngine::reservePlaceOrder(const BasicOrder& order, Balance marketBuyCost)
    {
        const Symbol* symbol = this->symbolManager.getSymbol(order.symbolId);
        if (!symbol || symbol->symbolType != SymbolType::Spot)
        {
            return PlaceOrderRejectReason::InvalidSymbol;
        }

        if (!this->userManager.getUser(order.userId))
        {
            return PlaceOrderRejectReason::UserNotFound;
        }

        auto amount = order.orderType == OrderType::Market && order.orderSide == OrderSide::Bid
            ? std::optional<Balance>(marketBuyCost)
            : requiredHold(order, order.getPrice(), order.getUnfilledQuantity());

        if (!amount || *amount < 0)
        {
            return PlaceOrderRejectReason::Overflow;
        }

        if (!increaseHold(order, *amount))
        {
            return PlaceOrderRejectReason::InsufficientBalance;
        }

        return std::nullopt;
    }

    std::optional<ModifyOrderState> SpotRiskEngine::reserveModifyOrder(const BasicOrder& order, Price price, Quantity remaining)
    {
        const Symbol* symbol = this->symbolManager.getSymbol(order.symbolId);
        if (!symbol || symbol->symbolType != SymbolType::Spot)
        {
            return ModifyOrderState::InvalidSymbol;
        }

        auto amount = requiredHold(order, price, remaining);
        if (!amount)
        {
            return ModifyOrderState::Overflow;
        }

        auto iterator = this->holds.find(order.orderId);
        const Balance held = iterator == this->holds.end() ? 0 : iterator->second.amount;
        if (*amount > held && !increaseHold(order, *amount - held))
        {
            return ModifyOrderState::InsufficientBalance;
        }

        return std::nullopt;
    }

    void SpotRiskEngine::settleOrder(const BasicOrder& order, Price tradePrice, Quantity quantity)
    {
        auto iterator = this->holds.find(order.orderId);
        User* user = this->userManager.getUser(order.userId);
        if (iterator == this->holds.end() || !user)
        {
            throw std::logic_error("spot reservation missing during settlement");
        }

        const Balance debit = order.orderSide == OrderSide::Ask ? static_cast<Balance>(quantity) : static_cast<Balance>(tradePrice * quantity);
        if (debit > iterator->second.amount || !user->consumeLockedBalance(iterator->second.assetId, static_cast<PositiveBalanceChange>(debit)))
        {
            throw std::logic_error("spot reservation insufficient during settlement");
        }

        iterator->second.amount -= debit;
        if (iterator->second.amount == 0)
        {
            this->holds.erase(iterator);
        }
    }

    void SpotRiskEngine::settleTrade(const Trade& trade, const BasicOrder& maker, const BasicOrder& taker)
    {
        const Symbol* symbol = this->symbolManager.getSymbol(maker.symbolId);
        if (!symbol || trade.quantity > static_cast<Quantity>(std::numeric_limits<Balance>::max()) ||
            (trade.price != 0 && trade.quantity > static_cast<Quantity>(std::numeric_limits<Balance>::max()) / trade.price))
        {
            throw std::logic_error("invalid spot trade amount");
        }

        const BasicOrder& buyerOrder = maker.orderSide == OrderSide::Bid ? maker : taker;
        const BasicOrder& sellerOrder = maker.orderSide == OrderSide::Ask ? maker : taker;
        User* buyer = this->userManager.getUser(buyerOrder.userId);
        User* seller = this->userManager.getUser(sellerOrder.userId);
        if (!buyer || !seller)
        {
            throw std::logic_error("trade user missing");
        }

        const auto baseAmount = static_cast<Balance>(trade.quantity);
        const auto quoteAmount = static_cast<Balance>(trade.price * trade.quantity);
        const Balance buyerBase = buyer->queryActiveBalance(symbol->baseAssetId).value_or(0);
        const Balance sellerQuote = seller->queryActiveBalance(symbol->quoteAssetId).value_or(0);
        if (baseAmount > std::numeric_limits<Balance>::max() - buyerBase || quoteAmount > std::numeric_limits<Balance>::max() - sellerQuote)
        {
            throw std::overflow_error("spot settlement balance overflow");
        }

        settleOrder(buyerOrder, trade.price, trade.quantity);
        settleOrder(sellerOrder, trade.price, trade.quantity);
        if (!buyer->updateBalance(symbol->baseAssetId, baseAmount) || !seller->updateBalance(symbol->quoteAssetId, quoteAmount))
        {
            throw std::logic_error("spot settlement credit failed");
        }
    }

    void SpotRiskEngine::reconcileOrder(const BasicOrder& order)
    {
        auto iterator = this->holds.find(order.orderId);
        if (iterator == this->holds.end())
        {
            return;
        }

        Balance required = 0;
        if (!isOrderStatusTerminal(order.getStatus()))
        {
            auto amount = requiredHold(order, order.getPrice(), order.getUnfilledQuantity());
            if (!amount)
            {
                throw std::logic_error("spot order hold overflow");
            }

            required = *amount;
        }

        if (required > iterator->second.amount)
        {
            throw std::logic_error("spot order hold below required amount");
        }

        reduceHold(order, iterator->second.amount - required);
    }

    void SpotRiskEngine::releaseOrder(const BasicOrder& order)
    {
        auto iterator = this->holds.find(order.orderId);
        if (iterator != this->holds.end())
        {
            reduceHold(order, iterator->second.amount);
        }
    }
} // advmatch
