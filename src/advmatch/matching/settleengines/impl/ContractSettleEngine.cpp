#include "ContractSettleEngine.h"
#include "advmatch/matching/MatchingEngine.h"
#include "advmatch/matching/riskengines/impl/ContractRiskEngine.h"
#include "advmatch/util/ContractMath.h"
#include "advmatch/matching/contracts/LiquidateEngine.h"

#include <algorithm>
#include <cassert>

namespace advmatch
{
    struct ContractSettleEngine::SimulationWorkspace
    {
        PositionPool positions;
        BigAllocs<ContractParticipant> participants;
        BigAllocs<StandardContractAccount, 1, 16> accountPool;
        BigAllocs<OrderNode, 1, 64> orderPool;
        ContractMarket market;
        DenseMap<ContractAccount*, StandardContractAccount*> accounts;

        void reset()
        {
            this->market.pendingLiquidations.clear();
            this->orderPool.clear();
            this->accountPool.clear();
            this->participants.clear();
            this->market.participants.clear();
            this->market.activePositions.clear();
            this->market.orderParticipants.clear();
            this->market.participantPool = &this->participants;
            this->accounts.clear();
        }
    };

    ContractSettleEngine::ContractSettleEngine(ContractAccountLedger& accountLedger, MatchingEngine& matching)
        : accountLedger(accountLedger), matching(matching)
    {
    }

    ContractSettleEngine::~ContractSettleEngine() = default;

