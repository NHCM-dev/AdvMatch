#include "AccountManager.h"

#include "../util/Utils.h"

#include <limits>
#include <algorithm>
#include <stdexcept>

namespace advmatch
{
    AccountId AccountManager::allocateAccountId()
    {
        if (this->nextAccountId == 0)
        {
            throw std::overflow_error("ledging id overflow");
        }

        return this->nextAccountId++;
    }

    void AccountManager::reserve(std::size_t userCount)
    {
        this->spotAccountPool.reserve(userCount);
        this->contractAccountPool.reserve(userCount);
        this->accounts.reserve(userCount * 2);
        this->accountsByUser.reserve(userCount);
    }

    std::optional<UserAccounts> AccountManager::createAccounts(UserId userId)
    {
        if (userId == 0 || this->accountsByUser.contains(userId))
        {
            return std::nullopt;
        }

        const AccountId spotAccountId = this->allocateAccountId();
        const AccountId contractAccountId = this->allocateAccountId();
        SpotAccount* spotAccount = this->spotAccountPool.emplace(spotAccountId, userId);
        StandardContractAccount* contractAccount = nullptr;

        try
        {
            contractAccount = this->contractAccountPool.emplace(contractAccountId, userId, &this->positionPool);
        }
        catch (...)
        {
            static_cast<void>(this->spotAccountPool.release(spotAccount));
            throw;
        }

        try
        {
            if (this->accounts.size() < contractAccountId)
            {
                this->accounts.resize(contractAccountId);
            }

            this->accounts[spotAccountId - 1] = spotAccount;
            this->accounts[contractAccountId - 1] = contractAccount;
            this->accountsByUser.emplace(userId, AccountPair{spotAccountId, contractAccountId});
        }
        catch (...)
        {
            if (this->accounts.size() >= contractAccountId)
            {
                this->accounts[spotAccountId - 1] = nullptr;
                this->accounts[contractAccountId - 1] = nullptr;
            }

            static_cast<void>(this->spotAccountPool.release(spotAccount));
            static_cast<void>(this->contractAccountPool.release(contractAccount));
            throw;
        }

        this->activeAccountCount += 2;
        return UserAccounts{spotAccount, contractAccount};
    }

    bool AccountManager::removeAccounts(UserId userId)
    {
        const auto iterator = this->accountsByUser.find(userId);
        if (iterator == this->accountsByUser.end())
        {
            return false;
        }

        Account* spotAccount = this->accounts[iterator->second.spotAccountId - 1];
        Account* contractAccount = this->accounts[iterator->second.contractAccountId - 1];
        if (!spotAccount->isActive() || !contractAccount->isActive())
        {
            return false;
        }

        if (static_cast<ContractAccount*>(contractAccount)->hasTradingExposure())
        {
            return false;
        }

        spotAccount->deactivate();
        contractAccount->deactivate();
        this->activeAccountCount -= 2;
        return true;
    }

    Account* AccountManager::getAccount(AccountId accountId) noexcept
    {
        if (accountId == 0 || accountId > this->accounts.size())
        {
            return nullptr;
        }

        Account* account = this->accounts[accountId - 1];
        return account && account->isActive() ? account : nullptr;
    }

    const Account* AccountManager::getAccount(AccountId accountId) const noexcept
    {
        if (accountId == 0 || accountId > this->accounts.size())
        {
            return nullptr;
        }

        const Account* account = this->accounts[accountId - 1];
        return account && account->isActive() ? account : nullptr;
    }

    SpotAccount* AccountManager::getSpotAccount(UserId userId) noexcept
    {
        const auto iterator = this->accountsByUser.find(userId);
        return iterator == this->accountsByUser.end()
                   ? nullptr
                   : static_cast<SpotAccount*>(this->getAccount(iterator->second.spotAccountId));
    }

    const SpotAccount* AccountManager::getSpotAccount(UserId userId) const noexcept
    {
        const auto iterator = this->accountsByUser.find(userId);
        return iterator == this->accountsByUser.end()
                   ? nullptr
                   : static_cast<const SpotAccount*>(this->getAccount(iterator->second.spotAccountId));
    }

    ContractAccount* AccountManager::getContractAccount(UserId userId) noexcept
    {
        const auto iterator = this->accountsByUser.find(userId);
        return iterator == this->accountsByUser.end()
                   ? nullptr
                   : static_cast<ContractAccount*>(this->getAccount(iterator->second.contractAccountId));
    }

    const ContractAccount* AccountManager::getContractAccount(UserId userId) const noexcept
    {
        const auto iterator = this->accountsByUser.find(userId);
        return iterator == this->accountsByUser.end()
                   ? nullptr
                   : static_cast<const ContractAccount*>(this->getAccount(iterator->second.contractAccountId));
    }

