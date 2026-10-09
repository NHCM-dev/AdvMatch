#ifndef ADVMATCH_SDK_CONTRACT_ACCOUNT_H
#define ADVMATCH_SDK_CONTRACT_ACCOUNT_H

#include "Account.h"

namespace advmatch::sdk
{
    class ContractAccount final : public Account
    {
        std::optional<AccountType> expectedType() const noexcept override
        {
            return AccountType::Contract;
        }

    public:
        using Account::Account;
    };
}

#endif // ADVMATCH_SDK_CONTRACT_ACCOUNT_H
