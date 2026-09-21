//
// Created by NHCM on 2026/9/20.
//

#include "User.h"

#include "../util/Utils.h"

namespace advmatch
{
    AssetBalance* User::findBalance(AssetId assetId) noexcept
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

    const AssetBalance* User::findBalance(AssetId assetId) const noexcept
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

    AssetBalance& User::addBalance(AssetId assetId, AssetBalance balance)
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

        // Promotion moves every balance out of inline storage. No pointer,
        // reference, or iterator obtained before this point remains valid.
        auto newOverflowBalances = std::make_unique<OverflowBalances>();
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

    void User::removeBalance(AssetId assetId)
    {
        if (this->overflowBalances)
        {
            // DenseMap can move its last element into the erased slot. Treat all
            // previously obtained balance addresses as invalid after erase.
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
                // SmallVector shifts every following entry into the empty slot.
                // Addresses at and after iterator become invalid.
                this->inlineBalances.erase(iterator);
                return;
            }
        }
    }

    std::optional<Balance> User::queryActiveBalance(AssetId assetId) const noexcept
    {
        const AssetBalance* balance = this->findBalance(assetId);
        if (balance)
        {
            return balance->active;
        }

        return std::nullopt;
    }

    std::optional<Balance> User::queryLockedBalance(AssetId assetId) const noexcept
    {
        const AssetBalance* balance = this->findBalance(assetId);
        if (balance)
        {
            return balance->locked;
        }

        return std::nullopt;
    }

    BalanceResult User::updateBalance(AssetId assetId, BalanceChange balanceChange)
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

            this->addBalance(assetId, {balanceChange, 0});
            return {BalanceOpResult::Success};
        }

        if (balanceChange > 0)
        {
            if (util::willAddOverflow(balance->active, balanceChange))
            {
                return {BalanceOpResult::Overflow, "Balance update in active balance will overflow"};
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

        if (balance->active == 0 && balance->locked == 0)
        {
            this->removeBalance(assetId);
        }

        return {BalanceOpResult::Success};
    }

    BalanceResult User::lockBalance(AssetId assetId, PositiveBalanceChange balanceChange)
    {
        if (balanceChange == 0)
        {
            return {BalanceOpResult::Success};
        }

        AssetBalance* balance = this->findBalance(assetId);
        if (!balance)
        {
            return {BalanceOpResult::NoSuchAsset};
        }

        if (!util::enoughSubtract(balance->active, balanceChange))
        {
            return {BalanceOpResult::Insufficient};
        }

        if (util::willAddOverflow(balance->locked, balanceChange))
        {
            return {BalanceOpResult::Overflow, "Balance update in locked balance will overflow"};
        }

        balance->active -= static_cast<Balance>(balanceChange);
        balance->locked += static_cast<Balance>(balanceChange);

        return {BalanceOpResult::Success};
    }

    BalanceResult User::unlockBalance(AssetId assetId, PositiveBalanceChange balanceChange)
    {
        if (balanceChange == 0)
        {
            return {BalanceOpResult::Success};
        }

        AssetBalance* balance = this->findBalance(assetId);
        if (!balance)
        {
            return {BalanceOpResult::NoSuchAsset};
        }

        if (!util::enoughSubtract(balance->locked, balanceChange))
        {
            return {BalanceOpResult::Insufficient};
        }

        if (util::willAddOverflow(balance->active, balanceChange))
        {
            return {BalanceOpResult::Overflow, "Balance update in active balance will overflow"};
        }

        balance->locked -= static_cast<Balance>(balanceChange);
        balance->active += static_cast<Balance>(balanceChange);

        return {BalanceOpResult::Success};
    }

    BalanceResult User::consumeLockedBalance(AssetId assetId, PositiveBalanceChange balanceChange)
    {
        if (balanceChange == 0)
        {
            return {BalanceOpResult::Success};
        }

        AssetBalance* balance = this->findBalance(assetId);
        if (!balance || !util::enoughSubtract(balance->locked, balanceChange))
        {
            return {BalanceOpResult::Insufficient};
        }

        balance->locked -= static_cast<Balance>(balanceChange);
        if (balance->active == 0 && balance->locked == 0)
        {
            this->removeBalance(assetId);
        }

        return {BalanceOpResult::Success};
    }

    BalanceResult User::lockAsset(AssetId assetId)
    {
        const AssetBalance* balance = this->findBalance(assetId);
        if (!balance)
        {
            return {BalanceOpResult::NoSuchAsset};
        }

        const Balance activeBalance = balance->active;
        if (activeBalance == 0)
        {
            return {BalanceOpResult::Success};
        }

        return this->lockBalance(assetId, static_cast<PositiveBalanceChange>(activeBalance));
    }

    BalanceResult User::unlockAsset(AssetId assetId)
    {
        const AssetBalance* balance = this->findBalance(assetId);
        if (!balance)
        {
            return {BalanceOpResult::NoSuchAsset};
        }

        const Balance lockedBalance = balance->locked;
        if (lockedBalance == 0)
        {
            return {BalanceOpResult::Success};
        }

        return this->unlockBalance(assetId, static_cast<PositiveBalanceChange>(lockedBalance));
    }
} // advmatch
