//
// Created by NHCM on 2026/9/20.
//

#ifndef ADVMATCH_USERMANAGER_H
#define ADVMATCH_USERMANAGER_H
#include <map>
#include "../account/User.h"

namespace advmatch
{
    class UserManager
    {
        std::map<UserId, User> users;
        UserId nextUserId = 1;
    public:

        [[nodiscard]]
        UserId createUser();

        [[nodiscard]]
        bool removeUser(UserId userId);

        [[nodiscard]]
        bool containsUser(UserId userId) const noexcept;

        [[nodiscard]]
        User* getUser(UserId userId) noexcept;

        [[nodiscard]]
        const User* getUser(UserId userId) const noexcept;

        [[nodiscard]]
        std::size_t getUserCount() const noexcept;
    };
} // advmatch

#endif //ADVMATCH_USERMANAGER_H
