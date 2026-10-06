#ifndef ADVMATCH_SPOT_ACCOUNT_H
#define ADVMATCH_SPOT_ACCOUNT_H

#include "../Account.h"

namespace advmatch
{
    class SpotAccount final : public Account
    {
    public:
        SpotAccount(AccountId accountId, UserId userId)
            : Account(accountId, userId)
        {
        }

        [[nodiscard]]
        AccountType getAccountType() const noexcept override
        {
            return AccountType::Spot;
        }
    };
}

#endif // ADVMATCH_SPOT_ACCOUNT_H
