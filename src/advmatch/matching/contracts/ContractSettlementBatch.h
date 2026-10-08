#ifndef ADVMATCH_CONTRACT_SETTLEMENT_BATCH_H
#define ADVMATCH_CONTRACT_SETTLEMENT_BATCH_H

#include "advmatch/matching/settleengines/impl/ContractSettleEngine.h"
#include "advmatch/util/ContractMath.h"
#include "LiquidateEngine.h"

#include <cassert>

namespace advmatch
{
    // Funding and delivery share this two-phase ledger transaction. Balance
    // changes for multiple positions belonging to one account are netted first.
    class ContractSettlementBatch
    {
        struct Entry
        {
            ContractParticipant* participant;
            PositionPlan plan;
        };

        struct Movement
        {
            WideBalance active = 0;
            WideBalance reserved = 0;
            WideBalance crossInitial = 0;
            WideBalance crossMaintenance = 0;
            WideBalance crossFee = 0;
            WideBalance crossPnl = 0;
            std::size_t firstEntry = 0;
            Account::BalanceUpdate balanceUpdate;
        };

        SmallVector<Entry, 16> entries;
        DenseMap<ContractAccount*, Movement> movements;
        WideBalance insurance = 0;
        WideBalance realized = 0;
        WideBalance fees = 0;
        WideBalance funding = 0;

    public:
        void reset(std::size_t positionCount)
        {
            this->entries.clear();
            this->entries.reserve(positionCount);
            this->movements.clear();
            this->insurance = 0;
            this->realized = 0;
            this->fees = 0;
            this->funding = 0;
        }

        void add(ContractParticipant& participant, const PositionPlan& plan)
        {
            this->entries.push_back({&participant, plan});
            auto [iterator, inserted] = this->movements.try_emplace(participant.account);
            Movement& movement = iterator->second;
            if (inserted)
            {
                movement.firstEntry = this->entries.size() - 1;
            }

            if (participant.position->marginMode == MarginMode::Cross)
            {
                movement.crossInitial += static_cast<WideBalance>(plan.margin) - participant.position->initialMargin;
                movement.crossMaintenance += static_cast<WideBalance>(plan.maintenanceMargin) - participant.position->maintenanceMargin;
                movement.crossFee += static_cast<WideBalance>(plan.estimatedCloseFee) - participant.position->estimatedCloseFee;
                movement.crossPnl += static_cast<WideBalance>(plan.unrealizedPnL) - participant.position->updatedUnrealizedPnl;
            }

            movement.active += plan.activeChange;
            movement.reserved += plan.reservedChange;
            this->insurance += plan.insurance;
            this->realized += plan.realizedPnL;
            this->fees += plan.fee;
            this->funding += plan.fundingChange;
        }

        ContractState balanceFunding()
        {
            WideBalance paid = 0;
            WideBalance received = 0;
            for (const Entry& entry : this->entries)
            {
                if (entry.plan.fundingChange < 0)
                {
                    paid -= entry.plan.fundingChange;
                }
                else
                {
                    received += entry.plan.fundingChange;
                }
            }

            if (received <= paid)
            {
                return ContractState::Success;
            }

            // Receiver entitlements can round to more than the payer total
            // when position sizes are split differently. Distribute the funded
            // total with cumulative integer rounding; no unbacked credit is
            // issued and no receiver gets more than their original entitlement.
            if (!ContractMath::balance(paid) || !ContractMath::balance(received))
            {
                return ContractState::Overflow;
            }

            WideBalance cumulative = 0;
            WideBalance distributed = 0;
            for (Entry& entry : this->entries)
            {
                if (entry.plan.fundingChange <= 0)
                {
                    continue;
                }

                cumulative += entry.plan.fundingChange;
                const WideBalance entitlement = cumulative * paid / received;
                const BalanceChange share = static_cast<BalanceChange>(entitlement - distributed);
                distributed = entitlement;
                const BalanceChange change = share - entry.plan.fundingChange;
                entry.plan.fundingChange = share;
                Movement& movement = this->movements.find(entry.participant->account)->second;
                if (entry.participant->position->marginMode == MarginMode::Isolated)
                {
                    entry.plan.margin += change;
                    entry.plan.reservedChange += change;
                    movement.reserved += change;
                }
                else
                {
                    entry.plan.activeChange += change;
                    movement.active += change;
                }

                this->funding += change;
            }

            return ContractState::Success;
        }

