#include "ContractAccountLedger.h"
#include "advmatch/matching/contracts/ContractMarket.h"

#include "advmatch/ledging/User.h"

namespace advmatch
{
    void ContractAccountLedger::reservationChanged(OrderNode& node, Balance previousBalance) const noexcept
    {
        if (node.contractParticipant && node.contractParticipant->position->marginMode == MarginMode::Cross)
        {
            node.contractParticipant->crossMargin->orderReservation += node.reservedBalance - previousBalance;
        }
    }

    Account& ContractAccountLedger::accountFor(User& user) const noexcept
    {
        return user.getContractAccount();
    }

    const Account& ContractAccountLedger::accountFor(const User& user) const noexcept
    {
        return user.getContractAccount();
    }

    AccountType ContractAccountLedger::getAccountType() const noexcept
    {
        return AccountType::Contract;
    }
}