    BalanceResult AccountManager::transfer(AccountId sourceAccountId, AccountId targetAccountId, AssetId assetId,
                                           PositiveBalanceChange amount)
    {
        Account* sourceAccount = this->getAccount(sourceAccountId);
        Account* targetAccount = this->getAccount(targetAccountId);
        if (!sourceAccount || !targetAccount || sourceAccount == targetAccount ||
            sourceAccount->getAccountType() == targetAccount->getAccountType() ||
            sourceAccount->getUserId() != targetAccount->getUserId())
        {
            return {BalanceOpResult::InvalidAccount, "Invalid ledging transfer"};
        }

        if (amount == 0)
        {
            return {BalanceOpResult::Success};
        }

        const Balance sourceBalance = sourceAccount->queryActiveBalance(assetId).value_or(0);
        const Balance targetBalance = targetAccount->queryActiveBalance(assetId).value_or(0);
        if (!util::enoughSubtract(sourceBalance, amount))
        {
            return {BalanceOpResult::Insufficient, "Insufficient transferable balance"};
        }

        if (amount > static_cast<PositiveBalanceChange>(std::numeric_limits<BalanceChange>::max()) ||
            targetBalance < 0 || util::willAddOverflow(targetBalance, static_cast<BalanceChange>(amount)))
        {
            return {BalanceOpResult::Overflow, "Target ledging balance would overflow"};
        }

        const BalanceChange balanceChange = static_cast<BalanceChange>(amount);
        const BalanceResult debitResult = sourceAccount->updateBalance(assetId, -balanceChange);
        if (!debitResult)
        {
            return debitResult;
        }

        const BalanceResult creditResult = targetAccount->updateBalance(assetId, balanceChange);
        if (!creditResult)
        {
            if (!sourceAccount->updateBalance(assetId, balanceChange))
            {
                throw std::logic_error("ledging transfer rollback failed");
            }

            return creditResult;
        }

        return {BalanceOpResult::Success};
    }

    BalanceResult AccountManager::transact(const std::vector<BalanceDelta>& changes, bool conserve)
    {
        if (changes.empty() || changes.size() > 256)
            return {BalanceOpResult::Unknown, "Transaction requires 1..256 unique legs"};

        struct Prepared
        {
            Account* account;
            std::unique_ptr<DenseMap<AssetId, AssetBalance>> balances;
        };
        std::vector<Prepared> prepared;
        DenseMap<AssetId, WideBalance> totals;
        // Allocate and validate everything before publishing any financial value.
        for (std::size_t i = 0; i < changes.size(); ++i)
        {
            const auto& leg = changes[i];
            for (std::size_t j = 0; j < i; ++j)
                if (changes[j].accountId == leg.accountId && changes[j].assetId == leg.assetId)
                    return {BalanceOpResult::Unknown, "Duplicate transaction leg"};
            Account* account = this->getAccount(leg.accountId);
            if (!account) return {BalanceOpResult::InvalidAccount};
            const WideBalance after = static_cast<WideBalance>(account->queryActiveBalance(leg.assetId).value_or(0)) + leg.change;
            if (after < 0) return {BalanceOpResult::Insufficient};
            if (after + account->queryReservedBalance(leg.assetId).value_or(0) > std::numeric_limits<Balance>::max())
                return {BalanceOpResult::Overflow};
            if (leg.change < 0 && !account->canWithdraw(leg.assetId, static_cast<PositiveBalanceChange>(-(leg.change + 1)) + 1))
                return {BalanceOpResult::Insufficient};
            totals[leg.assetId] += leg.change;
            auto found = std::find_if(prepared.begin(), prepared.end(), [account](const Prepared& item) { return item.account == account; });
            if (found == prepared.end())
            {
                auto balances = std::make_unique<DenseMap<AssetId, AssetBalance>>();
                balances->reserve(account->getAssetBalanceCount() + changes.size());
                account->forEachBalance([&](AssetId id, AssetBalance balance) { balances->emplace(id, balance); });
                prepared.push_back({account, std::move(balances)});
                found = std::prev(prepared.end());
            }
            auto& balance = (*found->balances)[leg.assetId];
            balance.active = static_cast<Balance>(after);
            if (balance.active == 0 && balance.reserved == 0) found->balances->erase(leg.assetId);
        }
        if (conserve)
            for (const auto& [asset, total] : totals)
                if (total != 0) return {BalanceOpResult::Unknown, "Transfer must conserve each asset"};

        // Only no-throw pointer swaps and inline destruction remain. No borrowed
        // balance pointers survive this commit; invalidate every snapshot token.
        for (auto& item : prepared)
        {
            item.account->overflowBalances.swap(item.balances);
            item.account->inlineBalances.clear();
            ++item.account->balanceGeneration;
        }
        return {BalanceOpResult::Success};
    }

    std::size_t AccountManager::getAccountCount() const noexcept
    {
        return this->activeAccountCount;
    }
}
