#include "ContractEngine.h"
#include "advmatch/matching/MatchingEngine.h"
#include "advmatch/util/ContractMath.h"

namespace advmatch
{
    ContractState ContractEngine::commandState(const ContractMarket& market, Timestamp timestamp) noexcept
    {
        if (timestamp < market.commandTimestamp)
        {
            return ContractState::InvalidTimestamp;
        }

        if (market.status != ContractStatus::Trading || (market.spec.expiryTimestamp != 0 && timestamp >= market.spec.expiryTimestamp))
        {
            return ContractState::Expired;
        }

        if (market.symbol->symbolType == SymbolType::Perpetual && timestamp >= market.nextFundingTimestamp)
        {
            return ContractState::FundingRequired;
        }

        return ContractState::Success;
    }

    ContractState ContractEngine::beginCommand(SymbolId symbolId, Timestamp timestamp)
    {
        ContractMarket* market = this->findMarket(symbolId);
        if (!market)
        {
            return ContractState::InvalidSymbol;
        }

        const ContractState state = commandState(*market, timestamp);
        if (state == ContractState::Expired && market->status == ContractStatus::Trading)
        {
            static_cast<void>(this->expire(symbolId, timestamp));
        }
        else if (state == ContractState::Success)
        {
            market->commandTimestamp = timestamp;
        }

        return state;
    }

    void ContractEngine::setEventSink(ContractEventSink* sink) noexcept
    {
        this->eventSink = sink;
        for (auto& [id, market] : this->markets)
        {
            market.eventSink = sink;
        }
    }

    ContractParticipant* ContractEngine::findParticipant(PositionId positionId) noexcept
    {
        const auto iterator = this->positionIndex.find(positionId);
        return iterator == this->positionIndex.end() ? nullptr : iterator->second;
    }

    const Position* ContractEngine::queryPosition(PositionId positionId) const noexcept
    {
        const auto iterator = this->positionIndex.find(positionId);
        return iterator == this->positionIndex.end() ? nullptr : iterator->second->position;
    }

    ContractMarket* ContractEngine::findMarket(SymbolId symbolId) noexcept
    {
        const auto iterator = this->markets.find(symbolId);
        return iterator == this->markets.end() ? nullptr : &iterator->second;
    }

    const ContractMarket* ContractEngine::findMarket(SymbolId symbolId) const noexcept
    {
        const auto iterator = this->markets.find(symbolId);
        return iterator == this->markets.end() ? nullptr : &iterator->second;
    }

    ContractResult ContractEngine::addMarket(const Symbol& symbol, const ContractSpec& spec, Price markPrice, Timestamp timestamp)
    {
        if (!symbol.isActive() || this->findMarket(symbol.symbolId))
        {
            return {ContractState::InvalidSymbol};
        }

        if (!ContractMath::validSpec(spec, symbol.symbolType) || markPrice == 0 ||
            markPrice > static_cast<Price>(std::numeric_limits<Balance>::max()) ||
            (symbol.symbolType == SymbolType::Future && spec.expiryTimestamp <= timestamp) ||
            (symbol.symbolType == SymbolType::Perpetual && spec.firstFundingTimestamp <= timestamp))
        {
            return {ContractState::InvalidConfiguration};
        }

        auto [iterator, inserted] = this->markets.try_emplace(symbol.symbolId);
        ContractMarket& market = iterator->second;
        market.symbol = &symbol;
        market.eventSink = this->eventSink;
        market.spec = spec;
        market.markPrice = markPrice;
        market.markTimestamp = timestamp;
        market.commandTimestamp = timestamp;
        market.nextFundingTimestamp = spec.firstFundingTimestamp;
        return {};
    }

    void ContractEngine::discardPreparedOrder(const OrderNode& node)
    {
        ContractParticipant* participant = node.contractParticipant;
        if (!participant || participant->position->positionSize != 0 || participant->openOrderCount != 0)
        {
            return;
        }

        const PositionId id = participant->position->positionId;
        participant->position->managed = false;
        static_cast<void>(participant->account->removePosition(id));
        this->positionIndex.erase(id);
        participant->market->participants.erase(id);
    }