        ContractState validate(const ContractMarket& market, bool coverCrossDeficit = true)
        {
            for (auto& [account, movement] : this->movements)
            {
                const auto balance = account->captureBalance(market.symbol->quoteAssetId);
                const WideBalance active = balance.active();
                if (active + movement.active < 0)
                {
                    if (!coverCrossDeficit)
                    {
                        return ContractState::InsufficientBalance;
                    }

                    const WideBalance deficit = -(active + movement.active);
                    const auto insuranceAmount = ContractMath::balance(deficit);
                    if (!insuranceAmount)
                    {
                        return ContractState::Overflow;
                    }

                    this->insurance += deficit;
                    movement.active += deficit;
                    Entry& entry = this->entries[movement.firstEntry];
                    const auto covered = ContractMath::balance(static_cast<WideBalance>(entry.plan.insurance) + deficit);
                    if (!covered)
                    {
                        return ContractState::Overflow;
                    }

                    entry.plan.insurance = *covered;
                }

                const CrossMarginState& cross = *this->entries[movement.firstEntry].participant->crossMargin;
                if (!ContractMath::balance(static_cast<WideBalance>(cross.initialMargin) + movement.crossInitial) ||
                    !ContractMath::balance(static_cast<WideBalance>(cross.maintenanceMargin) + movement.crossMaintenance) ||
                    !ContractMath::balance(static_cast<WideBalance>(cross.unrealizedPnL) + movement.crossPnl) ||
                    !ContractMath::balance(static_cast<WideBalance>(cross.liquidationFeeReserve) + movement.crossFee))
                {
                    return ContractState::Overflow;
                }

                const auto activeChange = ContractMath::balance(movement.active);
                const auto reservedChange = ContractMath::balance(movement.reserved);
                const auto prepared = activeChange && reservedChange ? account->prepareBalanceChange(balance, *activeChange, *reservedChange) : std::nullopt;
                if (!prepared)
                {
                    return ContractState::Overflow;
                }

                movement.balanceUpdate = *prepared;
            }

            if (this->insurance > market.insuranceBalance)
            {
                return ContractState::InsufficientInsurance;
            }

            if (!ContractMath::balance(static_cast<WideBalance>(market.clearingBalance) - this->realized) ||
                !ContractMath::balance(static_cast<WideBalance>(market.feeBalance) + this->fees) ||
                !ContractMath::balance(static_cast<WideBalance>(market.fundingRoundingBalance) - this->funding))
            {
                return ContractState::Overflow;
            }

            return ContractState::Success;
        }

        void commit(ContractMarket& market, Timestamp timestamp)
        {
            for (auto& [account, movement] : this->movements)
            {
                const auto result = account->commitBalanceChange(movement.balanceUpdate);
                assert(result);
                static_cast<void>(result);
                CrossMarginState& cross = *this->entries[movement.firstEntry].participant->crossMargin;
                cross.initialMargin = static_cast<Balance>(static_cast<WideBalance>(cross.initialMargin) + movement.crossInitial);
                cross.maintenanceMargin = static_cast<Balance>(static_cast<WideBalance>(cross.maintenanceMargin) + movement.crossMaintenance);
                cross.liquidationFeeReserve = static_cast<Balance>(static_cast<WideBalance>(cross.liquidationFeeReserve) + movement.crossFee);
                cross.unrealizedPnL = static_cast<BalanceChange>(static_cast<WideBalance>(cross.unrealizedPnL) + movement.crossPnl);
            }

            market.insuranceBalance -= static_cast<Balance>(this->insurance);
            market.clearingBalance = static_cast<BalanceChange>(static_cast<WideBalance>(market.clearingBalance) - this->realized);
            market.feeBalance += static_cast<Balance>(this->fees);
            for (const Entry& entry : this->entries)
            {
                ContractSettleEngine::apply(market, *entry.participant, entry.plan, timestamp, false, false, false);
            }

            market.fundingRoundingBalance = static_cast<BalanceChange>(static_cast<WideBalance>(market.fundingRoundingBalance) - this->funding);
            for (const Entry& entry : this->entries)
            {
                LiquidateEngine::enqueue(market, *entry.participant);
                ContractSettleEngine::publish(market, *entry.participant, entry.plan, timestamp);
            }
        }

        std::size_t size() const noexcept
        {
            return this->entries.size();
        }
    };
}

#endif // ADVMATCH_CONTRACT_SETTLEMENT_BATCH_H