    ContractState ContractSettleEngine::plan(const ContractMarket& market, const ContractParticipant& participant,
        OrderSide side, Price price, Quantity quantity, Balance fee, Balance reservation,
        Balance oldReservation, bool liquidation, PositionPlan& result, bool batch,
        std::optional<Balance> executionNotional, Account::BalanceUpdate* balanceUpdate) noexcept
    {
        const Position& position = *participant.position;
        const bool opening = (side == OrderSide::Bid) == (position.positionSide == PositionSide::Long);
        const auto value = executionNotional ? executionNotional : ContractMath::notional(price, quantity, market.spec.contractMultiplier);
        const auto balance = participant.account->captureBalance(market.symbol->quoteAssetId);
        if (!value || quantity == 0 || fee < 0 || reservation < 0)
        {
            return ContractState::Overflow;
        }

        result = {};
        result.fee = fee;
        result.eventType = liquidation ? ContractEventType::Liquidation : ContractEventType::Fill;
        result.executionPrice = price;
        result.executionQuantity = quantity;
        result.orderReservation = reservation;
        if (opening)
        {
            const auto entry = ContractMath::balance(static_cast<WideBalance>(position.entryNotional) + *value);
            const WideBalance size = static_cast<WideBalance>(position.positionSize) + quantity;
            const auto margin = ContractMath::balance(static_cast<WideBalance>(position.initialMargin) + ContractMath::margin(*value, position.leverage));
            if (!entry || !margin || *entry > market.spec.maxPositionNotional || size > std::numeric_limits<Quantity>::max())
            {
                return ContractState::Overflow;
            }

            result.size = static_cast<Quantity>(size);
            result.entryNotional = *entry;
            result.margin = position.marginMode == MarginMode::Isolated ? *margin : ContractMath::margin(*entry, position.leverage);
        }
        else
        {
            if (quantity > position.positionSize)
            {
                return ContractState::PositionNotFound;
            }

            const Balance entry = ContractMath::proportional(position.entryNotional, quantity, position.positionSize);
            result.size = position.positionSize - quantity;
            result.entryNotional = position.entryNotional - entry;
            result.margin = position.initialMargin - ContractMath::proportional(position.initialMargin, quantity, position.positionSize);
            result.realizedPnL = position.positionSide == PositionSide::Long ? *value - entry : entry - *value;
        }

        const auto marked = ContractMath::notional(market.markPrice, result.size, market.spec.contractMultiplier);
        if (!marked || (opening && *marked > market.spec.maxPositionNotional))
        {
            return ContractState::Overflow;
        }

        result.unrealizedPnL = position.positionSide == PositionSide::Long ? *marked - result.entryNotional : result.entryNotional - *marked;
        result.maintenanceMargin = ContractMath::rate(*marked, market.spec.maintenanceMarginRate, true);
        result.estimatedCloseFee = ContractMath::rate(*marked, market.spec.liquidationFeeRate, true);
        const bool isolated = position.marginMode == MarginMode::Isolated;
        const BalanceChange marginChange = isolated ? result.margin - position.initialMargin : 0;
        const auto reserved = ContractMath::balance(static_cast<WideBalance>(reservation) - oldReservation + marginChange);
        if (!reserved || !ContractMath::balance(static_cast<WideBalance>(position.realizedPnL) + result.realizedPnL) ||
            !ContractMath::balance(static_cast<WideBalance>(position.paidFees) + fee))
        {
            return ContractState::Overflow;
        }

        result.reservedChange = *reserved;
        WideBalance activeChange = -static_cast<WideBalance>(*reserved) + result.realizedPnL - fee;
        if (!opening && liquidation)
        {
            // Isolated losses cannot consume the trader's other collateral.
            // Cross liquidation can consume the whole cross collateral pool.
            const WideBalance available = isolated ? -static_cast<WideBalance>(marginChange) + result.realizedPnL - fee
                : static_cast<WideBalance>(balance.active()) + activeChange;
            if (available < 0 && (isolated || !batch))
            {
                const auto deficit = ContractMath::balance(-available);
                if (!deficit || *deficit > market.insuranceBalance)
                {
                    return ContractState::InsufficientInsurance;
                }

                result.insurance = *deficit;
                activeChange += *deficit;
            }
        }
        else if (!opening && isolated && -static_cast<WideBalance>(marginChange) + result.realizedPnL - fee < 0)
        {
            return ContractState::InsufficientBalance;
        }

        const auto active = ContractMath::balance(activeChange);
        const auto prepared = active && !batch ? participant.account->prepareBalanceChange(balance, *active, *reserved) : std::nullopt;
        if (!active || (!batch && !prepared))
        {
            return ContractState::InsufficientBalance;
        }

        result.activeChange = *active;
        if (balanceUpdate && prepared)
        {
            *balanceUpdate = *prepared;
        }
        const CrossMarginState& cross = *participant.crossMargin;
        const auto crossInitial = ContractMath::balance(static_cast<WideBalance>(cross.initialMargin) + (isolated ? 0 : result.margin - position.initialMargin));
        const auto crossMaintenance = ContractMath::balance(static_cast<WideBalance>(cross.maintenanceMargin) + (isolated ? 0 : result.maintenanceMargin - position.maintenanceMargin));
        const auto crossPnl = ContractMath::balance(static_cast<WideBalance>(cross.unrealizedPnL) + (isolated ? WideBalance(0) : static_cast<WideBalance>(result.unrealizedPnL) - position.updatedUnrealizedPnl));
        const auto crossFee = ContractMath::balance(static_cast<WideBalance>(cross.liquidationFeeReserve) + (isolated ? 0 : result.estimatedCloseFee - position.estimatedCloseFee));
        if (!crossInitial || !crossMaintenance || !crossPnl || !crossFee)
        {
            return ContractState::Overflow;
        }

        if (opening)
        {
            const WideBalance equity = isolated ? static_cast<WideBalance>(result.margin) + result.unrealizedPnL
                : static_cast<WideBalance>(balance.active()) + *active + *crossPnl;
            const WideBalance requirement = isolated ? static_cast<WideBalance>(result.maintenanceMargin) + result.estimatedCloseFee : *crossInitial;
            if (isolated ? equity <= requirement : equity < requirement)
            {
                return ContractState::InsufficientBalance;
            }
        }

        return ContractState::Success;
    }

