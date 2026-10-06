#ifndef ADVMATCH_USER_H
#define ADVMATCH_USER_H

#include "advmatch/ledging/account/impl/ContractAccount.h"
#include "advmatch/ledging/account/impl/SpotAccount.h"

namespace advmatch
{
    class AccountManager;
    class UserManager;

    class User
    {
        AccountManager* const accountManager;
        SpotAccount* const spotAccount;
        ContractAccount* const contractAccount;
        const UserId userId;
        bool active = true;

        friend class UserManager;

        void deactivate() noexcept;

    public:
        User(UserId userId, AccountManager& accountManager, SpotAccount& spotAccount,
             ContractAccount& contractAccount)
            : accountManager(&accountManager), spotAccount(&spotAccount), contractAccount(&contractAccount),
              userId(userId)
        {
        }

        User(const User&) = delete;
        User& operator=(const User&) = delete;
        User(User&&) = delete;
        User& operator=(User&&) = delete;

        [[nodiscard]]
        SpotAccount& getSpotAccount() noexcept;

        [[nodiscard]]
        const SpotAccount& getSpotAccount() const noexcept;

        [[nodiscard]]
        ContractAccount& getContractAccount() noexcept;

        [[nodiscard]]
        const ContractAccount& getContractAccount() const noexcept;

        [[nodiscard]]
        BalanceResult transferToContract(AssetId assetId, PositiveBalanceChange amount);

        [[nodiscard]]
        BalanceResult transferToSpot(AssetId assetId, PositiveBalanceChange amount);

        [[nodiscard]]
        UserId getUserId() const noexcept;

        [[nodiscard]]
        bool isActive() const noexcept;
    };
}

#endif // ADVMATCH_USER_H
