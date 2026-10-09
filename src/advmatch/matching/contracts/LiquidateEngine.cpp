#include "LiquidateEngine.h"
#include "ContractEngine.h"
#include "advmatch/matching/MatchingEngine.h"
#include "advmatch/util/ContractMath.h"

namespace advmatch
{
    void LiquidateEngine::enqueue(ContractMarket& market, ContractParticipant& participant)
    {
        if (participant.position->positionSize != 0 && (participant.position->liquidating || required(market, participant)))
        {
            market.pendingLiquidations.enqueue(participant, participant.liquidationLink);
        }
        else
        {
            market.pendingLiquidations.remove(participant.liquidationLink);
        }
    }

    bool LiquidateEngine::required(const ContractMarket& market, const ContractParticipant& participant) noexcept
    {
        const Position& position = *participant.position;
        if (position.positionSize == 0)
        {
            return false;
        }

        if (position.marginMode == MarginMode::Cross)
        {
            const CrossMarginState& state = *participant.crossMargin;
            const WideBalance equity = static_cast<WideBalance>(participant.account->queryActiveBalance(market.symbol->quoteAssetId).value_or(0)) + state.unrealizedPnL + state.orderReservation;
            return equity <= static_cast<WideBalance>(state.maintenanceMargin) + state.liquidationFeeReserve;
        }

        return static_cast<WideBalance>(position.initialMargin) + position.updatedUnrealizedPnl <=
            static_cast<WideBalance>(position.maintenanceMargin) + position.estimatedCloseFee;
    }

    ContractResult LiquidateEngine::execute(ContractEngine& contracts, MatchingEngine& matching,
        ContractMarket& market, ContractParticipant& participant, Timestamp timestamp, TradeBuffer& trades) const
    {
        trades.clear();
        Position& position = *participant.position;
        if (matching.getTradingControlEngine().halted(*market.symbol))
        {
            return {ContractState::TradingHalted};
        }

        if (timestamp < market.commandTimestamp)
        {
            return {ContractState::InvalidTimestamp};
        }

        if (market.status != ContractStatus::Trading || (market.spec.expiryTimestamp != 0 && timestamp >= market.spec.expiryTimestamp))
        {
            return {ContractState::Expired};
        }

        if (position.positionSize == 0 || (!position.liquidating && !required(market, participant)))
        {
            return {ContractState::LiquidationNotRequired};
        }

        auto& targets = this->targetsWorkspace;
        targets.clear();
        if (position.marginMode == MarginMode::Cross)
        {
            participant.crossMargin->liquidating = true;
            for (ContractParticipant* target = participant.crossMargin->firstParticipant; target; target = target->crossNext)
            {
                targets.push_back(target);
            }
        }
        else
        {
            targets.push_back(&participant);
        }

        // All orders are removed first so their reservations become available
        // before a cross liquidation consumes collateral. Intrusive lists make
        // each cancellation fixed pointer work plus the selected price index.
        for (ContractParticipant* target : targets)
        {
            target->position->liquidating = target->position->positionSize != 0;
            if (matching.getTradingControlEngine().halted(*target->market->symbol))
            {
                // Keep cross-liquidation membership while execution is paused.
                // Closing another symbol can restore equity above maintenance;
                // the remaining liquidation must still continue after resume.
                enqueue(*target->market, *target);
                continue;
            }

            contracts.cancelOrders(*target->market, *target, timestamp);
            enqueue(*target->market, *target);
        }

        ContractResult result;
        for (ContractParticipant* target : targets)
        {
            Position& candidate = *target->position;
            if (matching.getTradingControlEngine().halted(*target->market->symbol))
            {
                result.state = ContractState::PendingLiquidation;
                continue;
            }

            if (candidate.positionSize == 0)
            {
                continue;
            }

            ContractMarket& targetMarket = *target->market;
            const bool buy = candidate.positionSide == PositionSide::Short;
            OrderBook* book = matching.findBook(candidate.symbolId);
            // An empty opposing side cannot produce a fill. Keep the risk
            // state queued without allocating a doomed IOC OrderNode/history
            // record for every position on every liquidity-starved pass.
            if (!book || !(buy ? book->bestAsk() : book->bestBid()))
            {
                result.state = ContractState::PendingLiquidation;
                continue;
            }

            const Price limit = targetMarket.liquidationPriceLimit(buy);
            if (limit == 0 || limit > static_cast<Price>(std::numeric_limits<Balance>::max()))
            {
                result.state = ContractState::PendingLiquidation;
                continue;
            }

            ContractOrderOptions options;
            options.positionId = candidate.positionId;
            options.marginMode = candidate.marginMode;
            options.reduceOnly = true;
            options.priceLimit = limit;
            // Managers retain their nodes, and archive cannot run during this
            // synchronous call. No borrowed context survives the submission.
            MatchingEngine::OrderSubmissionContext context{
                matching.userManager.getUser(candidate.userId), targetMarket.symbol, book, target};
            const auto order = matching.submitOrder(candidate.userId, candidate.symbolId, buy ? OrderSide::Bid : OrderSide::Ask,
                OrderType::Market, TimeInForce::IOC, 0, candidate.positionSize, timestamp, trades, &options, true, &context);
            static_cast<void>(order);
            ++result.affectedPositions;
            enqueue(targetMarket, *target);
            if (candidate.positionSize != 0)
            {
                result.state = ContractState::PendingLiquidation;
            }
        }

        if (position.marginMode == MarginMode::Cross && participant.crossMargin->maintenanceMargin == 0)
        {
            participant.crossMargin->liquidating = false;
        }

        return result;
    }
}
