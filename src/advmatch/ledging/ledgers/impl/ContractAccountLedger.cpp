#include "ContractAccountLedger.h"

#include "advmatch/ledging/User.h"

namespace advmatch
{
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
