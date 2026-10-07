#ifndef ADVMATCH_CONTRACT_ACCOUNT_LEDGER_H
#define ADVMATCH_CONTRACT_ACCOUNT_LEDGER_H

#include "../AccountLedger.h"

namespace advmatch
{
    class ContractAccountLedger final : public AccountLedger
    {
    protected:
        void reservationChanged(OrderNode& node, Balance previousBalance) const noexcept override;
        [[nodiscard]]
        Account& accountFor(User& user) const noexcept override;

        [[nodiscard]]
        const Account& accountFor(const User& user) const noexcept override;

    public:
        [[nodiscard]]
        AccountType getAccountType() const noexcept override;
    };
}

#endif // ADVMATCH_CONTRACT_ACCOUNT_LEDGER_H
