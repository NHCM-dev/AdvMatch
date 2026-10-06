#include "SpotAccountLedger.h"

#include "advmatch/ledging/User.h"

namespace advmatch
{
    Account& SpotAccountLedger::accountFor(User& user) const noexcept
    {
        return user.getSpotAccount();
    }

    const Account& SpotAccountLedger::accountFor(const User& user) const noexcept
    {
        return user.getSpotAccount();
    }

    AccountType SpotAccountLedger::getAccountType() const noexcept
    {
        return AccountType::Spot;
    }
}
