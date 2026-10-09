#ifndef ADVMATCH_SDK_TRADING_CONTROL_MANAGER_H
#define ADVMATCH_SDK_TRADING_CONTROL_MANAGER_H

#include "Manager.h"

namespace advmatch::sdk
{
    class TradingControlManager final : public Manager
    {
        CompletableFuture<CommandReceipt> change(SymbolId symbolId, bool halted, Timestamp timestamp, std::string reason, std::uint64_t requestId) const
        {
            return detail::Results::checked(this->api, commands::SetTradingHalt{symbolId, halted, timestamp, std::move(reason)}, requestId);
        }

        CompletableFuture<CommandReceipt> change(const Symbol& symbol, bool halted, Timestamp timestamp, std::string reason, std::uint64_t requestId) const
        {
            return detail::Results::attempt<CommandReceipt>([&]
            {
                symbol.requireOwner(this->api);
                return this->change(symbol.id(), halted, timestamp, std::move(reason), requestId);
            });
        }

        CompletableFuture<TradingStatusView> readStatus(SymbolId symbolId) const
        {
            ObservationQuery query;
            query.symbolId = symbolId;
            return this->read<TradingStatusView>(ReadKind::TradingStatus, query).thenApply(detail::Results::one<TradingStatusView>);
        }

    public:
        using Manager::Manager;

        CompletableFuture<CommandReceipt> halt(std::string reason, Timestamp timestamp, std::uint64_t requestId = 0) const
        {
            return this->change(0, true, timestamp, std::move(reason), requestId);
        }

        CompletableFuture<CommandReceipt> halt(const Symbol& symbol, std::string reason, Timestamp timestamp, std::uint64_t requestId = 0) const
        {
            return this->change(symbol, true, timestamp, std::move(reason), requestId);
        }

        CompletableFuture<CommandReceipt> resume(Timestamp timestamp, std::uint64_t requestId = 0) const
        {
            return this->change(0, false, timestamp, {}, requestId);
        }

        CompletableFuture<CommandReceipt> resume(const Symbol& symbol, Timestamp timestamp, std::uint64_t requestId = 0) const
        {
            return this->change(symbol, false, timestamp, {}, requestId);
        }

        CompletableFuture<TradingStatusView> status() const
        {
            return this->readStatus(0);
        }

        CompletableFuture<TradingStatusView> status(const Symbol& symbol) const
        {
            return detail::Results::attempt<TradingStatusView>([&]
            {
                symbol.requireOwner(this->api);
                return this->readStatus(symbol.id());
            });
        }
    };
}

#endif // ADVMATCH_SDK_TRADING_CONTROL_MANAGER_H