    ContractState ContractEngine::prepareOrder(OrderNode& node, const ContractOrderOptions& options, Timestamp timestamp)
    {
        ContractMarket* market = this->findMarket(node.basicOrder.symbolId);
        if (!market)
        {
            return ContractState::InvalidSymbol;
        }

        const ContractState command = this->beginCommand(node.basicOrder.symbolId, timestamp);
        if (command != ContractState::Success)
        {
            return command;
        }

        if (node.basicOrder.orderType == OrderType::Market && options.priceLimit == 0)
        {
            return ContractState::InvalidPrice;
        }

        const LeverageMultiplier leverage = options.leverage == 0 ? market->spec.defaultLeverage : options.leverage;
        if (leverage == 0 || leverage > market->spec.maxLeverage)
        {
            return ContractState::InvalidLeverage;
        }

        auto& account = static_cast<ContractAccount&>(*node.account);
        ContractParticipant* participant = nullptr;
        if (options.positionId != 0)
        {
            participant = this->findParticipant(options.positionId);
            if (!participant || participant->account != &account || participant->market != market ||
                participant->position->closed)
            {
                return ContractState::PositionNotFound;
            }

            if (participant->position->marginMode != options.marginMode)
            {
                return ContractState::InvalidMarginMode;
            }

            if (options.leverage != 0 && participant->position->leverage != options.leverage)
            {
                return ContractState::InvalidLeverage;
            }
        }
        else
        {
            if (options.reduceOnly)
            {
                return ContractState::ReduceOnlyViolation;
            }

            if (this->nextPositionId == 0)
            {
                return ContractState::Overflow;
            }

            const PositionId id = this->nextPositionId++;
            const PositionSide side = node.basicOrder.orderSide == OrderSide::Bid ? PositionSide::Long : PositionSide::Short;
            Position position(id, node.basicOrder.userId, market->symbol->symbolId, side, options.marginMode,
                leverage, market->spec.contractMultiplier, timestamp);
            position.managed = true;
            position.mmr = market->spec.maintenanceMarginRate;
            if (!account.recordPosition(position))
            {
                return ContractState::PositionNotFound;
            }

            auto [iterator, inserted] = market->participants.try_emplace(id);
            participant = &iterator->second;
            participant->market = market;
            participant->account = &account;
            participant->position = account.findPosition(id);
            participant->crossMargin = &account.crossMargin(market->symbol->quoteAssetId);
            this->positionIndex.emplace(id, participant);
        }

        const bool closing = (node.basicOrder.orderSide == OrderSide::Bid) != (participant->position->positionSide == PositionSide::Long);
        if (options.reduceOnly && !closing)
        {
            return ContractState::ReduceOnlyViolation;
        }

        node.contractMarket = market;
        node.contractParticipant = participant;
        node.reservationAssetId = market->symbol->quoteAssetId;
        node.leverage = participant->position->leverage;
        node.reduceOnly = closing;
        node.executionPriceLimit = options.priceLimit;
        return ContractState::Success;
    }

