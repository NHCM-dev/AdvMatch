#ifndef ADVMATCH_SPOT_ACCOUNT_LEDGER_H
#define ADVMATCH_SPOT_ACCOUNT_LEDGER_H

#include "../AccountLedger.h"
#include "../SettlementContext.h"

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
        bool settle(OrderNode& buyer, OrderNode& seller, PositiveBalanceChange baseAmount, PositiveBalanceChange quoteAmount,
            SettlementContext* context = nullptr) const;

        [[nodiscard]]
        AccountType getAccountType() const noexcept override;
    };
}

#endif // ADVMATCH_SPOT_ACCOUNT_LEDGER_H