    void ContractSettleEngine::apply(ContractMarket& market, ContractParticipant& participant,
        const PositionPlan& plan, Timestamp timestamp, bool applyAccount, bool applyCross, bool applyMarket,
        const Account::BalanceUpdate* balanceUpdate)
    {
        Position& position = *participant.position;
        if (applyAccount)
        {
            const auto result = balanceUpdate ? participant.account->commitBalanceChange(*balanceUpdate)
                : participant.account->applyBalanceChange(market.symbol->quoteAssetId, plan.activeChange, plan.reservedChange);
            assert(result);
            static_cast<void>(result);
        }
        if (applyCross && position.marginMode == MarginMode::Cross)
        {
            participant.crossMargin->initialMargin += plan.margin - position.initialMargin;
            participant.crossMargin->maintenanceMargin += plan.maintenanceMargin - position.maintenanceMargin;
            participant.crossMargin->liquidationFeeReserve += plan.estimatedCloseFee - position.estimatedCloseFee;
            participant.crossMargin->unrealizedPnL = static_cast<BalanceChange>(static_cast<WideBalance>(participant.crossMargin->unrealizedPnL) + plan.unrealizedPnL - position.updatedUnrealizedPnl);
        }

        if (position.positionSize == 0 && plan.size != 0)
        {
            linkTradingParticipant(participant);
            ++participant.account->managedPositionCount;
            participant.account->activePositions[position.positionId] = &position;
            participant.account->tradingPositions[position.positionId] = &position;
            market.activePositions[position.positionId] = &participant;
        }
        else if (position.positionSize != 0 && plan.size == 0)
        {
            --participant.account->managedPositionCount;
            participant.account->activePositions.erase(position.positionId);
            market.activePositions.erase(position.positionId);
            if (participant.openOrderCount == 0)
            {
                participant.account->tradingPositions.erase(position.positionId);
                unlinkTradingParticipant(participant);
            }
        }

        position.positionSize = plan.size;
        position.entryNotional = plan.entryNotional;
        position.averageEntryPrice = plan.size == 0 ? 0 : static_cast<Price>(plan.entryNotional / plan.size / position.contractMultiplier);
        position.initialMargin = plan.margin;
        position.maintenanceMargin = plan.maintenanceMargin;
        position.estimatedCloseFee = plan.estimatedCloseFee;
        position.updatedUnrealizedPnl = plan.unrealizedPnL;
        position.realizedPnL += plan.realizedPnL;
        position.fundingPnL += plan.fundingChange;
        position.paidFees += plan.fee;
        ContractMath::updatePriceReferences(position, market.spec);
        position.updatedTimestamp = timestamp;
        position.closed = plan.size == 0;
        if (position.closed)
        {
            position.liquidating = false;
            if (position.marginMode == MarginMode::Cross && participant.crossMargin->maintenanceMargin == 0)
            {
                participant.crossMargin->liquidating = false;
            }
        }

        if (applyMarket)
        {
            market.insuranceBalance -= plan.insurance;
            market.clearingBalance -= plan.realizedPnL;
            market.feeBalance += plan.fee;
        }
    }

    void ContractSettleEngine::publish(const ContractMarket& market, const ContractParticipant& participant,
        const PositionPlan& plan, Timestamp timestamp) noexcept
    {
        if (market.eventSink)
        {
            market.eventSink->onEvent({plan.eventType, market.symbol->symbolId, participant.position->positionId,
                participant.position->userId, plan.executionPrice, plan.executionQuantity, plan.realizedPnL,
                plan.fundingChange, plan.fee, plan.insurance, timestamp, plan.orderId, plan.counterOrderId});
        }
    }

    void ContractSettleEngine::attach(OrderNode& node) noexcept
    {
        ContractParticipant& participant = *node.contractParticipant;
        linkTradingParticipant(participant);
        node.contractNext = participant.firstOrder;
        if (participant.firstOrder)
        {
            participant.firstOrder->contractPrev = &node;
        }

        participant.firstOrder = &node;
        participant.market->orderParticipants[participant.position->positionId] = &participant;
        participant.account->tradingPositions[participant.position->positionId] = participant.position;
        ++participant.openOrderCount;
        ++participant.account->managedOrderCount;
    }

    void ContractSettleEngine::detach(OrderNode& node) noexcept
    {
        ContractParticipant& participant = *node.contractParticipant;
        if (participant.firstOrder != &node && !node.contractPrev && !node.contractNext)
        {
            return;
        }

        if (node.contractPrev)
        {
            node.contractPrev->contractNext = node.contractNext;
        }
        else
        {
            participant.firstOrder = node.contractNext;
        }

        if (node.contractNext)
        {
            node.contractNext->contractPrev = node.contractPrev;
        }

        node.contractPrev = nullptr;
        node.contractNext = nullptr;
        --participant.openOrderCount;
        --participant.account->managedOrderCount;
        if (participant.openOrderCount == 0)
        {
            participant.market->orderParticipants.erase(participant.position->positionId);
            if (participant.position->positionSize == 0)
            {
                participant.account->tradingPositions.erase(participant.position->positionId);
                unlinkTradingParticipant(participant);
            }
        }
    }

