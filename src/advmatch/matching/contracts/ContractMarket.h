#ifndef ADVMATCH_CONTRACT_MARKET_H
#define ADVMATCH_CONTRACT_MARKET_H

#include "advmatch/ledging/account/impl/ContractAccount.h"
#include "advmatch/matching/Matching.h"
#include "advmatch/trading/futures/Contract.h"

namespace advmatch
{
    struct ContractParticipant
    {
        ContractMarket* market = nullptr;
        ContractAccount* account = nullptr;
        Position* position = nullptr;
        CrossMarginState* crossMargin = nullptr;
        OrderNode* firstOrder = nullptr;
        std::size_t openOrderCount = 0;
    };

    struct ContractMarket
    {
        const Symbol* symbol = nullptr;
        ContractEventSink* eventSink = nullptr;
        ContractSpec spec;
        ContractStatus status = ContractStatus::Trading;
        Price markPrice = 0;
        Timestamp markTimestamp = 0;
        Timestamp commandTimestamp = 0;
        Timestamp nextFundingTimestamp = 0;
        Timestamp lastFundingTimestamp = 0;
        Price deliveryPrice = 0;
        Balance insuranceBalance = 0;
        // Signed clearing cash: offsets all realized PnL. Funding rounding
        // residues and fees have separate accounts, so cash is auditable.
        BalanceChange clearingBalance = 0;
        Balance feeBalance = 0;
        Balance fundingRoundingBalance = 0;
        NodeHashMap<PositionId, ContractParticipant> participants;
        DenseMap<PositionId, ContractParticipant*> activePositions;
        DenseMap<PositionId, ContractParticipant*> orderParticipants;
        DenseMap<PositionId, ContractParticipant*> pendingLiquidations;
    };

    struct PositionPlan
    {
        ContractEventType eventType = ContractEventType::Fill;
        Price executionPrice = 0;
        Quantity executionQuantity = 0;
        OrderId orderId = 0;
        OrderId counterOrderId = 0;
        Quantity size = 0;
        Balance entryNotional = 0;
        Balance margin = 0;
        Balance maintenanceMargin = 0;
        Balance estimatedCloseFee = 0;
        BalanceChange unrealizedPnL = 0;
        BalanceChange realizedPnL = 0;
        Balance fee = 0;
        BalanceChange fundingChange = 0;
        Balance insurance = 0;
        Balance orderReservation = 0;
        BalanceChange activeChange = 0;
        BalanceChange reservedChange = 0;
    };
}

#endif // ADVMATCH_CONTRACT_MARKET_H
