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
        this->userIndex.resize(static_cast<std::size_t>(userId), nullptr);
        const std::optional<UserAccounts> accounts = this->accountManager.createAccounts(userId);
        if (!accounts)
        {
            return 0;
        }

        try
        {
            const auto created = this->users.emplace(
                std::piecewise_construct,
                std::forward_as_tuple(userId),
                std::forward_as_tuple(userId, this->accountManager, *accounts->spotAccount,
                                      *accounts->contractAccount));
            this->userIndex[userId - 1] = &created.first->second;
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

        if (!this->accountManager.removeAccounts(userId))
        {
            return false;
        }

        iterator->second.deactivate();
        --this->activeUserCount;
        return true;
    }

    bool UserManager::containsUser(UserId userId) const noexcept
    {
        return this->getUser(userId) != nullptr;
    }

    User* UserManager::getUser(UserId userId) noexcept
    {
        User* user = userId != 0 && userId <= this->userIndex.size() ? this->userIndex[userId - 1] : nullptr;
        return user && user->isActive() ? user : nullptr;
    }

    const User* UserManager::getUser(UserId userId) const noexcept
    {
        const User* user = userId != 0 && userId <= this->userIndex.size() ? this->userIndex[userId - 1] : nullptr;
        return user && user->isActive() ? user : nullptr;
    }

    std::size_t UserManager::getUserCount() const noexcept
    {
        return this->activeUserCount;
    }
} // advmatch
