//
// Created by NHCM on 2026/9/20.
//

#ifndef ADVMATCH_USERMANAGER_H
#define ADVMATCH_USERMANAGER_H
#include "../account/User.h"
#include "../util/Containers.h"

namespace advmatch
{
    class UserManager
    {
        NodeHashMap<UserId, User> users;
        UserId nextUserId = 1;
    public:

        void reserve(std::size_t userCount)
        {
            this->users.reserve(userCount);
        }

        [[nodiscard]]
        UserId createUser();

        [[nodiscard]]
        bool removeUser(UserId userId);

        [[nodiscard]]
        bool containsUser(UserId userId) const noexcept;

        // NodeHashMap keeps the returned address valid across insertions and
        // rehashes. It becomes invalid when that user is removed or this
        // manager dies. Concurrent access still requires external
        // synchronization.
        [[nodiscard]]
        User* getUser(UserId userId) noexcept;

        [[nodiscard]]
        const User* getUser(UserId userId) const noexcept;

        [[nodiscard]]
        std::size_t getUserCount() const noexcept;

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
    };
} // advmatch

#endif //ADVMATCH_USERMANAGER_H
