#include "SpotSettleEngine.h"

#include "advmatch/ledging/User.h"
#include "advmatch/matching/Matching.h"
#include "advmatch/util/BalanceMath.h"
#include "advmatch/util/ContractMath.h"
#include "advmatch/matching/rules/TradingRuleEngine.h"
#include "advmatch/matching/rules/SelfTradePolicy.h"
#include "advmatch/matching/MatchingEngine.h"

#include <limits>
#include <stdexcept>

namespace advmatch
{
    Balance SpotSettleEngine::feeBalance(AssetId assetId) const noexcept
    {
        const auto found = this->fees.find(assetId);
        return found == this->fees.end() ? 0 : found->second;
    }

    TradingRuleResult SpotSettleEngine::withdrawFees(Account& account, AssetId assetId, PositiveBalanceChange amount)
    {
        if (amount > static_cast<PositiveBalanceChange>(this->feeBalance(assetId)))
        {
            return {TradingRuleState::InsufficientFees};
        }

        if (!account.updateBalance(assetId, static_cast<BalanceChange>(amount)))
        {
            return {TradingRuleState::InvalidAccount};
        }

        if (amount != 0)
        {
            this->fees.at(assetId) -= static_cast<Balance>(amount);
        }

        return {};
    }

    MatchAction SpotSettleEngine::prepare(OrderNode& maker, OrderNode& taker, Price price, Quantity& quantity)
    {
        const auto prevention = SelfTradePolicy::prepare(maker, taker);
        if (prevention != MatchAction::Execute)
        {
            return prevention;
        }

        const auto quote = BalanceMath::multiply(price, quantity);
        if (!quote || quantity > static_cast<Quantity>(std::numeric_limits<Balance>::max()))
        {
            return MatchAction::CancelTaker;
        }

        OrderNode& buyer = maker.basicOrder.orderSide == OrderSide::Bid ? maker : taker;
        OrderNode& seller = maker.basicOrder.orderSide == OrderSide::Ask ? maker : taker;
        const Balance buyerFee = ContractMath::rate(static_cast<Balance>(quantity), &buyer == &maker ? maker.makerFeeRate : taker.takerFeeRate, false);
        const Balance sellerFee = ContractMath::rate(*quote, &seller == &maker ? maker.makerFeeRate : taker.takerFeeRate, false);
        // Allocate treasury slots before either fills or account mutations.
        if (buyerFee != 0)
        {
            this->fees.try_emplace(buyer.baseAssetId, 0);
        }

        if (sellerFee != 0)
        {
            this->fees.try_emplace(seller.quoteAssetId, 0);
        }

        if ((buyerFee != 0 && (!ContractMath::balance(static_cast<WideBalance>(buyer.paidFees) + buyerFee) || !ContractMath::balance(static_cast<WideBalance>(this->feeBalance(buyer.baseAssetId)) + buyerFee))) || (sellerFee != 0 && (!ContractMath::balance(static_cast<WideBalance>(seller.paidFees) + sellerFee) || !ContractMath::balance(static_cast<WideBalance>(this->feeBalance(seller.quoteAssetId)) + sellerFee))))
        {
            return MatchAction::CancelTaker;
        }

        this->preparedPlan = this->accountLedger.prepare(buyer, seller, quantity, static_cast<PositiveBalanceChange>(*quote), buyerFee, sellerFee, &this->context);
        if (!this->preparedPlan)
        {
            return MatchAction::CancelTaker;
        }

        this->preparedBuyerFee = buyerFee;
        this->preparedSellerFee = sellerFee;

        return MatchAction::Execute;
    }

    void SpotSettleEngine::commit(const Trade& trade, OrderNode& maker, OrderNode& taker, Timestamp)
    {
        OrderNode& buyer = maker.basicOrder.orderSide == OrderSide::Bid ? maker : taker;
        OrderNode& seller = maker.basicOrder.orderSide == OrderSide::Ask ? maker : taker;
        this->accountLedger.commit(*this->preparedPlan, buyer, seller);
        if (this->preparedBuyerFee != 0)
        {
            this->fees.at(buyer.baseAssetId) += this->preparedBuyerFee;
            buyer.paidFees += this->preparedBuyerFee;
        }

        if (this->preparedSellerFee != 0)
        {
            this->fees.at(seller.quoteAssetId) += this->preparedSellerFee;
            seller.paidFees += this->preparedSellerFee;
        }

        this->preparedPlan.reset();
        this->reconcileReservation(maker);
    }

    void SpotSettleEngine::cancelled(OrderNode& node)
    {
        this->releaseReservation(node);
    }

