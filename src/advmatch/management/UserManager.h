//
// Created by NHCM on 2026/9/20.
//

#ifndef ADVMATCH_USERMANAGER_H
#define ADVMATCH_USERMANAGER_H
#include "advmatch/ledging/User.h"
#include "AccountManager.h"
#include "../util/Containers.h"
#include <vector>

namespace advmatch
{
    class UserManager
    {
        friend class observe::TradingObserver;
        friend class ::advmatch::StateSnapshotCodec;
        AccountManager accountManager;
        NodeHashMap<UserId, User> users;
        // IDs are issued locally and never reused. Pointer slots provide direct
        // lookup; ownership and iteration remain in the stable node container.
        std::vector<User*> userIndex;
        UserId nextUserId = 1;
        std::size_t activeUserCount = 0;

    public:
        void reserve(std::size_t userCount)
        {
            this->accountManager.reserve(userCount);
            this->users.reserve(userCount);
            this->userIndex.reserve(userCount);
        }

        [[nodiscard]]
        UserId createUser();

        [[nodiscard]]
        bool removeUser(UserId userId);

        [[nodiscard]]
        bool containsUser(UserId userId) const noexcept;

        // NodeHashMap keeps the returned address valid across insertions and
        // rehashes. Removal only deactivates the User, so addresses cached by
        // accepted orders remain valid until this manager dies. New lookups do
        // not return deactivated users.
        [[nodiscard]]
        User* getUser(UserId userId) noexcept;

        [[nodiscard]]
        const User* getUser(UserId userId) const noexcept;

        [[nodiscard]]
        std::size_t getUserCount() const noexcept;

        [[nodiscard]]
        AccountManager& getAccountManager() noexcept
        {
            return this->accountManager;
        }

        [[nodiscard]]
        const AccountManager& getAccountManager() const noexcept
        {
            return this->accountManager;
        }

        auto begin()
        {
            return users.begin();
        }

        auto end()
        {
            return users.end();
        }

        auto begin() const
        {
            return users.begin();
        }

        auto end() const
        {
            return users.end();
        }
        // Run on the owning thread. The callback must not mutate this manager.
        template <typename Visitor>
        void forEachUser(Visitor&& visitor) const
        {
            for (const auto& [id, value] : this->users)
            {
                if (!value.isActive())
                {
                    continue;
                }

                visitor(id, value);
            }
        }

    };
} // advmatch

#endif //ADVMATCH_USERMANAGER_H
