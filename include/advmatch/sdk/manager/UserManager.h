#ifndef ADVMATCH_SDK_USER_MANAGER_H
#define ADVMATCH_SDK_USER_MANAGER_H

#include "Manager.h"

namespace advmatch::sdk
{
    class UserManager final : public EntityManager<User, ReadKind::Users>
    {
    public:
        using EntityManager::EntityManager;

        CompletableFuture<User> create(std::uint64_t requestId = 0) const
        {
            return detail::Results::created<User>(this->api, commands::CreateUser{}, requestId);
        }
    };
}

#endif // ADVMATCH_SDK_USER_MANAGER_H