    bool SpotSettleEngine::canFillAll(OrderNode& taker, const OrderBook& book)
    {
        // FOK preflights the same settlement/STP operations against private
        // balances. No maker cancellation, credit or fee escapes simulation.
        if (!this->matching)
        {
            return false;
        }

        SpotSettleEngine shadow(this->accountLedger, this->matching);
        shadow.fees = this->fees;
        DenseMap<AccountId, std::unique_ptr<SpotAccount>> accounts;
        const auto copy = [&](const OrderNode& original)
        {
            auto& account = accounts[original.account->getAccountId()];
            if (!account)
            {
                account = std::make_unique<SpotAccount>(original.account->getAccountId(), original.basicOrder.userId);
                for (AssetId assetId : {original.baseAssetId, original.quoteAssetId})
                {
                    static_cast<void>(account->applyBalanceChange(assetId, original.account->queryActiveBalance(assetId).value_or(0), original.account->queryReservedBalance(assetId).value_or(0)));
                }
            }

            auto node = std::make_unique<OrderNode>(original.basicOrder, *original.user, *account, *original.symbol, original.baseAssetId, original.quoteAssetId);
            node->reservedBalance = original.reservedBalance;
            node->selfTradePrevention = original.selfTradePrevention;
            node->makerFeeRate = original.makerFeeRate;
            node->takerFeeRate = original.takerFeeRate;
            node->paidFees = original.paidFees;
            return node;
        };

        auto buyer = copy(taker);
        bool eligible = true;
        const auto side = taker.basicOrder.orderSide == OrderSide::Bid ? OrderSide::Ask : OrderSide::Bid;
        book.forEachOrderWhile(side, [&](const BasicOrder& order)
        {
            if (buyer->basicOrder.getUnfilledQuantity() == 0 || (taker.basicOrder.orderType == OrderType::Limit && (side == OrderSide::Ask ? order.getPrice() > taker.basicOrder.getPrice() : order.getPrice() < taker.basicOrder.getPrice())))
            {
                return false;
            }

            const OrderNode* original = this->matching->findNode(order.orderId);
            auto maker = copy(*original);
            Quantity quantity = std::min(buyer->basicOrder.getUnfilledQuantity(), maker->basicOrder.getUnfilledQuantity());
            const auto action = shadow.prepare(*maker, *buyer, order.getPrice(), quantity);
            if (action == MatchAction::CancelMaker)
            {
                static_cast<void>(maker->basicOrder.cancel(taker.basicOrder.getCreatedTimestamp()));
                shadow.cancelled(*maker);
                return true;
            }

            if (action != MatchAction::Execute)
            {
                eligible = false;
                return false;
            }

            static_cast<void>(maker->basicOrder.fill(quantity, taker.basicOrder.getCreatedTimestamp()));
            static_cast<void>(buyer->basicOrder.fill(quantity, taker.basicOrder.getCreatedTimestamp()));
            shadow.commit({order.orderId, taker.basicOrder.orderId, order.getPrice(), quantity}, *maker, *buyer, taker.basicOrder.getCreatedTimestamp());
            return true;
        });
        return eligible && buyer->basicOrder.getUnfilledQuantity() == 0;
    }
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
        if (settledSynchronously(trades))
        {
            clearNodeReferences(trades);
            return;
        }
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
        const Balance buyerFee = ContractMath::rate(static_cast<Balance>(baseAmount), &buyer == &makerOrder ? makerOrder.makerFeeRate : takerOrder.takerFeeRate, false);
        const Balance sellerFee = ContractMath::rate(static_cast<Balance>(quoteAmount), &seller == &makerOrder ? makerOrder.makerFeeRate : takerOrder.takerFeeRate, false);
        if (!this->accountLedger.settle(buyer, seller, baseAmount, quoteAmount, context, buyerFee, sellerFee))
        {
            throw std::logic_error("spot settlement balances or reservations are invalid");
        }

        if (buyerFee != 0)
        {
            this->fees.at(buyer.baseAssetId) += buyerFee;
        }

        if (sellerFee != 0)
        {
            this->fees.at(seller.quoteAssetId) += sellerFee;
        }

        buyer.paidFees += buyerFee;
        seller.paidFees += sellerFee;
    }

    void SpotSettleEngine::reconcileReservation(OrderNode& order)
    {
        this->accountLedger.releaseTo(order, requiredReservationBalance(order));
        TradingRuleEngine::reconcile(order);
    }

    void SpotSettleEngine::releaseReservation(OrderNode& order)
    {
        this->accountLedger.releaseAll(order);
        TradingRuleEngine::release(order);
    }
} // advmatch
