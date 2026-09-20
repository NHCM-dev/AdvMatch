//
// Created by NHCM on 2026/9/20.
//

#include "UserManager.h"

namespace advmatch
{
    UserId UserManager::createUser()
    {
        const UserId userId = this->nextUserId++;

        this->users.try_emplace(
            userId,
            User::ConstructorKey{},
            userId
        );

        return userId;
    }

    bool UserManager::removeUser(UserId userId)
    {
        return this->users.erase(userId) != 0;
    }

    bool UserManager::containsUser(UserId userId) const noexcept
    {
        return this->users.contains(userId);
    }

    User* UserManager::getUser(UserId userId) noexcept
    {
        const auto iterator = this->users.find(userId);
        if (iterator == this->users.end())
        {
            return nullptr;
        }
        return &iterator->second;
    }

    const User* UserManager::getUser(UserId userId) const noexcept
    {
        const auto iterator = this->users.find(userId);
        if (iterator == this->users.end())
        {
            return nullptr;
        }
        return &iterator->second;
    }

    std::size_t UserManager::getUserCount() const noexcept
    {
        return this->users.size();
    }
} // advmatch