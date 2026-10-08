#ifndef ADVMATCH_ACCOUNT_MANAGER_H
#define ADVMATCH_ACCOUNT_MANAGER_H

#include "advmatch/ledging/account/impl/ContractAccount.h"
#include "advmatch/ledging/account/impl/SpotAccount.h"
#include "../util/BigAllocs.h"
#include "../util/Containers.h"

#include <vector>

namespace advmatch
{
    struct UserAccounts
    {
        SpotAccount* spotAccount;
        ContractAccount* contractAccount;
    };

    class AccountManager
    {
        struct AccountPair
        {
            AccountId spotAccountId;
            AccountId contractAccountId;
        };

        // Declared first so every account is destroyed before its position pool.
        PositionPool positionPool;
        BigAllocs<SpotAccount> spotAccountPool;
        BigAllocs<StandardContractAccount> contractAccountPool;
        std::vector<Account*> accounts;
        DenseMap<UserId, AccountPair> accountsByUser;
        AccountId nextAccountId = 1;
        std::size_t activeAccountCount = 0;

        AccountId allocateAccountId();

    public:
        void reserve(std::size_t userCount);

        [[nodiscard]]
        std::optional<UserAccounts> createAccounts(UserId userId);

        [[nodiscard]]
        bool removeAccounts(UserId userId);

        [[nodiscard]]
        Account* getAccount(AccountId accountId) noexcept;

        [[nodiscard]]
        const Account* getAccount(AccountId accountId) const noexcept;

        [[nodiscard]]
        SpotAccount* getSpotAccount(UserId userId) noexcept;

        [[nodiscard]]
        const SpotAccount* getSpotAccount(UserId userId) const noexcept;

        [[nodiscard]]
        ContractAccount* getContractAccount(UserId userId) noexcept;

        [[nodiscard]]
        const ContractAccount* getContractAccount(UserId userId) const noexcept;

        [[nodiscard]]
        BalanceResult transfer(AccountId sourceAccountId, AccountId targetAccountId, AssetId assetId,
                               PositiveBalanceChange amount);

        [[nodiscard]]
        std::size_t getAccountCount() const noexcept;
        // Run on the owning thread. The callback must not mutate this manager.
        template <typename Visitor>
        void forEachAccount(Visitor&& visitor) const
        {
            for (const Account* account : this->accounts)
            {
                if (account != nullptr && account->isActive())
                {
                    visitor(account->getAccountId(), *account);
                }
            }
        }

    };
}

#endif // ADVMATCH_ACCOUNT_MANAGER_H
