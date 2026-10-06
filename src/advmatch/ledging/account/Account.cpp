#include "Account.h"

#include "../../util/Utils.h"

namespace advmatch
{
    void Account::deactivate() noexcept
    {
        this->active = false;
    }

    AssetBalance* Account::findBalance(AssetId assetId) noexcept
    {
        if (this->overflowBalances)
        {
            const auto iterator = this->overflowBalances->find(assetId);
            return iterator == this->overflowBalances->end() ? nullptr : &iterator->second;
        }

        for (AssetBalanceEntry& entry : this->inlineBalances)
        {
            if (entry.assetId == assetId)
            {
                return &entry.balance;
            }
        }

        return nullptr;
    }

    const AssetBalance* Account::findBalance(AssetId assetId) const noexcept
    {
        if (this->overflowBalances)
        {
            const auto iterator = this->overflowBalances->find(assetId);
            return iterator == this->overflowBalances->end() ? nullptr : &iterator->second;
        }

        for (const AssetBalanceEntry& entry : this->inlineBalances)
        {
            if (entry.assetId == assetId)
            {
                return &entry.balance;
            }
        }

        return nullptr;
    }

    AssetBalance& Account::addBalance(AssetId assetId, AssetBalance balance)
    {
        if (this->overflowBalances)
        {
            return this->overflowBalances->emplace(assetId, balance).first->second;
        }

        if (this->inlineBalances.size() < InlineBalanceCapacity)
        {
            this->inlineBalances.push_back({assetId, balance});
            return this->inlineBalances.back().balance;
        }

        auto newOverflowBalances = std::make_unique<DenseMap<AssetId, AssetBalance>>();
        newOverflowBalances->reserve(InlineBalanceCapacity * 2);

        for (const AssetBalanceEntry& entry : this->inlineBalances)
        {
            newOverflowBalances->emplace(entry.assetId, entry.balance);
        }

        AssetBalance& insertedBalance = newOverflowBalances->emplace(assetId, balance).first->second;
        this->overflowBalances = std::move(newOverflowBalances);
        this->inlineBalances.clear();
        return insertedBalance;
    }

    void Account::removeBalance(AssetId assetId)
    {
        if (this->overflowBalances)
        {
            this->overflowBalances->erase(assetId);
            if (this->overflowBalances->empty())
            {
                this->overflowBalances.reset();
            }

            return;
        }

        for (auto iterator = this->inlineBalances.begin(); iterator != this->inlineBalances.end(); ++iterator)
        {
            if (iterator->assetId == assetId)
            {
                this->inlineBalances.erase(iterator);
                return;
            }
        }
    }

    std::optional<Balance> Account::queryActiveBalance(AssetId assetId) const noexcept
    {
        const AssetBalance* balance = this->findBalance(assetId);
        return balance ? std::optional<Balance>(balance->active) : std::nullopt;
    }

    std::optional<Balance> Account::queryReservedBalance(AssetId assetId) const noexcept
    {
        const AssetBalance* balance = this->findBalance(assetId);
        return balance ? std::optional<Balance>(balance->reserved) : std::nullopt;
    }

    BalanceResult Account::updateBalance(AssetId assetId, BalanceChange balanceChange)
    {
        if (balanceChange == 0)
        {
            return {BalanceOpResult::Success};
        }

        AssetBalance* balance = this->findBalance(assetId);
        if (!balance)
        {
            if (balanceChange < 0)
            {
                return {BalanceOpResult::Insufficient, "Insufficient active balance"};
            }

            this->addBalance(assetId, {static_cast<Balance>(balanceChange), 0});
            return {BalanceOpResult::Success};
        }

        if (balanceChange > 0)
        {
            if (util::willAddOverflow(balance->active, balanceChange))
            {
                return {BalanceOpResult::Overflow, "Active balance would overflow"};
            }

            balance->active += balanceChange;
        }
        else
        {
            const PositiveBalanceChange amount = static_cast<PositiveBalanceChange>(-(balanceChange + 1)) + 1;
            if (!util::enoughSubtract(balance->active, amount))
            {
                return {BalanceOpResult::Insufficient, "Insufficient active balance"};
            }

            balance->active -= static_cast<Balance>(amount);
        }

        if (balance->active == 0 && balance->reserved == 0)
        {
            this->removeBalance(assetId);
        }

        return {BalanceOpResult::Success};
    }