    ContractResult ContractEngine::setLeverage(PositionId positionId, UserId userId, SymbolId symbolId,
        LeverageMultiplier leverage, Timestamp timestamp)
    {
        ContractParticipant* participant = this->findParticipant(positionId);
        if (!participant || participant->position->userId != userId || participant->position->symbolId != symbolId)
        {
            return {ContractState::PositionNotFound};
        }

        ContractMarket& market = *participant->market;
        Position& position = *participant->position;
        if (position.closed)
        {
            return {ContractState::PositionNotFound};
        }

        if (leverage == 0 || leverage > market.spec.maxLeverage)
        {
            return {ContractState::InvalidLeverage};
        }

        const ContractState command = commandState(market, timestamp);
        if (command != ContractState::Success)
        {
            return {command};
        }

        if (participant->openOrderCount != 0 || position.liquidating || participant->crossMargin->liquidating)
        {
            return {ContractState::OpenOrdersExist};
        }

        const Balance required = ContractMath::margin(position.entryNotional, leverage);
        const BalanceChange change = required - position.initialMargin;
        const bool isolated = position.marginMode == MarginMode::Isolated;
        const Balance active = participant->account->queryActiveBalance(market.symbol->quoteAssetId).value_or(0);
        const WideBalance equity = isolated ? static_cast<WideBalance>(required) + position.updatedUnrealizedPnl
            : static_cast<WideBalance>(active) + participant->crossMargin->unrealizedPnL;
        const auto crossRequired = ContractMath::balance(static_cast<WideBalance>(participant->crossMargin->initialMargin) + change);
        if (!crossRequired || (isolated ? equity <= static_cast<WideBalance>(position.maintenanceMargin) + position.estimatedCloseFee : equity < *crossRequired) ||
            (isolated && change > 0 && static_cast<WideBalance>(active) - change + participant->crossMargin->unrealizedPnL < participant->crossMargin->initialMargin) ||
            !participant->account->canApplyBalanceChange(market.symbol->quoteAssetId, isolated ? -change : 0, isolated ? change : 0))
        {
            return {ContractState::InsufficientBalance};
        }

        if (isolated)
        {
            static_cast<void>(participant->account->applyBalanceChange(market.symbol->quoteAssetId, -change, change));
        }
        else
        {
            participant->crossMargin->initialMargin = *crossRequired;
        }

        position.leverage = leverage;
        position.initialMargin = required;
        ContractMath::updatePriceReferences(position, market.spec);
        position.updatedTimestamp = timestamp;
        market.commandTimestamp = timestamp;
        return {};
    }

    ContractResult ContractEngine::addMargin(PositionId positionId, UserId userId, SymbolId symbolId, BalanceChange amount)
    {
        ContractParticipant* participant = this->findParticipant(positionId);
        if (!participant || participant->position->userId != userId || participant->position->symbolId != symbolId || participant->position->closed)
        {
            return {ContractState::PositionNotFound};
        }

        Position& position = *participant->position;
        if (position.marginMode != MarginMode::Isolated)
        {
            return {ContractState::InvalidMarginMode};
        }

        const auto margin = ContractMath::balance(static_cast<WideBalance>(position.initialMargin) + amount);
        const auto activeChange = ContractMath::balance(-static_cast<WideBalance>(amount));
        const AssetId asset = participant->market->symbol->quoteAssetId;
        const WideBalance available = static_cast<WideBalance>(participant->account->queryActiveBalance(asset).value_or(0)) - amount;
        if (!margin || !activeChange || *margin < ContractMath::margin(position.entryNotional, position.leverage) ||
            static_cast<WideBalance>(*margin) + position.updatedUnrealizedPnl <= static_cast<WideBalance>(position.maintenanceMargin) + position.estimatedCloseFee ||
            available + participant->crossMargin->unrealizedPnL < participant->crossMargin->initialMargin ||
            !participant->account->canApplyBalanceChange(asset, *activeChange, amount))
        {
            return {ContractState::InsufficientBalance};
        }

        static_cast<void>(participant->account->applyBalanceChange(asset, *activeChange, amount));
        position.initialMargin = *margin;
        ContractMath::updatePriceReferences(position, participant->market->spec);
        return {};
    }

