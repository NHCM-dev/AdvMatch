#ifndef ADVMATCH_ACCOUNT_LEDGER_H
#define ADVMATCH_ACCOUNT_LEDGER_H

#include "advmatch/ledging/account/Account.h"
#include "advmatch/matching/Matching.h"

namespace advmatch
{
    class User;

    class AccountLedger
    {
    protected:
        [[nodiscard]]
        virtual Account& accountFor(User& user) const noexcept = 0;

        [[nodiscard]]
        virtual const Account& accountFor(const User& user) const noexcept = 0;

    public:
        virtual ~AccountLedger() = default;

        [[nodiscard]]
        virtual AccountType getAccountType() const noexcept = 0;

        [[nodiscard]]
        bool reserve(OrderNode& node, Balance requiredBalance) const;

        void releaseTo(OrderNode& node, Balance requiredBalance) const;
        void releaseAll(OrderNode& node) const;
        void consume(OrderNode& node, PositiveBalanceChange amount) const;
        void credit(User& user, AssetId assetId, PositiveBalanceChange amount) const;

        [[nodiscard]]
        const Account& getAccount(const User& user) const noexcept;
    };
}

#endif // ADVMATCH_ACCOUNT_LEDGER_H
