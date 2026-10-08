#include "ContractAccount.h"

#include <utility>

namespace advmatch
{
    StandardContractAccount::~StandardContractAccount()
    {
        for (const auto& [id, position] : this->positions)
        {
            this->positionPool->release(position);
        }
    }

    CrossMarginState& ContractAccount::crossMargin(AssetId assetId)
    {
        return this->crossMargins[assetId];
    }

    const CrossMarginState* ContractAccount::findCrossMargin(AssetId assetId) const noexcept
    {
        const auto iterator = this->crossMargins.find(assetId);
        return iterator == this->crossMargins.end() ? nullptr : &iterator->second;
    }

    bool ContractAccount::canWithdraw(AssetId assetId, PositiveBalanceChange amount) const noexcept
    {
        const CrossMarginState* state = this->findCrossMargin(assetId);
        if (!state)
        {
            return true;
        }

        const WideBalance equity = static_cast<WideBalance>(this->queryActiveBalance(assetId).value_or(0)) + state->unrealizedPnL;
        return !state->liquidating && equity - amount >= state->initialMargin;
    }

    Position* StandardContractAccount::findPosition(PositionId positionId) noexcept
    {
        const auto iterator = this->positions.find(positionId);
        return iterator == this->positions.end() ? nullptr : iterator->second;
    }

    const Position* StandardContractAccount::findPosition(PositionId positionId) const noexcept
    {
        const auto iterator = this->positions.find(positionId);
        return iterator == this->positions.end() ? nullptr : iterator->second;
    }

    bool StandardContractAccount::recordPosition(Position position)
    {
        if (position.userId != this->getUserId())
        {
            return false;
        }

        if (this->positions.contains(position.positionId))
        {
            return false;
        }

        if (!this->positionPool)
        {
            this->ownedPositionPool = std::make_unique<PositionPool>();
            this->positionPool = this->ownedPositionPool.get();
        }

        Position* stored = this->positionPool->emplace(std::move(position));
        try
        {
            this->positions.emplace(stored->positionId, stored);
        }
        catch (...)
        {
            this->positionPool->release(stored);
            throw;
        }

        return true;
    }

    bool StandardContractAccount::removePosition(PositionId positionId)
    {
        const auto iterator = this->positions.find(positionId);
        if (iterator == this->positions.end() || iterator->second->managed || iterator->second->positionSize != 0)
        {
            return false;
        }

        Position* position = iterator->second;
        this->positions.erase(iterator);
        this->positionPool->release(position);
        return true;
    }

    std::size_t StandardContractAccount::getPositionCount() const noexcept
    {
        return this->positions.size();
    }
}
