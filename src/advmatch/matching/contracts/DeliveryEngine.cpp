#include "DeliveryEngine.h"
#include "ContractSettlementBatch.h"

namespace advmatch
{
    ContractResult DeliveryEngine::settle(ContractMarket& market, Price price, Timestamp timestamp) const
    {
        if (market.symbol->symbolType != SymbolType::Future)
        {
            return {ContractState::InvalidSymbol};
        }

        if (market.status == ContractStatus::Settled)
        {
            return {ContractState::AlreadySettled};
        }

        if (timestamp < market.spec.expiryTimestamp || market.status != ContractStatus::Expired)
        {
            return {ContractState::NotDue};
        }

        if (timestamp < market.commandTimestamp || price == 0)
        {
            return {price == 0 ? ContractState::InvalidPrice : ContractState::InvalidTimestamp};
        }

        auto& batch = this->workspace;
        batch.reset(market.activePositions.size());
        for (const auto& [id, participantPointer] : market.activePositions)
        {
            ContractParticipant& participant = *participantPointer;
            Position& position = *participant.position;
            if (position.positionSize == 0)
            {
                continue;
            }

            PositionPlan plan;
            const OrderSide side = position.positionSide == PositionSide::Long ? OrderSide::Ask : OrderSide::Bid;
            const ContractState state = ContractSettleEngine::plan(market, participant, side, price,
                position.positionSize, 0, 0, 0, true, plan, true);
            if (state != ContractState::Success)
            {
                return {state};
            }

            plan.eventType = ContractEventType::Delivery;
            batch.add(participant, plan);
        }

        const ContractState state = batch.validate(market);
        if (state != ContractState::Success)
        {
            return {state};
        }

        market.deliveryPrice = price;
        market.status = ContractStatus::Settled;
        market.commandTimestamp = timestamp;
        batch.commit(market, timestamp);
        for (const auto& [id, participantPointer] : market.activePositions)
        {
            ContractParticipant& participant = *participantPointer;
            if (participant.crossMargin->maintenanceMargin == 0)
            {
                participant.crossMargin->liquidating = false;
            }
        }

        return {ContractState::Success, batch.size()};
    }
}
