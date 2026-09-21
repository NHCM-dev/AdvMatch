//
// Created by NHCM on 2026/9/20.
//

#include "UserManager.h"

namespace advmatch
{
    UserId UserManager::createUser()
    {
        const UserId userId = this->nextUserId++;

        this->users.emplace(
            userId,
            userId
        );

        ++this->activeUserCount;

        return userId;
    }

    bool UserManager::removeUser(UserId userId)
    {
        const auto iterator = this->users.find(userId);
        if (iterator == this->users.end() || !iterator->second.isActive())
        {
            return false;
        }

        iterator->second.deactivate();
        --this->activeUserCount;
        return true;
    }

    bool UserManager::containsUser(UserId userId) const noexcept
    {
        const auto iterator = this->users.find(userId);
        return iterator != this->users.end() && iterator->second.isActive();
    }

    User* UserManager::getUser(UserId userId) noexcept
    {
        const auto iterator = this->users.find(userId);
        if (iterator == this->users.end() || !iterator->second.isActive())
        {
            return nullptr;
        }
        return &iterator->second;
    }

    const User* UserManager::getUser(UserId userId) const noexcept
    {
        const auto iterator = this->users.find(userId);
        if (iterator == this->users.end() || !iterator->second.isActive())
        {
            return nullptr;
        }
        return &iterator->second;
    }

    std::size_t UserManager::getUserCount() const noexcept
    {
        return this->activeUserCount;
    }
} // advmatch
