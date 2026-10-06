#include "User.h"

#include "../management/AccountManager.h"

namespace advmatch
{
    void User::deactivate() noexcept
    {
        this->active = false;
    }

    SpotAccount& User::getSpotAccount() noexcept
    {
        return *this->spotAccount;
    }

    const SpotAccount& User::getSpotAccount() const noexcept
    {
        return *this->spotAccount;
    }

    ContractAccount& User::getContractAccount() noexcept
    {
        return *this->contractAccount;
    }

    const ContractAccount& User::getContractAccount() const noexcept
    {
        return *this->contractAccount;
    }

    BalanceResult User::transferToContract(AssetId assetId, PositiveBalanceChange amount)
    {
        return this->accountManager->transfer(this->spotAccount->getAccountId(),
                                              this->contractAccount->getAccountId(), assetId, amount);
    }

    BalanceResult User::transferToSpot(AssetId assetId, PositiveBalanceChange amount)
    {
        return this->accountManager->transfer(this->contractAccount->getAccountId(),
                                              this->spotAccount->getAccountId(), assetId, amount);
    }

    UserId User::getUserId() const noexcept
    {
        return this->userId;
    }

    bool User::isActive() const noexcept
    {
        return this->active;
    }
}