    void ContractSettleEngine::linkTradingParticipant(ContractParticipant& participant) noexcept
    {
        if (participant.crossLinked || participant.position->marginMode != MarginMode::Cross)
        {
            return;
        }

        CrossMarginState& state = *participant.crossMargin;
        participant.crossNext = state.firstParticipant;
        if (state.firstParticipant)
        {
            state.firstParticipant->crossPrevious = &participant;
        }

        state.firstParticipant = &participant;
        participant.crossLinked = true;
    }

    void ContractSettleEngine::unlinkTradingParticipant(ContractParticipant& participant) noexcept
    {
        if (!participant.crossLinked)
        {
            return;
        }

        if (participant.crossPrevious)
        {
            participant.crossPrevious->crossNext = participant.crossNext;
        }
        else
        {
            participant.crossMargin->firstParticipant = participant.crossNext;
        }

        if (participant.crossNext)
        {
            participant.crossNext->crossPrevious = participant.crossPrevious;
        }

        participant.crossPrevious = nullptr;
        participant.crossNext = nullptr;
        participant.crossLinked = false;
    }

    bool ContractSettleEngine::canFillAll(OrderNode& taker, const OrderBook& book)
    {
        // FOK simulates only visited counterparties. All copies are private to
        // this command; the real book, orders, positions and balances remain
        // untouched if any fill is ineligible. Ordinary GTC/IOC never allocate
        // these shadow accounts or orders.
        const ContractMarket& source = *taker.contractMarket;
        if (!this->simulation)
        {
            this->simulation = std::make_unique<SimulationWorkspace>();
        }

        auto& workspace = *this->simulation;
        workspace.reset();
        ContractMarket& shadow = workspace.market;
        shadow.symbol = source.symbol;
        shadow.spec = source.spec;
        shadow.markPrice = source.markPrice;
        shadow.commandTimestamp = source.commandTimestamp;
        shadow.insuranceBalance = source.insuranceBalance;
        shadow.clearingBalance = source.clearingBalance;
        shadow.feeBalance = source.feeBalance;
        auto& accounts = workspace.accounts;
        ContractSettleEngine policy(this->accountLedger, this->matching);

        const auto copyOrder = [&](const OrderNode& original) -> OrderNode&
        {
            ContractParticipant& originalParticipant = *original.contractParticipant;
            ContractAccount* originalAccount = originalParticipant.account;
            auto [accountIterator, newAccount] = accounts.try_emplace(originalAccount);
            if (newAccount)
            {
                auto* account = workspace.accountPool.emplace(originalAccount->getAccountId(), originalAccount->getUserId(), &workspace.positions);
                static_cast<void>(account->applyBalanceChange(source.symbol->quoteAssetId,
                    originalAccount->queryActiveBalance(source.symbol->quoteAssetId).value_or(0),
                    originalAccount->queryReservedBalance(source.symbol->quoteAssetId).value_or(0)));
                account->crossMargin(source.symbol->quoteAssetId) = *originalParticipant.crossMargin;
                auto& shadowCross = account->crossMargin(source.symbol->quoteAssetId);
                shadowCross.firstParticipant = nullptr;
                shadowCross.nextLiquidationVisited = nullptr;
                shadowCross.liquidationVisited = false;
                account->managedPositionCount = originalAccount->managedPositionCount;
                account->managedOrderCount = originalAccount->managedOrderCount;
                accountIterator->second = account;
            }

            ContractAccount& account = *accountIterator->second;
            const PositionId id = originalParticipant.position->positionId;
            const bool newParticipant = !shadow.participants.contains(id);
            ContractParticipant& participant = shadow.createParticipant(id);
            if (newParticipant)
            {
                static_cast<void>(account.recordPosition(*originalParticipant.position));
                participant.market = &shadow;
                participant.account = &account;
                participant.position = account.findPosition(id);
                participant.crossMargin = &account.crossMargin(source.symbol->quoteAssetId);
            }

            auto* node = workspace.orderPool.emplace(original.basicOrder, *original.user, account,
                *original.symbol, original.baseAssetId, original.quoteAssetId);
            node->contractMarket = &shadow;
            node->contractParticipant = &participant;
            ++participant.orderReferences;
            node->reservationAssetId = original.reservationAssetId;
            node->reservedBalance = original.reservedBalance;
            node->leverage = original.leverage;
            node->reduceOnly = original.reduceOnly;
            node->liquidationOrder = original.liquidationOrder;
            node->executionPriceLimit = original.executionPriceLimit;
            attach(*node);
            return *node;
        };

        OrderNode& shadowTaker = copyOrder(taker);
        bool eligible = true;
        const OrderSide side = taker.basicOrder.orderSide == OrderSide::Bid ? OrderSide::Ask : OrderSide::Bid;
        book.forEachOrderWhile(side, [&](const BasicOrder& makerOrder)
        {
            if (shadowTaker.basicOrder.getUnfilledQuantity() == 0)
            {
                return false;
            }

            const Price price = makerOrder.getPrice();
            if (taker.basicOrder.orderType == OrderType::Limit &&
                (side == OrderSide::Ask ? price > taker.basicOrder.getPrice() : price < taker.basicOrder.getPrice()))
            {
                return false;
            }

            const OrderNode* original = this->matching.findNode(makerOrder.orderId);
            assert(original);
            OrderNode& shadowMaker = copyOrder(*original);
            while (shadowTaker.basicOrder.getUnfilledQuantity() != 0 && shadowMaker.basicOrder.getUnfilledQuantity() != 0)
            {
                Quantity quantity = std::min(shadowTaker.basicOrder.getUnfilledQuantity(), shadowMaker.basicOrder.getUnfilledQuantity());
                const MatchAction action = policy.prepare(shadowMaker, shadowTaker, price, quantity);
                if (action == MatchAction::CancelMaker)
                {
                    static_cast<void>(shadowMaker.basicOrder.cancel(source.commandTimestamp));
                    policy.cancelled(shadowMaker);
                    break;
                }

                if (action == MatchAction::CancelTaker)
                {
                    eligible = false;
                    return false;
                }

                static_cast<void>(shadowMaker.basicOrder.fill(quantity, source.commandTimestamp));
                static_cast<void>(shadowTaker.basicOrder.fill(quantity, source.commandTimestamp));
                policy.commit({makerOrder.orderId, taker.basicOrder.orderId, price, quantity}, shadowMaker, shadowTaker, source.commandTimestamp);
            }

            return true;
        });

        return eligible && shadowTaker.basicOrder.getUnfilledQuantity() == 0;
    }

