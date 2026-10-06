#include "ContractAccount.h"

#include <utility>

namespace advmatch
{
    Position* StandardContractAccount::findPosition(PositionId positionId) noexcept
    {
        const auto iterator = this->positions.find(positionId);
        return iterator == this->positions.end() ? nullptr : &iterator->second;
    }

    const Position* StandardContractAccount::findPosition(PositionId positionId) const noexcept
    {
        const auto iterator = this->positions.find(positionId);
        return iterator == this->positions.end() ? nullptr : &iterator->second;
    }

    bool StandardContractAccount::recordPosition(Position position)
    {
        if (position.userId != this->getUserId())
        {
            return false;
        }

        return this->positions.emplace(position.positionId, std::move(position)).second;
    }

    bool StandardContractAccount::removePosition(PositionId positionId)
    {
        return this->positions.erase(positionId) != 0;
    }

    std::size_t StandardContractAccount::getPositionCount() const noexcept
    {
        return this->positions.size();
    }
}
