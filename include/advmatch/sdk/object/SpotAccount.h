#ifndef ADVMATCH_SDK_SPOT_ACCOUNT_H
#define ADVMATCH_SDK_SPOT_ACCOUNT_H

#include "Account.h"

namespace advmatch::sdk
{
    class SpotAccount final : public Account
    {
        std::optional<AccountType> expectedType() const noexcept override
        {
            return AccountType::Spot;
        }

    public:
        using Account::Account;
    };
}

#endif // ADVMATCH_SDK_SPOT_ACCOUNT_H