    MatchAction ContractSettleEngine::prepare(OrderNode& maker, OrderNode& taker, Price price, Quantity& quantity)
    {
        ContractMarket& market = *taker.contractMarket;
        if (maker.account == taker.account)
        {
            return MatchAction::CancelTaker;
        }

        if (!ContractRiskEngine::executable(maker, price))
        {
            return MatchAction::CancelMaker;
        }

        if (!ContractRiskEngine::executable(taker, price))
        {
            return MatchAction::CancelTaker;
        }

        for (OrderNode* node : {&maker, &taker})
        {
            if (node->reduceOnly)
            {
                quantity = std::min(quantity, node->contractParticipant->position->positionSize);
                if (quantity == 0)
                {
                    return node == &maker ? MatchAction::CancelMaker : MatchAction::CancelTaker;
                }
            }
        }

        const auto value = ContractMath::notional(price, quantity, market.spec.contractMultiplier);
        if (!value)
        {
            return MatchAction::CancelTaker;
        }

        auto prepareNode = [&](OrderNode& node, PositionPlan& result, Account::BalanceUpdate& balanceUpdate, MMRRate feeRate)
        {
            const Quantity remaining = node.basicOrder.getUnfilledQuantity() - quantity;
            const auto reservation = node.liquidationOrder || remaining == 0 ? std::optional<Balance>(0)
                : ContractRiskEngine::reservation(node, node.basicOrder.getPrice(), remaining);
            if (!reservation)
            {
                return ContractState::Overflow;
            }

            return plan(market, *node.contractParticipant, node.basicOrder.orderSide, price, quantity,
                ContractMath::rate(*value, feeRate, true), *reservation, node.reservedBalance, node.liquidationOrder, result, false, value, &balanceUpdate);
        };

        if (prepareNode(maker, this->makerPlan, this->makerBalance, market.spec.makerFeeRate) != ContractState::Success)
        {
            return MatchAction::CancelMaker;
        }

        const MMRRate takerFee = taker.liquidationOrder ? market.spec.liquidationFeeRate : market.spec.takerFeeRate;
        if (prepareNode(taker, this->takerPlan, this->takerBalance, takerFee) != ContractState::Success)
        {
            return MatchAction::CancelTaker;
        }

        if (!ContractMath::balance(static_cast<WideBalance>(market.clearingBalance) - this->makerPlan.realizedPnL - this->takerPlan.realizedPnL) ||
            !ContractMath::balance(static_cast<WideBalance>(market.feeBalance) + this->makerPlan.fee + this->takerPlan.fee) ||
            static_cast<WideBalance>(this->makerPlan.insurance) + this->takerPlan.insurance > market.insuranceBalance)
        {
            return MatchAction::CancelTaker;
        }

        this->makerPlan.orderId = maker.basicOrder.orderId;
        this->makerPlan.counterOrderId = taker.basicOrder.orderId;
        this->takerPlan.orderId = taker.basicOrder.orderId;
        this->takerPlan.counterOrderId = maker.basicOrder.orderId;
        return MatchAction::Execute;
    }