    ContractResult ContractEngine::updateMarkPrice(SymbolId symbolId, Price price, Timestamp timestamp)
    {
        ContractMarket* market = this->findMarket(symbolId);
        if (!market || market->status != ContractStatus::Trading)
        {
            return {ContractState::InvalidSymbol};
        }

        const ContractState command = commandState(*market, timestamp);
        if (command != ContractState::Success)
        {
            if (command == ContractState::Expired && market->status == ContractStatus::Trading)
            {
                static_cast<void>(this->expire(symbolId, timestamp));
            }

            return {command};
        }

        if (price == 0 || price > static_cast<Price>(std::numeric_limits<Balance>::max()))
        {
            return {ContractState::InvalidPrice};
        }

        struct Update
        {
            ContractParticipant* participant;
            BalanceChange pnl;
            Balance maintenance;
            Balance closeFee;
        };

        struct CrossUpdate
        {
            WideBalance pnl = 0;
            WideBalance maintenance = 0;
            WideBalance closeFee = 0;
        };

        SmallVector<Update, 16> updates;
        DenseMap<CrossMarginState*, CrossUpdate> crossUpdates;
        for (const auto& [id, participantPointer] : market->activePositions)
        {
            ContractParticipant& participant = *participantPointer;
            Position& position = *participant.position;
            const auto value = ContractMath::notional(price, position.positionSize, market->spec.contractMultiplier);
            const auto pnl = position.getUnrealizedPnL(price);
            if (!value || !pnl)
            {
                return {ContractState::Overflow};
            }

            const Balance maintenance = ContractMath::rate(*value, market->spec.maintenanceMarginRate, true);
            const Balance closeFee = ContractMath::rate(*value, market->spec.liquidationFeeRate, true);
            updates.push_back({&participant, *pnl, maintenance, closeFee});
            if (position.marginMode == MarginMode::Cross)
            {
                CrossUpdate& update = crossUpdates[participant.crossMargin];
                update.pnl += static_cast<WideBalance>(*pnl) - position.updatedUnrealizedPnl;
                update.closeFee += static_cast<WideBalance>(closeFee) - position.estimatedCloseFee;
                update.maintenance += static_cast<WideBalance>(maintenance) - position.maintenanceMargin;
            }
        }

        for (const auto& [state, update] : crossUpdates)
        {
            if (!ContractMath::balance(static_cast<WideBalance>(state->unrealizedPnL) + update.pnl) ||
                !ContractMath::balance(static_cast<WideBalance>(state->maintenanceMargin) + update.maintenance) ||
                !ContractMath::balance(static_cast<WideBalance>(state->liquidationFeeReserve) + update.closeFee))
            {
                return {ContractState::Overflow};
            }
        }

        for (const Update& update : updates)
        {
            update.participant->position->updatedUnrealizedPnl = update.pnl;
            update.participant->position->maintenanceMargin = update.maintenance;
            update.participant->position->estimatedCloseFee = update.closeFee;
            update.participant->position->updatedTimestamp = timestamp;
        }

        for (const auto& [state, update] : crossUpdates)
        {
            state->unrealizedPnL = static_cast<BalanceChange>(static_cast<WideBalance>(state->unrealizedPnL) + update.pnl);
            state->liquidationFeeReserve = static_cast<Balance>(static_cast<WideBalance>(state->liquidationFeeReserve) + update.closeFee);
            state->maintenanceMargin = static_cast<Balance>(static_cast<WideBalance>(state->maintenanceMargin) + update.maintenance);
        }

        market->markPrice = price;
        market->markTimestamp = timestamp;
        market->commandTimestamp = timestamp;
        for (const Update& update : updates)
        {
            LiquidateEngine::enqueue(*market, *update.participant);
        }

        return this->processLiquidations(symbolId, timestamp);
    }

    ContractResult ContractEngine::fundInsurance(SymbolId symbolId, Account& source, PositiveBalanceChange amount)
    {
        ContractMarket* market = this->findMarket(symbolId);
        const auto total = market ? ContractMath::balance(static_cast<WideBalance>(market->insuranceBalance) + amount) : std::nullopt;
        if (!market || !source.isActive())
        {
            return {ContractState::InvalidSymbol};
        }

        if (!total || amount > static_cast<PositiveBalanceChange>(std::numeric_limits<BalanceChange>::max()))
        {
            return {ContractState::Overflow};
        }

        if (!source.updateBalance(market->symbol->quoteAssetId, -static_cast<BalanceChange>(amount)))
        {
            return {ContractState::InsufficientBalance};
        }

        market->insuranceBalance = *total;
        return {};
    }

