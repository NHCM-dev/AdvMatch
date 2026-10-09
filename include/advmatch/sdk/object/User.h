#ifndef ADVMATCH_SDK_USER_H
#define ADVMATCH_SDK_USER_H

#include "SpotAccount.h"
#include "ContractAccount.h"

namespace advmatch::sdk
{
    class User final : public Entity<UserView, UserId, ReadKind::User>
    {
    public:
        using Entity::Entity;

        CompletableFuture<SpotAccount> spotAccount() const
        {
            return this->snapshot().thenApply([api = this->api](const UserView& user)
            {
                return SpotAccount(api, user.spotAccountId);
            });
        }

        CompletableFuture<ContractAccount> contractAccount() const
        {
            return this->snapshot().thenApply([api = this->api](const UserView& user)
            {
                return ContractAccount(api, user.contractAccountId);
            });
        }

        CompletableFuture<CommandReceipt> remove(std::uint64_t requestId = 0) const
        {
            return detail::Results::checked(this->api, commands::RemoveUser{this->id()}, requestId);
        }

        CompletableFuture<ReadResponse<OrderView>> orders(ObservationQuery query = {}) const
        {
            query.kind = ReadKind::UserOpenOrders;
            query.userId = this->id();
            return this->api->readRows<OrderView>(std::move(query));
        }

        CompletableFuture<ReadResponse<PositionView>> positions(ObservationQuery query = {}) const
        {
            query.kind = ReadKind::Positions;
            query.userId = this->id();
            return this->api->readRows<PositionView>(std::move(query));
        }
    };
}

#endif // ADVMATCH_SDK_USER_H
