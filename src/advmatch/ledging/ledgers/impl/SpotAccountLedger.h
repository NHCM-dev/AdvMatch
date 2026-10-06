#ifndef ADVMATCH_SPOT_ACCOUNT_LEDGER_H
#define ADVMATCH_SPOT_ACCOUNT_LEDGER_H

#include "../AccountLedger.h"

namespace advmatch
{
    class SpotAccountLedger final : public AccountLedger
    {
    protected:
        [[nodiscard]]
        Account& accountFor(User& user) const noexcept override;

        [[nodiscard]]
        const Account& accountFor(const User& user) const noexcept override;

    public:
        [[nodiscard]]
        AccountType getAccountType() const noexcept override;
    };
}

#endif // ADVMATCH_SPOT_ACCOUNT_LEDGER_H
