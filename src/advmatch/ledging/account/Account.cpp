#include "Account.h"

#include "../../util/Utils.h"

namespace advmatch
{
    Account::BalanceSnapshot Account::captureBalance(AssetId assetId) const noexcept
    {
        BalanceSnapshot snapshot;
        snapshot.owner = this;
        snapshot.assetId = assetId;
        snapshot.generation = this->balanceGeneration;
        snapshot.location = this->findBalance(assetId);
        snapshot.before = snapshot.location ? *snapshot.location : AssetBalance{};
        return snapshot;
    }

    std::optional<Account::BalanceUpdate> Account::prepareBalanceChange(const BalanceSnapshot& snapshot,
        BalanceChange activeChange, BalanceChange reservedChange) const noexcept
    {
        const WideBalance active = static_cast<WideBalance>(snapshot.before.active) + activeChange;
        const WideBalance reserved = static_cast<WideBalance>(snapshot.before.reserved) + reservedChange;
        if (snapshot.owner != this || snapshot.generation != this->balanceGeneration ||
            active < 0 || reserved < 0 || active + reserved > std::numeric_limits<Balance>::max())
        {
            return std::nullopt;
        }

        BalanceUpdate update;
        update.snapshot = snapshot;
        update.after = {static_cast<Balance>(active), static_cast<Balance>(reserved)};
        return update;
    }

    BalanceResult Account::commitBalanceChange(const BalanceUpdate& update)
    {
        const auto& snapshot = update.snapshot;
        if (snapshot.owner != this || snapshot.generation != this->balanceGeneration)
        {
            return {BalanceOpResult::StaleUpdate};
        }

        if (snapshot.location)
        {
            if (snapshot.location->active != snapshot.before.active || snapshot.location->reserved != snapshot.before.reserved)
            {
                return {BalanceOpResult::StaleUpdate};
            }

            *const_cast<AssetBalance*>(snapshot.location) = update.after;
        }
        else if (update.after.active != 0 || update.after.reserved != 0)
        {
            this->addBalance(snapshot.assetId, update.after);
        }

        return {BalanceOpResult::Success};
    }

    bool Account::canApplyBalanceChange(AssetId assetId, BalanceChange activeChange, BalanceChange reservedChange) const noexcept
    {
        const AssetBalance* current = this->findBalance(assetId);
        const WideBalance active = static_cast<WideBalance>(current ? current->active : 0) + activeChange;
        const WideBalance reserved = static_cast<WideBalance>(current ? current->reserved : 0) + reservedChange;
        return active >= 0 && reserved >= 0 && active + reserved <= std::numeric_limits<Balance>::max();
    }

    BalanceResult Account::applyBalanceChange(AssetId assetId, BalanceChange activeChange, BalanceChange reservedChange)
    {
        const auto update = this->prepareBalanceChange(this->captureBalance(assetId), activeChange, reservedChange);
        if (!update)
        {
            return {BalanceOpResult::Insufficient};
        }

        return this->commitBalanceChange(*update);
    }

    bool Account::canWithdraw(AssetId, PositiveBalanceChange) const noexcept
    {
        return true;
    }

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
        ++this->balanceGeneration;
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
        ++this->balanceGeneration;
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
        if (balanceChange < 0 && !this->canWithdraw(assetId, static_cast<PositiveBalanceChange>(-(balanceChange + 1)) + 1))
        {
            return {BalanceOpResult::Insufficient};
        }

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
            if (static_cast<WideBalance>(balance->active) + balance->reserved + balanceChange > std::numeric_limits<Balance>::max())
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
        if (!this->canWithdraw(assetId, amount))
        {
            return {BalanceOpResult::Insufficient};
        }

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
