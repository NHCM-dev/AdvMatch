#ifndef ADVMATCH_SDK_ACCOUNT_H
#define ADVMATCH_SDK_ACCOUNT_H

#include "Asset.h"

namespace advmatch::sdk
{
    class Account : public Entity<AccountView, AccountId, ReadKind::Account>
    {
    protected:
        virtual std::optional<AccountType> expectedType() const noexcept = 0;

    public:
        using Entity::Entity;

        CompletableFuture<AccountView> snapshot() const override
        {
            const auto type = this->expectedType();
            return Entity::snapshot().thenApply([type](const AccountView& value)
            {
                if (type && value.type != *type)
                {
                    throw std::invalid_argument("account type mismatch");
                }

                return value;
            });
        }

        CompletableFuture<CommandReceipt> updateBalance(const Asset& asset, BalanceChange change, std::uint64_t requestId = 0) const
        {
            return detail::Results::attempt<CommandReceipt>([&]
            {
                asset.requireOwner(this->api);
                return detail::Results::checked(this->api, commands::UpdateBalance{this->id(), asset.id(), change}, requestId);
            });
        }

        CompletableFuture<CommandReceipt> transferTo(const Account& target, const Asset& asset, PositiveBalanceChange amount, std::uint64_t requestId = 0) const
        {
            return detail::Results::attempt<CommandReceipt>([&]
            {
                target.requireOwner(this->api);
                asset.requireOwner(this->api);
                return detail::Results::checked(this->api, commands::Transfer{this->id(), target.id(), asset.id(), amount}, requestId);
            });
        }

        CompletableFuture<ReadResponse<BalanceView>> balances(ObservationQuery query = {}) const
        {
            query.accountId = this->id();
            query.kind = ReadKind::Balances;
            return this->api->readRows<BalanceView>(std::move(query));
        }

        CompletableFuture<BalanceView> balance(const Asset& asset) const
        {
            return detail::Results::attempt<BalanceView>([&]
            {
                asset.requireOwner(this->api);
                ObservationQuery query;
                query.kind = ReadKind::Balance;
                query.accountId = this->id();
                query.assetId = asset.id();
                return this->api->readRows<BalanceView>(query).thenApply(detail::Results::one<BalanceView>);
            });
        }

        CompletableFuture<ReadResponse<PositionView>> positions(ObservationQuery query = {}) const
        {
            query.accountId = this->id();
            query.kind = ReadKind::AccountPositions;
            return this->api->readRows<PositionView>(std::move(query));
        }

        CompletableFuture<ReadResponse<CrossMarginView>> crossMargins(ObservationQuery query = {}) const
        {
            query.accountId = this->id();
            query.kind = ReadKind::CrossMargins;
            return this->api->readRows<CrossMarginView>(std::move(query));
        }
    };
}

#endif // ADVMATCH_SDK_ACCOUNT_H
