//
// Created by NHCM on 2026/9/20.
//

#include "UserManager.h"

#include <tuple>

namespace advmatch
{
    UserId UserManager::createUser()
    {
        if (this->nextUserId == 0)
        {
            return 0;
        }

        const UserId userId = this->nextUserId++;
        const std::optional<UserAccounts> accounts = this->accountManager.createAccounts(userId);
        if (!accounts)
        {
            return 0;
        }

        try
        {
            this->users.emplace(
                std::piecewise_construct,
                std::forward_as_tuple(userId),
                std::forward_as_tuple(userId, this->accountManager, *accounts->spotAccount,
                                      *accounts->contractAccount));
        }
        catch (...)
        {
            static_cast<void>(this->accountManager.removeAccounts(userId));
            throw;
        }

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
        static_cast<void>(this->accountManager.removeAccounts(userId));
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
