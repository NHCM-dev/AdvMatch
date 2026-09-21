//
// Created by NHCM on 2026/9/20.
//

#include "User.h"

#include "../util/Utils.h"

namespace advmatch
{
    std::optional<Balance> User::queryActiveBalance(AssetId assetId) const noexcept
    {
        const auto iterator = this->balances.find(assetId);
        if (iterator != this->balances.end())
        {
            return std::make_optional(iterator->second.active);
        }
        return std::nullopt;
    }

    std::optional<Balance> User::queryLockedBalance(AssetId assetId) const noexcept
    {
        const auto iterator = this->balances.find(assetId);
        if (iterator != this->balances.end())
        {
            return std::make_optional(iterator->second.locked);
        }
        return std::nullopt;
    }

    BalanceResult User::updateBalance(AssetId assetId, BalanceChange balanceChange)
    {
        if (balanceChange == 0)
        {
            return {BalanceOpResult::Success};
        }

        auto iterator = this->balances.find(assetId);
        if (iterator == this->balances.end())
        {
            if (balanceChange < 0)
            {
                return {
                    BalanceOpResult::Insufficient,
                    "Insufficient active balance"};
            }

            this->balances.emplace(
                assetId,
                AssetBalance{
                    balanceChange,
                    0
                }
            );

            return {BalanceOpResult::Success};
        }

        AssetBalance& balance = iterator->second;

        if (balanceChange > 0)
        {
            if (util::willAddOverflow(balance.active, balanceChange))
            {
                return {
                    BalanceOpResult::Overflow,
                    "Balance update in active balance will overflow"};
            }

            balance.active += balanceChange;
        }
        else
        {
            const PositiveBalanceChange amount =
                static_cast<PositiveBalanceChange>(-(balanceChange + 1)) + 1;

            if (!util::enoughSubtract(balance.active, amount))
            {
                return {
                    BalanceOpResult::Insufficient,
                    "Insufficient active balance"};
            }

            balance.active -= static_cast<Balance>(amount);
        }

        if (balance.active == 0 && balance.locked == 0)
        {
            this->balances.erase(iterator);
        }

        return {BalanceOpResult::Success};
    }

    BalanceResult User::lockBalance(AssetId assetId, PositiveBalanceChange balanceChange)
    {
        if (balanceChange == 0)
        {
            return {BalanceOpResult::Success};
        }

        auto iterator = this->balances.find(assetId);
        if (iterator == this->balances.end())
        {
            return {BalanceOpResult::NoSuchAsset};
        }

        AssetBalance& balance = iterator->second;

        if (!util::enoughSubtract(balance.active, balanceChange))
        {
            return {BalanceOpResult::Insufficient};
        }

        if (util::willAddOverflow(balance.locked, balanceChange))
        {
            return {
                BalanceOpResult::Overflow,
                "Balance update in locked balance will overflow"};
        }

        balance.active -= static_cast<Balance>(balanceChange);
        balance.locked += static_cast<Balance>(balanceChange);

        return {BalanceOpResult::Success};
    }

    BalanceResult User::unlockBalance(AssetId assetId, PositiveBalanceChange balanceChange)
    {
        if (balanceChange == 0)
        {
            return {BalanceOpResult::Success};
        }

        auto iterator = this->balances.find(assetId);
        if (iterator == this->balances.end())
        {
            return {BalanceOpResult::NoSuchAsset};
        }

        AssetBalance& balance = iterator->second;

        if (!util::enoughSubtract(balance.locked, balanceChange))
        {
            return {BalanceOpResult::Insufficient};
        }

        if (util::willAddOverflow(balance.active, balanceChange))
        {
            return {
                BalanceOpResult::Overflow,
                "Balance update in active balance will overflow"};
        }

        balance.locked -= static_cast<Balance>(balanceChange);
        balance.active += static_cast<Balance>(balanceChange);

        return {BalanceOpResult::Success};
    }

    BalanceResult User::consumeLockedBalance(AssetId assetId, PositiveBalanceChange balanceChange)
    {
        if (balanceChange == 0)
        {
            return {BalanceOpResult::Success};
        }

        auto iterator = this->balances.find(assetId);
        if (iterator == this->balances.end() || !util::enoughSubtract(iterator->second.locked, balanceChange))
        {
            return {BalanceOpResult::Insufficient};
        }

        iterator->second.locked -= static_cast<Balance>(balanceChange);
        if (iterator->second.active == 0 && iterator->second.locked == 0)
        {
            this->balances.erase(iterator);
        }

        return {BalanceOpResult::Success};
    }

    BalanceResult User::lockAsset(AssetId assetId)
    {
        auto iterator = this->balances.find(assetId);
        if (iterator == this->balances.end())
        {
            return {BalanceOpResult::NoSuchAsset};
        }

        const Balance activeBalance = iterator->second.active;
        if (activeBalance == 0)
        {
            return {BalanceOpResult::Success};
        }

        return this->lockBalance(
            assetId,
            static_cast<PositiveBalanceChange>(activeBalance)
        );
    }

    BalanceResult User::unlockAsset(AssetId assetId)
    {
        auto iterator = this->balances.find(assetId);
        if (iterator == this->balances.end())
        {
            return {BalanceOpResult::NoSuchAsset};
        }

        const Balance lockedBalance = iterator->second.locked;
        if (lockedBalance == 0)
        {
            return {BalanceOpResult::Success};
        }

        return this->unlockBalance(
            assetId,
            static_cast<PositiveBalanceChange>(lockedBalance)
        );
    }
} // advmatch