    ContractResult ContractEngine::settleFunding(SymbolId symbolId, std::int32_t rate, Timestamp timestamp)
    {
        ContractMarket* market = this->findMarket(symbolId);
        if (!market)
        {
            return {ContractState::InvalidSymbol};
        }

        ContractResult result = this->funding.settle(*market, rate, timestamp);
        if (result.state == ContractState::InsufficientBalance)
        {
            // Release cross order holds before retrying a cash debit. Funding
            // never borrows isolated margin or subsidizes an unfunded cross
            // cash payment with the insurance fund.
            DenseSet<ContractAccount*> visited;
            for (const auto& [id, participantPointer] : market->activePositions)
            {
                ContractParticipant& participant = *participantPointer;
                if (!visited.insert(participant.account).second)
                {
                    continue;
                }

                SmallVector<ContractParticipant*, 8> targets;
                participant.account->forEachTradingPosition([&](PositionId positionId, const Position& position)
                {
                    ContractParticipant* target = this->findParticipant(positionId);
                    if (target && position.marginMode == MarginMode::Cross && target->market->symbol->quoteAssetId == market->symbol->quoteAssetId)
                    {
                        targets.push_back(target);
                    }
                });
                for (ContractParticipant* target : targets)
                {
                    this->cancelOrders(*target->market, *target, timestamp);
                }
            }

            result = this->funding.settle(*market, rate, timestamp);
        }

        if (result)
        {
            for (const auto& [id, participant] : market->activePositions)
            {
                LiquidateEngine::enqueue(*market, *participant);
            }

            static_cast<void>(this->processLiquidations(symbolId, timestamp));
        }

        return result;
    }

    void ContractEngine::cancelOrders(ContractMarket&, ContractParticipant& participant, Timestamp timestamp)
    {
        while (participant.firstOrder)
        {
            OrderNode& node = *participant.firstOrder;
            static_cast<void>(this->matching.cancelOrder(node.basicOrder.userId, node.basicOrder.orderId, timestamp));
        }
    }

    ContractResult ContractEngine::expire(SymbolId symbolId, Timestamp timestamp)
    {
        ContractMarket* market = this->findMarket(symbolId);
        if (!market || market->symbol->symbolType != SymbolType::Future)
        {
            return {ContractState::InvalidSymbol};
        }

        if (market->status == ContractStatus::Settled)
        {
            return {ContractState::AlreadySettled};
        }

        if (timestamp < market->spec.expiryTimestamp || timestamp < market->commandTimestamp)
        {
            return {ContractState::NotDue};
        }

        market->status = ContractStatus::Expired;
        market->commandTimestamp = timestamp;
        while (!market->orderParticipants.empty())
        {
            ContractParticipant& participant = *market->orderParticipants.begin()->second;
            this->cancelOrders(*market, participant, timestamp);
        }

        return {};
    }

    ContractResult ContractEngine::deliver(SymbolId symbolId, Price price, Timestamp timestamp)
    {
        const ContractResult result = this->expire(symbolId, timestamp);
        if (!result)
        {
            return result;
        }

        return this->delivery.settle(*this->findMarket(symbolId), price, timestamp);
    }

    ContractResult ContractEngine::liquidate(SymbolId symbolId, PositionId positionId, Timestamp timestamp, TradeBuffer& trades)
    {
        ContractParticipant* participant = this->findParticipant(positionId);
        if (!participant || participant->position->symbolId != symbolId)
        {
            trades.clear();
            return {ContractState::PositionNotFound};
        }

        return this->liquidation.execute(*this, this->matching, *participant->market, *participant, timestamp, trades);
    }

    ContractResult ContractEngine::processLiquidations(SymbolId symbolId, Timestamp timestamp)
    {
        ContractMarket* market = this->findMarket(symbolId);
        if (!market)
        {
            return {ContractState::InvalidSymbol};
        }

        if (this->processingLiquidations)
        {
            return {};
        }

        // Restores the guard even if a resource allocation fails.
        struct ProcessingGuard
        {
            bool& flag;
            explicit ProcessingGuard(bool& flag) : flag(flag)
            {
                this->flag = true;
            }
            ~ProcessingGuard()
            {
                this->flag = false;
            }
        } guard(this->processingLiquidations);

        TradeBuffer trades;
        ContractResult result;
        SmallVector<ContractParticipant*, 16> pending;
        for (const auto& [id, participant] : market->pendingLiquidations)
        {
            pending.push_back(participant);
        }

        for (ContractParticipant* participant : pending)
        {
            if (participant->position->positionSize != 0 && (participant->position->liquidating || LiquidateEngine::required(*market, *participant)))
            {
                const ContractResult attempt = this->liquidation.execute(*this, this->matching, *market, *participant, timestamp, trades);
                result.affectedPositions += attempt.affectedPositions;
                if (attempt.state != ContractState::Success)
                {
                    result.state = attempt.state;
                }
            }

            LiquidateEngine::enqueue(*market, *participant);
        }

        return result;
    }
}