    BalanceResult Account::reserveBalance(AssetId assetId, PositiveBalanceChange amount)
    {
        if (amount == 0)
        {
            return {BalanceOpResult::Success};
        }

        AssetBalance* balance = this->findBalance(assetId);
        if (!balance)
        {
            return {BalanceOpResult::NoSuchAsset};
        }

        if (!util::enoughSubtract(balance->active, amount))
        {
            return {BalanceOpResult::Insufficient};
        }

        if (util::willAddOverflow(balance->reserved, amount))
        {
            return {BalanceOpResult::Overflow, "Reserved balance would overflow"};
        }

        balance->active -= static_cast<Balance>(amount);
        balance->reserved += static_cast<Balance>(amount);
        return {BalanceOpResult::Success};
    }

    BalanceResult Account::releaseReservedBalance(AssetId assetId, PositiveBalanceChange amount)
    {
        if (amount == 0)
        {
            return {BalanceOpResult::Success};
        }

        AssetBalance* balance = this->findBalance(assetId);
        if (!balance)
        {
            return {BalanceOpResult::NoSuchAsset};
        }

        if (!util::enoughSubtract(balance->reserved, amount))
        {
            return {BalanceOpResult::Insufficient};
        }

        if (util::willAddOverflow(balance->active, amount))
        {
            return {BalanceOpResult::Overflow, "Active balance would overflow"};
        }

        balance->reserved -= static_cast<Balance>(amount);
        balance->active += static_cast<Balance>(amount);
        return {BalanceOpResult::Success};
    }

    BalanceResult Account::consumeReservedBalance(AssetId assetId, PositiveBalanceChange amount)
    {
        if (amount == 0)
        {
            return {BalanceOpResult::Success};
        }

        AssetBalance* balance = this->findBalance(assetId);
        if (!balance || !util::enoughSubtract(balance->reserved, amount))
        {
            return {BalanceOpResult::Insufficient};
        }

        balance->reserved -= static_cast<Balance>(amount);
        if (balance->active == 0 && balance->reserved == 0)
        {
            this->removeBalance(assetId);
        }

        return {BalanceOpResult::Success};
    }

    BalanceResult Account::reserveAsset(AssetId assetId)
    {
        const std::optional<Balance> activeBalanceResult = this->queryActiveBalance(assetId);
        if (!activeBalanceResult)
        {
            return {BalanceOpResult::NoSuchAsset};
        }

        const Balance activeBalance = *activeBalanceResult;
        if (activeBalance < 0)
        {
            return {BalanceOpResult::Insufficient};
        }

        return this->reserveBalance(assetId, static_cast<PositiveBalanceChange>(activeBalance));
    }

    BalanceResult Account::releaseAsset(AssetId assetId)
    {
        const std::optional<Balance> reservedBalanceResult = this->queryReservedBalance(assetId);
        if (!reservedBalanceResult)
        {
            return {BalanceOpResult::NoSuchAsset};
        }

        const Balance reservedBalance = *reservedBalanceResult;
        if (reservedBalance < 0)
        {
            return {BalanceOpResult::Insufficient};
        }

        return this->releaseReservedBalance(assetId, static_cast<PositiveBalanceChange>(reservedBalance));
    }

    AccountId Account::getAccountId() const noexcept
    {
        return this->accountId;
    }

    UserId Account::getUserId() const noexcept
    {
        return this->userId;
    }

    std::size_t Account::getAssetBalanceCount() const noexcept
    {
        return this->overflowBalances ? this->overflowBalances->size() : this->inlineBalances.size();
    }

    bool Account::isActive() const noexcept
    {
        return this->active;
    }
}