    void ContractSettleEngine::commit(const Trade&, OrderNode& maker, OrderNode& taker, Timestamp timestamp)
    {
        ContractMarket& market = *taker.contractMarket;
        apply(market, *maker.contractParticipant, this->makerPlan, timestamp, true, true, false, &this->makerBalance);
        apply(market, *taker.contractParticipant, this->takerPlan, timestamp, true, true, false, &this->takerBalance);
        market.clearingBalance = static_cast<BalanceChange>(static_cast<WideBalance>(market.clearingBalance) - this->makerPlan.realizedPnL - this->takerPlan.realizedPnL);
        market.insuranceBalance -= this->makerPlan.insurance + this->takerPlan.insurance;
        market.feeBalance += this->makerPlan.fee + this->takerPlan.fee;
        for (auto [node, reservation] : {std::pair{&maker, this->makerPlan.orderReservation}, std::pair{&taker, this->takerPlan.orderReservation}})
        {
            if (node->contractParticipant->position->marginMode == MarginMode::Cross)
            {
                node->contractParticipant->crossMargin->orderReservation += reservation - node->reservedBalance;
            }

            node->reservedBalance = reservation;
        }

        LiquidateEngine::enqueue(market, *maker.contractParticipant);
        LiquidateEngine::enqueue(market, *taker.contractParticipant);
        publish(market, *maker.contractParticipant, this->makerPlan, timestamp);
        publish(market, *taker.contractParticipant, this->takerPlan, timestamp);
        if (isOrderStatusTerminal(maker.basicOrder.getStatus()))
        {
            detach(maker);
        }
    }

    void ContractSettleEngine::cancelled(OrderNode& node)
    {
        this->releaseReservation(node);
    }

    void ContractSettleEngine::settleTrade(const Trade&, OrderNode&, OrderNode&)
    {
        // Contract fills settle synchronously via MatchPolicy::commit so the
        // next fill sees the updated position and aggregate cross risk.
    }

    void ContractSettleEngine::settleTrades(TradeBuffer& trades)
    {
        clearNodeReferences(trades);
    }

    void ContractSettleEngine::reconcileReservation(OrderNode& node)
    {
        if (isOrderStatusTerminal(node.basicOrder.getStatus()))
        {
            this->releaseReservation(node);
            return;
        }

        const auto required = ContractRiskEngine::reservation(node, node.basicOrder.getPrice(), node.basicOrder.getUnfilledQuantity());
        if (required && *required < node.reservedBalance)
        {
            this->accountLedger.releaseTo(node, *required);
        }
    }

    void ContractSettleEngine::releaseReservation(OrderNode& node)
    {
        this->accountLedger.releaseAll(node);
        detach(node);
    }
}
