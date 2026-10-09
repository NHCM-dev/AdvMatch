#ifndef ADVMATCH_SDK_ACCOUNT_MANAGER_H
#define ADVMATCH_SDK_ACCOUNT_MANAGER_H

#include "Manager.h"

namespace advmatch::sdk
{
    class AccountManager final : public Manager
    {
    public:
        using Manager::Manager;

        CompletableFuture<std::shared_ptr<Account>> find(AccountId id) const
        {
            ObservationQuery query;
            query.kind = ReadKind::Account;
            query.accountId = id;
            return this->api->readRows<AccountView>(query).thenApply([api = this->api](const ReadResponse<AccountView>& response) -> std::shared_ptr<Account>
            {
                const auto account = detail::Results::one(response);
                if (account.type == AccountType::Spot)
                {
                    return std::make_shared<SpotAccount>(api, account.accountId);
                }

                return std::make_shared<ContractAccount>(api, account.accountId);
            });
        }

        SpotAccount spot(AccountId id) const
        {
            return SpotAccount(this->api, id);
        }

        ContractAccount contract(AccountId id) const
        {
            return ContractAccount(this->api, id);
        }

        CompletableFuture<ReadResponse<AccountView>> list(ObservationQuery query = {}) const
        {
            return this->read<AccountView>(ReadKind::Accounts, std::move(query));
        }

        CompletableFuture<ReadResponse<BalanceView>> balances(ObservationQuery query = {}) const
        {
            return this->read<BalanceView>(ReadKind::Balances, std::move(query));
        }

        CompletableFuture<ReadResponse<CrossMarginView>> crossMargins(ObservationQuery query = {}) const
        {
            return this->read<CrossMarginView>(ReadKind::CrossMargins, std::move(query));
        }
    };
}

#endif // ADVMATCH_SDK_ACCOUNT_MANAGER_H
