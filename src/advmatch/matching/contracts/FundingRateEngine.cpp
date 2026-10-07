#include "FundingRateEngine.h"
#include "ContractSettlementBatch.h"

namespace advmatch
{
    ContractResult FundingRateEngine::settle(ContractMarket& market, std::int32_t rate, Timestamp timestamp) const
    {
        if (market.symbol->symbolType != SymbolType::Perpetual || market.status != ContractStatus::Trading)
        {
            return {ContractState::InvalidSymbol};
        }

        if (timestamp != market.nextFundingTimestamp || timestamp < market.commandTimestamp || timestamp < market.markTimestamp)
        {
            return {timestamp <= market.lastFundingTimestamp ? ContractState::AlreadySettled : ContractState::NotDue};
        }

        if (static_cast<std::int64_t>(rate) > market.spec.maxFundingRate || -static_cast<std::int64_t>(rate) > market.spec.maxFundingRate ||
            market.spec.fundingInterval > std::numeric_limits<Timestamp>::max() - timestamp)
        {
            return {ContractState::InvalidConfiguration};
        }

        ContractSettlementBatch batch;
        for (const auto& [id, participantPointer] : market.activePositions)
        {
            ContractParticipant& participant = *participantPointer;
            const Position& position = *participant.position;
            if (position.positionSize == 0)
            {
                continue;
            }

            const auto notional = ContractMath::notional(market.markPrice, position.positionSize, market.spec.contractMultiplier);
            if (!notional)
            {
                return {ContractState::Overflow};
            }

            const BalanceChange longPayment = static_cast<BalanceChange>(static_cast<WideBalance>(*notional) * rate / RATE_SCALE);
            const BalanceChange payment = position.positionSide == PositionSide::Long ? longPayment : -longPayment;
            PositionPlan plan;
            plan.eventType = ContractEventType::Funding;
            plan.executionPrice = market.markPrice;
            plan.executionQuantity = position.positionSize;
            plan.size = position.positionSize;
            plan.entryNotional = position.entryNotional;
            plan.margin = position.initialMargin;
            plan.maintenanceMargin = position.maintenanceMargin;
            plan.estimatedCloseFee = position.estimatedCloseFee;
            plan.unrealizedPnL = position.updatedUnrealizedPnl;
            plan.fundingChange = -payment;
            if (!ContractMath::balance(static_cast<WideBalance>(position.fundingPnL) - payment))
            {
                return {ContractState::Overflow};
            }

            if (position.marginMode == MarginMode::Isolated)
            {
                const auto margin = ContractMath::balance(static_cast<WideBalance>(position.initialMargin) - payment);
                if (!margin)
                {
                    return {ContractState::Overflow};
                }

                plan.margin = std::max(Balance(0), *margin);
                plan.insurance = *margin < 0 ? -*margin : 0;
                plan.reservedChange = plan.margin - position.initialMargin;
            }
            else
            {
                plan.activeChange = -payment;
            }

            batch.add(participant, plan);
        }

        const ContractState rounding = batch.balanceFunding();
        if (rounding != ContractState::Success)
        {
            return {rounding};
        }

        const ContractState state = batch.validate(market, false);
        if (state != ContractState::Success)
        {
            return {state};
        }

        market.lastFundingTimestamp = timestamp;
        market.nextFundingTimestamp = timestamp + market.spec.fundingInterval;
        market.commandTimestamp = timestamp;
        batch.commit(market, timestamp);
        return {ContractState::Success, batch.size()};
    }
}
