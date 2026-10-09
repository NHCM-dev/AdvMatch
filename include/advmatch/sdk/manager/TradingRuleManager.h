#ifndef ADVMATCH_SDK_TRADING_RULE_MANAGER_H
#define ADVMATCH_SDK_TRADING_RULE_MANAGER_H

#include "Manager.h"

namespace advmatch::sdk
{
    class TradingRuleManager final : public Manager
    {
    public:
        using Manager::Manager;

        CompletableFuture<CommandReceipt> configure(const Symbol& symbol, TradingRules rules, std::uint64_t requestId = 0) const
        {
            return detail::Results::attempt<CommandReceipt>([&]
            {
                symbol.requireOwner(this->api);
                return detail::Results::checked(this->api, commands::ConfigureTradingRules{symbol.id(), rules}, requestId);
            });
        }

        CompletableFuture<UserRiskView> userRisk(const Symbol& symbol, const User& user) const
        {
            return detail::Results::attempt<UserRiskView>([&]
            {
                symbol.requireOwner(this->api);
                user.requireOwner(this->api);
                ObservationQuery query;
                query.symbolId = symbol.id();
                query.userId = user.id();
                return this->read<UserRiskView>(ReadKind::UserRisk, query).thenApply(detail::Results::one<UserRiskView>);
            });
        }

        CompletableFuture<FeeBalanceView> spotFees(const Asset& asset) const
        {
            return detail::Results::attempt<FeeBalanceView>([&]
            {
                asset.requireOwner(this->api);
                ObservationQuery query;
                query.assetId = asset.id();
                return this->read<FeeBalanceView>(ReadKind::SpotFeeBalance, query).thenApply(detail::Results::one<FeeBalanceView>);
            });
        }

        CompletableFuture<CommandReceipt> withdrawSpotFees(const Account& destination, const Asset& asset, PositiveBalanceChange amount, std::uint64_t requestId = 0) const
        {
            return detail::Results::attempt<CommandReceipt>([&]
            {
                destination.requireOwner(this->api);
                asset.requireOwner(this->api);
                return detail::Results::checked(this->api, commands::WithdrawSpotFees{destination.id(), asset.id(), amount}, requestId);
            });
        }
    };
}

#endif // ADVMATCH_SDK_TRADING_RULE_MANAGER_H
